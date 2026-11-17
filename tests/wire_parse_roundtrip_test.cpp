// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// wire_parse_roundtrip_test.cpp — the P6b response parser (P6-02 §3, §4).
//
// Two complementary mechanisms:
//   1. serialize∘parse identity for the AttributeValue layer — for an item
//      exercising every Value type (incl. nested L/M and empty collections),
//      serialize a PutItem (whose body carries the item under "Item"), parse it
//      back with parse_get_item, and assert the recovered item equals the
//      original (maps modulo key order — the serializer sorts; sets modulo
//      element order). This pins the parser as the exact inverse of P6a's
//      byte-proven serializer with no hand fixtures for the AV layer.
//   2. Envelope + error fixtures — hand-authored canonical AWS-JSON-1.0 bodies
//      asserting each *Out field against the AWS adapter's documented behaviour
//      (nullopt where the adapter leaves it unset, Count/ScannedCount ignored,
//      ConsumedCapacity `any`-flag semantics) and the __type -> Error mapping.

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "ddbsh/iddbclient.hpp"
#include "ddbsh/value.hpp"
#include "ddbsh/wire/codec.hpp"

namespace {

using ddbsh::ConsumedCapacity;
using ddbsh::DeleteItemOut;
using ddbsh::Errc;
using ddbsh::GetItemOut;
using ddbsh::ItemRow;
using ddbsh::PutItemIn;
using ddbsh::PutItemOut;
using ddbsh::QueryOut;
using ddbsh::ScanOut;
using ddbsh::UpdateItemOut;
using ddbsh::Value;
using ddbsh::ValueList;
using ddbsh::ValueMap;

// ---- order-insensitive Value equality --------------------------------------
// Maps are compared modulo key order (the serializer sorts keys); sets modulo
// element order (DynamoDB sets are unordered). Lists are order-sensitive.

bool value_equal(const Value& a, const Value& b);

bool map_equal(const ValueMap& a, const ValueMap& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (const auto& [ka, va] : a) {
        bool found = false;
        for (const auto& [kb, vb] : b) {
            if (ka == kb) {
                if (!value_equal(va, vb)) {
                    return false;
                }
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

bool set_equal(std::vector<std::string> a, std::vector<std::string> b) {
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    return a == b;
}

bool value_equal(const Value& a, const Value& b) {
    if (a.type != b.type) {
        return false;
    }
    switch (a.type) {
        case Value::Type::S:
        case Value::Type::N:
        case Value::Type::B:
            return a.s == b.s;
        case Value::Type::BOOL:
            return a.boolean == b.boolean;
        case Value::Type::NUL:
            return true;
        case Value::Type::L:
            if (a.list.size() != b.list.size()) {
                return false;
            }
            for (std::size_t i = 0; i < a.list.size(); ++i) {
                if (!value_equal(a.list[i], b.list[i])) {
                    return false;
                }
            }
            return true;
        case Value::Type::M:
            return map_equal(a.map, b.map);
        case Value::Type::SS:
        case Value::Type::NS:
        case Value::Type::BS:
            return set_equal(a.set, b.set);
    }
    return false;
}

Value list_of(ValueList elems) {
    Value v;
    v.type = Value::Type::L;
    v.list = std::move(elems);
    return v;
}

Value map_of(ValueMap entries) {
    Value v;
    v.type = Value::Type::M;
    v.map = std::move(entries);
    return v;
}

// Every AttributeValue type, including nested L/M and empty collections.
ValueMap all_types_item() {
    ValueMap m;
    m.emplace_back("s", Value::string_value("hello world"));  // quote-forcing chars
    m.emplace_back("n", Value::number_value("3.14159"));
    m.emplace_back("b", Value::binary_value("aGVsbG8="));
    m.emplace_back("bt", Value::bool_value(true));
    m.emplace_back("bf", Value::bool_value(false));
    m.emplace_back("nul", Value::null_value());
    m.emplace_back("ss", Value::string_set({"x", "y", "z"}));
    m.emplace_back("ns", Value::number_set({"1", "2", "-3"}));
    m.emplace_back("bs", Value::binary_set({"YQ==", "Yg=="}));
    m.emplace_back("l", list_of({Value::number_value("4"), Value::string_value("five"),
                                 Value::bool_value(true)}));
    m.emplace_back("m", map_of({{"inner", Value::string_value("v")},
                                {"deep", list_of({Value::number_value("9")})}}));
    m.emplace_back("empty_l", list_of({}));
    m.emplace_back("empty_m", map_of({}));
    return m;
}

// ---- serialize∘parse identity ----------------------------------------------

TEST(WireParseRoundtrip, AttributeValueIdentityAllTypes) {
    const ValueMap original = all_types_item();
    PutItemIn in;
    in.table_name = "t";
    in.item = original;
    // The PutItem body carries the item under "Item"; parse_get_item reads it.
    const std::string body = ddbsh::wire::serialize(in).body;
    auto out = ddbsh::wire::parse_get_item(body);
    ASSERT_TRUE(out.has_value());
    ASSERT_TRUE(out->item.has_value());
    EXPECT_TRUE(map_equal(out->item->attributes, original));
}

// ---- GetItem envelopes -----------------------------------------------------

TEST(WireParseRoundtrip, GetItemPresent) {
    auto out = ddbsh::wire::parse_get_item(R"({"Item":{"id":{"S":"k1"},"v":{"N":"7"}}})");
    ASSERT_TRUE(out.has_value());
    ASSERT_TRUE(out->item.has_value());
    EXPECT_EQ(out->item->attributes.size(), 2u);
    EXPECT_FALSE(out->consumed_capacity.has_value());
}

TEST(WireParseRoundtrip, GetItemAbsentWhenNoItemMember) {
    auto out = ddbsh::wire::parse_get_item(R"({})");
    ASSERT_TRUE(out.has_value());
    EXPECT_FALSE(out->item.has_value());
}

TEST(WireParseRoundtrip, GetItemAbsentWhenEmptyItem) {
    auto out = ddbsh::wire::parse_get_item(R"({"Item":{}})");
    ASSERT_TRUE(out.has_value());
    EXPECT_FALSE(out->item.has_value());  // empty item -> nullopt (adapter's !item.empty())
}

// ---- Query / Scan envelopes ------------------------------------------------

TEST(WireParseRoundtrip, QueryItemsAndLek) {
    const char* body = R"({
        "Items": [ {"id":{"S":"a"}}, {"id":{"S":"b"}} ],
        "LastEvaluatedKey": {"id":{"S":"b"}},
        "Count": 2, "ScannedCount": 5
    })";
    auto out = ddbsh::wire::parse_query(body);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->items.size(), 2u);  // Count/ScannedCount ignored
    ASSERT_TRUE(out->last_evaluated_key.has_value());
    EXPECT_EQ(out->last_evaluated_key->size(), 1u);
    EXPECT_FALSE(out->consumed_capacity.has_value());
}

TEST(WireParseRoundtrip, QueryNoItemsNoLek) {
    auto out = ddbsh::wire::parse_query(R"({"Count":0,"ScannedCount":0})");
    ASSERT_TRUE(out.has_value());
    EXPECT_TRUE(out->items.empty());
    EXPECT_FALSE(out->last_evaluated_key.has_value());
}

TEST(WireParseRoundtrip, ScanEmptyLekTreatedAsAbsent) {
    auto out = ddbsh::wire::parse_scan(R"({"Items":[],"LastEvaluatedKey":{}})");
    ASSERT_TRUE(out.has_value());
    EXPECT_TRUE(out->items.empty());
    EXPECT_FALSE(out->last_evaluated_key.has_value());
}

// ---- ConsumedCapacity `any`-flag semantics ---------------------------------

TEST(WireParseRoundtrip, ConsumedCapacityAllFields) {
    const char* body = R"({
        "Items": [],
        "ConsumedCapacity": {
            "TableName": "t", "CapacityUnits": 3.5,
            "ReadCapacityUnits": 2.0, "WriteCapacityUnits": 1.5
        }
    })";
    auto out = ddbsh::wire::parse_scan(body);
    ASSERT_TRUE(out.has_value());
    ASSERT_TRUE(out->consumed_capacity.has_value());
    const ConsumedCapacity& cc = *out->consumed_capacity;
    ASSERT_TRUE(cc.table_name.has_value());
    EXPECT_EQ(*cc.table_name, "t");
    ASSERT_TRUE(cc.capacity_units.has_value());
    EXPECT_DOUBLE_EQ(*cc.capacity_units, 3.5);
    ASSERT_TRUE(cc.read_capacity_units.has_value());
    EXPECT_DOUBLE_EQ(*cc.read_capacity_units, 2.0);
    ASSERT_TRUE(cc.write_capacity_units.has_value());
    EXPECT_DOUBLE_EQ(*cc.write_capacity_units, 1.5);
}

TEST(WireParseRoundtrip, ConsumedCapacityOnlyCapacityUnits) {
    auto out = ddbsh::wire::parse_query(R"({"Items":[],"ConsumedCapacity":{"CapacityUnits":1}})");
    ASSERT_TRUE(out.has_value());
    ASSERT_TRUE(out->consumed_capacity.has_value());
    EXPECT_FALSE(out->consumed_capacity->table_name.has_value());
    ASSERT_TRUE(out->consumed_capacity->capacity_units.has_value());
    EXPECT_DOUBLE_EQ(*out->consumed_capacity->capacity_units, 1.0);
}

TEST(WireParseRoundtrip, ConsumedCapacityEmptyObjectIsNullopt) {
    auto out = ddbsh::wire::parse_query(R"({"Items":[],"ConsumedCapacity":{}})");
    ASSERT_TRUE(out.has_value());
    EXPECT_FALSE(out->consumed_capacity.has_value());  // no sub-field -> nullopt
}

// ---- Put / Update / Delete envelopes ---------------------------------------

TEST(WireParseRoundtrip, PutAttributesPresent) {
    auto out = ddbsh::wire::parse_put_item(R"({"Attributes":{"old":{"N":"1"}}})");
    ASSERT_TRUE(out.has_value());
    ASSERT_TRUE(out->attributes.has_value());
    EXPECT_EQ(out->attributes->attributes.size(), 1u);
}

TEST(WireParseRoundtrip, UpdateAttributesAbsent) {
    auto out = ddbsh::wire::parse_update_item(R"({})");
    ASSERT_TRUE(out.has_value());
    EXPECT_FALSE(out->attributes.has_value());
}

TEST(WireParseRoundtrip, DeleteAttributesEmptyIsAbsent) {
    auto out = ddbsh::wire::parse_delete_item(R"({"Attributes":{}})");
    ASSERT_TRUE(out.has_value());
    EXPECT_FALSE(out->attributes.has_value());
}

// ---- __type error documents ------------------------------------------------

TEST(WireParseRoundtrip, ErrorConditionalCheckFailed) {
    const char* body = R"({
        "__type": "com.amazonaws.dynamodb.v20120810#ConditionalCheckFailedException",
        "message": "The conditional request failed"
    })";
    auto out = ddbsh::wire::parse_put_item(body);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Errc::conditional_check_failed);
    EXPECT_EQ(out.error().message, "The conditional request failed");
    ASSERT_TRUE(out.error().service_error_name.has_value());
    EXPECT_EQ(*out.error().service_error_name, "ConditionalCheckFailedException");
    EXPECT_FALSE(out.error().request_id.has_value());  // D-P6b-2
}

TEST(WireParseRoundtrip, ErrorGenericServiceError) {
    const char* body = R"({
        "__type": "com.amazonaws.dynamodb.v20120810#ResourceNotFoundException",
        "message": "Requested resource not found"
    })";
    auto out = ddbsh::wire::parse_get_item(body);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Errc::service_error);
    EXPECT_EQ(out.error().message, "Requested resource not found");
    EXPECT_EQ(*out.error().service_error_name, "ResourceNotFoundException");
}

TEST(WireParseRoundtrip, ErrorUppercaseMessageKey) {
    const char* body = R"({"__type":"x#ValidationException","Message":"bad input"})";
    auto out = ddbsh::wire::parse_query(body);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Errc::service_error);
    EXPECT_EQ(out.error().message, "bad input");
    EXPECT_EQ(*out.error().service_error_name, "ValidationException");
}

TEST(WireParseRoundtrip, ErrorTypeWithoutHashUsesWholeString) {
    auto out = ddbsh::wire::parse_scan(R"({"__type":"SomeBareException","message":"m"})");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Errc::service_error);
    EXPECT_EQ(*out.error().service_error_name, "SomeBareException");
}

TEST(WireParseRoundtrip, ErrorTakesPrecedenceOverBody) {
    // A top-level __type wins even if other members look like a success body.
    auto out = ddbsh::wire::parse_get_item(
        R"({"__type":"x#AccessDeniedException","message":"no","Item":{"id":{"S":"k"}}})");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(*out.error().service_error_name, "AccessDeniedException");
}

// ---- malformed bodies ------------------------------------------------------

TEST(WireParseRoundtrip, MalformedBody) {
    auto out = ddbsh::wire::parse_get_item("not json");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Errc::service_error);
}

TEST(WireParseRoundtrip, NonObjectTopLevel) {
    auto out = ddbsh::wire::parse_get_item("[1,2,3]");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Errc::service_error);
}

TEST(WireParseRoundtrip, MalformedAttributeValueMultiKey) {
    auto out = ddbsh::wire::parse_get_item(R"({"Item":{"a":{"S":"x","N":"1"}}})");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Errc::service_error);
}

TEST(WireParseRoundtrip, MalformedAttributeValueUnknownType) {
    auto out = ddbsh::wire::parse_get_item(R"({"Item":{"a":{"Z":"x"}}})");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Errc::service_error);
}

TEST(WireParseRoundtrip, MalformedAttributeValueWrongBodyKind) {
    // S must be a string, not a number.
    auto out = ddbsh::wire::parse_get_item(R"({"Item":{"a":{"S":5}}})");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Errc::service_error);
}

}  // namespace
