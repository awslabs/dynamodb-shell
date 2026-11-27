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
//   * `<>` and `!=` are one operator (P15, D-P15-8 Rule 1). The node keeps the
//     typed spelling for rendering where it is legal (a filter or condition:
//     `#a <> :v` for `<>`, `NOT (#a = :v)` for `!=`), but every planner probe
//     asks what the node MEANS (means_equal / means_not_equal), so the two
//     spellings plan identically. v0.8.1 tested only for a negated `=`, so a
//     `<>` on the sort key reached the KeyConditionExpression, which DynamoDB
//     rejects (B6).
//   * A not-equal on the sort key that the planner splits (KeySchemaView
//     ne_split) renders `#sk < :v` on the lower leg and `#sk > :v` on the
//     upper leg (D-P15-8 Rule 2).

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
        return (m_lhs == lhs && means_equal()) ? 1 : 0;
    }

    const Value* ne_key_value(const std::string& lhs) const override {
        return (m_lhs == lhs && means_not_equal()) ? &m_rhs : nullptr;
    }

    /// `lhs = rhs` in meaning: `=`, or a negated `<>` (`NOT x <> v`).
    bool means_equal() const {
        return (m_op == "=" && !is_negated()) || (m_op == "<>" && is_negated());
    }

    /// `lhs <> rhs` in meaning: `<>`, `!=` (a negated `=`) or `NOT x = v`.
    bool means_not_equal() const {
        return (m_op == "=" && is_negated()) || (m_op == "<>" && !is_negated());
    }

    KeyMap get_gud_key(const std::string& pk, const std::string& rk) const override {
        KeyMap rv;
        if (count_gud_key(pk) || count_gud_key(rk)) {
            rv.emplace(m_lhs, m_rhs);
        }
        return rv;
    }

    void collect_param_slots(std::vector<Value*>& out) override { collect_value_slots(m_rhs, out); }

private:
    std::string do_serialize(SymbolTable* st) override {
        std::string lhs = st ? st->new_attribute(m_lhs) : m_lhs;
        std::string rhs = st ? st->new_value(m_rhs) : serialize_attribute(m_rhs);
        return lhs + " " + m_op + " " + rhs;
    }

    std::string do_query_key_condition_expression(const KeySchemaView& ks,
                                                  SymbolTable* st) override {
        if (!key_contains(ks.pk, m_lhs) && !key_contains(ks.sk, m_lhs)) {
            return "";
        }
        if (is_split(ks)) {
            // One leg of a not-equal split: the range below, or above, the value.
            return render(ks.leg == NeLeg::upper ? ">" : "<", st);
        }
        if (m_op == "<>") {
            // Reached only for a query-safe `<>` on a key, which is a negated one
            // (`NOT x <> v`, i.e. equality): the key condition says `=`.
            return render("=", st);
        }
        return do_serialize(st);
    }

    bool negate_in_key_condition() const override {
        // A negated `=` reaches the key condition only as a split leg, and a
        // negated `<>` only as an equality; both render without `NOT (...)`.
        // Any other negated operator keeps v0.8.1's wrapper.
        return is_negated() && m_op != "=" && m_op != "<>";
    }

    std::string do_query_filter_expression(const KeySchemaView& ks, SymbolTable* st) override {
        if (!key_contains(ks.pk, m_lhs) && !key_contains(ks.sk, m_lhs)) {
            return do_serialize(st);
        }
        return "";
    }

    std::string do_update_delete_condition_check(const std::string& pk, const std::string& rk,
                                                 SymbolTable* st, bool top) override {
        if (!top) {
            return do_serialize(st);
        }
        // A plain key comparison (v0.8.1: a non-negated operator other than
        // `<>`; P15 adds the negated `<>`, which means `=`) is carried as the
        // item key, so it is not repeated in the condition. A not-equal on a
        // key, in either spelling, is kept: `sk <> 5` as `#sk <> :v`, as
        // `sk != 5` is kept as `NOT (#sk = :v)` (D-P15-8 Rule 1).
        const bool plain_key_comparison = m_op == "<>" ? is_negated() : !is_negated();
        if ((m_lhs == pk || m_lhs == rk) && plain_key_comparison) {
            return "";
        }
        return do_serialize(st);
    }

    bool do_query_safe(const KeySchemaView& ks) const override {
        if (!key_contains(ks.pk, m_lhs) && !key_contains(ks.sk, m_lhs)) {
            return true;
        }
        if (key_contains(ks.pk, m_lhs)) {
            // A partition-key predicate is query-legal only as an equality.
            return means_equal();
        }
        // lhs is a sort-key prefix attr: a not-equal (either spelling) is
        // legal only as the attribute the planner splits on. Every other
        // operator keeps v0.8.1's answer.
        if (means_not_equal()) {
            return is_split(ks);
        }
        return true;
    }

    // This node is the not-equal the view splits on.
    bool is_split(const KeySchemaView& ks) const {
        return ks.ne_split != nullptr && *ks.ne_split == m_lhs && means_not_equal();
    }

    // `lhs <op> rhs` with the given operator (aliases minted as do_serialize).
    std::string render(const char* op, SymbolTable* st) {
        std::string lhs = st ? st->new_attribute(m_lhs) : m_lhs;
        std::string rhs = st ? st->new_value(m_rhs) : serialize_attribute(m_rhs);
        return lhs + " " + op + " " + rhs;
    }

    std::string m_lhs;
    Value m_rhs;
    std::string m_op;
};

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_EXPRESSIONS_LOGICAL_COMPARISON_HPP
