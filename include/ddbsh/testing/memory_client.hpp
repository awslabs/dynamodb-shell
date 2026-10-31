// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// memory_client.hpp — a genuine in-memory IDDBClient (D30).
//
// A public, SDK-free, documented in-memory DynamoDB backend an embedder can
// drive a ddbsh::Connection against with NO AWS account, NO credentials, NO
// network, and zero cost. Unlike the engine-test double tests/fake_ddb_client.hpp
// (which returns CANNED responses to assert request shapes and drive
// pagination), MemoryClient actually STORES items: CREATE TABLE registers a key
// schema, INSERT/REPLACE/UPDATE/DELETE mutate a real store keyed by primary
// key, and GetItem/Query/Scan read it back. This makes a round-trip demo
// (CREATE -> INSERT -> SELECT -> DROP) behave end-to-end offline, and lets an
// embedder unit-test their own ddbsh usage the same way (a selling point of the
// library-first design).
//
// Scope and honest limitations (documented, not hidden):
//   * Primary-key addressing is faithful: a get_item by full key, a query by a
//     partition-key equality (plus an optional sort-key predicate: =, <, <=, >,
//     >=, begins_with, BETWEEN), and a scan of the whole table all work.
//   * FilterExpressions are NOT evaluated (a scan/query returns the key-matched
//     items without applying a filter). The engine issues the filter to the
//     service; MemoryClient does not interpret it. This is sufficient for the
//     GetItem/Query/Scan access patterns a demo exercises.
//   * UpdateItem applies simple `SET #a = :v` assignments and `REMOVE #a`
//     clauses; arithmetic (`#a = #a + :v`) and if_not_exists are not
//     interpreted.
//   * Index (`.index`) queries filter by the same predicates over the whole
//     table (the index key attributes are ordinary item attributes here).
//   * Conditions honored: attribute_not_exists(pk) (INSERT) and
//     attribute_exists(pk) (UPDATE/DELETE guards) by primary-key presence; a
//     mismatch returns Errc::conditional_check_failed, which the engine reports
//     as the original's "The conditional request failed".
//   * Control-plane reports (DESCRIBE detail, SHOW LIMITS, backups, PITR, TTL,
//     replicas, transactions, vector search, EXPLAIN serialization) are NOT
//     modeled; those methods return empty/not-implemented results. MemoryClient
//     targets the data-plane round-trip, not the full control plane.
//
// It is SDK-free (fills the library's own structs) and header-only, so it can
// be included by an example or a test without a separate build step; it is part
// of the engine library's public, installed surface under include/ddbsh/testing/.

#ifndef DDBSH_TESTING_MEMORY_CLIENT_HPP
#define DDBSH_TESTING_MEMORY_CLIENT_HPP

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ddbsh/iddbclient.hpp"

namespace ddbsh::testing {

/// A genuine in-memory DynamoDB backend. Construct one, open a
/// ddbsh::Connection over it, and run statements; data persists in this object
/// for the lifetime of the client. Not thread-safe (one client per thread, as
/// the engine is one session per thread).
class MemoryClient : public ddbsh::IDDBClient {
public:
    MemoryClient() = default;

    // ---- data plane --------------------------------------------------------

    Result<GetItemOut> get_item(const GetItemIn& in) override {
        GetItemOut out;
        const Table* t = find_table(in.table_name);
        if (t == nullptr) {
            return fail(Errc::no_such_table, "Table: " + in.table_name + " not found");
        }
        const ItemRow* item = t->find(key_string(in.key));
        if (item != nullptr) {
            out.item = *item;
        }
        return out;
    }

    Result<QueryOut> query(const QueryIn& in) override {
        QueryOut out;
        const Table* t = find_table(in.table_name);
        if (t == nullptr) {
            return fail(Errc::no_such_table, "Table: " + in.table_name + " not found");
        }
        auto preds = parse_key_condition(in.key_condition_expression, in.expression_attribute_names,
                                         in.expression_attribute_values);
        for (const auto& item : t->items) {
            if (matches_all(item, preds)) {
                out.items.push_back(item);
            }
        }
        return out;
    }

    Result<ScanOut> scan(const ScanIn& in) override {
        ScanOut out;
        const Table* t = find_table(in.table_name);
        if (t == nullptr) {
            return fail(Errc::no_such_table, "Table: " + in.table_name + " not found");
        }
        out.items = t->items;  // whole table (no filter evaluation — see header)
        return out;
    }

    Result<PutItemOut> put_item(const PutItemIn& in) override {
        PutItemOut out;
        Table* t = find_table(in.table_name);
        if (t == nullptr) {
            return fail(Errc::no_such_table, "Table: " + in.table_name + " not found");
        }
        ItemRow item;
        item.attributes = in.item;
        std::string k = key_string(extract_key(item, t->schema));
        const bool exists = t->find(k) != nullptr;
        if (auto e = check_condition(in.condition_expression, exists)) {
            return fail(*e);
        }
        if (in.return_values == ReturnValue::all_old) {
            if (const ItemRow* old = t->find(k)) {
                out.attributes = *old;
            }
        }
        t->put(k, std::move(item));
        return out;
    }

    Result<UpdateItemOut> update_item(const UpdateItemIn& in) override {
        UpdateItemOut out;
        Table* t = find_table(in.table_name);
        if (t == nullptr) {
            return fail(Errc::no_such_table, "Table: " + in.table_name + " not found");
        }
        std::string k = key_string(in.key);
        const bool exists = t->find(k) != nullptr;
        if (auto e = check_condition(in.condition_expression, exists)) {
            return fail(*e);
        }
        ItemRow item;
        if (const ItemRow* cur = t->find(k)) {
            item = *cur;
        } else {
            item.attributes = in.key;  // UPSERT create: seed with the key
        }
        apply_update(item, in.update_expression, in.expression_attribute_names,
                     in.expression_attribute_values);
        t->put(k, item);
        if (in.return_values == ReturnValue::all_new || in.return_values == ReturnValue::updated_new) {
            out.attributes = item;
        }
        return out;
    }

    Result<DeleteItemOut> delete_item(const DeleteItemIn& in) override {
        DeleteItemOut out;
        Table* t = find_table(in.table_name);
        if (t == nullptr) {
            return fail(Errc::no_such_table, "Table: " + in.table_name + " not found");
        }
        std::string k = key_string(in.key);
        const bool exists = t->find(k) != nullptr;
        if (auto e = check_condition(in.condition_expression, exists)) {
            return fail(*e);
        }
        if (in.return_values == ReturnValue::all_old) {
            if (const ItemRow* old = t->find(k)) {
                out.attributes = *old;
            }
        }
        t->erase(k);
        return out;
    }

    // ---- control plane (table lifecycle) -----------------------------------

    Result<CreateTableOut> create_table(const CreateTableIn& in) override {
        if (find_table(in.table_name) != nullptr) {
            return fail(Errc::table_already_exists, "Table already exists: " + in.table_name);
        }
        Table t;
        t.name = in.table_name;
        t.schema.hash_keys = in.key_schema.hash;
        t.schema.range_keys = in.key_schema.range;
        tables_.emplace(in.table_name, std::move(t));
        return CreateTableOut{};
    }

    Result<DeleteTableOut> delete_table(const DeleteTableIn& in) override {
        if (tables_.erase(in.table_name) == 0) {
            return fail(Errc::no_such_table, "Table: " + in.table_name + " not found");
        }
        return DeleteTableOut{};
    }

    Result<ListTablesOut> list_tables(const ListTablesIn&) override {
        ListTablesOut out;
        for (const auto& [name, t] : tables_) {
            out.table_names.push_back(name);
        }
        return out;
    }

    Result<DescribeTableOut> describe_table(const DescribeTableIn& in) override {
        const Table* t = find_table(in.table_name);
        if (t == nullptr) {
            return fail(Errc::no_such_table, "Table: " + in.table_name + " not found");
        }
        DescribeTableOut out;
        out.table_name = t->name;
        out.key_schema = t->schema;
        return out;
    }

    Result<void> wait_for_table_active(const std::string&) override { return {}; }
    Result<void> wait_for_table_gone(const std::string&) override { return {}; }

    // ---- not modeled (control-plane reports / transactions / vectors) ------
    // MemoryClient targets the data-plane round-trip (see the header note).
    // These return empty success or a clear not-implemented Error so a misuse
    // surfaces plainly rather than silently misbehaving.

    Result<TimeToLiveOut> describe_time_to_live(const DescribeTimeToLiveIn&) override {
        return TimeToLiveOut{"DISABLED", ""};
    }
    Result<ContinuousBackupsOut> describe_continuous_backups(
        const DescribeContinuousBackupsIn&) override {
        return ContinuousBackupsOut{"DISABLED", "", ""};
    }
    Result<TableReplicaAutoScalingOut> describe_table_replica_auto_scaling(
        const DescribeTableReplicaAutoScalingIn&) override {
        return TableReplicaAutoScalingOut{};
    }
    Result<UpdateTableOut> update_table(const UpdateTableIn&) override { return UpdateTableOut{}; }
    Result<UpdateTimeToLiveOut> update_time_to_live(const UpdateTimeToLiveIn&) override {
        return UpdateTimeToLiveOut{};
    }
    Result<UpdateContinuousBackupsOut> update_continuous_backups(
        const UpdateContinuousBackupsIn&) override {
        return UpdateContinuousBackupsOut{};
    }
    Result<CreateBackupOut> create_backup(const CreateBackupIn&) override {
        return not_impl<CreateBackupOut>("CreateBackup");
    }
    Result<DeleteBackupOut> delete_backup(const DeleteBackupIn&) override {
        return not_impl<DeleteBackupOut>("DeleteBackup");
    }
    Result<DescribeBackupOut> describe_backup(const DescribeBackupIn&) override {
        return not_impl<DescribeBackupOut>("DescribeBackup");
    }
    Result<RestoreFromBackupOut> restore_table_from_backup(const RestoreFromBackupIn&) override {
        return not_impl<RestoreFromBackupOut>("RestoreTableFromBackup");
    }
    Result<RestoreToPitrOut> restore_table_to_point_in_time(const RestoreToPitrIn&) override {
        return not_impl<RestoreToPitrOut>("RestoreTableToPointInTime");
    }
    Result<DescribeLimitsOut> describe_limits(const DescribeLimitsIn&) override {
        return DescribeLimitsOut{};
    }
    Result<ListBackupsOut> list_backups(const ListBackupsIn&) override { return ListBackupsOut{}; }
    Result<TransactGetItemsOut> transact_get_items(const TransactGetItemsIn&) override {
        return not_impl<TransactGetItemsOut>("TransactGetItems");
    }
    Result<TransactWriteItemsOut> transact_write_items(const TransactWriteItemsIn&) override {
        return not_impl<TransactWriteItemsOut>("TransactWriteItems");
    }
    Result<std::string> explain_request(const ExplainRequest&) override {
        return not_impl<std::string>("explain_request");
    }

private:
    struct Table {
        std::string name;
        TableKeySchema schema;
        std::vector<ItemRow> items;

        const ItemRow* find(const std::string& key) const {
            for (std::size_t i = 0; i < keys.size(); ++i) {
                if (keys[i] == key) {
                    return &items[i];
                }
            }
            return nullptr;
        }
        void put(const std::string& key, ItemRow item) {
            for (std::size_t i = 0; i < keys.size(); ++i) {
                if (keys[i] == key) {
                    items[i] = std::move(item);
                    return;
                }
            }
            keys.push_back(key);
            items.push_back(std::move(item));
        }
        void erase(const std::string& key) {
            for (std::size_t i = 0; i < keys.size(); ++i) {
                if (keys[i] == key) {
                    keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(i));
                    items.erase(items.begin() + static_cast<std::ptrdiff_t>(i));
                    return;
                }
            }
        }
        std::vector<std::string> keys;  // parallel to items
    };

    struct Predicate {
        std::string attr;  // real attribute name
        std::string op;    // "=", "<", "<=", ">", ">=", "begins_with", "between"
        Value v1;
        Value v2;  // for "between"
    };

    Table* find_table(const std::string& name) {
        auto it = tables_.find(name);
        return it == tables_.end() ? nullptr : &it->second;
    }
    const Table* find_table(const std::string& name) const {
        auto it = tables_.find(name);
        return it == tables_.end() ? nullptr : &it->second;
    }

    template <class T>
    Result<T> not_impl(const char* op) {
        return fail(Errc::not_implemented,
                    std::string("MemoryClient does not implement ") + op);
    }

    static std::string value_key(const Value& v) {
        // A collision-free-enough key segment: type tag + payload.
        switch (v.type) {
            case Value::Type::S:
                return "S:" + v.s;
            case Value::Type::N:
                return "N:" + v.s;
            case Value::Type::B:
                return "B:" + v.s;
            case Value::Type::BOOL:
                return v.boolean ? "BOOL:1" : "BOOL:0";
            case Value::Type::NUL:
                return "NUL:";
            default:
                return "?:";
        }
    }

    static ValueMap extract_key(const ItemRow& item, const TableKeySchema& schema) {
        ValueMap key;
        auto add = [&](const std::string& attr) {
            for (const auto& [name, value] : item.attributes) {
                if (name == attr) {
                    key.emplace_back(name, value);
                    return;
                }
            }
        };
        for (const auto& h : schema.hash_keys) {
            add(h);
        }
        for (const auto& r : schema.range_keys) {
            add(r);
        }
        return key;
    }

    static std::string key_string(const ValueMap& key) {
        std::string s;
        for (const auto& [name, value] : key) {
            s += name;
            s += '=';
            s += value_key(value);
            s += '\x1f';
        }
        return s;
    }

    static std::optional<Error> check_condition(const std::optional<std::string>& cond,
                                                bool key_exists) {
        if (!cond.has_value()) {
            return std::nullopt;
        }
        const std::string& c = *cond;
        const bool wants_absent = c.find("attribute_not_exists") != std::string::npos;
        const bool wants_present =
            !wants_absent && c.find("attribute_exists") != std::string::npos;
        if (wants_absent && key_exists) {
            return Error{Errc::conditional_check_failed, "The conditional request failed",
                         std::nullopt, std::nullopt};
        }
        if (wants_present && !key_exists) {
            return Error{Errc::conditional_check_failed, "The conditional request failed",
                         std::nullopt, std::nullopt};
        }
        return std::nullopt;
    }

    static std::string resolve_name(const ExpressionAttributeNames& names, const std::string& tok) {
        for (const auto& [token, real] : names) {
            if (token == tok) {
                return real;
            }
        }
        // Dotted/subscripted tokens are not modeled; return the token minus '#'.
        return tok.empty() || tok[0] != '#' ? tok : tok.substr(1);
    }

    static Value resolve_value(const ValueMap& values, const std::string& alias) {
        for (const auto& [name, value] : values) {
            if (name == alias) {
                return value;
            }
        }
        return Value::null_value();
    }

    // Parse a serialized KeyConditionExpression into predicates. Handles the
    // forms the engine emits (arch §8): "#a = :v", optional AND with a sort-key
    // predicate "#b <op> :v", "#b BETWEEN :v1 AND :v2", "begins_with(#b, :v)".
    std::vector<Predicate> parse_key_condition(const std::string& expr,
                                               const ExpressionAttributeNames& names,
                                               const ValueMap& values) const {
        // Normalize punctuation to spaces, then tokenize.
        std::string norm;
        norm.reserve(expr.size());
        for (char c : expr) {
            norm.push_back((c == '(' || c == ')' || c == ',') ? ' ' : c);
        }
        std::vector<std::string> toks;
        std::size_t i = 0;
        while (i < norm.size()) {
            while (i < norm.size() && norm[i] == ' ') {
                ++i;
            }
            std::size_t start = i;
            while (i < norm.size() && norm[i] != ' ') {
                ++i;
            }
            if (i > start) {
                toks.push_back(norm.substr(start, i - start));
            }
        }

        std::vector<Predicate> preds;
        std::size_t t = 0;
        auto upper = [](std::string s) {
            for (char& c : s) {
                if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
            }
            return s;
        };
        while (t < toks.size()) {
            if (upper(toks[t]) == "AND") {
                ++t;
                continue;
            }
            if (toks[t] == "begins_with" && t + 2 < toks.size()) {
                Predicate p;
                p.op = "begins_with";
                p.attr = resolve_name(names, toks[t + 1]);
                p.v1 = resolve_value(values, toks[t + 2]);
                preds.push_back(std::move(p));
                t += 3;
                continue;
            }
            // "#attr OP :val" or "#attr BETWEEN :v1 AND :v2"
            if (t + 1 < toks.size()) {
                std::string attr = resolve_name(names, toks[t]);
                std::string op = toks[t + 1];
                if (upper(op) == "BETWEEN" && t + 4 < toks.size()) {
                    Predicate p;
                    p.op = "between";
                    p.attr = attr;
                    p.v1 = resolve_value(values, toks[t + 2]);
                    p.v2 = resolve_value(values, toks[t + 4]);  // toks[t+3] == AND
                    preds.push_back(std::move(p));
                    t += 5;
                    continue;
                }
                if (t + 2 < toks.size()) {
                    Predicate p;
                    p.op = op;
                    p.attr = attr;
                    p.v1 = resolve_value(values, toks[t + 2]);
                    preds.push_back(std::move(p));
                    t += 3;
                    continue;
                }
            }
            ++t;  // unrecognized token; skip defensively
        }
        return preds;
    }

    static const Value* item_value(const ItemRow& item, const std::string& attr) {
        for (const auto& [name, value] : item.attributes) {
            if (name == attr) {
                return &value;
            }
        }
        return nullptr;
    }

    // -1 / 0 / +1 ordering of two values (numbers compared numerically, else
    // lexicographically on the stored string).
    static int compare(const Value& a, const Value& b) {
        if (a.type == Value::Type::N && b.type == Value::Type::N) {
            try {
                double da = std::stod(a.s);
                double db = std::stod(b.s);
                if (da < db) return -1;
                if (da > db) return 1;
                return 0;
            } catch (...) {
                // fall through to string compare
            }
        }
        if (a.s < b.s) return -1;
        if (a.s > b.s) return 1;
        return 0;
    }

    static bool matches(const Value& item_v, const Predicate& p) {
        if (p.op == "=") return compare(item_v, p.v1) == 0;
        if (p.op == "<") return compare(item_v, p.v1) < 0;
        if (p.op == "<=") return compare(item_v, p.v1) <= 0;
        if (p.op == ">") return compare(item_v, p.v1) > 0;
        if (p.op == ">=") return compare(item_v, p.v1) >= 0;
        if (p.op == "begins_with") {
            return item_v.s.size() >= p.v1.s.size() && item_v.s.compare(0, p.v1.s.size(), p.v1.s) == 0;
        }
        if (p.op == "between") {
            return compare(item_v, p.v1) >= 0 && compare(item_v, p.v2) <= 0;
        }
        return false;
    }

    static bool matches_all(const ItemRow& item, const std::vector<Predicate>& preds) {
        for (const auto& p : preds) {
            const Value* v = item_value(item, p.attr);
            if (v == nullptr || !matches(*v, p)) {
                return false;
            }
        }
        return true;
    }

    // Apply a serialized UpdateExpression: simple "SET #a = :v, ..." assignments
    // and "REMOVE #a, ..." clauses (see the header note on scope).
    static void apply_update(ItemRow& item, const std::string& expr,
                             const ExpressionAttributeNames& names, const ValueMap& values) {
        // Split into tokens on spaces and commas.
        std::string norm;
        for (char c : expr) {
            norm.push_back(c == ',' ? ' ' : c);
        }
        std::vector<std::string> toks;
        std::size_t i = 0;
        while (i < norm.size()) {
            while (i < norm.size() && norm[i] == ' ') ++i;
            std::size_t start = i;
            while (i < norm.size() && norm[i] != ' ') ++i;
            if (i > start) toks.push_back(norm.substr(start, i - start));
        }
        auto upper = [](std::string s) {
            for (char& c : s) {
                if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
            }
            return s;
        };
        enum class Mode { none, set, remove } mode = Mode::none;
        std::size_t t = 0;
        while (t < toks.size()) {
            std::string u = upper(toks[t]);
            if (u == "SET") {
                mode = Mode::set;
                ++t;
                continue;
            }
            if (u == "REMOVE") {
                mode = Mode::remove;
                ++t;
                continue;
            }
            if (mode == Mode::set && t + 2 < toks.size() && toks[t + 1] == "=") {
                std::string attr = resolve_name(names, toks[t]);
                Value v = resolve_value(values, toks[t + 2]);
                set_attr(item, attr, std::move(v));
                t += 3;
                continue;
            }
            if (mode == Mode::remove) {
                std::string attr = resolve_name(names, toks[t]);
                remove_attr(item, attr);
                ++t;
                continue;
            }
            ++t;  // skip unrecognized (arithmetic / if_not_exists — not modeled)
        }
    }

    static void set_attr(ItemRow& item, const std::string& attr, Value v) {
        for (auto& [name, value] : item.attributes) {
            if (name == attr) {
                value = std::move(v);
                return;
            }
        }
        item.attributes.emplace_back(attr, std::move(v));
    }

    static void remove_attr(ItemRow& item, const std::string& attr) {
        for (std::size_t i = 0; i < item.attributes.size(); ++i) {
            if (item.attributes[i].first == attr) {
                item.attributes.erase(item.attributes.begin() + static_cast<std::ptrdiff_t>(i));
                return;
            }
        }
    }

    std::map<std::string, Table> tables_;
};

}  // namespace ddbsh::testing

#endif  // DDBSH_TESTING_MEMORY_CLIENT_HPP
