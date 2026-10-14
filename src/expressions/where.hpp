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
#include <string>
#include <utility>

#include "expressions/logical_expression.hpp"

namespace ddbsh::engine {

class Where {
public:
    explicit Where(std::unique_ptr<LogicalExpression> root) : m_where(std::move(root)) {}

    bool can_getitem(const std::string& pk, const std::string& rk) const {
        if (!rk.empty()) {
            return m_where->count() == 2 && m_where->count_gud_key(pk) == 1 &&
                   m_where->count_gud_key(rk) == 1;
        }
        return m_where->count() == 1 && m_where->count_gud_key(pk) == 1;
    }

    bool can_query_table(const std::string& pk, const std::string& rk) const {
        // Need exactly one PK equality, referenced once, and a query-safe tree.
        if (!(m_where->count_gud_key(pk) == 1 && m_where->count(pk) == 1)) {
            return false;
        }
        return m_where->query_safe(pk, rk);
    }

    bool can_query_index(const std::string& pk, const std::string& rk) const {
        // An index key is not unique, so GetItem is impossible on an index; the
        // query-ability test is can_getitem || can_query_table.
        return can_getitem(pk, rk) || can_query_table(pk, rk);
    }

    bool can_update_item(const std::string& pk, const std::string& rk) const {
        if (!rk.empty()) {
            return m_where->count_gud_key(pk) == 1 && m_where->count_gud_key(rk) == 1;
        }
        return m_where->count_gud_key(pk) == 1;
    }

    bool can_delete_item(const std::string& pk, const std::string& rk) const {
        return can_update_item(pk, rk);
    }

    KeyMap get_gud_key(const std::string& pk, const std::string& rk) const {
        return m_where->get_gud_key(pk, rk);
    }

    std::string query_key_condition_expression(const std::string& pk, const std::string& rk,
                                               SymbolTable* st) const {
        return m_where->query_key_condition_expression(pk, rk, st);
    }

    std::string query_filter_expression(const std::string& pk, const std::string& rk,
                                        SymbolTable* st) const {
        return m_where->query_filter_expression(pk, rk, st);
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
