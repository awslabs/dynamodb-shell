// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// where.hpp — WHERE-clause wrapper and access-pattern capability probes.
//
// PRIVATE engine header. SDK-free port of expressions/where.hpp (CWhere). Owns
// the root LogicalExpression (unique_ptr, RAII). The can_* probes drive the
// GetItem/Query/Scan decision (arch §6, §8):
//   * can_getitem    — base table only; EXACTLY the full key, nothing else.
//   * can_query_table — exactly one non-negated PK `=` (referenced once) AND
//                       the whole clause is query_safe.
//   * can_query_index — can_getitem || can_query_table (indexes never GetItem).
//   * can_update_item / can_delete_item — full PK present; extra predicates
//                       are tolerated (they become the ConditionExpression).
//   * plan_query     — the generalized Query probe (phase 4a), which also
//                       splits a sort-key not-equal into two Queries (P15).
//
// D12: the original's internal-invariant assert()s (e.g. "pk not empty",
// "can_query_table implies !can_getitem") are dropped; the planner always
// probes in order (getitem, then query, then scan), so the invariants hold by
// construction without aborting on unexpected input.

#ifndef DDBSH_SRC_EXPRESSIONS_WHERE_HPP
#define DDBSH_SRC_EXPRESSIONS_WHERE_HPP

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ddbsh/table_schema.hpp"
#include "expressions/logical_expression.hpp"

namespace ddbsh::engine {

/// The chosen Query key plan (phase 4a): the partition-key attributes plus the
/// gap-free, key-legal sort-key prefix. Returned by Where::plan_query; its
/// absence means the clause is not query-able against this schema and the
/// caller must Scan. For a single-key schema this reduces to pk=={p},
/// sk_prefix=={} or {r}.
///
/// `ne_split` (P15, D-P15-8 Rule 2) is set when the clause's only predicate on
/// the last prefix attribute is a not-equal: the plan is then two Queries, the
/// lower leg (`#sk < :v`) and the upper leg (`#sk > :v`), with the same filter.
struct QueryKeyPlan {
    std::vector<std::string> pk;
    std::vector<std::string> sk_prefix;
    std::optional<std::string> ne_split{};
};

/// Declared scalar types of key attributes (AttributeDefinitions), by name.
using KeyTypes = std::map<std::string, ScalarType>;

class Where {
public:
    explicit Where(std::unique_ptr<LogicalExpression> root) : m_where(std::move(root)) {}

    // Base table only; EXACTLY the full key and nothing else. The base key is
    // single (|pk|==1, |rk|<=1); a composite key never GetItems.
    bool can_getitem(const std::vector<std::string>& pk, const std::vector<std::string>& rk) const {
        if (pk.size() != 1) {
            return false;
        }
        if (!rk.empty()) {
            return m_where->count() == 2 && m_where->count_gud_key(pk[0]) == 1 &&
                   m_where->count_gud_key(rk[0]) == 1;
        }
        return m_where->count() == 1 && m_where->count_gud_key(pk[0]) == 1;
    }

    /// Plan a Query against `pk`/`rk` (phase 4a). Succeeds when every partition
    /// key carries a non-negated `=` referenced once, the specified sort keys
    /// form a gap-free left-to-right prefix with a single `=` on every non-last
    /// prefix element, and the whole clause is query-safe against the chosen
    /// pk ∪ prefix. Returns the plan, or nullopt to signal "Scan". Replaces the
    /// original's can_query_table/can_query_index + separate key-condition
    /// recompute with ONE computation reused for rendering. Proven to reduce to
    /// the original single-key behavior byte-for-byte (design §4.4).
    ///
    /// When the clause is not query-safe only because of a not-equal on the
    /// last prefix attribute, the plan splits it into two Queries (P15,
    /// D-P15-8 Rule 2; see split_on_not_equal). `key_types` are the declared
    /// key types from DescribeTable (null or missing entries: unknown).
    std::optional<QueryKeyPlan> plan_query(const std::vector<std::string>& pk,
                                           const std::vector<std::string>& rk,
                                           const KeyTypes* key_types = nullptr) const {
        // 1. Every partition key: exactly one non-negated '=', referenced once.
        for (const auto& p : pk) {
            if (!(m_where->count_gud_key(p) == 1 && m_where->count(p) == 1)) {
                return std::nullopt;
            }
        }
        // 2. Longest gap-free sort-key prefix of specified attributes.
        std::vector<std::string> prefix;
        bool ended = false;
        for (const auto& s : rk) {
            if (m_where->count(s) == 0) {
                ended = true;
                continue;
            }
            if (ended) {
                return std::nullopt;  // a gap (sk_i absent, sk_j>i present) -> Scan
            }
            prefix.push_back(s);
        }
        // 3. Every NON-LAST prefix element must be exactly one non-negated '='.
        for (std::size_t j = 0; j + 1 < prefix.size(); ++j) {
            if (!(m_where->count(prefix[j]) == 1 && m_where->count_gud_key(prefix[j]) == 1)) {
                return std::nullopt;  // range/dup on a middle SK -> Scan
            }
        }
        // 4. The whole tree must be query-safe against pk ∪ prefix.
        KeySchemaView view{pk, prefix};
        if (m_where->query_safe(view)) {
            return QueryKeyPlan{pk, prefix};
        }
        // 5. Or query-safe once a not-equal on the last prefix attribute is
        //    split into two Queries.
        return split_on_not_equal(pk, prefix, key_types);
    }

    // Full base-table PK present; extra predicates tolerated (they become the
    // ConditionExpression). Base key is single.
    bool can_update_item(const std::vector<std::string>& pk,
                         const std::vector<std::string>& rk) const {
        if (pk.size() != 1) {
            return false;
        }
        if (!rk.empty()) {
            return m_where->count_gud_key(pk[0]) == 1 && m_where->count_gud_key(rk[0]) == 1;
        }
        return m_where->count_gud_key(pk[0]) == 1;
    }

    bool can_delete_item(const std::vector<std::string>& pk,
                         const std::vector<std::string>& rk) const {
        return can_update_item(pk, rk);
    }

    // The GetItem/Update/Delete key for a single base-table item. The base key
    // is single, so only the first pk/rk element is consulted.
    KeyMap get_gud_key(const std::vector<std::string>& pk,
                       const std::vector<std::string>& rk) const {
        const std::string p = pk.empty() ? std::string() : pk[0];
        const std::string r = rk.empty() ? std::string() : rk[0];
        return m_where->get_gud_key(p, r);
    }

    /// The KeyConditionExpression for `plan`. For a not-equal split, `leg`
    /// picks the side of the value (lower: `<`, upper: `>`); otherwise it is
    /// ignored.
    std::string query_key_condition_expression(const QueryKeyPlan& plan, SymbolTable* st,
                                               NeLeg leg = NeLeg::lower) const {
        KeySchemaView view{plan.pk, plan.sk_prefix, split_of(plan), leg};
        return m_where->query_key_condition_expression(view, st);
    }

    /// The FilterExpression for `plan` (the same for both legs of a split).
    std::string query_filter_expression(const QueryKeyPlan& plan, SymbolTable* st) const {
        KeySchemaView view{plan.pk, plan.sk_prefix, split_of(plan), NeLeg::lower};
        return m_where->query_filter_expression(view, st);
    }

    std::string scan_filter_expression(SymbolTable* st) const { return m_where->serialize(st); }

    std::string update_delete_condition_check(const std::string& pk, const std::string& rk,
                                              SymbolTable* st) const {
        return m_where->update_delete_condition_check(pk, rk, st, /*top=*/true);
    }

    /// Direct access to the root (for probes the planner composes itself).
    const LogicalExpression& root() const { return *m_where; }

    /// Collect pointers to every unresolved parameter-marker Value slot in the
    /// WHERE tree (prepared-statement facility, D-P5-2), in traversal order.
    void collect_param_slots(std::vector<Value*>& out) { m_where->collect_param_slots(out); }

private:
    static const std::string* split_of(const QueryKeyPlan& plan) {
        return plan.ne_split.has_value() ? &*plan.ne_split : nullptr;
    }

    // D-P15-8 Rule 2: a not-equal on the last sort-key prefix attribute `s`
    // plans as two Queries (`s < v`, then `s > v`) when it is the only
    // predicate on `s` (referenced once), a conjunct of the top-level AND
    // chain, and its value is a scalar of the key's type (a mistyped literal
    // would make the key condition a ValidationException, so it keeps
    // v0.8.1's Scan; P15 §4.3). Every other predicate must be query-safe as
    // before. Returns nullopt (Scan) otherwise.
    std::optional<QueryKeyPlan> split_on_not_equal(const std::vector<std::string>& pk,
                                                   const std::vector<std::string>& prefix,
                                                   const KeyTypes* key_types) const {
        if (prefix.empty()) {
            return std::nullopt;
        }
        const std::string& s = prefix.back();
        if (m_where->count(s) != 1) {
            return std::nullopt;
        }
        const Value* v = m_where->ne_key_value(s);
        if (v == nullptr || !key_typed_scalar(*v, s, key_types)) {
            return std::nullopt;
        }
        KeySchemaView view{pk, prefix, &s, NeLeg::lower};
        if (!m_where->query_safe(view)) {
            return std::nullopt;
        }
        return QueryKeyPlan{pk, prefix, s};
    }

    // Whether `v` can stand in a key condition on `name`: a resolved S, N or
    // B, of the declared type when DescribeTable reported one.
    static bool key_typed_scalar(const Value& v, const std::string& name,
                                 const KeyTypes* key_types) {
        if (v.param_ordinal > 0) {
            return false;  // an unbound marker is not a value yet
        }
        std::optional<ScalarType> type;
        switch (v.type) {
            case Value::Type::S:
                type = ScalarType::s;
                break;
            case Value::Type::N:
                type = ScalarType::n;
                break;
            case Value::Type::B:
                type = ScalarType::b;
                break;
            default:
                return false;
        }
        if (key_types != nullptr) {
            if (auto it = key_types->find(name); it != key_types->end()) {
                return it->second == *type;
            }
        }
        return true;
    }

    std::unique_ptr<LogicalExpression> m_where;
};

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_EXPRESSIONS_WHERE_HPP
