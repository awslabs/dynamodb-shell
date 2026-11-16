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
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace ddbsh::wire {
namespace {

// ---- A minimal ordered JSON tree reproducing cJSON's readable output -------
// Only the value kinds the request bodies need: object (ordered members),
// array, string, and bool. Numbers never appear as JSON numbers in a DynamoDB
// request body (AttributeValue N is a string; ConsistentRead etc. are bools).

struct Json {
    enum class Kind { Object, Array, String, Bool };
    Kind kind = Kind::Object;
    std::vector<std::pair<std::string, Json>> members;  // Object, in order
    std::vector<Json> elements;                         // Array, in order
    std::string str;                                    // String
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

}  // namespace ddbsh::wire
