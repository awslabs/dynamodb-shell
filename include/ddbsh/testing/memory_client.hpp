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
// Scope and honest limitations (documented, not hidden; extended in phase 5a):
//   * Primary-key addressing is faithful: a get_item by full key, a query by a
//     partition-key equality (plus an optional sort-key predicate: =, <, <=, >,
//     >=, begins_with, BETWEEN), and a scan of the whole table all work.
//   * FilterExpressions ARE evaluated (phase 5a, D-P5-4 Tier 1). A query/scan
//     applies the serialized FilterExpression the engine emits over its alias
//     maps: comparisons (=, <, <=, >, >=; `!=` arrives as `NOT (#a = :v)`),
//     AND / OR / NOT and parenthesized groups, attribute_exists /
//     attribute_not_exists, begins_with, contains, IN, attribute_type, BETWEEN,
//     and the size(#a) left-hand-side form. (Only the forms the engine emits;
//     values are always aliased, never literal, on this path.)
//   * Bounded paging is OPT-IN (phase 5a). By default a query/scan returns the
//     whole matched set in one page with no LastEvaluatedKey (every pre-5a test
//     is unaffected). set_page_size(n) makes query/scan return at most n items
//     plus a LastEvaluatedKey cursor and honor an ExclusiveStartKey to resume —
//     enough to drive the streaming cursor across real page boundaries offline.
//   * UpdateItem applies simple `SET #a = :v` assignments and `REMOVE #a`
//     clauses; arithmetic (`#a = #a + :v`) and if_not_exists are not
//     interpreted.
//   * Index (`.index`) queries filter by the same predicates over the whole
//     table (the index key attributes are ordinary item attributes here); the
//     paging cursor keys off the base-table primary key.
//   * Conditions honored: attribute_not_exists(pk) (INSERT) and
//     attribute_exists(pk) (UPDATE/DELETE guards) by primary-key presence; a
//     mismatch returns Errc::conditional_check_failed, which the engine reports
//     as the original's "The conditional request failed".
//   * Transactions (phase 5a, Tier 2): transact_get_items reads N items by key;
//     transact_write_items applies Put / Update / Delete / ConditionCheck over
//     the same primary-key store, atomically — every sub-item's condition is
//     validated first, and if any fails NO mutation is applied and
//     Errc::conditional_check_failed is returned.
//   * Minimal control-plane state (phase 5a, Tier 2): update_time_to_live,
//     update_continuous_backups (PITR), and update_table (deletion protection,
//     table class, billing mode) record state that the matching describe_*
//     reflects, so an embedder's ALTER -> DESCRIBE round-trip works offline.
//     Report FORMATTING fidelity remains the golden suite's job (real DDB);
//     this is only enough state for an offline round-trip assertion.
//   * Still NOT modeled (documented, not hidden): backups/restore, vector
//     search, SHOW LIMITS figures, and EXPLAIN serialization (explain_request
//     has no SDK-free serializer off the AWS SDK). Those methods return
//     empty/not-implemented results. MemoryClient targets the data-plane
//     round-trip plus the control-plane state above, not the full control plane.
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
#include <type_traits>
#include <utility>
#include <variant>
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

    /// Opt-in bounded paging (phase 5a, D-P5-4 Tier 1). When `n > 0`, query()
    /// and scan() return at most `n` matched items per call plus a
    /// LastEvaluatedKey cursor, and honor an ExclusiveStartKey to resume —
    /// letting an offline test drive the engine's streaming cursor across real
    /// page boundaries. `n == 0` (the default) means "unbounded / one page, no
    /// cursor", preserving every pre-5a test's behavior.
    void set_page_size(std::size_t n) { page_size_ = n; }

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
        // Match: key predicates AND (when present) the FilterExpression.
        std::vector<const ItemRow*> matched;
        for (const auto& item : t->items) {
            if (matches_all(item, preds) &&
                filter_matches(item, in.filter_expression, in.expression_attribute_names,
                               in.expression_attribute_values)) {
                matched.push_back(&item);
            }
        }
        paginate(matched, *t, in.exclusive_start_key, in.projection_expression,
                 in.expression_attribute_names, out.items, out.last_evaluated_key);
        return out;
    }

    Result<ScanOut> scan(const ScanIn& in) override {
        ScanOut out;
        const Table* t = find_table(in.table_name);
        if (t == nullptr) {
            return fail(Errc::no_such_table, "Table: " + in.table_name + " not found");
        }
        // The whole table, filtered by the serialized FilterExpression (phase
        // 5a) and projected (the engine projects just the key attributes to
        // drive bulk writes), with optional bounded paging.
        std::vector<const ItemRow*> matched;
        for (const auto& item : t->items) {
            if (filter_matches(item, in.filter_expression, in.expression_attribute_names,
                               in.expression_attribute_values)) {
                matched.push_back(&item);
            }
        }
        paginate(matched, *t, in.exclusive_start_key, in.projection_expression,
                 in.expression_attribute_names, out.items, out.last_evaluated_key);
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
        if (in.return_values == ReturnValue::all_new ||
            in.return_values == ReturnValue::updated_new) {
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
        t.deletion_protection = in.deletion_protection;
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
        // Minimal control-plane state recorded by update_table (phase 5a, Tier
        // 2) so an ALTER -> DESCRIBE round-trip reflects it offline.
        out.deletion_protection_enabled = t->deletion_protection;
        out.billing_mode_summary = t->billing_mode;  // std::optional
        out.table_class_summary = t->table_class;    // std::optional
        return out;
    }

    Result<void> wait_for_table_active(const std::string&) override { return {}; }
    Result<void> wait_for_table_gone(const std::string&) override { return {}; }

    // ---- control-plane state round-trips (phase 5a, Tier 2) ----------------
    // update_time_to_live / update_continuous_backups / update_table record
    // just enough state that the matching describe_* reflects it offline. These
    // are NOT report-formatting fidelity (that is the golden suite's job on real
    // DDB); they make an embedder's ALTER -> DESCRIBE assertion work with no
    // AWS account.

    Result<TimeToLiveOut> describe_time_to_live(const DescribeTimeToLiveIn& in) override {
        if (const Table* t = find_table(in.table_name)) {
            return TimeToLiveOut{t->ttl_status, t->ttl_attribute};
        }
        return TimeToLiveOut{"DISABLED", ""};
    }
    Result<ContinuousBackupsOut> describe_continuous_backups(
        const DescribeContinuousBackupsIn& in) override {
        if (const Table* t = find_table(in.table_name)) {
            return ContinuousBackupsOut{t->pitr_status, "", ""};
        }
        return ContinuousBackupsOut{"DISABLED", "", ""};
    }
    Result<TableReplicaAutoScalingOut> describe_table_replica_auto_scaling(
        const DescribeTableReplicaAutoScalingIn&) override {
        return TableReplicaAutoScalingOut{};
    }
    Result<UpdateTableOut> update_table(const UpdateTableIn& in) override {
        Table* t = find_table(in.table_name);
        if (t == nullptr) {
            return fail(Errc::no_such_table, "Table: " + in.table_name + " not found");
        }
        if (in.deletion_protection.has_value()) {
            t->deletion_protection = *in.deletion_protection;
        }
        if (in.table_class.has_value()) {
            t->table_class = (*in.table_class == TableClass::standard_infrequent_access)
                                 ? "STANDARD_INFREQUENT_ACCESS"
                                 : "STANDARD";
        }
        if (in.billing.has_value()) {
            t->billing_mode = (in.billing->mode == BillingMode::provisioned) ? "PROVISIONED"
                                                                             : "PAY_PER_REQUEST";
        }
        return UpdateTableOut{};
    }
    Result<UpdateTimeToLiveOut> update_time_to_live(const UpdateTimeToLiveIn& in) override {
        if (Table* t = find_table(in.table_name)) {
            t->ttl_status = in.enabled ? "ENABLED" : "DISABLED";
            t->ttl_attribute = in.enabled ? in.attribute_name : "";
        }
        return UpdateTimeToLiveOut{};
    }
    Result<UpdateContinuousBackupsOut> update_continuous_backups(
        const UpdateContinuousBackupsIn& in) override {
        if (Table* t = find_table(in.table_name)) {
            t->pitr_status = in.enabled ? "ENABLED" : "DISABLED";
        }
        return UpdateContinuousBackupsOut{};
    }

    // ---- transactions (phase 5a, Tier 2) -----------------------------------

    Result<TransactGetItemsOut> transact_get_items(const TransactGetItemsIn& in) override {
        TransactGetItemsOut out;
        for (const auto& get : in.items) {
            const Table* t = find_table(get.table_name);
            if (t == nullptr) {
                return fail(Errc::no_such_table, "Table: " + get.table_name + " not found");
            }
            const ItemRow* item = t->find(key_string(get.key));
            if (item != nullptr) {
                out.responses.push_back(
                    project(*item, get.projection_expression, get.expression_attribute_names));
            } else {
                out.responses.push_back(std::nullopt);
            }
        }
        return out;
    }

    Result<TransactWriteItemsOut> transact_write_items(const TransactWriteItemsIn& in) override {
        // Atomic: validate every sub-item's condition against the CURRENT state
        // first; if any fails, apply nothing and report the failure.
        for (const auto& item : in.items) {
            if (auto e = validate_txn_item(item)) {
                return fail(*e);
            }
        }
        for (const auto& item : in.items) {
            apply_txn_item(item);
        }
        return TransactWriteItemsOut{};
    }

    // ---- not modeled (reports / backups / vectors / EXPLAIN) ---------------
    // MemoryClient targets the data-plane round-trip plus the Tier-2
    // control-plane state above (see the header note). These return empty
    // success or a clear not-implemented Error so a misuse surfaces plainly
    // rather than silently misbehaving.

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
    Result<std::string> explain_request(const ExplainRequest&) override {
        return not_impl<std::string>("explain_request");
    }

private:
    struct Table {
        std::string name;
        TableKeySchema schema;
        std::vector<ItemRow> items;

        // ---- Tier-2 control-plane state (phase 5a) -------------------------
        bool deletion_protection = false;
        std::optional<std::string> billing_mode;  // "PROVISIONED" / "PAY_PER_REQUEST"
        std::optional<std::string> table_class;   // "STANDARD" / "STANDARD_INFREQUENT_ACCESS"
        std::string ttl_status = "DISABLED";
        std::string ttl_attribute;
        std::string pitr_status = "DISABLED";

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
        return fail(Errc::not_implemented, std::string("MemoryClient does not implement ") + op);
    }

    // A total, collision-free-enough key segment: type tag + payload. Scalars
    // (S/N/B) are the only legal key attribute types, but every type is encoded
    // so no two distinct values ever collide (phase 5a fix of the old "?:").
    static std::string value_key(const Value& v) {
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
            case Value::Type::SS:
            case Value::Type::NS:
            case Value::Type::BS: {
                std::string s = (v.type == Value::Type::SS   ? "SS:"
                                 : v.type == Value::Type::NS ? "NS:"
                                                             : "BS:");
                for (const auto& e : v.set) {
                    s += e;
                    s += '\x1e';
                }
                return s;
            }
            case Value::Type::L: {
                std::string s = "L:";
                for (const auto& e : v.list) {
                    s += value_key(e);
                    s += '\x1e';
                }
                return s;
            }
            case Value::Type::M: {
                std::string s = "M:";
                for (const auto& [name, value] : v.map) {
                    s += name;
                    s += '=';
                    s += value_key(value);
                    s += '\x1e';
                }
                return s;
            }
        }
        return "?:";
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

    // Slice `matched` (store order) into one page: skip past the
    // ExclusiveStartKey if set, take up to page_size_ items (all if 0), project
    // each, and set `lek` to the last returned item's key iff more remain.
    void paginate(const std::vector<const ItemRow*>& matched, const Table& t,
                  const std::optional<ValueMap>& exclusive_start_key,
                  const std::optional<std::string>& projection,
                  const ExpressionAttributeNames& names, std::vector<ItemRow>& out_items,
                  std::optional<ValueMap>& lek) const {
        std::size_t start = 0;
        if (exclusive_start_key.has_value() && !exclusive_start_key->empty()) {
            std::string cursor = key_string(*exclusive_start_key);
            for (std::size_t i = 0; i < matched.size(); ++i) {
                if (key_string(extract_key(*matched[i], t.schema)) == cursor) {
                    start = i + 1;
                    break;
                }
            }
        }
        std::size_t end = matched.size();
        if (page_size_ > 0 && start + page_size_ < end) {
            end = start + page_size_;
        }
        for (std::size_t i = start; i < end; ++i) {
            out_items.push_back(project(*matched[i], projection, names));
        }
        if (page_size_ > 0 && end < matched.size() && end > start) {
            lek = extract_key(*matched[end - 1], t.schema);
        }
    }

    static std::optional<Error> check_condition(const std::optional<std::string>& cond,
                                                bool key_exists) {
        if (!cond.has_value()) {
            return std::nullopt;
        }
        const std::string& c = *cond;
        const bool wants_absent = c.find("attribute_not_exists") != std::string::npos;
        const bool wants_present = !wants_absent && c.find("attribute_exists") != std::string::npos;
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

    // ---- transaction helpers (phase 5a, Tier 2) ----------------------------

    std::optional<Error> validate_txn_item(const TransactWriteItem& item) {
        return std::visit(
            [&](const auto& w) -> std::optional<Error> {
                using T = std::decay_t<decltype(w)>;
                const Table* t = find_table(w.table_name);
                if (t == nullptr) {
                    return Error{Errc::no_such_table, "Table: " + w.table_name + " not found",
                                 std::nullopt, std::nullopt};
                }
                if constexpr (std::is_same_v<T, TransactPut>) {
                    ItemRow row;
                    row.attributes = w.item;
                    bool exists = t->find(key_string(extract_key(row, t->schema))) != nullptr;
                    return check_condition(w.condition_expression, exists);
                } else if constexpr (std::is_same_v<T, TransactConditionCheck>) {
                    bool exists = t->find(key_string(w.key)) != nullptr;
                    return check_condition(w.condition_expression, exists);
                } else {
                    bool exists = t->find(key_string(w.key)) != nullptr;
                    return check_condition(w.condition_expression, exists);
                }
            },
            item);
    }

    void apply_txn_item(const TransactWriteItem& item) {
        std::visit(
            [&](const auto& w) {
                using T = std::decay_t<decltype(w)>;
                Table* t = find_table(w.table_name);
                if (t == nullptr) {
                    return;
                }
                if constexpr (std::is_same_v<T, TransactPut>) {
                    ItemRow row;
                    row.attributes = w.item;
                    t->put(key_string(extract_key(row, t->schema)), std::move(row));
                } else if constexpr (std::is_same_v<T, TransactUpdate>) {
                    std::string k = key_string(w.key);
                    ItemRow row;
                    if (const ItemRow* cur = t->find(k)) {
                        row = *cur;
                    } else {
                        row.attributes = w.key;
                    }
                    apply_update(row, w.update_expression, w.expression_attribute_names,
                                 w.expression_attribute_values);
                    t->put(k, row);
                } else if constexpr (std::is_same_v<T, TransactDelete>) {
                    t->erase(key_string(w.key));
                }
                // TransactConditionCheck mutates nothing.
            },
            item);
    }

    // Apply a ProjectionExpression: return a copy of `item` keeping only the
    // projected attributes (source order preserved). A ProjectionExpression is
    // a comma-separated list of `#token`s (resolved via expression_attribute_
    // names) or bare attribute names. When no projection is set the whole item
    // is returned. Real DynamoDB does this; the engine relies on it to project
    // just the key attributes when enumerating rows for a bulk write, so the
    // per-item write key carries no stray non-key attributes. Dotted/nested
    // projection paths are not modeled (out of scope — see the header note).
    static ItemRow project(const ItemRow& item, const std::optional<std::string>& projection,
                           const ExpressionAttributeNames& names) {
        if (!projection.has_value() || projection->empty()) {
            return item;
        }
        std::vector<std::string> wanted;
        std::size_t i = 0;
        const std::string& p = *projection;
        while (i < p.size()) {
            while (i < p.size() && (p[i] == ' ' || p[i] == ',')) {
                ++i;
            }
            std::size_t start = i;
            while (i < p.size() && p[i] != ' ' && p[i] != ',') {
                ++i;
            }
            if (i > start) {
                wanted.push_back(resolve_name(names, p.substr(start, i - start)));
            }
        }
        ItemRow out;
        for (const auto& [name, value] : item.attributes) {
            for (const auto& w : wanted) {
                if (w == name) {
                    out.attributes.emplace_back(name, value);
                    break;
                }
            }
        }
        return out;
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
                if (c >= 'a' && c <= 'z')
                    c = static_cast<char>(c - 'a' + 'A');
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
                if (da < db)
                    return -1;
                if (da > db)
                    return 1;
                return 0;
            } catch (...) {
                // fall through to string compare
            }
        }
        if (a.s < b.s)
            return -1;
        if (a.s > b.s)
            return 1;
        return 0;
    }

    static bool matches(const Value& item_v, const Predicate& p) {
        if (p.op == "=")
            return compare(item_v, p.v1) == 0;
        if (p.op == "<")
            return compare(item_v, p.v1) < 0;
        if (p.op == "<=")
            return compare(item_v, p.v1) <= 0;
        if (p.op == ">")
            return compare(item_v, p.v1) > 0;
        if (p.op == ">=")
            return compare(item_v, p.v1) >= 0;
        if (p.op == "begins_with") {
            return item_v.s.size() >= p.v1.s.size() &&
                   item_v.s.compare(0, p.v1.s.size(), p.v1.s) == 0;
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

    // ---- FilterExpression evaluation (phase 5a, D-P5-4 Tier 1) -------------
    // A tiny recursive-descent evaluator over the serialized FilterExpression
    // the engine emits (arch §6/§8). Values are always aliased (`:v...`) and
    // names always aliased (`#a...`) on this path, so the expression never
    // contains a literal string/quote/space inside an operand — tokenizing on
    // whitespace plus the glued punctuation ('(', ')', ',') is safe. Precedence
    // (low->high): OR < AND < NOT < primary. Grammar (the forms the engine
    // emits): comparisons `#a OP :v`, `#a BETWEEN :v AND :v`, `#a IN (:v, ...)`,
    // begins_with/contains/attribute_type/attribute_exists, the `size(#a)` LHS,
    // `NOT (...)`, and parenthesized groups.

    static bool filter_matches(const ItemRow& item, const std::optional<std::string>& expr,
                               const ExpressionAttributeNames& names, const ValueMap& values) {
        if (!expr.has_value() || expr->empty()) {
            return true;  // no filter => every key-matched row passes
        }
        std::vector<std::string> toks = tokenize_filter(*expr);
        std::size_t pos = 0;
        FilterCtx ctx{item, names, values};
        bool rv = eval_or(toks, pos, ctx);
        return rv;
    }

    struct FilterCtx {
        const ItemRow& item;
        const ExpressionAttributeNames& names;
        const ValueMap& values;
    };

    static std::vector<std::string> tokenize_filter(const std::string& expr) {
        std::string norm;
        norm.reserve(expr.size() * 2);
        for (char c : expr) {
            if (c == '(' || c == ')' || c == ',') {
                norm.push_back(' ');
                norm.push_back(c);
                norm.push_back(' ');
            } else {
                norm.push_back(c);
            }
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
        return toks;
    }

    static std::string upper_tok(std::string s) {
        for (char& c : s) {
            if (c >= 'a' && c <= 'z')
                c = static_cast<char>(c - 'a' + 'A');
        }
        return s;
    }

    static bool eval_or(const std::vector<std::string>& t, std::size_t& p, const FilterCtx& ctx) {
        bool v = eval_and(t, p, ctx);
        while (p < t.size() && upper_tok(t[p]) == "OR") {
            ++p;
            bool rhs = eval_and(t, p, ctx);
            v = v || rhs;
        }
        return v;
    }

    static bool eval_and(const std::vector<std::string>& t, std::size_t& p, const FilterCtx& ctx) {
        bool v = eval_unary(t, p, ctx);
        while (p < t.size() && upper_tok(t[p]) == "AND") {
            ++p;
            bool rhs = eval_unary(t, p, ctx);
            v = v && rhs;
        }
        return v;
    }

    static bool eval_unary(const std::vector<std::string>& t, std::size_t& p,
                           const FilterCtx& ctx) {
        if (p < t.size() && upper_tok(t[p]) == "NOT") {
            ++p;
            return !eval_unary(t, p, ctx);
        }
        return eval_primary(t, p, ctx);
    }

    static bool eval_primary(const std::vector<std::string>& t, std::size_t& p,
                             const FilterCtx& ctx) {
        if (p >= t.size()) {
            return false;
        }
        if (t[p] == "(") {
            ++p;  // consume '('
            bool v = eval_or(t, p, ctx);
            if (p < t.size() && t[p] == ")") {
                ++p;  // consume ')'
            }
            return v;
        }
        // Function predicates with their own parentheses.
        if (t[p] == "attribute_exists" || t[p] == "attribute_not_exists") {
            bool want_exists = (t[p] == "attribute_exists");
            ++p;
            std::string attr = fn_one_arg_name(t, p, ctx);
            bool present = item_value(ctx.item, attr) != nullptr;
            return want_exists ? present : !present;
        }
        if (t[p] == "begins_with" || t[p] == "contains" || t[p] == "attribute_type") {
            std::string fn = t[p];
            ++p;
            std::string attr;
            Value arg;
            fn_two_args(t, p, ctx, attr, arg);
            const Value* iv = item_value(ctx.item, attr);
            if (iv == nullptr) {
                return false;
            }
            if (fn == "begins_with") {
                return iv->s.size() >= arg.s.size() && iv->s.compare(0, arg.s.size(), arg.s) == 0;
            }
            if (fn == "contains") {
                return value_contains(*iv, arg);
            }
            return type_token(*iv) == arg.s;  // attribute_type
        }
        // Operand (name or size(name)) followed by a comparison / BETWEEN / IN.
        bool is_size = false;
        std::string attr;
        if (t[p] == "size") {
            // A literal `size` token (robustness; the engine does not emit it).
            is_size = true;
            ++p;
            attr = fn_one_arg_name(t, p, ctx);
        } else {
            attr = resolve_name(ctx.names, t[p]);
            ++p;
            // The engine renders `size(<attr>) op val` by aliasing the WHOLE
            // literal path "size(<attr>)" as one attribute name (arch §6/§14,
            // symbol_table new_bare); unwrap it here to evaluate the size.
            if (attr.size() > 6 && attr.compare(0, 5, "size(") == 0 && attr.back() == ')') {
                is_size = true;
                attr = attr.substr(5, attr.size() - 6);
            }
        }
        const Value* iv = item_value(ctx.item, attr);
        std::optional<Value> lhs;
        if (is_size) {
            lhs = Value::number_value(std::to_string(value_size(iv)));
        } else if (iv != nullptr) {
            lhs = *iv;
        }
        if (p >= t.size()) {
            return false;
        }
        std::string op = upper_tok(t[p]);
        if (op == "BETWEEN") {
            ++p;
            Value lo = next_operand_value(t, p, ctx);
            if (p < t.size() && upper_tok(t[p]) == "AND") {
                ++p;
            }
            Value hi = next_operand_value(t, p, ctx);
            if (!lhs.has_value()) {
                return false;
            }
            return compare(*lhs, lo) >= 0 && compare(*lhs, hi) <= 0;
        }
        if (op == "IN") {
            ++p;
            if (p < t.size() && t[p] == "(") {
                ++p;
            }
            bool found = false;
            while (p < t.size() && t[p] != ")") {
                if (t[p] == ",") {
                    ++p;
                    continue;
                }
                Value v = resolve_value(ctx.values, t[p]);
                ++p;
                if (lhs.has_value() && value_equal(*lhs, v)) {
                    found = true;
                }
            }
            if (p < t.size() && t[p] == ")") {
                ++p;
            }
            return found;
        }
        // Binary comparison `OP :v` (op is the raw token: = < <= > >=).
        std::string raw_op = t[p];
        ++p;
        Value rhs = next_operand_value(t, p, ctx);
        if (!lhs.has_value()) {
            return false;
        }
        int c = compare(*lhs, rhs);
        if (raw_op == "=")
            return value_equal(*lhs, rhs);
        if (raw_op == "<")
            return c < 0;
        if (raw_op == "<=")
            return c <= 0;
        if (raw_op == ">")
            return c > 0;
        if (raw_op == ">=")
            return c >= 0;
        return false;
    }

    // Consume `( #name )` and return the resolved real attribute name.
    static std::string fn_one_arg_name(const std::vector<std::string>& t, std::size_t& p,
                                       const FilterCtx& ctx) {
        if (p < t.size() && t[p] == "(") {
            ++p;
        }
        std::string attr;
        if (p < t.size() && t[p] != ")") {
            attr = resolve_name(ctx.names, t[p]);
            ++p;
        }
        if (p < t.size() && t[p] == ")") {
            ++p;
        }
        return attr;
    }

    // Consume `( #name , :value )` and yield the resolved name + bound value.
    static void fn_two_args(const std::vector<std::string>& t, std::size_t& p, const FilterCtx& ctx,
                            std::string& attr, Value& arg) {
        if (p < t.size() && t[p] == "(") {
            ++p;
        }
        if (p < t.size() && t[p] != ")") {
            attr = resolve_name(ctx.names, t[p]);
            ++p;
        }
        if (p < t.size() && t[p] == ",") {
            ++p;
        }
        if (p < t.size() && t[p] != ")") {
            arg = resolve_value(ctx.values, t[p]);
            ++p;
        }
        if (p < t.size() && t[p] == ")") {
            ++p;
        }
    }

    // A single value-alias operand (e.g. the RHS of a comparison or BETWEEN
    // bound); resolves the `:v...` token to its bound Value.
    static Value next_operand_value(const std::vector<std::string>& t, std::size_t& p,
                                    const FilterCtx& ctx) {
        if (p >= t.size()) {
            return Value::null_value();
        }
        Value v = resolve_value(ctx.values, t[p]);
        ++p;
        return v;
    }

    static bool value_equal(const Value& a, const Value& b) {
        if (a.type == Value::Type::N && b.type == Value::Type::N) {
            return compare(a, b) == 0;  // 2 == 2.0
        }
        if (a.type != b.type) {
            return false;
        }
        switch (a.type) {
            case Value::Type::S:
            case Value::Type::B:
                return a.s == b.s;
            case Value::Type::BOOL:
                return a.boolean == b.boolean;
            case Value::Type::NUL:
                return true;
            case Value::Type::SS:
            case Value::Type::NS:
            case Value::Type::BS:
                return a.set == b.set;
            default:
                return value_key(a) == value_key(b);
        }
    }

    static bool value_contains(const Value& container, const Value& needle) {
        switch (container.type) {
            case Value::Type::S:
                return container.s.find(needle.s) != std::string::npos;
            case Value::Type::SS:
            case Value::Type::NS:
            case Value::Type::BS:
                for (const auto& e : container.set) {
                    if (e == needle.s) {
                        return true;
                    }
                }
                return false;
            case Value::Type::L:
                for (const auto& e : container.list) {
                    if (value_equal(e, needle)) {
                        return true;
                    }
                }
                return false;
            default:
                return false;
        }
    }

    static std::string type_token(const Value& v) {
        switch (v.type) {
            case Value::Type::S:
                return "S";
            case Value::Type::N:
                return "N";
            case Value::Type::B:
                return "B";
            case Value::Type::BOOL:
                return "BOOL";
            case Value::Type::NUL:
                return "NULL";
            case Value::Type::L:
                return "L";
            case Value::Type::M:
                return "M";
            case Value::Type::SS:
                return "SS";
            case Value::Type::NS:
                return "NS";
            case Value::Type::BS:
                return "BS";
        }
        return "";
    }

    static long long value_size(const Value* v) {
        if (v == nullptr) {
            return 0;
        }
        switch (v->type) {
            case Value::Type::S:
            case Value::Type::B:
                return static_cast<long long>(v->s.size());
            case Value::Type::SS:
            case Value::Type::NS:
            case Value::Type::BS:
                return static_cast<long long>(v->set.size());
            case Value::Type::L:
                return static_cast<long long>(v->list.size());
            case Value::Type::M:
                return static_cast<long long>(v->map.size());
            default:
                return 0;
        }
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
            while (i < norm.size() && norm[i] == ' ')
                ++i;
            std::size_t start = i;
            while (i < norm.size() && norm[i] != ' ')
                ++i;
            if (i > start)
                toks.push_back(norm.substr(start, i - start));
        }
        auto upper = [](std::string s) {
            for (char& c : s) {
                if (c >= 'a' && c <= 'z')
                    c = static_cast<char>(c - 'a' + 'A');
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
    std::size_t page_size_ = 0;  // 0 => unbounded (one page, no cursor)
};

}  // namespace ddbsh::testing

#endif  // DDBSH_TESTING_MEMORY_CLIENT_HPP
