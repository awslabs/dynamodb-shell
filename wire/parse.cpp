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
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "ddbsh/wire/codec.hpp"
#include "wire/json_reader.hpp"

namespace ddbsh::wire {
namespace {

using json::JsonValue;

std::unexpected<Error> malformed(const std::string& detail) {
    return fail(Errc::service_error, "malformed response: " + detail);
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
    double value = 0.0;
    const char* first = node.text.data();
    const char* last = first + node.text.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr != last) {
        return malformed(std::string("invalid ") + which + " number");
    }
    return value;
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

// __type document -> ddbsh::Error (reproduces from_sdk_error). Precondition:
// `root` has a "__type" member.
Error error_from_type_document(const JsonValue& root) {
    std::string type_name;
    if (const JsonValue* t = root.find("__type"); t != nullptr && t->is_string()) {
        type_name = t->text;
    }
    // Exception name = the substring after the last '#' (strip the
    // shape/namespace prefix). No '#' => the whole string.
    const std::size_t hash = type_name.rfind('#');
    const std::string name = (hash == std::string::npos) ? type_name : type_name.substr(hash + 1);

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

}  // namespace ddbsh::wire
