// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// wire_json_reader_test.cpp — the SDK-free JSON reader in isolation (P6b).
//
// Exercises literals, nesting, every string escape (incl. \uXXXX and a
// surrogate pair), verbatim number tokens, empty [] / {}, object key order, and
// the malformed-input failure paths (tenet 6 — error paths are first-class).

#include <string>

#include <gtest/gtest.h>

#include "ddbsh/result.hpp"
#include "wire/json_reader.hpp"

namespace {

using ddbsh::Errc;
using ddbsh::wire::json::JsonValue;
using ddbsh::wire::json::parse;

// ---- literals --------------------------------------------------------------

TEST(JsonReader, Literals) {
    auto t = parse("true");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->kind, JsonValue::Kind::Bool);
    EXPECT_TRUE(t->boolean);

    auto f = parse("false");
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->kind, JsonValue::Kind::Bool);
    EXPECT_FALSE(f->boolean);

    auto n = parse("null");
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(n->kind, JsonValue::Kind::Null);
}

TEST(JsonReader, LeadingAndTrailingWhitespace) {
    auto v = parse("  \t\n\r {\"a\": 1}  \n");
    ASSERT_TRUE(v.has_value());
    EXPECT_TRUE(v->is_object());
}

// ---- numbers (kept verbatim) ----------------------------------------------

TEST(JsonReader, NumberTokensVerbatim) {
    for (const char* tok : {"0", "-0", "42", "-17", "3.14159", "1e10", "1E10", "-2.5e-3",
                            "123456789012345678901234567890"}) {
        auto v = parse(tok);
        ASSERT_TRUE(v.has_value()) << tok;
        EXPECT_EQ(v->kind, JsonValue::Kind::Number);
        EXPECT_EQ(v->text, tok);
    }
}

TEST(JsonReader, MalformedNumbers) {
    for (const char* bad : {"01", "-", "1.", "1e", "1e+", ".5", "+1"}) {
        auto v = parse(bad);
        EXPECT_FALSE(v.has_value()) << bad;
        if (!v.has_value()) {
            EXPECT_EQ(v.error().code, Errc::service_error);
        }
    }
}

// ---- strings & escapes -----------------------------------------------------

TEST(JsonReader, SimpleString) {
    auto v = parse(R"("hello world")");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->kind, JsonValue::Kind::String);
    EXPECT_EQ(v->text, "hello world");
}

TEST(JsonReader, NamedEscapes) {
    auto v = parse(R"("a\"b\\c\/d\be\ff\ng\rh\ti")");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->text, std::string("a\"b\\c/d\be\ff\ng\rh\ti"));
}

TEST(JsonReader, UnicodeEscapeBmp) {
    // U+00E9 (é) -> UTF-8 0xC3 0xA9; U+2713 (✓) -> 0xE2 0x9C 0x93.
    auto v = parse(R"("caf\u00e9 \u2713")");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->text, std::string("caf\xc3\xa9 \xe2\x9c\x93"));
}

TEST(JsonReader, UnicodeSurrogatePair) {
    // U+1F600 (😀) encoded as the surrogate pair \uD83D\uDE00 -> UTF-8
    // 0xF0 0x9F 0x98 0x80.
    auto v = parse(R"("\uD83D\uDE00")");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->text, std::string("\xf0\x9f\x98\x80"));
}

TEST(JsonReader, LiteralUtf8CopiedVerbatim) {
    const std::string in = std::string("\"caf\xc3\xa9\"");
    auto v = parse(in);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->text, std::string("caf\xc3\xa9"));
}

TEST(JsonReader, MalformedStrings) {
    EXPECT_FALSE(parse("\"unterminated").has_value());
    EXPECT_FALSE(parse(R"("bad\xescape")").has_value());
    EXPECT_FALSE(parse(R"("\u12")").has_value());     // truncated \u
    EXPECT_FALSE(parse(R"("\uZZZZ")").has_value());   // non-hex \u
    EXPECT_FALSE(parse(R"("\uD83D")").has_value());   // unpaired high surrogate
    EXPECT_FALSE(parse(R"("\uDE00")").has_value());   // lone low surrogate
    EXPECT_FALSE(parse(R"("\uD83Dx")").has_value());  // high surrogate not followed by \u
    EXPECT_FALSE(parse(std::string("\"ctrl\x01here\"")).has_value());  // raw control byte
}

// ---- arrays & objects ------------------------------------------------------

TEST(JsonReader, EmptyContainers) {
    auto a = parse("[]");
    ASSERT_TRUE(a.has_value());
    EXPECT_TRUE(a->is_array());
    EXPECT_TRUE(a->elements.empty());

    auto o = parse("{}");
    ASSERT_TRUE(o.has_value());
    EXPECT_TRUE(o->is_object());
    EXPECT_TRUE(o->members.empty());
}

TEST(JsonReader, Array) {
    auto v = parse(R"([1, "two", true, null, []])");
    ASSERT_TRUE(v.has_value());
    ASSERT_EQ(v->elements.size(), 5u);
    EXPECT_EQ(v->elements[0].kind, JsonValue::Kind::Number);
    EXPECT_EQ(v->elements[1].text, "two");
    EXPECT_TRUE(v->elements[2].boolean);
    EXPECT_EQ(v->elements[3].kind, JsonValue::Kind::Null);
    EXPECT_TRUE(v->elements[4].is_array());
}

TEST(JsonReader, ObjectKeyOrderPreserved) {
    auto v = parse(R"({"z": 1, "a": 2, "m": 3})");
    ASSERT_TRUE(v.has_value());
    ASSERT_EQ(v->members.size(), 3u);
    EXPECT_EQ(v->members[0].first, "z");
    EXPECT_EQ(v->members[1].first, "a");
    EXPECT_EQ(v->members[2].first, "m");
    const JsonValue* a = v->find("a");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->text, "2");
    EXPECT_EQ(v->find("absent"), nullptr);
}

TEST(JsonReader, NestedStructure) {
    auto v = parse(R"({"Item": {"id": {"S": "k1"}, "n": {"N": "42"}}})");
    ASSERT_TRUE(v.has_value());
    const JsonValue* item = v->find("Item");
    ASSERT_NE(item, nullptr);
    const JsonValue* id = item->find("id");
    ASSERT_NE(id, nullptr);
    const JsonValue* s = id->find("S");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->text, "k1");
}

TEST(JsonReader, DuplicateKeysBothPresent) {
    // Not expected in DynamoDB; the reader keeps both, find() returns the first.
    auto v = parse(R"({"k": 1, "k": 2})");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->members.size(), 2u);
    EXPECT_EQ(v->find("k")->text, "1");
}

// ---- structural failure paths ---------------------------------------------

TEST(JsonReader, MalformedStructures) {
    EXPECT_FALSE(parse("").has_value());                    // empty input
    EXPECT_FALSE(parse("{").has_value());                   // unterminated object
    EXPECT_FALSE(parse("[").has_value());                   // unterminated array
    EXPECT_FALSE(parse(R"({"a")").has_value());             // missing ':'
    EXPECT_FALSE(parse(R"({"a": 1,})").has_value());        // trailing comma -> key expected
    EXPECT_FALSE(parse(R"([1 2])").has_value());            // missing ','
    EXPECT_FALSE(parse(R"({a: 1})").has_value());           // unquoted key
    EXPECT_FALSE(parse("{\"a\": 1} garbage").has_value());  // trailing garbage
    EXPECT_FALSE(parse("tru").has_value());                 // partial literal
    EXPECT_FALSE(parse("@").has_value());                   // stray token
}

TEST(JsonReader, DeepNestingIsRejectedNotCrashing) {
    std::string deep;
    for (int i = 0; i < 2000; ++i) {
        deep += '[';
    }
    auto v = parse(deep);
    EXPECT_FALSE(v.has_value());
    if (!v.has_value()) {
        EXPECT_EQ(v.error().code, Errc::service_error);
    }
}

}  // namespace
