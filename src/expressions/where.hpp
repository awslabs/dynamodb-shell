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
//
// D12: the original's internal-invariant assert()s (e.g. "pk not empty",
// "can_query_table implies !can_getitem") are dropped; the planner always
// probes in order (getitem, then query, then scan), so the invariants hold by
// construction without aborting on unexpected input.

#ifndef DDBSH_SRC_EXPRESSIONS_WHERE_HPP
#define DDBSH_SRC_EXPRESSIONS_WHERE_HPP

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "expressions/logical_expression.hpp"

namespace ddbsh::engine {

/// The chosen Query key plan (phase 4a): the partition-key attributes plus the
/// gap-free, key-legal sort-key prefix. Returned by Where::plan_query; its
/// absence means the clause is not query-able against this schema and the
/// caller must Scan. For a single-key schema this reduces to pk=={p},
/// sk_prefix=={} or {r}.
struct QueryKeyPlan {
    std::vector<std::string> pk;
    std::vector<std::string> sk_prefix;
};

class Where {
public:
    explicit Where(std::unique_ptr<LogicalExpression> root) : m_where(std::move(root)) {}

    // Base table only; EXACTLY the full key and nothing else. The base key is
    // single (|pk|==1, |rk|<=1); a composite key never GetItems.
    bool can_getitem(const std::vector<std::string>& pk,
                     const std::vector<std::string>& rk) const {
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
    std::optional<QueryKeyPlan> plan_query(const std::vector<std::string>& pk,
                                           const std::vector<std::string>& rk) const {
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
        if (!m_where->query_safe(view)) {
            return std::nullopt;
        }
        return QueryKeyPlan{pk, prefix};
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

    std::string query_key_condition_expression(const QueryKeyPlan& plan, SymbolTable* st) const {
        KeySchemaView view{plan.pk, plan.sk_prefix};
        return m_where->query_key_condition_expression(view, st);
    }

    std::string query_filter_expression(const QueryKeyPlan& plan, SymbolTable* st) const {
        KeySchemaView view{plan.pk, plan.sk_prefix};
        return m_where->query_filter_expression(view, st);
    }

    std::string scan_filter_expression(SymbolTable* st) const { return m_where->serialize(st); }

    std::string update_delete_condition_check(const std::string& pk, const std::string& rk,
                                              SymbolTable* st) const {
        return m_where->update_delete_condition_check(pk, rk, st, /*top=*/true);
    }

    /// Direct access to the root (for probes the planner composes itself).
    const LogicalExpression& root() const { return *m_where; }

private:
    std::unique_ptr<LogicalExpression> m_where;
};

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_EXPRESSIONS_WHERE_HPP
