// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// help_test.cpp — HELP parsing + rendering unit tests (no backend, D-B).
//
// HELP contacts no API (arch §7): the original printed a fixed usage string
// from the parser; the reimplementation parses to a HelpCmd and emits the same
// bytes from execute() as a ReportOutcome. These tests pin:
//   * every HELP form parses to StatementKind::help with the right HelpTopic,
//   * the general HELP output is byte-exact (the full listing), and
//   * a single-line topic (HELP COMMIT) and a blank-line-bearing topic
//     (HELP SHOW VERSION) round-trip verbatim — covering the help_lines split
//     at both the no-trailing-blank and embedded-blank boundaries.
// The remaining 28 forms are proven byte-for-byte against the `ddbsh-old`
// oracle by the golden/parity path; here we assert the engine-side contract.

#include <string>

#include <gtest/gtest.h>

#include "ddbsh/renderer.hpp"
#include "ddbsh/session.hpp"
#include "ddbsh/statement.hpp"

#include "fake_ddb_client.hpp"
#include "parser/ast.hpp"

namespace {

using ddbsh::StatementKind;
using ddbsh::engine::HelpCmd;
using ddbsh::engine::HelpTopic;

ddbsh::Session make_session(ddbsh::test::FakeDDBClient& client) {
    return ddbsh::Session(ddbsh::SessionConfig{"us-east-1", ""}, client);
}

HelpTopic topic_of(const ddbsh::Statement& stmt) {
    return std::get<HelpCmd>(stmt.ast()->command).topic;
}

TEST(Help, ParsesToHelpKindWithTopic) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    struct Case {
        const char* text;
        HelpTopic topic;
    };
    const Case cases[] = {
        {"help;", HelpTopic::general},
        {"help explain;", HelpTopic::explain},
        {"help begin;", HelpTopic::begin},
        {"help commit;", HelpTopic::commit},
        {"help rollback;", HelpTopic::rollback},
        {"help connect;", HelpTopic::connect},
        {"help drop;", HelpTopic::drop},
        {"help drop table;", HelpTopic::drop_table},
        {"help drop backup;", HelpTopic::drop_backup},
        {"help describe;", HelpTopic::describe},
        {"help describe backup;", HelpTopic::describe_backup},
        {"help show;", HelpTopic::show},
        {"help show tables;", HelpTopic::show_tables},
        {"help show limits;", HelpTopic::show_limits},
        {"help show backups;", HelpTopic::show_backups},
        {"help show version;", HelpTopic::show_version},
        {"help show create table;", HelpTopic::show_create_table},
        {"help create;", HelpTopic::create},
        {"help create table;", HelpTopic::create_table},
        {"help alter table;", HelpTopic::alter_table},
        {"help backup;", HelpTopic::backup},
        {"help restore;", HelpTopic::restore},
        {"help select;", HelpTopic::select_statement},
        {"help insert;", HelpTopic::insert},
        {"help replace;", HelpTopic::replace},
        {"help update;", HelpTopic::update},
        {"help upsert;", HelpTopic::upsert},
        {"help delete;", HelpTopic::del},
    };
    for (const auto& c : cases) {
        auto result = session.parse(c.text);
        ASSERT_TRUE(result.has_value()) << "parse failed: " << c.text;
        EXPECT_EQ(result->kind(), StatementKind::help) << c.text;
        EXPECT_EQ(topic_of(*result), c.topic) << c.text;
        EXPECT_FALSE(result->explain()) << c.text;
    }
}

TEST(Help, IsCaseInsensitive) {
    // arch §4: keywords are matched case-insensitively.
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto result = session.parse("HELP SHOW CREATE TABLE;");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->kind(), StatementKind::help);
    EXPECT_EQ(topic_of(*result), HelpTopic::show_create_table);
}

TEST(Help, IsNotExplainable) {
    // HELP is a plain command, not in the grammar's `explainable` set; EXPLAIN
    // HELP is a syntax error (matches the original).
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto result = session.parse("explain help;");
    EXPECT_FALSE(result.has_value());
}

TEST(Help, GeneralOutputIsByteExact) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    ddbsh::render::Renderer renderer;

    auto outcome = session.run("help;");
    ASSERT_TRUE(outcome.has_value()) << outcome.error().message;
    EXPECT_EQ(renderer.render(*outcome),
              "HELP - provide help in ddbsh\n\n"
              "   HELP <keyword> [keyword [keyword ...]]\n\n"
              "      Provides help about the specified keyword, or statement.\n\n"
              "      HELP ALTER TABLE\n"
              "      HELP BACKUP\n"
              "      HELP BEGIN\n"
              "      HELP COMMIT\n"
              "      HELP CONNECT\n"
              "      HELP CREATE\n"
              "      HELP DELETE\n"
              "      HELP DESCRIBE BACKUP\n"
              "      HELP DESCRIBE\n"
              "      HELP DROP BACKUP\n"
              "      HELP DROP TABLE\n"
              "      HELP DROP\n"
              "      HELP EXPLAIN\n"
              "      HELP INSERT\n"
              "      HELP REPLACE\n"
              "      HELP RESTORE\n"
              "      HELP ROLLBACK\n"
              "      HELP SELECT\n"
              "      HELP SHOW BACKUPS\n"
              "      HELP SHOW CREATE TABLE\n"
              "      HELP SHOW LIMITS\n"
              "      HELP SHOW VERSION\n"
              "      HELP SHOW\n"
              "      HELP UPDATE\n"
              "      HELP UPSERT\n\n");
}

TEST(Help, SingleLineTopicRoundTrips) {
    // HELP COMMIT is a single '\n'-terminated line (no trailing blank): the
    // help_lines split must not emit a spurious empty line.
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    ddbsh::render::Renderer renderer;

    auto outcome = session.run("help commit;");
    ASSERT_TRUE(outcome.has_value()) << outcome.error().message;
    EXPECT_EQ(renderer.render(*outcome), "COMMIT - See BEGIN\n");
}

TEST(Help, EmbeddedBlankLinesRoundTrip) {
    // HELP SHOW VERSION carries embedded blank lines; the split/join must
    // preserve them exactly.
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    ddbsh::render::Renderer renderer;

    auto outcome = session.run("help show version;");
    ASSERT_TRUE(outcome.has_value()) << outcome.error().message;
    EXPECT_EQ(renderer.render(*outcome),
              "SHOW VERSION\n\n   Show the version of DynamoDB Shell.\n\n");
}

}  // namespace
