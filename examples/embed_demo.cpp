// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// embed_demo.cpp — a standalone program that embeds the ddbsh engine library.
//
// This is a WORKED EXAMPLE of the library/CLI boundary (tenet 3): an external
// consumer that depends ONLY on the installed public headers (`ddbsh/*.hpp`)
// and `libddbsh`. It is built by the plain Makefile next to it — NOT the
// project CMake — to prove the library is consumable on its own.
//
// What it demonstrates, end to end:
//   1. Implement the injected client seam `ddbsh::IDDBClient` (here, a tiny
//      in-memory fake so the demo runs OFFLINE with no AWS account and no
//      cost).
//   2. Construct a `ddbsh::Session` with a `ddbsh::SessionConfig` and the
//      borrowed client (no globals, no singleton — T1/D3).
//   3. Parse + execute SQL-ish statements through `Session::run()`.
//   4. Render the structured `ExecOutcome` (or `Error`) into the exact ddbsh
//      output bytes with `ddbsh::render::Renderer`.
//
// The engine itself contacts no backend: the SQL->API planning (GetItem vs
// Query vs Scan) and all formatting happen in the library; this program's
// IDDBClient supplies the "service" responses.
//
// OFFLINE BY DESIGN. A real embedder would implement IDDBClient against the
// AWS SDK for C++ exactly as the shipping CLI does in `cli/aws_ddb_client.cpp`
// (translating the library's SDK-free request/response structs to/from
// `Aws::DynamoDB::Model::*`). That lives in the CLI, behind the same seam, so
// the engine stays SDK-free and terminal-free. We ship the offline fake here
// so the sample builds and runs with zero credentials and zero spend.

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ddbsh/iddbclient.hpp"
#include "ddbsh/renderer.hpp"
#include "ddbsh/session.hpp"

namespace {

// True when two scalar attribute values are equal for the GetItem key match
// below. The demo only ever compares key attributes (S / N), so this handles
// those two cases; a full comparison is a service concern, not an embedder's.
bool scalar_equal(const ddbsh::Value& a, const ddbsh::Value& b) {
    return a.type == b.type && a.s == b.s;
}

// A minimal in-memory IDDBClient for the demo. It knows ONE table ("Movies")
// and a handful of seeded rows. It is intentionally not a DynamoDB engine:
//   * describe_table serves the registered key schema so the library's planner
//     can decide GetItem/Query/Scan offline;
//   * get_item matches the literal key the planner supplies;
//   * query/scan return every seeded row (DynamoDB does the real server-side
//     key-condition/filter work; this fake just hands back the canned rows so
//     the demo shows the render path).
// Everything the demo does not exercise returns a clear "not implemented"
// Error rather than pretending — the library never exits/throws across the
// seam (D12).
class InMemoryClient : public ddbsh::IDDBClient {
public:
    InMemoryClient() {
        // One table: Movies, keyed (year HASH, title RANGE).
        schema_.table_name = "Movies";
        schema_.key_schema = ddbsh::TableKeySchema{"year", "title"};
        schema_.table_status = "ACTIVE";
        schema_.table_id = "00000000-0000-0000-0000-000000000000";
        schema_.table_arn = "arn:aws:dynamodb:us-east-1:000000000000:table/Movies";
        schema_.billing_mode_summary = "PAY_PER_REQUEST";
        schema_.table_class_summary = "STANDARD";

        items_.push_back(make_movie("2010", "Inception", "8.8"));
        items_.push_back(make_movie("2014", "Interstellar", "8.6"));
    }

    ddbsh::Result<ddbsh::GetItemOut> get_item(const ddbsh::GetItemIn& in) override {
        ddbsh::GetItemOut out;
        for (const auto& row : items_) {
            if (key_matches(row, in.key)) {
                out.item = row;
                break;
            }
        }
        return out;
    }

    ddbsh::Result<ddbsh::QueryOut> query(const ddbsh::QueryIn& in) override {
        ddbsh::QueryOut out;
        // Demo: the backend would apply the key condition itself; here we hand
        // back every seeded row, honoring only the ProjectionExpression the
        // engine built (so a `SELECT a, b` narrows the columns visibly).
        out.items = project(items_, in.projection_expression, in.expression_attribute_names);
        return out;
    }

    ddbsh::Result<ddbsh::ScanOut> scan(const ddbsh::ScanIn& in) override {
        ddbsh::ScanOut out;
        out.items = project(items_, in.projection_expression, in.expression_attribute_names);
        return out;
    }

    ddbsh::Result<ddbsh::ListTablesOut> list_tables(const ddbsh::ListTablesIn&) override {
        ddbsh::ListTablesOut out;
        out.table_names.push_back(schema_.table_name);
        return out;
    }

    ddbsh::Result<ddbsh::DescribeTableOut> describe_table(
        const ddbsh::DescribeTableIn& in) override {
        if (in.table_name != schema_.table_name) {
            return ddbsh::fail(ddbsh::Errc::no_such_table,
                               "Table: " + in.table_name + " not found");
        }
        return schema_;
    }

    ddbsh::Result<ddbsh::TimeToLiveOut> describe_time_to_live(
        const ddbsh::DescribeTimeToLiveIn&) override {
        return ddbsh::TimeToLiveOut{"DISABLED", ""};
    }

    // --- Mutating ops (2c): a trivially simple vector-backed implementation --
    // These let the demo exercise a WRITE end to end (INSERT / UPDATE / DELETE),
    // proving goal 3 for a mutating command offline. A real embedder translates
    // these to Aws::DynamoDB::Model::{PutItem,UpdateItem,DeleteItem} (see
    // cli/aws_ddb_client.cpp); here the fake keeps an in-memory table.
    ddbsh::Result<ddbsh::PutItemOut> put_item(const ddbsh::PutItemIn& in) override {
        // Derive the key from the item by the table's schema; INSERT carries a
        // `attribute_not_exists(...)` condition that must fail on an existing
        // key (the ccf path), REPLACE (no condition) is unconditional.
        ddbsh::ValueMap key = key_of_item(in.item);
        const std::optional<std::size_t> existing = find_by_key(key);
        if (in.condition_expression.has_value() && existing.has_value()) {
            return conditional_failed<ddbsh::PutItemOut>();
        }
        if (existing.has_value()) {
            items_[*existing] = ddbsh::ItemRow{in.item};
        } else {
            items_.push_back(ddbsh::ItemRow{in.item});
        }
        return ddbsh::PutItemOut{};
    }

    ddbsh::Result<ddbsh::UpdateItemOut> update_item(const ddbsh::UpdateItemIn& in) override {
        const std::optional<std::size_t> existing = find_by_key(in.key);
        // UPDATE carries attribute_exists(pk); a missing row fails the
        // condition (ccf). A matched row is "modified" (the demo does not parse
        // the UpdateExpression — that is the service's job).
        if (in.condition_expression.has_value() && !existing.has_value()) {
            return conditional_failed<ddbsh::UpdateItemOut>();
        }
        return ddbsh::UpdateItemOut{};
    }

    ddbsh::Result<ddbsh::DeleteItemOut> delete_item(const ddbsh::DeleteItemIn& in) override {
        const std::optional<std::size_t> existing = find_by_key(in.key);
        if (in.condition_expression.has_value() && !existing.has_value()) {
            return conditional_failed<ddbsh::DeleteItemOut>();
        }
        if (existing.has_value()) {
            items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(*existing));
        }
        return ddbsh::DeleteItemOut{};
    }

    // --- Ops this offline demo does not exercise -----------------------------
    ddbsh::Result<ddbsh::CreateTableOut> create_table(const ddbsh::CreateTableIn&) override {
        return not_impl<ddbsh::CreateTableOut>("create_table");
    }
    ddbsh::Result<ddbsh::DeleteTableOut> delete_table(const ddbsh::DeleteTableIn&) override {
        return not_impl<ddbsh::DeleteTableOut>("delete_table");
    }
    ddbsh::Result<ddbsh::ContinuousBackupsOut> describe_continuous_backups(
        const ddbsh::DescribeContinuousBackupsIn&) override {
        return not_impl<ddbsh::ContinuousBackupsOut>("describe_continuous_backups");
    }
    ddbsh::Result<ddbsh::TableReplicaAutoScalingOut> describe_table_replica_auto_scaling(
        const ddbsh::DescribeTableReplicaAutoScalingIn&) override {
        return not_impl<ddbsh::TableReplicaAutoScalingOut>("describe_table_replica_auto_scaling");
    }
    ddbsh::Result<void> wait_for_table_active(const std::string&) override { return {}; }
    ddbsh::Result<void> wait_for_table_gone(const std::string&) override { return {}; }
    ddbsh::Result<std::string> explain_request(const ddbsh::ExplainRequest&) override {
        return not_impl<std::string>("explain_request");
    }

private:
    template <class T>
    static ddbsh::Result<T> not_impl(std::string_view op) {
        return ddbsh::fail(ddbsh::Errc::not_implemented,
                           "offline demo client does not implement " + std::string(op));
    }

    // The service's own ConditionalCheckFailed message (arch §9); tagged with
    // Errc::conditional_check_failed so the UPDATE/DELETE executors count it as
    // a ccf rather than aborting (D-2c-7).
    template <class T>
    static ddbsh::Result<T> conditional_failed() {
        return ddbsh::fail(ddbsh::Errc::conditional_check_failed, "The conditional request failed");
    }

    // Extract the table's key attributes (year, title) from a full item.
    ddbsh::ValueMap key_of_item(const ddbsh::ValueMap& item) const {
        ddbsh::ValueMap key;
        for (const auto& want : {schema_.key_schema.hash_key, schema_.key_schema.range_key}) {
            if (want.empty()) {
                continue;
            }
            for (const auto& [name, val] : item) {
                if (name == want) {
                    key.emplace_back(name, val);
                    break;
                }
            }
        }
        return key;
    }

    // Index of the row matching every attribute of `key`, if any.
    std::optional<std::size_t> find_by_key(const ddbsh::ValueMap& key) const {
        for (std::size_t i = 0; i < items_.size(); ++i) {
            if (key_matches(items_[i], key)) {
                return i;
            }
        }
        return std::nullopt;
    }

    static ddbsh::ItemRow make_movie(std::string year, std::string title, std::string rating) {
        ddbsh::ItemRow row;
        row.attributes.emplace_back("year", ddbsh::Value::number_value(std::move(year)));
        row.attributes.emplace_back("title", ddbsh::Value::string_value(std::move(title)));
        row.attributes.emplace_back("rating", ddbsh::Value::number_value(std::move(rating)));
        return row;
    }

    // Apply the engine-built ProjectionExpression to each row: keep only the
    // attributes the SELECT asked for. The projection is a comma-separated list
    // of alias tokens (e.g. "#a1, #a2"); `names` maps each token back to its
    // real attribute name (arch §6). An empty projection means SELECT * — keep
    // everything.
    static std::vector<ddbsh::ItemRow> project(const std::vector<ddbsh::ItemRow>& rows,
                                               const std::optional<std::string>& projection,
                                               const ddbsh::ExpressionAttributeNames& names) {
        if (!projection.has_value() || projection->empty()) {
            return rows;
        }
        std::vector<std::string> wanted;
        for (std::string_view tok : split_csv(*projection)) {
            for (const auto& [alias, real] : names) {
                if (alias == tok) {
                    wanted.push_back(real);
                    break;
                }
            }
        }
        std::vector<ddbsh::ItemRow> out;
        out.reserve(rows.size());
        for (const auto& row : rows) {
            ddbsh::ItemRow kept;
            for (const auto& [name, val] : row.attributes) {
                for (const auto& w : wanted) {
                    if (w == name) {
                        kept.attributes.emplace_back(name, val);
                        break;
                    }
                }
            }
            out.push_back(std::move(kept));
        }
        return out;
    }

    // Split "a, b ,c" into trimmed non-empty tokens.
    static std::vector<std::string> split_csv(std::string_view s) {
        std::vector<std::string> out;
        std::size_t start = 0;
        while (start <= s.size()) {
            std::size_t comma = s.find(',', start);
            std::string_view piece =
                s.substr(start, comma == std::string_view::npos ? s.size() - start : comma - start);
            std::size_t b = piece.find_first_not_of(" \t");
            if (b != std::string_view::npos) {
                std::size_t e = piece.find_last_not_of(" \t");
                out.emplace_back(piece.substr(b, e - b + 1));
            }
            if (comma == std::string_view::npos) {
                break;
            }
            start = comma + 1;
        }
        return out;
    }

    static bool key_matches(const ddbsh::ItemRow& row, const ddbsh::ValueMap& key) {
        for (const auto& [name, want] : key) {
            bool found = false;
            for (const auto& [have_name, have_val] : row.attributes) {
                if (have_name == name) {
                    found = scalar_equal(have_val, want);
                    break;
                }
            }
            if (!found) {
                return false;
            }
        }
        return true;
    }

    ddbsh::DescribeTableOut schema_;
    std::vector<ddbsh::ItemRow> items_;
};

// Parse+execute one statement and print the rendered bytes, labelling each so
// the demo output is self-explanatory. stdout gets success bytes, stderr gets
// error bytes — exactly as the CLI wrapper routes them.
void demo(ddbsh::Session& session, const ddbsh::render::Renderer& renderer, std::string_view sql) {
    std::printf("ddbsh> %.*s\n", static_cast<int>(sql.size()), sql.data());
    ddbsh::Result<ddbsh::ExecOutcome> outcome = session.run(sql);
    if (!outcome.has_value()) {
        std::fputs(renderer.render_error(outcome.error()).c_str(), stderr);
        std::fflush(stderr);
        return;
    }
    std::fputs(renderer.render(*outcome).c_str(), stdout);
    std::fflush(stdout);
}

}  // namespace

int main() {
    InMemoryClient client;

    ddbsh::SessionConfig cfg;
    cfg.region = "us-east-1";  // for the prompt / client wiring; no call is made
    ddbsh::Session session(cfg, client);

    ddbsh::render::Renderer renderer;

    std::puts("=== ddbsh library embedding demo (offline, in-memory client) ===\n");

    // SHOW TABLES: ListTables + a DescribeTable/DescribeTimeToLive per table.
    demo(session, renderer, "SHOW TABLES;");
    std::puts("");

    // Scan: no WHERE -> the planner chooses Scan.
    demo(session, renderer, "SELECT * FROM Movies;");
    std::puts("");

    // Query: a hash-key equality (and nothing else) -> the planner chooses
    // Query.
    demo(session, renderer, "SELECT * FROM Movies WHERE year = 2014;");
    std::puts("");

    // GetItem: the full primary key (hash + range) -> the planner chooses
    // GetItem, and the in-memory client matches the literal key.
    demo(session, renderer, "SELECT * FROM Movies WHERE year = 2010 AND title = \"Inception\";");
    std::puts("");

    // A projection: only selected attributes come back.
    demo(session, renderer, "SELECT title, rating FROM Movies;");
    std::puts("");

    // --- Mutating commands (2c): INSERT / UPDATE / DELETE, offline ----------
    // INSERT a new movie (conditional attribute_not_exists on the key).
    demo(session, renderer,
         "INSERT INTO Movies ( year, title, rating ) VALUES ( 2019, \"Parasite\", 8.6 );");
    std::puts("");

    // UPDATE by full key -> one conditional UpdateItem (0 read, 1 modified).
    demo(session, renderer,
         "UPDATE Movies SET rating = 9.0 WHERE year = 2014 AND title = \"Interstellar\";");
    std::puts("");

    // DELETE by full key -> one conditional DeleteItem (0 read, 1 modified).
    demo(session, renderer, "DELETE FROM Movies WHERE year = 2010 AND title = \"Inception\";");
    std::puts("");

    // The table now reflects the writes (Parasite added, Inception gone).
    demo(session, renderer, "SELECT title FROM Movies;");

    return 0;
}
