// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// logical_comparison.hpp — the `lhs <op> rhs` comparison predicate.
//
// PRIVATE engine header. SDK-free port of expressions/logical_comparison.hpp
// (CLogicalComparison). Fidelity points (arch §6, §14 item 4):
//   * `!=` is stored as `=` plus a toggled negate flag, so it renders as
//     `NOT (#a = :v)`.
//   * A non-negated `=` on the partition/sort key is the only form that
//     counts toward a GetItem/Update/Delete key.
//   * The node decides for itself whether it belongs in the
//     KeyConditionExpression (lhs is pk/rk) or the FilterExpression.

#ifndef DDBSH_SRC_EXPRESSIONS_LOGICAL_COMPARISON_HPP
#define DDBSH_SRC_EXPRESSIONS_LOGICAL_COMPARISON_HPP

#include <string>
#include <utility>

#include "expressions/attribute_serialize.hpp"
#include "expressions/logical_expression.hpp"

namespace ddbsh::engine {

class LogicalComparison : public LogicalExpression {
public:
    LogicalComparison(std::string lhs, const std::string& op, Value rhs)
        : m_lhs(std::move(lhs)), m_rhs(std::move(rhs)) {
        m_node_type = "CLogicalComparison";
        if (op == "!=") {
            // `!=` desugars to a negated `=` (arch §14 item 4).
            m_op = "=";
            negate();
        } else {
            m_op = op;
        }
    }

    int count(const std::string& lhs) const override { return m_lhs == lhs ? 1 : 0; }

    int count_gud_key(const std::string& lhs) const override {
        return (m_lhs == lhs && !is_negated() && m_op == "=") ? 1 : 0;
    }

    int count_query_rk(const std::string& lhs) const override {
        return (m_lhs == lhs && !is_negated()) ? 1 : 0;
    }

    KeyMap get_gud_key(const std::string& pk, const std::string& rk) const override {
        KeyMap rv;
        if (count_gud_key(pk) || count_gud_key(rk)) {
            rv.emplace(m_lhs, m_rhs);
        }
        return rv;
    }

private:
    std::string do_serialize(SymbolTable* st) override {
        std::string lhs = st ? st->new_attribute(m_lhs) : m_lhs;
        std::string rhs = st ? st->new_value(m_rhs) : serialize_attribute(m_rhs);
        return lhs + " " + m_op + " " + rhs;
    }

    std::string do_query_key_condition_expression(const std::string& pk, const std::string& rk,
                                                  SymbolTable* st) override {
        if (m_lhs == pk || m_lhs == rk) {
            return do_serialize(st);
        }
        return "";
    }

    std::string do_query_filter_expression(const std::string& pk, const std::string& rk,
                                           SymbolTable* st) override {
        if (!(pk == m_lhs || rk == m_lhs)) {
            return do_serialize(st);
        }
        return "";
    }

    std::string do_update_delete_condition_check(const std::string& pk, const std::string& rk,
                                                 SymbolTable* st, bool top) override {
        if (!top) {
            return do_serialize(st);
        }
        // A non-negated key equality is already carried as the item key, so it
        // need not be repeated in the condition.
        if ((m_lhs == pk || m_lhs == rk) && !is_negated()) {
            return "";
        }
        return do_serialize(st);
    }

    bool do_query_safe(const std::string& pk, const std::string& rk) const override {
        if (m_lhs != pk && m_lhs != rk) {
            return true;
        }
        if (m_lhs == pk) {
            // A partition-key predicate is query-legal only as a non-negated `=`.
            return m_op == "=" && !is_negated();
        }
        // m_lhs == rk: a negated `=` (i.e. `!=`) on the range key is not legal.
        if (m_op == "=" && is_negated()) {
            return false;
        }
        return true;
    }

    std::string m_lhs;
    Value m_rhs;
    std::string m_op;
};

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_EXPRESSIONS_LOGICAL_COMPARISON_HPP
