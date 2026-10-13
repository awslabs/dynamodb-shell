%{
/* Copyright 2026 ddbsh reimplementation contributors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * lexer.l - the engine scanner (reentrant flex, D3).
 *
 * Phase 2b, W1: a full port of the original tokenizer (parser/lexer.l +
 * keywords.cpp), preserving every quirk in arch §4:
 *   - "--" and "//" single-line comments (discarded); no block comments.
 *   - T_TIMESTAMP ("YYYY-MM-DDThh:mm:ssZ") matched BEFORE the general quoted
 *     string (flex breaks the equal-length tie in favor of the earlier rule).
 *   - T_DQUOTED_STRING: double-quoted, with SQL-style "" doubling to embed a
 *     quote (NOT backslash); the verbatim lexeme (quotes included) is kept and
 *     unquote() is applied later in the grammar (W2).
 *   - T_WHOLE_NUMBER: -?[1-9][0-9]* or 0 (leading-zero ints like 01 do NOT
 *     match here; they fall through to the identifier/float rules).
 *   - T_FLOATING_POINT_NUMBER: decimals and exponents with UPPERCASE 'E' only.
 *   - identifier/keyword: [a-zA-Z0-9_][a-zA-Z0-9_-]* — may start with a digit
 *     and contain hyphens (so us-east-1, my-table are single tokens);
 *     case-insensitive keyword lookup, miss => T_UNQUOTED_STRING.
 *   - operators <, >, <=, >=, =, != and <> (both => T_NOT_EQUAL).
 *   - <<EOF>> ends the parse; '.' catch-all returns the raw character so the
 *     grammar sees ( ) , ; . [ ] { } : + - * etc.
 *
 * %option reentrant makes the scanner state a per-call yyscan_t (no globals),
 * so multiple Sessions scan concurrently (P2a-02 §7). Input is pulled from the
 * Driver's borrowed text via a custom YY_INPUT, decoupling the scanner from any
 * FILE pointer or terminal (T1). Tokens are bison symbol_types
 * (api.token.constructor). NOTE: we deliberately do NOT use flex
 * %option case-insensitive — that would also fold the float exponent 'E', which
 * must be uppercase-only (arch §4). Case-insensitive KEYWORD matching is done
 * by lowercasing the lexeme in the keyword table lookup (as the original's
 * strcasecmp did), leaving the numeric/float rules case-sensitive.
 */

#include <cctype>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "parser/driver.hpp"
#include "parser/grammar.hpp"

// Pull input from the Driver's borrowed text (no FILE*, no terminal).
#define YY_INPUT(buf, result, max_size) \
    (result) = ddbsh_yy_read(yyextra, (buf), (max_size))

// Record the verbatim lexeme of every matched rule on the Driver and advance
// its line/column cursor, so the token-stream debug helper (lex_debug.cpp) and
// byte-exact error locations can recover them. Runs before each rule's action;
// the last returning rule's lexeme is the one in effect when yylex() returns.
#define YY_USER_ACTION yyextra->on_token(yytext, yyleng);

// The scanner returns bison symbol_types (api.token.constructor). Override the
// generated lex signature so it yields Parser::symbol_type.
#define YY_DECL ddbsh::engine::Parser::symbol_type ddbsh_yylex(yyscan_t yyscanner)

static int ddbsh_yy_read(ddbsh::engine::Driver* drv, char* buf, int max_size);

using ddbsh::engine::Parser;

// Keyword table: lowercased keyword -> a factory producing the token's
// symbol_type. Ported one-for-one from the original keywords.cpp, preserving
// its aliases and quirks: end=>K_COMMIT, numeric=>K_NUMBER (declared, unused),
// number=>K_ATTRIBUTE_TYPE_NUMBER, string=>K_ATTRIBUTE_TYPE_STRING,
// binary=>K_ATTRIBUTE_TYPE_BINARY, updateD=>K_UPDATED. Lookup lowercases the
// matched text, matching the original's case-insensitive strcasecmp search.
static std::optional<Parser::symbol_type> ddbsh_keyword(const char* text);
%}

%option reentrant
%option noyywrap nounput noinput
%option prefix="ddbsh_yy"
%option extra-type="ddbsh::engine::Driver *"

dquote                 [\"]
not_dquote             [^\"]

%%

"--".*          { /* single-line comment ("--"): discard to end of line */ }
"//".*          { /* single-line comment ("//"): discard to end of line */ }
[ \t\r\n]+      { /* whitespace: discard */ }

{dquote}[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z{dquote}  {
                  /* ISO-8601 timestamp literal; matched before general strings */
                  return Parser::make_T_TIMESTAMP(yytext);
                }

{dquote}({not_dquote}|{dquote}{dquote})*{dquote} {
                  /* double-quoted string; "" doubles to embed a quote (SQL
                   * style, not backslash). Verbatim lexeme kept (quotes incl). */
                  return Parser::make_T_DQUOTED_STRING(yytext);
                }

-?[1-9][0-9]*   { return Parser::make_T_WHOLE_NUMBER(yytext); }
0               { return Parser::make_T_WHOLE_NUMBER(yytext); }

-?[0-9]+"."[0-9]*            |
-?"."[0-9]+                  |
-?[0-9]+E[-+]?[0-9]+         |
-?[0-9]+"."[0-9]*E[-+]?[0-9]+ |
-?"."[0-9]+E[-+]?[0-9]+      {
                  return Parser::make_T_FLOATING_POINT_NUMBER(yytext);
                }

[a-zA-Z0-9_][a-zA-Z0-9_-]* {
                  /* identifier or keyword (may start with a digit, may contain
                   * hyphens: us-east-1, my-table are single tokens) */
                  if (auto kw = ddbsh_keyword(yytext)) {
                      return std::move(*kw);
                  }
                  return Parser::make_T_UNQUOTED_STRING(yytext);
                }

"<="            { return Parser::make_T_LESS_THAN_OR_EQUAL(yytext); }
">="            { return Parser::make_T_GREATER_THAN_OR_EQUAL(yytext); }
"<>"            { return Parser::make_T_NOT_EQUAL(yytext); }
"!="            { return Parser::make_T_NOT_EQUAL(yytext); }
"<"             { return Parser::make_T_LESS_THAN(yytext); }
">"             { return Parser::make_T_GREATER_THAN(yytext); }
"="             { return Parser::make_T_EQUAL(yytext); }
";"             { return Parser::make_SEMICOLON(); }

<<EOF>>         { return Parser::make_END(); }

.               { /* any other char: hand the raw character to the grammar
                   * (this is how ( ) , . [ ] { } : + - * reach the parser) */
                  return Parser::symbol_type(static_cast<int>(yytext[0]));
                }

%%

// Reentrant YY_INPUT backend: copy up to max_size bytes from the Driver's
// borrowed input at its current offset. Returns the number of bytes read (0 at
// end of input).
static int ddbsh_yy_read(ddbsh::engine::Driver* drv, char* buf, int max_size) {
    if (drv == nullptr) {
        return 0;
    }
    std::string_view in = drv->input();
    std::size_t& off = drv->scan_offset();
    if (off >= in.size()) {
        return 0;
    }
    std::size_t n = in.size() - off;
    if (n > static_cast<std::size_t>(max_size)) {
        n = static_cast<std::size_t>(max_size);
    }
    std::memcpy(buf, in.data() + off, n);
    off += n;
    return static_cast<int>(n);
}

static std::optional<Parser::symbol_type> ddbsh_keyword(const char* text) {
    using sym = Parser::symbol_type;
    using factory = sym (*)();
    static const std::unordered_map<std::string, factory> table = {
        {"access", [] { return Parser::make_K_ACCESS(); }},
        {"add", [] { return Parser::make_K_ADD(); }},
        {"aes256", [] { return Parser::make_K_AES256(); }},
        {"all", [] { return Parser::make_K_ALL(); }},
        {"alter", [] { return Parser::make_K_ALTER(); }},
        {"and", [] { return Parser::make_K_AND(); }},
        {"as", [] { return Parser::make_K_AS(); }},
        {"attribute_exists", [] { return Parser::make_K_ATTRIBUTE_EXISTS(); }},
        {"attribute_not_exists", [] { return Parser::make_K_ATTRIBUTE_NOT_EXISTS(); }},
        {"attribute_type", [] { return Parser::make_K_ATTRIBUTE_TYPE(); }},
        {"backup", [] { return Parser::make_K_BACKUP(); }},
        {"backups", [] { return Parser::make_K_BACKUPS(); }},
        {"begin", [] { return Parser::make_K_BEGIN(); }},
        {"begins_with", [] { return Parser::make_K_BEGINS_WITH(); }},
        {"between", [] { return Parser::make_K_BETWEEN(); }},
        {"billing", [] { return Parser::make_K_BILLING(); }},
        {"binary", [] { return Parser::make_K_ATTRIBUTE_TYPE_BINARY(); }},
        {"both", [] { return Parser::make_K_BOTH(); }},
        {"brief", [] { return Parser::make_K_BRIEF(); }},
        {"called", [] { return Parser::make_K_CALLED(); }},
        {"capacity", [] { return Parser::make_K_CAPACITY(); }},
        {"check", [] { return Parser::make_K_CHECK(); }},
        {"class", [] { return Parser::make_K_CLASS(); }},
        {"commit", [] { return Parser::make_K_COMMIT(); }},
        {"connect", [] { return Parser::make_K_CONNECT(); }},
        {"consistent", [] { return Parser::make_K_CONSISTENT(); }},
        {"consumed", [] { return Parser::make_K_CONSUMED(); }},
        {"contains", [] { return Parser::make_K_CONTAINS(); }},
        {"create", [] { return Parser::make_K_CREATE(); }},
        {"delete", [] { return Parser::make_K_DELETE(); }},
        {"deletion", [] { return Parser::make_K_DELETION(); }},
        {"demand", [] { return Parser::make_K_DEMAND(); }},
        {"describe", [] { return Parser::make_K_DESCRIBE(); }},
        {"disabled", [] { return Parser::make_K_DISABLED(); }},
        {"drop", [] { return Parser::make_K_DROP(); }},
        {"enabled", [] { return Parser::make_K_ENABLED(); }},
        {"end", [] { return Parser::make_K_COMMIT(); }},
        {"endpoint", [] { return Parser::make_K_ENDPOINT(); }},
        {"exists", [] { return Parser::make_K_EXISTS(); }},
        {"explain", [] { return Parser::make_K_EXPLAIN(); }},
        {"false", [] { return Parser::make_K_FALSE(); }},
        {"from", [] { return Parser::make_K_FROM(); }},
        {"global", [] { return Parser::make_K_GLOBAL(); }},
        {"gsi", [] { return Parser::make_K_GSI(); }},
        {"hash", [] { return Parser::make_K_HASH(); }},
        {"help", [] { return Parser::make_K_HELP(); }},
        {"if", [] { return Parser::make_K_IF(); }},
        {"if_not_exists", [] { return Parser::make_K_IF_NOT_EXISTS(); }},
        {"image", [] { return Parser::make_K_IMAGE(); }},
        {"images", [] { return Parser::make_K_IMAGES(); }},
        {"in", [] { return Parser::make_K_IN(); }},
        {"include", [] { return Parser::make_K_INCLUDE(); }},
        {"index", [] { return Parser::make_K_INDEX(); }},
        {"indexes", [] { return Parser::make_K_INDEXES(); }},
        {"infrequent", [] { return Parser::make_K_INFREQUENT(); }},
        {"insert", [] { return Parser::make_K_INSERT(); }},
        {"into", [] { return Parser::make_K_INTO(); }},
        {"is", [] { return Parser::make_K_IS(); }},
        {"key", [] { return Parser::make_K_KEY(); }},
        {"keyid", [] { return Parser::make_K_KEYID(); }},
        {"keys", [] { return Parser::make_K_KEYS(); }},
        {"kms", [] { return Parser::make_K_KMS(); }},
        {"like", [] { return Parser::make_K_LIKE(); }},
        {"limit", [] { return Parser::make_K_LIMIT(); }},
        {"limits", [] { return Parser::make_K_LIMITS(); }},
        {"lsi", [] { return Parser::make_K_LSI(); }},
        {"mode", [] { return Parser::make_K_MODE(); }},
        {"new", [] { return Parser::make_K_NEW(); }},
        {"not", [] { return Parser::make_K_NOT(); }},
        {"nowait", [] { return Parser::make_K_NOWAIT(); }},
        {"null", [] { return Parser::make_K_NULL(); }},
        {"number", [] { return Parser::make_K_ATTRIBUTE_TYPE_NUMBER(); }},
        {"numeric", [] { return Parser::make_K_NUMBER(); }},
        {"old", [] { return Parser::make_K_OLD(); }},
        {"on", [] { return Parser::make_K_ON(); }},
        {"only", [] { return Parser::make_K_ONLY(); }},
        {"or", [] { return Parser::make_K_OR(); }},
        {"pitr", [] { return Parser::make_K_PITR(); }},
        {"primary", [] { return Parser::make_K_PRIMARY(); }},
        {"projecting", [] { return Parser::make_K_PROJECTING(); }},
        {"protection", [] { return Parser::make_K_PROTECTION(); }},
        {"provisioned", [] { return Parser::make_K_PROVISIONED(); }},
        {"quit", [] { return Parser::make_K_QUIT(); }},
        {"range", [] { return Parser::make_K_RANGE(); }},
        {"ratelimit", [] { return Parser::make_K_RATELIMIT(); }},
        {"rcu", [] { return Parser::make_K_RCU(); }},
        {"remove", [] { return Parser::make_K_REMOVE(); }},
        {"replace", [] { return Parser::make_K_REPLACE(); }},
        {"replica", [] { return Parser::make_K_REPLICA(); }},
        {"restore", [] { return Parser::make_K_RESTORE(); }},
        {"return", [] { return Parser::make_K_RETURN(); }},
        {"rollback", [] { return Parser::make_K_ROLLBACK(); }},
        {"select", [] { return Parser::make_K_SELECT(); }},
        {"set", [] { return Parser::make_K_SET(); }},
        {"show", [] { return Parser::make_K_SHOW(); }},
        {"size", [] { return Parser::make_K_SIZE(); }},
        {"sse", [] { return Parser::make_K_SSE(); }},
        {"standard", [] { return Parser::make_K_STANDARD(); }},
        {"stream", [] { return Parser::make_K_STREAM(); }},
        {"string", [] { return Parser::make_K_ATTRIBUTE_TYPE_STRING(); }},
        {"table", [] { return Parser::make_K_TABLE(); }},
        {"tables", [] { return Parser::make_K_TABLES(); }},
        {"tags", [] { return Parser::make_K_TAGS(); }},
        {"throughput", [] { return Parser::make_K_THROUGHPUT(); }},
        {"to", [] { return Parser::make_K_TO(); }},
        {"total", [] { return Parser::make_K_TOTAL(); }},
        {"true", [] { return Parser::make_K_TRUE(); }},
        {"ttl", [] { return Parser::make_K_TTL(); }},
        {"unlimited", [] { return Parser::make_K_UNLIMITED(); }},
        {"update", [] { return Parser::make_K_UPDATE(); }},
        {"updated", [] { return Parser::make_K_UPDATED(); }},
        {"upsert", [] { return Parser::make_K_UPSERT(); }},
        {"values", [] { return Parser::make_K_VALUES(); }},
        {"version", [] { return Parser::make_K_VERSION(); }},
        {"warm", [] { return Parser::make_K_WARM(); }},
        {"wcu", [] { return Parser::make_K_WCU(); }},
        {"where", [] { return Parser::make_K_WHERE(); }},
        {"with", [] { return Parser::make_K_WITH(); }},
    };

    std::string lower(text);
    for (char& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    auto it = table.find(lower);
    if (it == table.end()) {
        return std::nullopt;
    }
    return it->second();
}

// The bison %code block calls ddbsh_lex(drv); bridge it to the reentrant flex
// entry point (ddbsh_yylex(yyscan_t), generated by %option prefix). The Driver
// is wired as yyextra at scanner-init time (see driver.cpp) so YY_INPUT and
// YY_USER_ACTION can reach it.
ddbsh::engine::Parser::symbol_type ddbsh_lex(ddbsh::engine::Driver& drv) {
    return ddbsh_yylex(static_cast<yyscan_t>(drv.scanner()));
}
