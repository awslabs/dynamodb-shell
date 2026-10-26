// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// lexer_test.cpp — tokenizer fidelity tests (arch §4, Phase 2b W1).
//
// Drives the ported reentrant scanner through the engine-internal debug_lex
// helper and asserts the token sequence for the quirks that must be preserved
// byte-for-byte to match the original tokenizer. These need no backend and are
// fully deterministic.

#include "parser/lex_debug.hpp"

#include <string>
#include <vector>

#include <gtest/gtest.h>

using ddbsh::engine::debug_lex;
using ddbsh::engine::LexToken;

namespace {

std::vector<std::string> names(std::string_view in) {
    std::vector<std::string> out;
    for (const auto& t : debug_lex(in)) {
        out.push_back(t.name);
    }
    return out;
}

}  // namespace

TEST(Lexer, KeywordsAreCaseInsensitive) {
    EXPECT_EQ(names("SELECT"), (std::vector<std::string>{"K_SELECT"}));
    EXPECT_EQ(names("select"), (std::vector<std::string>{"K_SELECT"}));
    EXPECT_EQ(names("SeLeCt"), (std::vector<std::string>{"K_SELECT"}));
}

TEST(Lexer, KeywordAliasesAndQuirks) {
    // end => K_COMMIT; numeric => K_NUMBER; number => K_ATTRIBUTE_TYPE_NUMBER;
    // string => K_ATTRIBUTE_TYPE_STRING; binary => K_ATTRIBUTE_TYPE_BINARY.
    EXPECT_EQ(names("end"), (std::vector<std::string>{"K_COMMIT"}));
    EXPECT_EQ(names("numeric"), (std::vector<std::string>{"K_NUMBER"}));
    EXPECT_EQ(names("number"), (std::vector<std::string>{"K_ATTRIBUTE_TYPE_NUMBER"}));
    EXPECT_EQ(names("string"), (std::vector<std::string>{"K_ATTRIBUTE_TYPE_STRING"}));
    EXPECT_EQ(names("binary"), (std::vector<std::string>{"K_ATTRIBUTE_TYPE_BINARY"}));
}

TEST(Lexer, UnknownIdentifierIsUnquotedString) {
    auto toks = debug_lex("mytable");
    ASSERT_EQ(toks.size(), 1u);
    EXPECT_EQ(toks[0].name, "T_UNQUOTED_STRING");
    EXPECT_EQ(toks[0].text, "mytable");
}

TEST(Lexer, IdentifierMayStartWithDigitAndContainHyphens) {
    // us-east-1 is a SINGLE unquoted-string token (region names, table names).
    auto toks = debug_lex("us-east-1");
    ASSERT_EQ(toks.size(), 1u);
    EXPECT_EQ(toks[0].name, "T_UNQUOTED_STRING");
    EXPECT_EQ(toks[0].text, "us-east-1");
}

TEST(Lexer, WholeNumberVsLeadingZero) {
    // -?[1-9][0-9]* or 0 are whole numbers.
    EXPECT_EQ(names("42"), (std::vector<std::string>{"T_WHOLE_NUMBER"}));
    EXPECT_EQ(names("0"), (std::vector<std::string>{"T_WHOLE_NUMBER"}));
    EXPECT_EQ(names("-7"), (std::vector<std::string>{"T_WHOLE_NUMBER"}));
    // A leading-zero int like 01 is NOT a whole number: flex maximal-munch
    // prefers the longer identifier rule ([a-zA-Z0-9_][a-zA-Z0-9_-]*, which
    // may start with a digit), so 01 is a single T_UNQUOTED_STRING (matching
    // the original tokenizer).
    EXPECT_EQ(names("01"), (std::vector<std::string>{"T_UNQUOTED_STRING"}));
}

TEST(Lexer, FloatingPointRequiresUppercaseE) {
    EXPECT_EQ(names("3.14"), (std::vector<std::string>{"T_FLOATING_POINT_NUMBER"}));
    EXPECT_EQ(names("1E5"), (std::vector<std::string>{"T_FLOATING_POINT_NUMBER"}));
    EXPECT_EQ(names(".5"), (std::vector<std::string>{"T_FLOATING_POINT_NUMBER"}));
    // Lowercase 'e' is NOT an exponent. 1e5 then matches the identifier rule
    // in full (digit-leading, so maximal-munch beats the whole-number rule) and
    // is a single T_UNQUOTED_STRING (matching the original tokenizer).
    EXPECT_EQ(names("1e5"), (std::vector<std::string>{"T_UNQUOTED_STRING"}));
}

TEST(Lexer, Timestamp) {
    auto toks = debug_lex("\"2024-01-02T03:04:05Z\"");
    ASSERT_EQ(toks.size(), 1u);
    EXPECT_EQ(toks[0].name, "T_TIMESTAMP");
    EXPECT_EQ(toks[0].text, "\"2024-01-02T03:04:05Z\"");
}

TEST(Lexer, DoubleQuotedStringWithDoubledQuote) {
    // SQL-style "" embeds a quote; the verbatim lexeme (quotes included) is kept.
    auto toks = debug_lex("\"a\"\"b\"");
    ASSERT_EQ(toks.size(), 1u);
    EXPECT_EQ(toks[0].name, "T_DQUOTED_STRING");
    EXPECT_EQ(toks[0].text, "\"a\"\"b\"");
}

TEST(Lexer, Operators) {
    EXPECT_EQ(names("<"), (std::vector<std::string>{"T_LESS_THAN"}));
    EXPECT_EQ(names(">"), (std::vector<std::string>{"T_GREATER_THAN"}));
    EXPECT_EQ(names("<="), (std::vector<std::string>{"T_LESS_THAN_OR_EQUAL"}));
    EXPECT_EQ(names(">="), (std::vector<std::string>{"T_GREATER_THAN_OR_EQUAL"}));
    EXPECT_EQ(names("="), (std::vector<std::string>{"T_EQUAL"}));
    // Both != and <> map to T_NOT_EQUAL.
    EXPECT_EQ(names("!="), (std::vector<std::string>{"T_NOT_EQUAL"}));
    EXPECT_EQ(names("<>"), (std::vector<std::string>{"T_NOT_EQUAL"}));
}

TEST(Lexer, CommentsAreDiscarded) {
    EXPECT_EQ(names("select -- a comment\nfrom"), (std::vector<std::string>{"K_SELECT", "K_FROM"}));
    EXPECT_EQ(names("select // another\nfrom"), (std::vector<std::string>{"K_SELECT", "K_FROM"}));
}

TEST(Lexer, SelectStatementSequence) {
    // Assert the keyword/value token sequence (punctuation symbol names are
    // bison-internal and not part of this contract).
    EXPECT_EQ(
        names("select a from t where a = 1"),
        (std::vector<std::string>{"K_SELECT", "T_UNQUOTED_STRING", "K_FROM", "T_UNQUOTED_STRING",
                                  "K_WHERE", "T_UNQUOTED_STRING", "T_EQUAL", "T_WHOLE_NUMBER"}));
}

TEST(Lexer, EmptyInputYieldsNoTokens) {
    EXPECT_TRUE(debug_lex("").empty());
    EXPECT_TRUE(debug_lex("   \t\n  ").empty());
}
