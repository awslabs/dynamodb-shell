// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// parse.cpp — AWS JSON 1.0 response parsing (the ddbsh-wire component, P6b).
//
// The inverse of wire/codec.cpp's serialize(): turn a response body into the
// engine's SDK-free *Out structs, field-identical to the AWS adapter's
// from_sdk_* helpers (aws/aws_ddb_client.cpp — the fidelity oracle):
//
//   * parse_value is the exact inverse of from_sdk_value: one AttributeValue
//     object carries exactly one type key.
//   * parse_item mirrors from_sdk_item (object -> ValueMap, response order; the
//     Renderer sorts keys at format time, so order does not affect output).
//   * parse_consumed_capacity mirrors from_sdk_consumed_capacity's `any`-flag
//     logic: nullopt unless at least one of TableName / CapacityUnits /
//     ReadCapacityUnits / WriteCapacityUnits is present.
//   * The per-op assembly mirrors the adapter: item/attributes set only when
//     non-empty; last_evaluated_key set only when non-empty; Count/ScannedCount
//     ignored (the *Out structs have no field for them).
//   * error_from_type_document reproduces from_sdk_error: a
//     ConditionalCheckFailedException maps to Errc::conditional_check_failed
//     (so the UPDATE/DELETE executors count a ccf instead of aborting), every
//     other exception to Errc::service_error, message verbatim.
//
// request_id is left nullopt on the wire path (D-P6b-2): the (operation, body)
// -> body Transport does not surface the x-amzn-RequestId header. This is
// observable only in the composed transaction-failure message (P6c) and any
// caller that prints request_id; no golden path exercises the wire client (the
// goldens run against real DynamoDB via the SDK client), so fidelity is
// unaffected.

#include <charconv>
#include <cstddef>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "ddbsh/wire/codec.hpp"
#include "wire/datetime.hpp"
#include "wire/json_reader.hpp"

namespace ddbsh::wire {
namespace {

using json::JsonValue;

std::unexpected<Error> malformed(const std::string& detail) {
    return fail(Errc::service_error, "malformed response: " + detail);
}

// Locale-independent, strict parse of a JSON number token into a double.
// AWS-JSON number tokens always use '.' as the decimal point, so the parse must
// NOT follow the global C++ locale. FLOATING-POINT std::from_chars is the ideal
// parser, but it is not universally available: libstdc++ (gcc >= 11) ships it,
// while libc++ — the Emscripten/WASM toolchain's standard library — does not
// implement the FP overloads yet. Use from_chars where the library's
// feature-test macro reports the FP overloads (__cpp_lib_to_chars is defined
// only once BOTH integer and FP to/from_chars exist), and otherwise fall back
// to a classic-locale istringstream (also locale-independent). Both require the
// entire token to be consumed (strict), so they are behaviorally identical
// (tenet 4 / portable). Returns nullopt on a malformed/trailing/out-of-range
// token.
std::optional<double> parse_double_token(std::string_view tok) {
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
    double value = 0.0;
    const char* first = tok.data();
    const char* last = first + tok.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr != last) {
        return std::nullopt;
    }
    return value;
#else
    std::istringstream iss{std::string(tok)};
    iss.imbue(std::locale::classic());
    double value = 0.0;
    iss >> value;
    if (iss.fail() || iss.peek() != std::char_traits<char>::eof()) {
        return std::nullopt;
    }
    return value;
#endif
}

// One AttributeValue object -> ddbsh::Value (inverse of codec.cpp av()). The
// object must carry exactly one member whose key selects the type.
Result<Value> parse_value(const JsonValue& node);

// A JSON object of name -> AttributeValue -> ValueMap, in object (response)
// order (mirrors from_sdk_item).
Result<ValueMap> parse_item(const JsonValue& obj) {
    if (!obj.is_object()) {
        return malformed("expected an item object");
    }
    ValueMap out;
    out.reserve(obj.members.size());
    for (const auto& [name, member] : obj.members) {
        auto v = parse_value(member);
        if (!v) {
            return fail(v.error());
        }
        out.emplace_back(name, std::move(*v));
    }
    return out;
}

// A JSON array of AttributeValues -> the string-set element list (SS/NS/BS).
Result<std::vector<std::string>> parse_string_array(const JsonValue& arr, const char* which) {
    if (!arr.is_array()) {
        return malformed(std::string("expected an array for ") + which);
    }
    std::vector<std::string> out;
    out.reserve(arr.elements.size());
    for (const auto& e : arr.elements) {
        if (!e.is_string()) {
            return malformed(std::string("expected a string element in ") + which);
        }
        out.push_back(e.text);
    }
    return out;
}

Result<Value> parse_value(const JsonValue& node) {
    if (!node.is_object() || node.members.size() != 1) {
        return malformed("attribute value must be a single-key object");
    }
    const auto& [tag, body] = node.members.front();

    if (tag == "S") {
        if (!body.is_string()) {
            return malformed("S must be a string");
        }
        return Value::string_value(body.text);
    }
    if (tag == "N") {
        if (!body.is_string()) {
            return malformed("N must be a string");  // AWS JSON 1.0 carries N as a string
        }
        return Value::number_value(body.text);
    }
    if (tag == "B") {
        // Carried verbatim. A B value read from a real DynamoDB/ExtendDB
        // response is already canonical standard base64 (the service re-encodes),
        // so binary_value(verbatim) matches from_sdk_value's Base64Encode(GetB()).
        // Symmetric with codec.cpp's av() B arm; no decode/re-encode needed (and
        // introducing one would require a base64 codec in ddbsh-wire for no
        // fidelity gain today).
        if (!body.is_string()) {
            return malformed("B must be a string");
        }
        return Value::binary_value(body.text);
    }
    if (tag == "BOOL") {
        if (!body.is_bool()) {
            return malformed("BOOL must be a boolean");
        }
        return Value::bool_value(body.boolean);
    }
    if (tag == "NULL") {
        if (!body.is_bool()) {
            return malformed("NULL must be a boolean");
        }
        return Value::null_value();
    }
    if (tag == "L") {
        if (!body.is_array()) {
            return malformed("L must be an array");
        }
        Value out;
        out.type = Value::Type::L;
        out.list.reserve(body.elements.size());
        for (const auto& e : body.elements) {
            auto v = parse_value(e);
            if (!v) {
                return fail(v.error());
            }
            out.list.push_back(std::move(*v));
        }
        return out;
    }
    if (tag == "M") {
        if (!body.is_object()) {
            return malformed("M must be an object");
        }
        Value out;
        out.type = Value::Type::M;
        out.map.reserve(body.members.size());
        for (const auto& [k, mv] : body.members) {
            auto v = parse_value(mv);
            if (!v) {
                return fail(v.error());
            }
            out.map.emplace_back(k, std::move(*v));
        }
        return out;
    }
    if (tag == "SS") {
        auto elems = parse_string_array(body, "SS");
        if (!elems) {
            return fail(elems.error());
        }
        return Value::string_set(std::move(*elems));
    }
    if (tag == "NS") {
        auto elems = parse_string_array(body, "NS");
        if (!elems) {
            return fail(elems.error());
        }
        return Value::number_set(std::move(*elems));
    }
    if (tag == "BS") {
        // Each element verbatim; same canonical-base64 assumption as the B arm.
        auto elems = parse_string_array(body, "BS");
        if (!elems) {
            return fail(elems.error());
        }
        return Value::binary_set(std::move(*elems));
    }
    return malformed("unknown attribute value type '" + tag + "'");
}

// Reads a ConsumedCapacity figure from a JSON number token into a double,
// matching the SDK's GetCapacityUnits() double. A malformed token is a
// malformed response.
Result<double> parse_capacity_double(const JsonValue& node, const char* which) {
    if (!node.is_number()) {
        return malformed(std::string(which) + " must be a number");
    }
    std::optional<double> value = parse_double_token(node.text);
    if (!value) {
        return malformed(std::string("invalid ") + which + " number");
    }
    return *value;
}

// Mirror from_sdk_consumed_capacity: nullopt unless at least one sub-field is
// present. `root` is the top-level response object; the ConsumedCapacity member
// may be absent.
Result<std::optional<ConsumedCapacity>> parse_consumed_capacity(const JsonValue& root) {
    const JsonValue* cc = root.find("ConsumedCapacity");
    if (cc == nullptr) {
        return std::optional<ConsumedCapacity>{std::nullopt};
    }
    if (!cc->is_object()) {
        return malformed("ConsumedCapacity must be an object");
    }
    ConsumedCapacity out;
    bool any = false;
    if (const JsonValue* tn = cc->find("TableName"); tn != nullptr) {
        if (!tn->is_string()) {
            return malformed("ConsumedCapacity.TableName must be a string");
        }
        out.table_name = tn->text;
        any = true;
    }
    if (const JsonValue* cu = cc->find("CapacityUnits"); cu != nullptr) {
        auto d = parse_capacity_double(*cu, "ConsumedCapacity.CapacityUnits");
        if (!d) {
            return fail(d.error());
        }
        out.capacity_units = *d;
        any = true;
    }
    if (const JsonValue* rcu = cc->find("ReadCapacityUnits"); rcu != nullptr) {
        auto d = parse_capacity_double(*rcu, "ConsumedCapacity.ReadCapacityUnits");
        if (!d) {
            return fail(d.error());
        }
        out.read_capacity_units = *d;
        any = true;
    }
    if (const JsonValue* wcu = cc->find("WriteCapacityUnits"); wcu != nullptr) {
        auto d = parse_capacity_double(*wcu, "ConsumedCapacity.WriteCapacityUnits");
        if (!d) {
            return fail(d.error());
        }
        out.write_capacity_units = *d;
        any = true;
    }
    if (!any) {
        return std::optional<ConsumedCapacity>{std::nullopt};
    }
    return std::optional<ConsumedCapacity>{std::move(out)};
}

// The bare exception name from a raw `__type` string: the substring after the
// last '#' (strip the shape/namespace prefix). No '#' => the whole string.
// Shared by error_from_type_document and the public error_type_name so the
// name the retry classifier sees matches the name parse_* reports.
std::string exception_name_from_type(const std::string& type_name) {
    const std::size_t hash = type_name.rfind('#');
    return (hash == std::string::npos) ? type_name : type_name.substr(hash + 1);
}

// __type document -> ddbsh::Error (reproduces from_sdk_error). Precondition:
// `root` has a "__type" member.
Error error_from_type_document(const JsonValue& root) {
    std::string type_name;
    if (const JsonValue* t = root.find("__type"); t != nullptr && t->is_string()) {
        type_name = t->text;
    }
    const std::string name = exception_name_from_type(type_name);

    // DynamoDB uses lowercase "message" in the __type document; accept "Message"
    // too for robustness (matches GetMessage()).
    std::string message;
    if (const JsonValue* m = root.find("message"); m != nullptr && m->is_string()) {
        message = m->text;
    } else if (const JsonValue* m2 = root.find("Message"); m2 != nullptr && m2->is_string()) {
        message = m2->text;
    }

    const Errc code = (name == "ConditionalCheckFailedException") ? Errc::conditional_check_failed
                                                                  : Errc::service_error;
    // request_id is not available from the body alone (D-P6b-2): the wire
    // Transport returns the body, not the x-amzn-RequestId header.
    return Error{code, std::move(message), name, std::nullopt};
}

// Shared entry: read the body to a JSON object and split the error-document
// case from the success case. On success returns the root object by value; on a
// __type document or malformed body returns the Error.
Result<JsonValue> parse_response_object(const std::string& body) {
    auto doc = json::parse(body);
    if (!doc) {
        return doc;
    }
    if (!doc->is_object()) {
        return malformed("expected a top-level object");
    }
    if (doc->find("__type") != nullptr) {
        return fail(error_from_type_document(*doc));
    }
    return doc;
}

// Reads an optional item-bearing member ("Item"/"Attributes"): set only when
// present AND non-empty (mirrors the adapter's `if (!x.empty())`).
Result<std::optional<ItemRow>> parse_optional_item(const JsonValue& root, const char* member) {
    const JsonValue* m = root.find(member);
    if (m == nullptr) {
        return std::optional<ItemRow>{std::nullopt};
    }
    if (!m->is_object()) {
        return malformed(std::string(member) + " must be an object");
    }
    if (m->members.empty()) {
        return std::optional<ItemRow>{std::nullopt};
    }
    auto vm = parse_item(*m);
    if (!vm) {
        return fail(vm.error());
    }
    return std::optional<ItemRow>{ItemRow{std::move(*vm)}};
}

// Reads the "Items" array (Query/Scan): absent => empty vector.
Result<std::vector<ItemRow>> parse_items(const JsonValue& root) {
    std::vector<ItemRow> out;
    const JsonValue* items = root.find("Items");
    if (items == nullptr) {
        return out;
    }
    if (!items->is_array()) {
        return malformed("Items must be an array");
    }
    out.reserve(items->elements.size());
    for (const auto& e : items->elements) {
        auto vm = parse_item(e);
        if (!vm) {
            return fail(vm.error());
        }
        out.push_back(ItemRow{std::move(*vm)});
    }
    return out;
}

// Reads "LastEvaluatedKey": set only when present AND non-empty (mirrors the
// adapter's `if (!lek.empty())`).
Result<std::optional<ValueMap>> parse_last_evaluated_key(const JsonValue& root) {
    const JsonValue* lek = root.find("LastEvaluatedKey");
    if (lek == nullptr) {
        return std::optional<ValueMap>{std::nullopt};
    }
    if (!lek->is_object()) {
        return malformed("LastEvaluatedKey must be an object");
    }
    if (lek->members.empty()) {
        return std::optional<ValueMap>{std::nullopt};
    }
    auto vm = parse_item(*lek);
    if (!vm) {
        return fail(vm.error());
    }
    return std::optional<ValueMap>{std::move(*vm)};
}

}  // namespace

std::optional<std::string> error_type_name(const std::string& body) {
    // Classify a response body for the retry path (HT-1 §4.3) WITHOUT committing
    // to a full parse: return the bare exception name iff `body` is a DynamoDB
    // `__type` error document, else nullopt. A success body or a malformed/
    // non-object body yields nullopt (treated as "not a retryable service
    // error"). Reuses the exact extraction error_from_type_document uses, so the
    // name the retry classifier sees matches what parse_* reports.
    auto doc = json::parse(body);
    if (!doc || !doc->is_object()) {
        return std::nullopt;
    }
    const json::JsonValue* t = doc->find("__type");
    if (t == nullptr || !t->is_string()) {
        return std::nullopt;
    }
    return exception_name_from_type(t->text);
}

Result<GetItemOut> parse_get_item(const std::string& body) {
    auto root = parse_response_object(body);
    if (!root) {
        return fail(root.error());
    }
    GetItemOut out;
    auto item = parse_optional_item(*root, "Item");
    if (!item) {
        return fail(item.error());
    }
    out.item = std::move(*item);
    auto cc = parse_consumed_capacity(*root);
    if (!cc) {
        return fail(cc.error());
    }
    out.consumed_capacity = std::move(*cc);
    return out;
}

Result<QueryOut> parse_query(const std::string& body) {
    auto root = parse_response_object(body);
    if (!root) {
        return fail(root.error());
    }
    QueryOut out;
    auto items = parse_items(*root);
    if (!items) {
        return fail(items.error());
    }
    out.items = std::move(*items);
    auto lek = parse_last_evaluated_key(*root);
    if (!lek) {
        return fail(lek.error());
    }
    out.last_evaluated_key = std::move(*lek);
    auto cc = parse_consumed_capacity(*root);
    if (!cc) {
        return fail(cc.error());
    }
    out.consumed_capacity = std::move(*cc);
    return out;
}

Result<ScanOut> parse_scan(const std::string& body) {
    auto root = parse_response_object(body);
    if (!root) {
        return fail(root.error());
    }
    ScanOut out;
    auto items = parse_items(*root);
    if (!items) {
        return fail(items.error());
    }
    out.items = std::move(*items);
    auto lek = parse_last_evaluated_key(*root);
    if (!lek) {
        return fail(lek.error());
    }
    out.last_evaluated_key = std::move(*lek);
    auto cc = parse_consumed_capacity(*root);
    if (!cc) {
        return fail(cc.error());
    }
    out.consumed_capacity = std::move(*cc);
    return out;
}

// Put/Update/Delete share the same response shape: an optional "Attributes"
// item plus optional ConsumedCapacity.
namespace {
template <class Out>
Result<Out> parse_write_response(const std::string& body) {
    auto root = parse_response_object(body);
    if (!root) {
        return fail(root.error());
    }
    Out out;
    auto attrs = parse_optional_item(*root, "Attributes");
    if (!attrs) {
        return fail(attrs.error());
    }
    out.attributes = std::move(*attrs);
    auto cc = parse_consumed_capacity(*root);
    if (!cc) {
        return fail(cc.error());
    }
    out.consumed_capacity = std::move(*cc);
    return out;
}
}  // namespace

Result<PutItemOut> parse_put_item(const std::string& body) {
    return parse_write_response<PutItemOut>(body);
}

Result<UpdateItemOut> parse_update_item(const std::string& body) {
    return parse_write_response<UpdateItemOut>(body);
}

Result<DeleteItemOut> parse_delete_item(const std::string& body) {
    return parse_write_response<DeleteItemOut>(body);
}

// ============================================================================
// P6c-b: control-plane / backup-restore / transaction / vector parsers.
//
// Each is the field-for-field inverse of the AWS adapter's response assembly
// (aws/aws_ddb_client.cpp). Report fields are read leniently — a missing or
// wrong-typed member yields the struct default, exactly as the SDK returns a
// default-constructed value for an unset field (the adapter reads them with
// Get*() unconditionally). Enum-valued fields are read as the raw response NAME
// string the SDK mapper would have produced (§11). Datetimes are epoch-seconds
// JSON numbers formatted via epoch_to_iso8601 to match ToGmtString(ISO_8601).
// The helpers below reuse the first anonymous namespace's parse_response_object
// / parse_item / malformed (internal linkage, visible for the rest of the TU).
// ============================================================================
namespace {

// Lenient scalar readers mirroring the adapter's Get*()-with-default reads.
std::string str_field(const JsonValue& obj, const char* key) {
    const JsonValue* m = obj.find(key);
    return (m != nullptr && m->is_string()) ? m->text : std::string();
}

bool bool_field(const JsonValue& obj, const char* key) {
    const JsonValue* m = obj.find(key);
    return (m != nullptr && m->is_bool()) ? m->boolean : false;
}

long long ll_field(const JsonValue& obj, const char* key, long long dflt = 0) {
    const JsonValue* m = obj.find(key);
    if (m == nullptr || !m->is_number()) {
        return dflt;
    }
    long long v = dflt;
    const char* first = m->text.data();
    const char* last = first + m->text.size();
    const auto [ptr, ec] = std::from_chars(first, last, v);
    return (ec == std::errc{} && ptr == last) ? v : dflt;
}

double double_field(const JsonValue& obj, const char* key, double dflt = 0.0) {
    const JsonValue* m = obj.find(key);
    if (m == nullptr || !m->is_number()) {
        return dflt;
    }
    return parse_double_token(m->text).value_or(dflt);
}

// An epoch-seconds JSON number formatted to the exact string the adapter's
// ToGmtString(ISO_8601) produced. A missing member defaults to epoch 0
// ("1970-01-01T00:00:00Z"), matching the SDK's default-constructed DateTime.
std::string iso_datetime_field(const JsonValue& obj, const char* key) {
    return epoch_to_iso8601(double_field(obj, key, 0.0));
}

// Load an SDK-style KeySchema array (HASH->hash_keys, RANGE->range_keys, in
// response/composite order). nullptr or non-array => empty (mirrors
// load_key_schema over an unset member).
TableKeySchema load_key_schema(const JsonValue* arr) {
    TableKeySchema out;
    if (arr == nullptr || !arr->is_array()) {
        return out;
    }
    for (const auto& e : arr->elements) {
        const std::string kt = str_field(e, "KeyType");
        const std::string name = str_field(e, "AttributeName");
        if (kt == "HASH") {
            out.hash_keys.push_back(name);
        } else if (kt == "RANGE") {
            out.range_keys.push_back(name);
        }
    }
    return out;
}

// ProjectionType string -> Projection (inverse of from_sdk_projection). An
// absent/unknown type maps to ALL (the SDK's NOT_SET default case).
Projection load_projection(const JsonValue& obj) {
    Projection out;
    const std::string type = str_field(obj, "ProjectionType");
    if (type == "KEYS_ONLY") {
        out.type = ProjectionType::keys_only;
    } else if (type == "INCLUDE") {
        out.type = ProjectionType::include;
        if (const JsonValue* nk = obj.find("NonKeyAttributes"); nk != nullptr && nk->is_array()) {
            for (const auto& a : nk->elements) {
                if (a.is_string()) {
                    out.non_key_attributes.push_back(a.text);
                }
            }
        }
    } else {
        out.type = ProjectionType::all;
    }
    return out;
}

ScalarType load_scalar(const std::string& t) {
    if (t == "N") {
        return ScalarType::n;
    }
    if (t == "B") {
        return ScalarType::b;
    }
    return ScalarType::s;
}

// Full DESCRIBE detail for one GSI (inverse of load_gsi_detail).
SecondaryIndexSchema load_gsi_detail(const JsonValue& gsi) {
    SecondaryIndexSchema out;
    out.index_name = str_field(gsi, "IndexName");
    out.key_schema = load_key_schema(gsi.find("KeySchema"));
    if (const JsonValue* p = gsi.find("Projection"); p != nullptr && p->is_object()) {
        out.projection = load_projection(*p);
        out.projection_set = true;
    }
    out.index_status = str_field(gsi, "IndexStatus");
    out.backfilling = bool_field(gsi, "Backfilling");
    if (const JsonValue* pt = gsi.find("ProvisionedThroughput"); pt != nullptr && pt->is_object()) {
        out.provisioned_read_capacity_units = ll_field(*pt, "ReadCapacityUnits");
        out.provisioned_write_capacity_units = ll_field(*pt, "WriteCapacityUnits");
    }
    if (const JsonValue* od = gsi.find("OnDemandThroughput"); od != nullptr && od->is_object()) {
        out.on_demand_throughput_set = true;
        out.on_demand_max_read_request_units = ll_field(*od, "MaxReadRequestUnits", -1);
        out.on_demand_max_write_request_units = ll_field(*od, "MaxWriteRequestUnits", -1);
    }
    if (const JsonValue* w = gsi.find("WarmThroughput"); w != nullptr && w->is_object()) {
        out.warm_throughput =
            WarmThroughputDescription{ll_field(*w, "ReadUnitsPerSecond"),
                                      ll_field(*w, "WriteUnitsPerSecond"), str_field(*w, "Status")};
    }
    return out;
}

// One vector index read back from DescribeTable (inverse of
// load_vector_index_detail).
VectorIndexSchema load_vector_index_detail(const JsonValue& vi) {
    VectorIndexSchema out;
    out.index_name = str_field(vi, "IndexName");
    if (const JsonValue* va = vi.find("VectorAttribute"); va != nullptr && va->is_object()) {
        out.vector_attr = str_field(*va, "AttributeName");
    }
    out.dimensions = ll_field(vi, "Dimensions");
    const std::string df = str_field(vi, "DistanceFunction");
    if (df == "EUCLIDEAN") {
        out.distance = VectorDistance::euclidean;
    } else if (df == "DOT_PRODUCT") {
        out.distance = VectorDistance::dot_product;
    } else {
        out.distance = VectorDistance::cosine;
    }
    if (const JsonValue* ss = vi.find("SearchSchema"); ss != nullptr && ss->is_array()) {
        for (const auto& se : ss->elements) {
            const SearchSchemaKind kind = (str_field(se, "SearchSchemaElementType") == "HASH")
                                              ? SearchSchemaKind::hash
                                              : SearchSchemaKind::inline_filter;
            out.search_schema.push_back(SearchSchemaElem{str_field(se, "AttributeName"), kind});
        }
    }
    if (const JsonValue* p = vi.find("Projection"); p != nullptr && p->is_object()) {
        out.projection = load_projection(*p);
        out.projection_set = true;
    }
    out.index_status = str_field(vi, "IndexStatus");
    return out;
}

// Token-only acknowledgement: the mutating control-plane ops and
// TransactWriteItems ignore their success body (the adapter only checks
// IsSuccess()); the parser returns the empty Out on success, the mapped Error
// on a `__type` document, or a malformed Error on an unparseable body.
template <class Out>
Result<Out> parse_ack(const std::string& body) {
    auto root = parse_response_object(body);
    if (!root) {
        return fail(root.error());
    }
    return Out{};
}

}  // namespace

Result<DescribeTableOut> parse_describe_table(const std::string& body) {
    auto root = parse_response_object(body);
    if (!root) {
        return fail(root.error());
    }
    const JsonValue* table = root->find("Table");
    if (table == nullptr || !table->is_object()) {
        return malformed("DescribeTable response missing Table");
    }
    DescribeTableOut out;
    out.table_name = str_field(*table, "TableName");
    out.key_schema = load_key_schema(table->find("KeySchema"));
    if (const JsonValue* lsis = table->find("LocalSecondaryIndexes");
        lsis != nullptr && lsis->is_array()) {
        for (const auto& lsi : lsis->elements) {
            SecondaryIndexSchema s;
            s.index_name = str_field(lsi, "IndexName");
            s.key_schema = load_key_schema(lsi.find("KeySchema"));
            out.local_secondary_indexes.push_back(std::move(s));
        }
    }
    if (const JsonValue* gsis = table->find("GlobalSecondaryIndexes");
        gsis != nullptr && gsis->is_array()) {
        for (const auto& gsi : gsis->elements) {
            out.global_secondary_indexes.push_back(load_gsi_detail(gsi));
        }
    }
    if (const JsonValue* vis = table->find("VectorIndexes"); vis != nullptr && vis->is_array()) {
        for (const auto& vi : vis->elements) {
            out.vector_indexes.push_back(load_vector_index_detail(vi));
        }
    }
    out.table_status = str_field(*table, "TableStatus");
    out.table_id = str_field(*table, "TableId");
    out.table_arn = str_field(*table, "TableArn");
    if (const JsonValue* bms = table->find("BillingModeSummary");
        bms != nullptr && bms->is_object()) {
        out.billing_mode_summary = str_field(*bms, "BillingMode");
    }
    out.provisioned_throughput_set = (table->find("ProvisionedThroughput") != nullptr);
    if (const JsonValue* tcs = table->find("TableClassSummary");
        tcs != nullptr && tcs->is_object()) {
        out.table_class_summary = str_field(*tcs, "TableClass");
    }
    if (const JsonValue* ads = table->find("AttributeDefinitions");
        ads != nullptr && ads->is_array()) {
        for (const auto& ad : ads->elements) {
            out.attribute_definitions.push_back(AttributeDef{
                str_field(ad, "AttributeName"), load_scalar(str_field(ad, "AttributeType"))});
        }
    }
    out.creation_date_time = iso_datetime_field(*table, "CreationDateTime");
    out.table_size_bytes = ll_field(*table, "TableSizeBytes");
    out.item_count = ll_field(*table, "ItemCount");
    out.deletion_protection_enabled = bool_field(*table, "DeletionProtectionEnabled");
    if (const JsonValue* pt = table->find("ProvisionedThroughput");
        pt != nullptr && pt->is_object()) {
        out.provisioned_read_capacity_units = ll_field(*pt, "ReadCapacityUnits");
        out.provisioned_write_capacity_units = ll_field(*pt, "WriteCapacityUnits");
    }
    if (const JsonValue* od = table->find("OnDemandThroughput"); od != nullptr && od->is_object()) {
        out.on_demand_throughput_set = true;
        out.on_demand_max_read_request_units = ll_field(*od, "MaxReadRequestUnits", -1);
        out.on_demand_max_write_request_units = ll_field(*od, "MaxWriteRequestUnits", -1);
    }
    if (const JsonValue* w = table->find("WarmThroughput"); w != nullptr && w->is_object()) {
        out.warm_throughput =
            WarmThroughputDescription{ll_field(*w, "ReadUnitsPerSecond"),
                                      ll_field(*w, "WriteUnitsPerSecond"), str_field(*w, "Status")};
    }
    if (const JsonValue* ss = table->find("StreamSpecification");
        ss != nullptr && ss->is_object()) {
        out.stream_specification_set = true;
        if (bool_field(*ss, "StreamEnabled")) {
            out.stream_view_type = str_field(*ss, "StreamViewType");
        }
    }
    if (const JsonValue* sse = table->find("SSEDescription"); sse != nullptr && sse->is_object()) {
        out.sse_status = str_field(*sse, "Status");
    }
    return out;
}

Result<TimeToLiveOut> parse_describe_time_to_live(const std::string& body) {
    auto root = parse_response_object(body);
    if (!root) {
        return fail(root.error());
    }
    TimeToLiveOut out;
    if (const JsonValue* d = root->find("TimeToLiveDescription"); d != nullptr && d->is_object()) {
        out.status = str_field(*d, "TimeToLiveStatus");
        out.attribute_name = str_field(*d, "AttributeName");
    }
    return out;
}

Result<ContinuousBackupsOut> parse_describe_continuous_backups(const std::string& body) {
    auto root = parse_response_object(body);
    if (!root) {
        return fail(root.error());
    }
    ContinuousBackupsOut out;
    const JsonValue* cbd = root->find("ContinuousBackupsDescription");
    const JsonValue* pitr = (cbd != nullptr && cbd->is_object())
                                ? cbd->find("PointInTimeRecoveryDescription")
                                : nullptr;
    if (pitr != nullptr && pitr->is_object()) {
        out.pitr_status = str_field(*pitr, "PointInTimeRecoveryStatus");
        if (out.pitr_status == "ENABLED") {
            out.earliest_restorable_date_time =
                iso_datetime_field(*pitr, "EarliestRestorableDateTime");
            out.latest_restorable_date_time = iso_datetime_field(*pitr, "LatestRestorableDateTime");
        }
    }
    return out;
}

Result<TableReplicaAutoScalingOut> parse_describe_table_replica_auto_scaling(
    const std::string& body) {
    auto root = parse_response_object(body);
    if (!root) {
        return fail(root.error());
    }
    TableReplicaAutoScalingOut out;
    if (const JsonValue* asd = root->find("TableAutoScalingDescription");
        asd != nullptr && asd->is_object()) {
        if (const JsonValue* reps = asd->find("Replicas"); reps != nullptr && reps->is_array()) {
            for (const auto& r : reps->elements) {
                out.replicas.push_back(ReplicaAutoScalingDescription{
                    str_field(r, "RegionName"), str_field(r, "ReplicaStatus")});
            }
        }
    }
    return out;
}

Result<DescribeLimitsOut> parse_describe_limits(const std::string& body) {
    auto root = parse_response_object(body);
    if (!root) {
        return fail(root.error());
    }
    DescribeLimitsOut out;
    out.account_max_rcu = ll_field(*root, "AccountMaxReadCapacityUnits");
    out.account_max_wcu = ll_field(*root, "AccountMaxWriteCapacityUnits");
    out.table_max_rcu = ll_field(*root, "TableMaxReadCapacityUnits");
    out.table_max_wcu = ll_field(*root, "TableMaxWriteCapacityUnits");
    return out;
}

Result<DescribeBackupOut> parse_describe_backup(const std::string& body) {
    auto root = parse_response_object(body);
    if (!root) {
        return fail(root.error());
    }
    const JsonValue* bd = root->find("BackupDescription");
    if (bd == nullptr || !bd->is_object()) {
        return malformed("DescribeBackup response missing BackupDescription");
    }
    DescribeBackupOut out;
    if (const JsonValue* sd = bd->find("SourceTableDetails"); sd != nullptr && sd->is_object()) {
        out.source_table_name = str_field(*sd, "TableName");
        out.source_table_id = str_field(*sd, "TableId");
        out.source_table_arn = str_field(*sd, "TableArn");
        out.table_size_bytes = ll_field(*sd, "TableSizeBytes");
        out.item_count = ll_field(*sd, "ItemCount");
        if (const JsonValue* ks = sd->find("KeySchema"); ks != nullptr && ks->is_array()) {
            for (const auto& e : ks->elements) {
                out.key_schema.push_back(
                    BackupKeySchemaElement{str_field(e, "KeyType"), str_field(e, "AttributeName")});
            }
        }
        out.table_creation_datetime = iso_datetime_field(*sd, "TableCreationDateTime");
        if (sd->find("BillingMode") != nullptr) {
            out.billing_mode = str_field(*sd, "BillingMode");
        }
        if (const JsonValue* pt = sd->find("ProvisionedThroughput");
            pt != nullptr && pt->is_object()) {
            out.provisioned = std::make_pair(ll_field(*pt, "ReadCapacityUnits"),
                                             ll_field(*pt, "WriteCapacityUnits"));
        }
    }
    if (const JsonValue* bdet = bd->find("BackupDetails"); bdet != nullptr && bdet->is_object()) {
        out.backup_name = str_field(*bdet, "BackupName");
        out.backup_arn = str_field(*bdet, "BackupArn");
        out.backup_size_bytes = ll_field(*bdet, "BackupSizeBytes");
        out.backup_status = str_field(*bdet, "BackupStatus");
        out.backup_type = str_field(*bdet, "BackupType");
        out.backup_creation_datetime = iso_datetime_field(*bdet, "BackupCreationDateTime");
        out.backup_expiry_datetime = iso_datetime_field(*bdet, "BackupExpiryDateTime");
    }
    return out;
}

Result<ListBackupsOut> parse_list_backups(const std::string& body) {
    auto root = parse_response_object(body);
    if (!root) {
        return fail(root.error());
    }
    ListBackupsOut out;
    if (const JsonValue* summaries = root->find("BackupSummaries");
        summaries != nullptr && summaries->is_array()) {
        for (const auto& s : summaries->elements) {
            out.summaries.push_back(BackupSummaryRow{
                str_field(s, "TableName"), str_field(s, "BackupName"), str_field(s, "BackupStatus"),
                str_field(s, "BackupArn"), iso_datetime_field(s, "BackupCreationDateTime"),
                iso_datetime_field(s, "BackupExpiryDateTime")});
        }
    }
    const std::string cursor = str_field(*root, "LastEvaluatedBackupArn");
    if (!cursor.empty()) {
        out.last_evaluated_backup_arn = cursor;
    }
    return out;
}

Result<SearchVectorsOut> parse_search_vectors(const std::string& body) {
    auto root = parse_response_object(body);
    if (!root) {
        return fail(root.error());
    }
    SearchVectorsOut out;
    if (const JsonValue* results = root->find("SearchResults");
        results != nullptr && results->is_array()) {
        for (const auto& r : results->elements) {
            SearchVectorsOut::Row sr;
            if (const JsonValue* item = r.find("Item"); item != nullptr && item->is_object()) {
                auto vm = parse_item(*item);
                if (!vm) {
                    return fail(vm.error());
                }
                sr.item = ItemRow{std::move(*vm)};
            }
            sr.score = double_field(r, "Score", 0.0);
            out.results.push_back(std::move(sr));
        }
    }
    // The adapter always sets vector_capacity (it reads both doubles, defaulting
    // to 0 when the member is absent).
    VectorConsumedCapacity vcc;
    if (const JsonValue* cc = root->find("ConsumedCapacity"); cc != nullptr && cc->is_object()) {
        vcc.vector_search_request_bytes = double_field(*cc, "VectorSearchRequestBytes", 0.0);
        vcc.vector_write_request_bytes = double_field(*cc, "VectorWriteRequestBytes", 0.0);
    } else {
        vcc.vector_search_request_bytes = 0.0;
        vcc.vector_write_request_bytes = 0.0;
    }
    out.vector_capacity = vcc;
    return out;
}

Result<TransactGetItemsOut> parse_transact_get_items(const std::string& body) {
    auto root = parse_response_object(body);
    if (!root) {
        return fail(root.error());
    }
    TransactGetItemsOut out;
    if (const JsonValue* resps = root->find("Responses"); resps != nullptr && resps->is_array()) {
        for (const auto& r : resps->elements) {
            const JsonValue* item = r.find("Item");
            if (item == nullptr || !item->is_object() || item->members.empty()) {
                out.responses.emplace_back(std::nullopt);
            } else {
                auto vm = parse_item(*item);
                if (!vm) {
                    return fail(vm.error());
                }
                out.responses.emplace_back(ItemRow{std::move(*vm)});
            }
        }
    }
    return out;
}

Result<ListTablesPage> parse_list_tables(const std::string& body) {
    auto root = parse_response_object(body);
    if (!root) {
        return fail(root.error());
    }
    ListTablesPage out;
    if (const JsonValue* names = root->find("TableNames"); names != nullptr && names->is_array()) {
        for (const auto& n : names->elements) {
            if (n.is_string()) {
                out.table_names.push_back(n.text);
            }
        }
    }
    const std::string cursor = str_field(*root, "LastEvaluatedTableName");
    if (!cursor.empty()) {
        out.last_evaluated_table_name = cursor;
    }
    return out;
}

Result<CreateTableOut> parse_create_table(const std::string& body) {
    return parse_ack<CreateTableOut>(body);
}
Result<UpdateTableOut> parse_update_table(const std::string& body) {
    return parse_ack<UpdateTableOut>(body);
}
Result<DeleteTableOut> parse_delete_table(const std::string& body) {
    return parse_ack<DeleteTableOut>(body);
}
Result<UpdateTimeToLiveOut> parse_update_time_to_live(const std::string& body) {
    return parse_ack<UpdateTimeToLiveOut>(body);
}
Result<UpdateContinuousBackupsOut> parse_update_continuous_backups(const std::string& body) {
    return parse_ack<UpdateContinuousBackupsOut>(body);
}
Result<CreateBackupOut> parse_create_backup(const std::string& body) {
    return parse_ack<CreateBackupOut>(body);
}
Result<DeleteBackupOut> parse_delete_backup(const std::string& body) {
    return parse_ack<DeleteBackupOut>(body);
}
Result<RestoreFromBackupOut> parse_restore_from_backup(const std::string& body) {
    return parse_ack<RestoreFromBackupOut>(body);
}
Result<RestoreToPitrOut> parse_restore_to_pitr(const std::string& body) {
    return parse_ack<RestoreToPitrOut>(body);
}
Result<TransactWriteItemsOut> parse_transact_write_items(const std::string& body) {
    return parse_ack<TransactWriteItemsOut>(body);
}

}  // namespace ddbsh::wire
