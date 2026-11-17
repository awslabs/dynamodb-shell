// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// codec.cpp — AWS JSON 1.0 request serialization (the ddbsh-wire component).
//
// Reproduces, byte-for-byte and SDK-free, what the AWS SDK adapter's
// SerializePayload() emits for each in-scope request (the P6-02 §9.1 contract).
// Two things determine those bytes and both are reproduced here:
//
//   1. FIELD ORDER + PRESENCE. The SDK serializes a request by calling its
//      generated SerializePayload(), which writes members in their Jsonize()
//      declaration order (NOT the order the adapter's build_* set them). Each
//      serialize() below emits members in that Jsonize order and only when the
//      adapter set the field (verified against aws/aws_ddb_client.cpp build_*
//      and the SDK model *Request.cpp at AWS SDK for C++).
//
//   2. FORMAT. SerializePayload returns JsonView::WriteReadable() ==
//      cJSON_Print (readable). That printer uses: `{` then `\n`; each object
//      member indented with (depth) tab characters, key as a quoted string,
//      then `:` and a single `\t`, then the value; `,` after a member that has
//      a successor, then `\n`; the closing `}` indented with (depth-1) tabs; no
//      trailing newline at the top level. Arrays are single-line: `[`, elements
//      separated by `, ` (comma + space), `]` (no internal newlines/indent of
//      their own, though a nested object inside an array brings its own). String
//      escaping matches cJSON: `"`, `\`, `\b`, `\f`, `\n`, `\r`, `\t` by name,
//      other control bytes (< 0x20) as lowercase `\uXXXX`, every other byte
//      (including UTF-8 continuation bytes) copied verbatim.
//
// AttributeValues serialize exactly as the SDK's AttributeValueValue Jsonize:
// S/N/B as a string, BOOL/NULL as a JSON bool, L as an array of AVs, M as an
// object of name -> AV, SS/NS/BS as string arrays. A ddbsh::Value sets exactly
// one type, so each AV object carries exactly one key. Binary (B/BS) values are
// carried as their canonical standard-base64 string (the adapter base64-decodes
// then re-encodes, a no-op for canonical input), matching value.hpp.

#include "ddbsh/wire/codec.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ddbsh/table_schema.hpp"
#include "wire/datetime.hpp"

namespace ddbsh::wire {
namespace {

// ---- A minimal ordered JSON tree reproducing cJSON's readable output -------
// Only the value kinds the request bodies need: object (ordered members),
// array, string, and bool. Numbers never appear as JSON numbers in a DynamoDB
// request body (AttributeValue N is a string; ConsistentRead etc. are bools).

struct Json {
    enum class Kind { Object, Array, String, Bool, Number };
    Kind kind = Kind::Object;
    std::vector<std::pair<std::string, Json>> members;  // Object, in order
    std::vector<Json> elements;                         // Array, in order
    std::string str;                                    // String, or Number token
    bool boolean = false;                               // Bool
};

Json make_string(std::string s) {
    Json j;
    j.kind = Json::Kind::String;
    j.str = std::move(s);
    return j;
}

Json make_bool(bool b) {
    Json j;
    j.kind = Json::Kind::Bool;
    j.boolean = b;
    return j;
}

// A JSON number carrying a VERBATIM token string. The writer emits it unquoted.
// Request bodies carry two number shapes (P6c / D-P6c-2): control-plane
// integers (ReadCapacityUnits, Dimensions, TopK, Limit, …) and the one double,
// RestoreDateTime. The token is produced by make_number_int / make_number_double
// below, which reproduce the SDK's cJSON number formatting exactly.
Json make_number(std::string token) {
    Json j;
    j.kind = Json::Kind::Number;
    j.str = std::move(token);
    return j;
}

// Reproduce cJSON's readable number printing for a double (print_number in the
// SDK's vendored cJSON): "%1.15g", and if that does not round-trip (cJSON's
// compare_double, a relative-epsilon test) fall back to "%1.17g". The decimal
// point is '.' (the SDK builds cJSON with locale support off). An
// integer-valued double (e.g. a whole-second RestoreDateTime) prints without a
// fraction, exactly as cJSON does. cJSON's integer fast path (d ==
// (double)valueint -> "%d") is intentionally omitted: within int range an
// integer-valued double formats identically under "%d" and "%1.15g", and beyond
// INT_MAX cJSON clamps valueint and itself falls to "%1.15g", so the fast path
// never produces a different byte here.
std::string cjson_number_double(double d) {
    if (std::isnan(d) || std::isinf(d)) {
        return "null";
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%1.15g", d);
    double test = 0.0;
    const bool recovered =
        std::sscanf(buf, "%lg", &test) == 1 &&
        std::fabs(test - d) <= (std::fabs(d) > std::fabs(test) ? std::fabs(d) : std::fabs(test)) *
                                   std::numeric_limits<double>::epsilon();
    if (!recovered) {
        std::snprintf(buf, sizeof(buf), "%1.17g", d);
    }
    return std::string(buf);
}

// An integer field (SDK WithInt64 / WithInteger). cJSON stores these as a
// number whose printed form is the plain decimal integer: WithInt64 keeps an
// exact "%lld" literal for values outside [INT_MIN, INT_MAX] and otherwise
// prints the integer-valued double via "%1.15g"; WithInteger routes through a
// double. Both reduce to std::to_string for any integer the adapter sets.
Json make_number_int(long long n) {
    return make_number(std::to_string(n));
}

Json make_number_double(double d) {
    return make_number(cjson_number_double(d));
}

Json make_object() {
    Json j;
    j.kind = Json::Kind::Object;
    return j;
}

Json make_array() {
    Json j;
    j.kind = Json::Kind::Array;
    return j;
}

// Sort an object's members by key, reproducing how the SDK adapter serializes
// a DynamoDB item/name map: to_sdk_item / to_sdk_names build an Aws::Map
// (std::map), so the SDK emits map keys in ASCENDING key order — NOT the
// insertion order of the engine's ValueMap. std::string's comparison is
// unsigned-byte lexicographic (char_traits<char>), matching std::map<Aws::String>.
// Applied to every DATA map (Item/Key/ExpressionAttributeValues/
// ExclusiveStartKey/ExpressionAttributeNames and nested M values); the
// REQUEST-level fields are left in their Jsonize order (cJSON preserves the
// SDK's WithX insertion order for those).
void sort_members(Json& obj) {
    std::sort(obj.members.begin(), obj.members.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
}

// ---- cJSON-faithful readable writer ----------------------------------------

void write_string(const std::string& s, std::string& out) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
                break;
        }
    }
    out += '"';
}

// `depth` is the nesting depth at which `node` starts, matching cJSON's
// printbuffer depth when print_value is entered (a container increments it
// before printing its children).
void write_value(const Json& node, int depth, std::string& out) {
    switch (node.kind) {
        case Json::Kind::String:
            write_string(node.str, out);
            break;
        case Json::Kind::Bool:
            out += node.boolean ? "true" : "false";
            break;
        case Json::Kind::Number:
            out += node.str;
            break;
        case Json::Kind::Object: {
            const int inner = depth + 1;
            out += '{';
            out += '\n';
            for (std::size_t i = 0; i < node.members.size(); ++i) {
                out.append(static_cast<std::size_t>(inner), '\t');
                write_string(node.members[i].first, out);
                out += ':';
                out += '\t';
                write_value(node.members[i].second, inner, out);
                if (i + 1 < node.members.size()) {
                    out += ',';
                }
                out += '\n';
            }
            out.append(static_cast<std::size_t>(inner - 1), '\t');
            out += '}';
            break;
        }
        case Json::Kind::Array: {
            const int inner = depth + 1;
            out += '[';
            for (std::size_t i = 0; i < node.elements.size(); ++i) {
                write_value(node.elements[i], inner, out);
                if (i + 1 < node.elements.size()) {
                    out += ',';
                    out += ' ';
                }
            }
            out += ']';
            break;
        }
    }
}

std::string render(const Json& root) {
    std::string out;
    write_value(root, 0, out);
    return out;
}

// ---- ddbsh::Value -> AttributeValue JSON (the inverse of the parser side) --

Json av(const Value& v) {
    Json o = make_object();
    switch (v.type) {
        case Value::Type::S:
            o.members.emplace_back("S", make_string(v.s));
            break;
        case Value::Type::N:
            o.members.emplace_back("N", make_string(v.s));
            break;
        case Value::Type::B:
            // Emits v.s verbatim. This matches the SDK adapter (which base64-
            // decodes then re-encodes) ONLY for CANONICAL standard-base64 input.
            // Load-bearing assumption: every B value in the tree is already
            // canonical (it originates from the parser or a response re-encode),
            // so the adapter's round-trip is a no-op and bytes agree. A future
            // source of non-canonical base64 (e.g. the P6b parse side, or raw
            // user-supplied B/BS) must canonicalize before this point, or the
            // differential gate's byte-parity contract breaks silently (the
            // current corpus uses canonical input and cannot catch it).
            o.members.emplace_back("B", make_string(v.s));
            break;
        case Value::Type::BOOL:
            o.members.emplace_back("BOOL", make_bool(v.boolean));
            break;
        case Value::Type::NUL:
            o.members.emplace_back("NULL", make_bool(true));
            break;
        case Value::Type::L: {
            Json a = make_array();
            a.elements.reserve(v.list.size());
            for (const auto& e : v.list) {
                a.elements.push_back(av(e));
            }
            o.members.emplace_back("L", std::move(a));
            break;
        }
        case Value::Type::M: {
            Json m = make_object();
            m.members.reserve(v.map.size());
            for (const auto& [k, val] : v.map) {
                m.members.emplace_back(k, av(val));
            }
            sort_members(m);  // SDK M is an Aws::Map (sorted by key)
            o.members.emplace_back("M", std::move(m));
            break;
        }
        case Value::Type::SS: {
            Json a = make_array();
            a.elements.reserve(v.set.size());
            for (const auto& s : v.set) {
                a.elements.push_back(make_string(s));
            }
            o.members.emplace_back("SS", std::move(a));
            break;
        }
        case Value::Type::NS: {
            Json a = make_array();
            a.elements.reserve(v.set.size());
            for (const auto& s : v.set) {
                a.elements.push_back(make_string(s));
            }
            o.members.emplace_back("NS", std::move(a));
            break;
        }
        case Value::Type::BS: {
            Json a = make_array();
            a.elements.reserve(v.set.size());
            for (const auto& s : v.set) {
                // Each element emitted verbatim; same canonical-base64
                // assumption as the B arm above (adapter round-trips, no-op for
                // canonical input).
                a.elements.push_back(make_string(s));
            }
            o.members.emplace_back("BS", std::move(a));
            break;
        }
    }
    return o;
}

// A ValueMap (Item / Key / ExpressionAttributeValues) -> a JSON object of
// name -> AttributeValue. The SDK adapter's to_sdk_item builds an Aws::Map
// (std::map), so the serialized keys are in ASCENDING key order regardless of
// the engine ValueMap's source order; sort_members reproduces that.
Json value_map_object(const ValueMap& m) {
    Json o = make_object();
    o.members.reserve(m.size());
    for (const auto& [name, value] : m) {
        o.members.emplace_back(name, av(value));
    }
    sort_members(o);  // SDK Item/Key/EAV/ExclusiveStartKey are Aws::Maps
    return o;
}

// ExpressionAttributeNames (token -> real name), in token order.
Json names_object(const ExpressionAttributeNames& names) {
    Json o = make_object();
    o.members.reserve(names.size());
    for (const auto& [token, name] : names) {
        o.members.emplace_back(token, make_string(name));
    }
    sort_members(o);  // SDK ExpressionAttributeNames is an Aws::Map (sorted)
    return o;
}

// Enum -> the SDK mapper's wire name (ReturnConsumedCapacityMapper /
// ReturnValueMapper), so the serialized token matches SerializePayload exactly.
const char* rcc_name(ReturnConsumedCapacity rcc) {
    switch (rcc) {
        case ReturnConsumedCapacity::total:
            return "TOTAL";
        case ReturnConsumedCapacity::indexes:
            return "INDEXES";
        case ReturnConsumedCapacity::none:
            break;
    }
    return "NONE";
}

const char* rv_name(ReturnValue rv) {
    switch (rv) {
        case ReturnValue::all_old:
            return "ALL_OLD";
        case ReturnValue::all_new:
            return "ALL_NEW";
        case ReturnValue::updated_old:
            return "UPDATED_OLD";
        case ReturnValue::updated_new:
            return "UPDATED_NEW";
        case ReturnValue::none:
            break;
    }
    return "NONE";
}

// ---- Control-plane enum -> SDK mapper name helpers (P6c) --------------------
// Each returns exactly the string the matching SDK *Mapper::GetNameFor*()
// returns, so the serialized token matches SerializePayload byte-for-byte.

const char* scalar_type_name(ScalarType t) {
    switch (t) {
        case ScalarType::n:
            return "N";
        case ScalarType::b:
            return "B";
        case ScalarType::s:
            break;
    }
    return "S";
}

const char* projection_type_name(ProjectionType t) {
    switch (t) {
        case ProjectionType::keys_only:
            return "KEYS_ONLY";
        case ProjectionType::include:
            return "INCLUDE";
        case ProjectionType::all:
            break;
    }
    return "ALL";
}

const char* table_class_name(TableClass tc) {
    return tc == TableClass::standard_infrequent_access ? "STANDARD_INFREQUENT_ACCESS" : "STANDARD";
}

const char* stream_view_type_name(StreamViewType v) {
    switch (v) {
        case StreamViewType::new_image:
            return "NEW_IMAGE";
        case StreamViewType::old_image:
            return "OLD_IMAGE";
        case StreamViewType::new_and_old_images:
            return "NEW_AND_OLD_IMAGES";
        case StreamViewType::keys_only:
            break;
    }
    return "KEYS_ONLY";
}

const char* sse_type_name(SseType t) {
    return t == SseType::aes256 ? "AES256" : "KMS";
}

const char* vector_distance_name(VectorDistance d) {
    switch (d) {
        case VectorDistance::euclidean:
            return "EUCLIDEAN";
        case VectorDistance::dot_product:
            return "DOT_PRODUCT";
        case VectorDistance::cosine:
            break;
    }
    return "COSINE";
}

const char* search_schema_kind_name(SearchSchemaKind k) {
    return k == SearchSchemaKind::hash ? "HASH" : "INLINE_FILTER";
}

// ---- Shared nested-object builders (SDK model Jsonize order) ---------------
// Each reproduces one SDK model type's Jsonize(), emitting members in the SDK's
// declaration order and only when the adapter (aws/aws_ddb_client.cpp) sets the
// field. Verified against the SDK model *.cpp Jsonize at AWS SDK for C++.

// KeySchema -> [ {AttributeName, KeyType}, ... ] in [HASH..., RANGE...] order.
// The adapter's to_sdk_key_schema emits HASH elements then RANGE elements.
Json key_schema_array(const KeySchema& ks) {
    Json a = make_array();
    a.elements.reserve(ks.hash.size() + ks.range.size());
    for (const auto& name : ks.hash) {
        Json e = make_object();
        e.members.emplace_back("AttributeName", make_string(name));
        e.members.emplace_back("KeyType", make_string("HASH"));
        a.elements.push_back(std::move(e));
    }
    for (const auto& name : ks.range) {
        Json e = make_object();
        e.members.emplace_back("AttributeName", make_string(name));
        e.members.emplace_back("KeyType", make_string("RANGE"));
        a.elements.push_back(std::move(e));
    }
    return a;
}

// Projection Jsonize: ProjectionType, then NonKeyAttributes (only for INCLUDE
// with a non-empty list — the adapter AddNonKeyAttributes only in that case).
Json projection_object(const Projection& p) {
    Json o = make_object();
    o.members.emplace_back("ProjectionType", make_string(projection_type_name(p.type)));
    if (p.type == ProjectionType::include && !p.non_key_attributes.empty()) {
        Json a = make_array();
        a.elements.reserve(p.non_key_attributes.size());
        for (const auto& attr : p.non_key_attributes) {
            a.elements.push_back(make_string(attr));
        }
        o.members.emplace_back("NonKeyAttributes", std::move(a));
    }
    return o;
}

// ProvisionedThroughput Jsonize: ReadCapacityUnits, WriteCapacityUnits (both
// Int64, both always set by to_sdk_provisioned).
Json provisioned_object(const BillingModeAndThroughput& b) {
    Json o = make_object();
    o.members.emplace_back("ReadCapacityUnits", make_number_int(b.rcu));
    o.members.emplace_back("WriteCapacityUnits", make_number_int(b.wcu));
    return o;
}

// OnDemandThroughput Jsonize: MaxReadRequestUnits, MaxWriteRequestUnits (Int64,
// each only when the optional has a value — to_sdk_ondemand sets each
// independently; a bare ON DEMAND yields an empty object).
Json on_demand_object(const OnDemandThroughput& od) {
    Json o = make_object();
    if (od.max_read_request_units.has_value()) {
        o.members.emplace_back("MaxReadRequestUnits", make_number_int(*od.max_read_request_units));
    }
    if (od.max_write_request_units.has_value()) {
        o.members.emplace_back("MaxWriteRequestUnits",
                               make_number_int(*od.max_write_request_units));
    }
    return o;
}

// WarmThroughput Jsonize: ReadUnitsPerSecond, WriteUnitsPerSecond (Int64, both
// always set by to_sdk_warm).
Json warm_object(const WarmThroughput& w) {
    Json o = make_object();
    o.members.emplace_back("ReadUnitsPerSecond", make_number_int(w.read_units_per_second));
    o.members.emplace_back("WriteUnitsPerSecond", make_number_int(w.write_units_per_second));
    return o;
}

// StreamSpecification Jsonize: StreamEnabled, then StreamViewType (only when
// enabled — to_sdk_stream sets the view type only in that case).
Json stream_object(const StreamSpec& s) {
    Json o = make_object();
    o.members.emplace_back("StreamEnabled", make_bool(s.enabled));
    if (s.enabled) {
        o.members.emplace_back("StreamViewType", make_string(stream_view_type_name(s.view_type)));
    }
    return o;
}

// SSESpecification Jsonize: Enabled, SSEType, KMSMasterKeyId (type/key only when
// enabled — to_sdk_sse; sse_type defaults to KMS when unset but enabled, but
// the grammar always supplies one on enable).
Json sse_object(const SseSpec& s) {
    Json o = make_object();
    o.members.emplace_back("Enabled", make_bool(s.enabled));
    if (s.enabled) {
        o.members.emplace_back("SSEType",
                               make_string(sse_type_name(s.sse_type.value_or(SseType::kms))));
        if (s.kms_master_key_id.has_value()) {
            o.members.emplace_back("KMSMasterKeyId", make_string(*s.kms_master_key_id));
        }
    }
    return o;
}

// AttributeDefinitions -> [ {AttributeName, AttributeType}, ... ].
Json attribute_definitions_array(const std::vector<AttributeDef>& defs) {
    Json a = make_array();
    a.elements.reserve(defs.size());
    for (const auto& d : defs) {
        Json e = make_object();
        e.members.emplace_back("AttributeName", make_string(d.name));
        e.members.emplace_back("AttributeType", make_string(scalar_type_name(d.type)));
        a.elements.push_back(std::move(e));
    }
    return a;
}

// VectorAttributeDefinition Jsonize: AttributeName only.
Json vector_attribute_object(const std::string& attr_name) {
    Json o = make_object();
    o.members.emplace_back("AttributeName", make_string(attr_name));
    return o;
}

// SearchSchema -> [ {AttributeName, SearchSchemaElementType}, ... ].
Json search_schema_array(const std::vector<SearchSchemaElem>& schema) {
    Json a = make_array();
    a.elements.reserve(schema.size());
    for (const auto& e : schema) {
        Json o = make_object();
        o.members.emplace_back("AttributeName", make_string(e.attr));
        o.members.emplace_back("SearchSchemaElementType",
                               make_string(search_schema_kind_name(e.kind)));
        a.elements.push_back(std::move(o));
    }
    return a;
}

// VectorIndex / CreateVectorIndexAction Jsonize (same shape): IndexName,
// VectorAttribute, SearchSchema (only when non-empty), Projection, Dimensions,
// DistanceFunction. fill_vector_index sets IndexName, VectorAttribute,
// Dimensions, DistanceFunction, SearchSchema (if non-empty), Projection.
Json vector_index_object(const VectorIndexDef& v) {
    Json o = make_object();
    o.members.emplace_back("IndexName", make_string(v.index_name));
    o.members.emplace_back("VectorAttribute", vector_attribute_object(v.vector_attr));
    if (!v.search_schema.empty()) {
        o.members.emplace_back("SearchSchema", search_schema_array(v.search_schema));
    }
    o.members.emplace_back("Projection", projection_object(v.projection));
    o.members.emplace_back("Dimensions", make_number_int(v.dimensions));
    o.members.emplace_back("DistanceFunction", make_string(vector_distance_name(v.distance)));
    return o;
}

// GlobalSecondaryIndex Jsonize: IndexName, KeySchema, Projection,
// ProvisionedThroughput, OnDemandThroughput, WarmThroughput. The adapter sets
// name + key schema + projection always, throughput per the GSI's own billing
// (apply_gsi_throughput), warm when present.
Json gsi_object(const Gsi& gsi) {
    Json o = make_object();
    o.members.emplace_back("IndexName", make_string(gsi.name));
    o.members.emplace_back("KeySchema", key_schema_array(gsi.key_schema));
    o.members.emplace_back("Projection", projection_object(gsi.projection));
    if (gsi.billing.has_value()) {
        if (gsi.billing->mode == BillingMode::provisioned) {
            o.members.emplace_back("ProvisionedThroughput", provisioned_object(*gsi.billing));
        } else {
            o.members.emplace_back("OnDemandThroughput", on_demand_object(gsi.billing->od));
        }
    }
    if (gsi.warm.has_value()) {
        o.members.emplace_back("WarmThroughput", warm_object(*gsi.warm));
    }
    return o;
}

// LocalSecondaryIndex Jsonize: IndexName, KeySchema, Projection.
Json lsi_object(const Lsi& lsi) {
    Json o = make_object();
    o.members.emplace_back("IndexName", make_string(lsi.name));
    o.members.emplace_back("KeySchema", key_schema_array(lsi.key_schema));
    o.members.emplace_back("Projection", projection_object(lsi.projection));
    return o;
}

// Tag Jsonize: Key, Value.
Json tag_object(const Tag& t) {
    Json o = make_object();
    o.members.emplace_back("Key", make_string(t.key));
    o.members.emplace_back("Value", make_string(t.value));
    return o;
}

// GlobalSecondaryIndexUpdate: one of Create / Update / Delete (exactly one set
// by to_sdk_gsi_update). The inner action Jsonize orders:
//   CreateGlobalSecondaryIndexAction: IndexName, KeySchema, Projection,
//     ProvisionedThroughput|OnDemandThroughput, WarmThroughput.
//   UpdateGlobalSecondaryIndexAction: IndexName, ProvisionedThroughput|
//     OnDemandThroughput, WarmThroughput.
//   DeleteGlobalSecondaryIndexAction: IndexName.
Json gsi_update_object(const GsiUpdate& gu) {
    Json wrapper = make_object();
    switch (gu.action) {
        case GsiUpdate::Action::create: {
            Json a = make_object();
            a.members.emplace_back("IndexName", make_string(gu.index_name));
            if (gu.key_schema.has_value()) {
                a.members.emplace_back("KeySchema", key_schema_array(*gu.key_schema));
            }
            if (gu.projection.has_value()) {
                a.members.emplace_back("Projection", projection_object(*gu.projection));
            }
            if (gu.billing.has_value()) {
                if (gu.billing->mode == BillingMode::provisioned) {
                    a.members.emplace_back("ProvisionedThroughput",
                                           provisioned_object(*gu.billing));
                } else {
                    a.members.emplace_back("OnDemandThroughput", on_demand_object(gu.billing->od));
                }
            }
            if (gu.warm.has_value()) {
                a.members.emplace_back("WarmThroughput", warm_object(*gu.warm));
            }
            wrapper.members.emplace_back("Create", std::move(a));
            break;
        }
        case GsiUpdate::Action::update: {
            Json a = make_object();
            a.members.emplace_back("IndexName", make_string(gu.index_name));
            if (gu.billing.has_value()) {
                if (gu.billing->mode == BillingMode::provisioned) {
                    a.members.emplace_back("ProvisionedThroughput",
                                           provisioned_object(*gu.billing));
                } else {
                    a.members.emplace_back("OnDemandThroughput", on_demand_object(gu.billing->od));
                }
            }
            if (gu.warm.has_value()) {
                a.members.emplace_back("WarmThroughput", warm_object(*gu.warm));
            }
            wrapper.members.emplace_back("Update", std::move(a));
            break;
        }
        case GsiUpdate::Action::drop: {
            Json a = make_object();
            a.members.emplace_back("IndexName", make_string(gu.index_name));
            wrapper.members.emplace_back("Delete", std::move(a));
            break;
        }
    }
    return wrapper;
}

// VectorIndexUpdate: Create (a CreateVectorIndexAction, same shape as
// VectorIndex) or Delete (DeleteVectorIndexAction: IndexName only).
Json vector_index_update_object(const VectorIndexUpdateDef& u) {
    Json wrapper = make_object();
    if (u.create.has_value()) {
        wrapper.members.emplace_back("Create", vector_index_object(*u.create));
    } else if (u.drop.has_value()) {
        Json a = make_object();
        a.members.emplace_back("IndexName", make_string(*u.drop));
        wrapper.members.emplace_back("Delete", std::move(a));
    }
    return wrapper;
}

// ProvisionedThroughputOverride Jsonize: ReadCapacityUnits only (Int64).
Json throughput_override_object(const ProvisionedThroughputOverride& o) {
    Json out = make_object();
    out.members.emplace_back("ReadCapacityUnits", make_number_int(o.read_capacity_units));
    return out;
}

// ReplicaGlobalSecondaryIndex Jsonize: IndexName, ProvisionedThroughputOverride
// (the adapter sets both).
Json replica_gsi_array(const std::vector<ReplicaGsi>& gsis) {
    Json a = make_array();
    a.elements.reserve(gsis.size());
    for (const auto& g : gsis) {
        Json o = make_object();
        o.members.emplace_back("IndexName", make_string(g.index_name));
        o.members.emplace_back("ProvisionedThroughputOverride",
                               throughput_override_object(g.throughput_override));
        a.elements.push_back(std::move(o));
    }
    return a;
}

// ReplicationGroupUpdate: one of Create / Update / Delete (exactly one set).
//   Create/UpdateReplicationGroupMemberAction Jsonize: RegionName,
//     KMSMasterKeyId, ProvisionedThroughputOverride, OnDemandThroughputOverride,
//     GlobalSecondaryIndexes, TableClassOverride. The adapter sets RegionName,
//     TableClassOverride (always, value_or STANDARD), ProvisionedThroughput-
//     Override (if present), GlobalSecondaryIndexes (if non-empty).
//   DeleteReplicationGroupMemberAction Jsonize: RegionName.
Json replica_update_object(const ReplicaUpdate& r) {
    Json wrapper = make_object();
    auto build_add_or_update = [&](const ReplicaUpdate& ru) {
        Json a = make_object();
        a.members.emplace_back("RegionName", make_string(ru.region));
        if (ru.throughput_override.has_value()) {
            a.members.emplace_back("ProvisionedThroughputOverride",
                                   throughput_override_object(*ru.throughput_override));
        }
        if (!ru.replica_gsis.empty()) {
            a.members.emplace_back("GlobalSecondaryIndexes", replica_gsi_array(ru.replica_gsis));
        }
        a.members.emplace_back(
            "TableClassOverride",
            make_string(table_class_name(ru.table_class.value_or(TableClass::standard))));
        return a;
    };
    switch (r.action) {
        case ReplicaUpdate::Action::add:
            wrapper.members.emplace_back("Create", build_add_or_update(r));
            break;
        case ReplicaUpdate::Action::update:
            wrapper.members.emplace_back("Update", build_add_or_update(r));
            break;
        case ReplicaUpdate::Action::drop: {
            Json a = make_object();
            a.members.emplace_back("RegionName", make_string(r.region));
            wrapper.members.emplace_back("Delete", std::move(a));
            break;
        }
    }
    return wrapper;
}

// A list of AttributeValues (SearchVectors SearchVector) -> JSON array of AVs.
Json value_list_array(const ValueList& values) {
    Json a = make_array();
    a.elements.reserve(values.size());
    for (const auto& v : values) {
        a.elements.push_back(av(v));
    }
    return a;
}

}  // namespace

// ---- GetItem ---------------------------------------------------------------
// Jsonize order (GetItemRequest.cpp): TableName, Key, AttributesToGet,
// ConsistentRead, ReturnConsumedCapacity, ProjectionExpression,
// ExpressionAttributeNames. The adapter always sets TableName, Key,
// ConsistentRead, and ReturnConsumedCapacity; ProjectionExpression and
// ExpressionAttributeNames only when present/non-empty.
WireRequest serialize(const GetItemIn& in) {
    Json root = make_object();
    root.members.emplace_back("TableName", make_string(in.table_name));
    root.members.emplace_back("Key", value_map_object(in.key));
    root.members.emplace_back("ConsistentRead", make_bool(in.consistent_read));
    root.members.emplace_back("ReturnConsumedCapacity",
                              make_string(rcc_name(in.return_consumed_capacity)));
    if (in.projection_expression.has_value()) {
        root.members.emplace_back("ProjectionExpression", make_string(*in.projection_expression));
    }
    if (!in.expression_attribute_names.empty()) {
        root.members.emplace_back("ExpressionAttributeNames",
                                  names_object(in.expression_attribute_names));
    }
    return WireRequest{"GetItem", render(root)};
}

// ---- Query -----------------------------------------------------------------
// Jsonize order (QueryRequest.cpp), restricted to the fields the adapter sets:
// TableName, IndexName, ConsistentRead, ExclusiveStartKey,
// ReturnConsumedCapacity, ProjectionExpression, FilterExpression,
// KeyConditionExpression, ExpressionAttributeNames, ExpressionAttributeValues.
WireRequest serialize(const QueryIn& in) {
    Json root = make_object();
    root.members.emplace_back("TableName", make_string(in.table_name));
    if (in.index_name.has_value()) {
        root.members.emplace_back("IndexName", make_string(*in.index_name));
    }
    root.members.emplace_back("ConsistentRead", make_bool(in.consistent_read));
    if (in.exclusive_start_key.has_value()) {
        root.members.emplace_back("ExclusiveStartKey", value_map_object(*in.exclusive_start_key));
    }
    root.members.emplace_back("ReturnConsumedCapacity",
                              make_string(rcc_name(in.return_consumed_capacity)));
    if (in.projection_expression.has_value()) {
        root.members.emplace_back("ProjectionExpression", make_string(*in.projection_expression));
    }
    if (in.filter_expression.has_value()) {
        root.members.emplace_back("FilterExpression", make_string(*in.filter_expression));
    }
    root.members.emplace_back("KeyConditionExpression", make_string(in.key_condition_expression));
    if (!in.expression_attribute_names.empty()) {
        root.members.emplace_back("ExpressionAttributeNames",
                                  names_object(in.expression_attribute_names));
    }
    if (!in.expression_attribute_values.empty()) {
        root.members.emplace_back("ExpressionAttributeValues",
                                  value_map_object(in.expression_attribute_values));
    }
    return WireRequest{"Query", render(root)};
}

// ---- Scan ------------------------------------------------------------------
// Jsonize order (ScanRequest.cpp), restricted to the fields the adapter sets:
// TableName, IndexName, ExclusiveStartKey, ReturnConsumedCapacity,
// ProjectionExpression, FilterExpression, ExpressionAttributeNames,
// ExpressionAttributeValues, ConsistentRead. (ConsistentRead serializes LAST
// for Scan, unlike Query — a real SDK model asymmetry.)
WireRequest serialize(const ScanIn& in) {
    Json root = make_object();
    root.members.emplace_back("TableName", make_string(in.table_name));
    if (in.index_name.has_value()) {
        root.members.emplace_back("IndexName", make_string(*in.index_name));
    }
    if (in.exclusive_start_key.has_value()) {
        root.members.emplace_back("ExclusiveStartKey", value_map_object(*in.exclusive_start_key));
    }
    root.members.emplace_back("ReturnConsumedCapacity",
                              make_string(rcc_name(in.return_consumed_capacity)));
    if (in.projection_expression.has_value()) {
        root.members.emplace_back("ProjectionExpression", make_string(*in.projection_expression));
    }
    if (in.filter_expression.has_value()) {
        root.members.emplace_back("FilterExpression", make_string(*in.filter_expression));
    }
    if (!in.expression_attribute_names.empty()) {
        root.members.emplace_back("ExpressionAttributeNames",
                                  names_object(in.expression_attribute_names));
    }
    if (!in.expression_attribute_values.empty()) {
        root.members.emplace_back("ExpressionAttributeValues",
                                  value_map_object(in.expression_attribute_values));
    }
    root.members.emplace_back("ConsistentRead", make_bool(in.consistent_read));
    return WireRequest{"Scan", render(root)};
}

// ---- PutItem ---------------------------------------------------------------
// Jsonize order (PutItemRequest.cpp), restricted to the fields the adapter
// sets: TableName, Item, ReturnValues, ReturnConsumedCapacity,
// ConditionExpression, ExpressionAttributeNames, ExpressionAttributeValues.
// Unlike the reads, PutItem sets ReturnValues/ReturnConsumedCapacity ONLY when
// they are not the default (none) — INSERT forces none (grammar), REPLACE may
// request ALL_OLD and a ratelimit forces TOTAL.
WireRequest serialize(const PutItemIn& in) {
    Json root = make_object();
    root.members.emplace_back("TableName", make_string(in.table_name));
    root.members.emplace_back("Item", value_map_object(in.item));
    if (in.return_values != ReturnValue::none) {
        root.members.emplace_back("ReturnValues", make_string(rv_name(in.return_values)));
    }
    if (in.return_consumed_capacity != ReturnConsumedCapacity::none) {
        root.members.emplace_back("ReturnConsumedCapacity",
                                  make_string(rcc_name(in.return_consumed_capacity)));
    }
    if (in.condition_expression.has_value()) {
        root.members.emplace_back("ConditionExpression", make_string(*in.condition_expression));
    }
    if (!in.expression_attribute_names.empty()) {
        root.members.emplace_back("ExpressionAttributeNames",
                                  names_object(in.expression_attribute_names));
    }
    if (!in.expression_attribute_values.empty()) {
        root.members.emplace_back("ExpressionAttributeValues",
                                  value_map_object(in.expression_attribute_values));
    }
    return WireRequest{"PutItem", render(root)};
}

// ---- UpdateItem ------------------------------------------------------------
// Jsonize order (UpdateItemRequest.cpp), restricted to the fields the adapter
// sets: TableName, Key, ReturnValues, ReturnConsumedCapacity, UpdateExpression,
// ConditionExpression, ExpressionAttributeNames, ExpressionAttributeValues.
WireRequest serialize(const UpdateItemIn& in) {
    Json root = make_object();
    root.members.emplace_back("TableName", make_string(in.table_name));
    root.members.emplace_back("Key", value_map_object(in.key));
    if (in.return_values != ReturnValue::none) {
        root.members.emplace_back("ReturnValues", make_string(rv_name(in.return_values)));
    }
    if (in.return_consumed_capacity != ReturnConsumedCapacity::none) {
        root.members.emplace_back("ReturnConsumedCapacity",
                                  make_string(rcc_name(in.return_consumed_capacity)));
    }
    root.members.emplace_back("UpdateExpression", make_string(in.update_expression));
    if (in.condition_expression.has_value()) {
        root.members.emplace_back("ConditionExpression", make_string(*in.condition_expression));
    }
    if (!in.expression_attribute_names.empty()) {
        root.members.emplace_back("ExpressionAttributeNames",
                                  names_object(in.expression_attribute_names));
    }
    if (!in.expression_attribute_values.empty()) {
        root.members.emplace_back("ExpressionAttributeValues",
                                  value_map_object(in.expression_attribute_values));
    }
    return WireRequest{"UpdateItem", render(root)};
}

// ---- DeleteItem ------------------------------------------------------------
// Jsonize order (DeleteItemRequest.cpp), restricted to the fields the adapter
// sets: TableName, Key, ReturnValues, ReturnConsumedCapacity,
// ConditionExpression, ExpressionAttributeNames, ExpressionAttributeValues.
WireRequest serialize(const DeleteItemIn& in) {
    Json root = make_object();
    root.members.emplace_back("TableName", make_string(in.table_name));
    root.members.emplace_back("Key", value_map_object(in.key));
    if (in.return_values != ReturnValue::none) {
        root.members.emplace_back("ReturnValues", make_string(rv_name(in.return_values)));
    }
    if (in.return_consumed_capacity != ReturnConsumedCapacity::none) {
        root.members.emplace_back("ReturnConsumedCapacity",
                                  make_string(rcc_name(in.return_consumed_capacity)));
    }
    if (in.condition_expression.has_value()) {
        root.members.emplace_back("ConditionExpression", make_string(*in.condition_expression));
    }
    if (!in.expression_attribute_names.empty()) {
        root.members.emplace_back("ExpressionAttributeNames",
                                  names_object(in.expression_attribute_names));
    }
    if (!in.expression_attribute_values.empty()) {
        root.members.emplace_back("ExpressionAttributeValues",
                                  value_map_object(in.expression_attribute_values));
    }
    return WireRequest{"DeleteItem", render(root)};
}

// ===========================================================================
// Control-plane, backup/restore, transaction, and vector serializers (P6c).
// Each emits members in the SDK model's Jsonize declaration order (NOT the
// adapter's build_* set order) and only when the adapter set the field; nested
// objects use the shared builders above. Verified against the SDK model
// *Request.cpp SerializePayload + the adapter build_* (the fidelity oracle).
// ===========================================================================

// ---- CreateTable -----------------------------------------------------------
// Jsonize order (CreateTableRequest.cpp): AttributeDefinitions, TableName,
// KeySchema, LocalSecondaryIndexes, GlobalSecondaryIndexes, BillingMode,
// ProvisionedThroughput, StreamSpecification, SSESpecification, Tags,
// TableClass, DeletionProtectionEnabled, WarmThroughput, OnDemandThroughput,
// VectorIndexes. The adapter (build_create_table + apply_billing) always sets
// KeySchema, BillingMode, TableClass, DeletionProtectionEnabled; the rest per
// clause. A nullopt billing defaults to PAY_PER_REQUEST with NO throughput.
WireRequest serialize(const CreateTableIn& in) {
    Json root = make_object();
    if (!in.attribute_definitions.empty()) {
        root.members.emplace_back("AttributeDefinitions",
                                  attribute_definitions_array(in.attribute_definitions));
    }
    root.members.emplace_back("TableName", make_string(in.table_name));
    root.members.emplace_back("KeySchema", key_schema_array(in.key_schema));
    if (!in.lsis.empty()) {
        Json a = make_array();
        a.elements.reserve(in.lsis.size());
        for (const auto& lsi : in.lsis) {
            a.elements.push_back(lsi_object(lsi));
        }
        root.members.emplace_back("LocalSecondaryIndexes", std::move(a));
    }
    if (!in.gsis.empty()) {
        Json a = make_array();
        a.elements.reserve(in.gsis.size());
        for (const auto& gsi : in.gsis) {
            a.elements.push_back(gsi_object(gsi));
        }
        root.members.emplace_back("GlobalSecondaryIndexes", std::move(a));
    }
    // BillingMode is ALWAYS set (apply_billing): a nullopt billing is
    // PAY_PER_REQUEST with no throughput; a present billing sets the matching
    // throughput object (provisioned here, on-demand in its later slot).
    if (!in.billing.has_value()) {
        root.members.emplace_back("BillingMode", make_string("PAY_PER_REQUEST"));
    } else if (in.billing->mode == BillingMode::provisioned) {
        root.members.emplace_back("BillingMode", make_string("PROVISIONED"));
        root.members.emplace_back("ProvisionedThroughput", provisioned_object(*in.billing));
    } else {
        root.members.emplace_back("BillingMode", make_string("PAY_PER_REQUEST"));
    }
    if (in.stream.has_value()) {
        root.members.emplace_back("StreamSpecification", stream_object(*in.stream));
    }
    if (in.sse.has_value()) {
        root.members.emplace_back("SSESpecification", sse_object(*in.sse));
    }
    if (!in.tags.empty()) {
        Json a = make_array();
        a.elements.reserve(in.tags.size());
        for (const auto& t : in.tags) {
            a.elements.push_back(tag_object(t));
        }
        root.members.emplace_back("Tags", std::move(a));
    }
    root.members.emplace_back("TableClass", make_string(table_class_name(in.table_class)));
    root.members.emplace_back("DeletionProtectionEnabled", make_bool(in.deletion_protection));
    if (in.warm.has_value()) {
        root.members.emplace_back("WarmThroughput", warm_object(*in.warm));
    }
    if (in.billing.has_value() && in.billing->mode == BillingMode::pay_per_request) {
        root.members.emplace_back("OnDemandThroughput", on_demand_object(in.billing->od));
    }
    if (!in.vector_indexes.empty()) {
        Json a = make_array();
        a.elements.reserve(in.vector_indexes.size());
        for (const auto& vi : in.vector_indexes) {
            a.elements.push_back(vector_index_object(vi));
        }
        root.members.emplace_back("VectorIndexes", std::move(a));
    }
    return WireRequest{"CreateTable", render(root)};
}

// ---- UpdateTable -----------------------------------------------------------
// Jsonize order (UpdateTableRequest.cpp), restricted to the fields the adapter
// sets: AttributeDefinitions, TableName, BillingMode, ProvisionedThroughput,
// GlobalSecondaryIndexUpdates, StreamSpecification, SSESpecification,
// ReplicaUpdates, TableClass, DeletionProtectionEnabled, OnDemandThroughput,
// WarmThroughput, VectorIndexUpdates. Every field is per-clause (build_update_
// table sets only what the ALTER provided); a present billing sets BOTH the
// mode and the matching throughput.
WireRequest serialize(const UpdateTableIn& in) {
    Json root = make_object();
    if (!in.attribute_definitions.empty()) {
        root.members.emplace_back("AttributeDefinitions",
                                  attribute_definitions_array(in.attribute_definitions));
    }
    root.members.emplace_back("TableName", make_string(in.table_name));
    if (in.billing.has_value()) {
        if (in.billing->mode == BillingMode::provisioned) {
            root.members.emplace_back("BillingMode", make_string("PROVISIONED"));
            root.members.emplace_back("ProvisionedThroughput", provisioned_object(*in.billing));
        } else {
            root.members.emplace_back("BillingMode", make_string("PAY_PER_REQUEST"));
        }
    }
    if (!in.gsi_updates.empty()) {
        Json a = make_array();
        a.elements.reserve(in.gsi_updates.size());
        for (const auto& gu : in.gsi_updates) {
            a.elements.push_back(gsi_update_object(gu));
        }
        root.members.emplace_back("GlobalSecondaryIndexUpdates", std::move(a));
    }
    if (in.stream.has_value()) {
        root.members.emplace_back("StreamSpecification", stream_object(*in.stream));
    }
    if (in.sse.has_value()) {
        root.members.emplace_back("SSESpecification", sse_object(*in.sse));
    }
    if (in.replica.has_value()) {
        Json a = make_array();
        a.elements.push_back(replica_update_object(*in.replica));
        root.members.emplace_back("ReplicaUpdates", std::move(a));
    }
    if (in.table_class.has_value()) {
        root.members.emplace_back("TableClass", make_string(table_class_name(*in.table_class)));
    }
    if (in.deletion_protection.has_value()) {
        root.members.emplace_back("DeletionProtectionEnabled", make_bool(*in.deletion_protection));
    }
    if (in.billing.has_value() && in.billing->mode == BillingMode::pay_per_request) {
        root.members.emplace_back("OnDemandThroughput", on_demand_object(in.billing->od));
    }
    if (in.warm.has_value()) {
        root.members.emplace_back("WarmThroughput", warm_object(*in.warm));
    }
    if (!in.vector_index_updates.empty()) {
        Json a = make_array();
        a.elements.reserve(in.vector_index_updates.size());
        for (const auto& vu : in.vector_index_updates) {
            a.elements.push_back(vector_index_update_object(vu));
        }
        root.members.emplace_back("VectorIndexUpdates", std::move(a));
    }
    return WireRequest{"UpdateTable", render(root)};
}

// ---- DeleteTable -----------------------------------------------------------
WireRequest serialize(const DeleteTableIn& in) {
    Json root = make_object();
    root.members.emplace_back("TableName", make_string(in.table_name));
    return WireRequest{"DeleteTable", render(root)};
}

// ---- UpdateTimeToLive ------------------------------------------------------
// Jsonize order: TableName, TimeToLiveSpecification{Enabled, AttributeName}.
// The adapter always sets Enabled and AttributeName.
WireRequest serialize(const UpdateTimeToLiveIn& in) {
    Json root = make_object();
    root.members.emplace_back("TableName", make_string(in.table_name));
    Json spec = make_object();
    spec.members.emplace_back("Enabled", make_bool(in.enabled));
    spec.members.emplace_back("AttributeName", make_string(in.attribute_name));
    root.members.emplace_back("TimeToLiveSpecification", std::move(spec));
    return WireRequest{"UpdateTimeToLive", render(root)};
}

// ---- UpdateContinuousBackups (PITR) ----------------------------------------
// Jsonize order: TableName, PointInTimeRecoverySpecification. The adapter sets
// only PointInTimeRecoveryEnabled (not RecoveryPeriodInDays).
WireRequest serialize(const UpdateContinuousBackupsIn& in) {
    Json root = make_object();
    root.members.emplace_back("TableName", make_string(in.table_name));
    Json spec = make_object();
    spec.members.emplace_back("PointInTimeRecoveryEnabled", make_bool(in.enabled));
    root.members.emplace_back("PointInTimeRecoverySpecification", std::move(spec));
    return WireRequest{"UpdateContinuousBackups", render(root)};
}

// ---- Describe family (table-name-only bodies) ------------------------------
WireRequest serialize(const DescribeTableIn& in) {
    Json root = make_object();
    root.members.emplace_back("TableName", make_string(in.table_name));
    return WireRequest{"DescribeTable", render(root)};
}

WireRequest serialize(const DescribeTimeToLiveIn& in) {
    Json root = make_object();
    root.members.emplace_back("TableName", make_string(in.table_name));
    return WireRequest{"DescribeTimeToLive", render(root)};
}

WireRequest serialize(const DescribeContinuousBackupsIn& in) {
    Json root = make_object();
    root.members.emplace_back("TableName", make_string(in.table_name));
    return WireRequest{"DescribeContinuousBackups", render(root)};
}

WireRequest serialize(const DescribeTableReplicaAutoScalingIn& in) {
    Json root = make_object();
    root.members.emplace_back("TableName", make_string(in.table_name));
    return WireRequest{"DescribeTableReplicaAutoScaling", render(root)};
}

// ---- DescribeLimits --------------------------------------------------------
// The SDK's DescribeLimitsRequest::SerializePayload() is hardcoded to return
// "{}" (it has no request fields), NOT the readable-printer's "{\n}". Match it
// verbatim.
WireRequest serialize(const DescribeLimitsIn&) {
    return WireRequest{"DescribeLimits", "{}"};
}

// ---- ListTables ------------------------------------------------------------
// The engine's ListTablesIn carries no fields (the paging cursor is threaded by
// the WireClient in P6c-b, which splices ExclusiveStartTableName/Limit as the
// SDK's ListTablesRequest serializes them). ListTablesRequest::SerializePayload
// builds an all-optional JsonValue; with nothing set it is a NULL cJSON, and
// JsonView::WriteReadable(treatAsObject=true) returns the hardcoded "{\n}\n"
// (NOT the printer's "{\n}"). Match that empty-body byte string verbatim.
WireRequest serialize(const ListTablesIn&) {
    return WireRequest{"ListTables", "{\n}\n"};
}

// ---- ListBackups -----------------------------------------------------------
// Jsonize order (ListBackupsRequest.cpp), restricted to what the adapter sets:
// TableName, Limit, ExclusiveStartBackupArn, BackupType. Limit is always set
// (WithInteger). BackupType is emitted verbatim (the adapter round-trips a
// valid filter name through the mapper to itself; arch N3 unvalidated
// pass-through).
WireRequest serialize(const ListBackupsIn& in) {
    Json root = make_object();
    if (in.table_name.has_value()) {
        root.members.emplace_back("TableName", make_string(*in.table_name));
    }
    root.members.emplace_back("Limit", make_number_int(in.limit));
    if (in.exclusive_start_backup_arn.has_value()) {
        root.members.emplace_back("ExclusiveStartBackupArn",
                                  make_string(*in.exclusive_start_backup_arn));
    }
    if (in.backup_type.has_value()) {
        root.members.emplace_back("BackupType", make_string(*in.backup_type));
    }
    return WireRequest{"ListBackups", render(root)};
}

// ---- CreateBackup ----------------------------------------------------------
WireRequest serialize(const CreateBackupIn& in) {
    Json root = make_object();
    root.members.emplace_back("TableName", make_string(in.table_name));
    root.members.emplace_back("BackupName", make_string(in.backup_name));
    return WireRequest{"CreateBackup", render(root)};
}

// ---- DeleteBackup ----------------------------------------------------------
WireRequest serialize(const DeleteBackupIn& in) {
    Json root = make_object();
    root.members.emplace_back("BackupArn", make_string(in.backup_arn));
    return WireRequest{"DeleteBackup", render(root)};
}

// ---- DescribeBackup --------------------------------------------------------
WireRequest serialize(const DescribeBackupIn& in) {
    Json root = make_object();
    root.members.emplace_back("BackupArn", make_string(in.backup_arn));
    return WireRequest{"DescribeBackup", render(root)};
}

// ---- RestoreTableFromBackup ------------------------------------------------
// Jsonize order: TargetTableName, BackupArn (the adapter sets both).
WireRequest serialize(const RestoreFromBackupIn& in) {
    Json root = make_object();
    root.members.emplace_back("TargetTableName", make_string(in.target_table_name));
    root.members.emplace_back("BackupArn", make_string(in.backup_arn));
    return WireRequest{"RestoreTableFromBackup", render(root)};
}

// ---- RestoreTableToPointInTime ---------------------------------------------
// Jsonize order (restricted to what the adapter sets): SourceTableName,
// TargetTableName, RestoreDateTime (a JSON double = the SDK's
// DateTime::SecondsWithMSPrecision() formatted by cJSON). The AWS parser
// discards fractional seconds, so the value is a whole number. This serializer
// is TOTAL: it assumes the caller (the WireClient / explain_request arm)
// already validated the timestamp via iso8601_to_epoch and returned the SDK's
// invalid_argument Error on failure (D-P6c-2 / §2.2); an unparseable string
// falls back to epoch 0.0, which is never emitted on the validated path.
WireRequest serialize(const RestoreToPitrIn& in) {
    Json root = make_object();
    root.members.emplace_back("SourceTableName", make_string(in.source_table_name));
    root.members.emplace_back("TargetTableName", make_string(in.target_table_name));
    const double epoch = iso8601_to_epoch(in.restore_datetime).value_or(0.0);
    root.members.emplace_back("RestoreDateTime", make_number_double(epoch));
    return WireRequest{"RestoreTableToPointInTime", render(root)};
}

// ---- TransactGetItems ------------------------------------------------------
// Jsonize order: TransactItems (the adapter sets no ReturnConsumedCapacity).
// Each TransactGetItem wraps a Get; Get Jsonize order: Key, TableName,
// ProjectionExpression, ExpressionAttributeNames.
WireRequest serialize(const TransactGetItemsIn& in) {
    Json root = make_object();
    Json items = make_array();
    items.elements.reserve(in.items.size());
    for (const auto& g : in.items) {
        Json get = make_object();
        get.members.emplace_back("Key", value_map_object(g.key));
        get.members.emplace_back("TableName", make_string(g.table_name));
        if (g.projection_expression.has_value()) {
            get.members.emplace_back("ProjectionExpression", make_string(*g.projection_expression));
        }
        if (!g.expression_attribute_names.empty()) {
            get.members.emplace_back("ExpressionAttributeNames",
                                     names_object(g.expression_attribute_names));
        }
        Json item = make_object();
        item.members.emplace_back("Get", std::move(get));
        items.elements.push_back(std::move(item));
    }
    root.members.emplace_back("TransactItems", std::move(items));
    return WireRequest{"TransactGetItems", render(root)};
}

// ---- TransactWriteItems ----------------------------------------------------
// Jsonize order: TransactItems (the adapter sets no ReturnConsumedCapacity /
// ClientRequestToken). Each TransactWriteItem wraps exactly one of Put / Update
// / Delete / ConditionCheck. The sub-item Jsonize orders:
//   Put:            Item, TableName, ConditionExpression, EAN, EAV.
//   Update:         Key, UpdateExpression, TableName, ConditionExpression,
//                   EAN, EAV.
//   Delete:         Key, TableName, ConditionExpression, EAN, EAV.
//   ConditionCheck: Key, TableName, ConditionExpression, EAN, EAV.
// No sub-item carries ReturnValues (the adapter never sets them, D-2d-10).
WireRequest serialize(const TransactWriteItemsIn& in) {
    Json root = make_object();
    Json items = make_array();
    items.elements.reserve(in.items.size());
    for (const auto& sub : in.items) {
        Json item = make_object();
        std::visit(
            [&](const auto& w) {
                using T = std::decay_t<decltype(w)>;
                if constexpr (std::is_same_v<T, TransactPut>) {
                    Json put = make_object();
                    put.members.emplace_back("Item", value_map_object(w.item));
                    put.members.emplace_back("TableName", make_string(w.table_name));
                    if (w.condition_expression.has_value()) {
                        put.members.emplace_back("ConditionExpression",
                                                 make_string(*w.condition_expression));
                    }
                    if (!w.expression_attribute_names.empty()) {
                        put.members.emplace_back("ExpressionAttributeNames",
                                                 names_object(w.expression_attribute_names));
                    }
                    if (!w.expression_attribute_values.empty()) {
                        put.members.emplace_back("ExpressionAttributeValues",
                                                 value_map_object(w.expression_attribute_values));
                    }
                    item.members.emplace_back("Put", std::move(put));
                } else if constexpr (std::is_same_v<T, TransactUpdate>) {
                    Json upd = make_object();
                    upd.members.emplace_back("Key", value_map_object(w.key));
                    upd.members.emplace_back("UpdateExpression", make_string(w.update_expression));
                    upd.members.emplace_back("TableName", make_string(w.table_name));
                    if (w.condition_expression.has_value()) {
                        upd.members.emplace_back("ConditionExpression",
                                                 make_string(*w.condition_expression));
                    }
                    if (!w.expression_attribute_names.empty()) {
                        upd.members.emplace_back("ExpressionAttributeNames",
                                                 names_object(w.expression_attribute_names));
                    }
                    if (!w.expression_attribute_values.empty()) {
                        upd.members.emplace_back("ExpressionAttributeValues",
                                                 value_map_object(w.expression_attribute_values));
                    }
                    item.members.emplace_back("Update", std::move(upd));
                } else if constexpr (std::is_same_v<T, TransactDelete>) {
                    Json del = make_object();
                    del.members.emplace_back("Key", value_map_object(w.key));
                    del.members.emplace_back("TableName", make_string(w.table_name));
                    if (w.condition_expression.has_value()) {
                        del.members.emplace_back("ConditionExpression",
                                                 make_string(*w.condition_expression));
                    }
                    if (!w.expression_attribute_names.empty()) {
                        del.members.emplace_back("ExpressionAttributeNames",
                                                 names_object(w.expression_attribute_names));
                    }
                    if (!w.expression_attribute_values.empty()) {
                        del.members.emplace_back("ExpressionAttributeValues",
                                                 value_map_object(w.expression_attribute_values));
                    }
                    item.members.emplace_back("Delete", std::move(del));
                } else if constexpr (std::is_same_v<T, TransactConditionCheck>) {
                    Json cc = make_object();
                    cc.members.emplace_back("Key", value_map_object(w.key));
                    cc.members.emplace_back("TableName", make_string(w.table_name));
                    cc.members.emplace_back("ConditionExpression",
                                            make_string(w.condition_expression));
                    if (!w.expression_attribute_names.empty()) {
                        cc.members.emplace_back("ExpressionAttributeNames",
                                                names_object(w.expression_attribute_names));
                    }
                    if (!w.expression_attribute_values.empty()) {
                        cc.members.emplace_back("ExpressionAttributeValues",
                                                value_map_object(w.expression_attribute_values));
                    }
                    item.members.emplace_back("ConditionCheck", std::move(cc));
                }
            },
            sub);
        items.elements.push_back(std::move(item));
    }
    root.members.emplace_back("TransactItems", std::move(items));
    return WireRequest{"TransactWriteItems", render(root)};
}

// ---- SearchVectors ---------------------------------------------------------
// Jsonize order (SearchVectorsRequest.cpp): TableName, IndexName,
// ReturnConsumedCapacity, ExpressionAttributeNames, ExpressionAttributeValues,
// ProjectionExpression, SearchVector, SearchConditionExpression, TopK. The
// adapter always sets TableName, IndexName, ReturnConsumedCapacity (even NONE),
// SearchVector, and TopK; the rest per clause. The filter member is
// SearchConditionExpression (NOT FilterExpression).
WireRequest serialize(const SearchVectorsIn& in) {
    Json root = make_object();
    root.members.emplace_back("TableName", make_string(in.table_name));
    root.members.emplace_back("IndexName", make_string(in.index_name));
    root.members.emplace_back("ReturnConsumedCapacity",
                              make_string(rcc_name(in.return_consumed_capacity)));
    if (!in.expression_attribute_names.empty()) {
        root.members.emplace_back("ExpressionAttributeNames",
                                  names_object(in.expression_attribute_names));
    }
    if (!in.expression_attribute_values.empty()) {
        root.members.emplace_back("ExpressionAttributeValues",
                                  value_map_object(in.expression_attribute_values));
    }
    if (in.projection_expression.has_value()) {
        root.members.emplace_back("ProjectionExpression", make_string(*in.projection_expression));
    }
    root.members.emplace_back("SearchVector", value_list_array(in.search_vector));
    if (in.search_condition_expression.has_value()) {
        root.members.emplace_back("SearchConditionExpression",
                                  make_string(*in.search_condition_expression));
    }
    root.members.emplace_back("TopK", make_number_int(in.topk));
    return WireRequest{"SearchVectors", render(root)};
}

}  // namespace ddbsh::wire
