// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// logical_or.hpp — the OR disjunction node.
//
// PRIVATE engine header. SDK-free port of expressions/logical_or.hpp
// (CLogicalOr), carrying forward the Step-1 cleanup that is now the oracle's
// behavior (see expressions/logical_or.hpp in the oracle-archive-v0.8.1 tag):
//   * An OR never contributes to a KeyConditionExpression (a KCE is a single
//     anded chain), so the ENTIRE OR subtree is emitted into the
//     FilterExpression — including key-attribute comparisons that the pristine
//     original silently dropped.
//   * query_safe() returns false if the OR subtree references the partition or
//     sort key: a Query cannot carry a key attribute in its FilterExpression,
//     so such a predicate forces a Scan.
//
// This is the B-CLASS carry-forward divergence noted in the Phase-2b scope
// (and arch §12): a PK `=` under a top-level OR plans as a Scan here (and in
// the oracle), not the pristine original's behavior. The oracle `.exp`
// goldens already reflect this fixed behavior, so porting it keeps golden
// parity. Children owned by unique_ptr (RAII; no manual double-free guard).

#ifndef DDBSH_SRC_EXPRESSIONS_LOGICAL_OR_HPP
#define DDBSH_SRC_EXPRESSIONS_LOGICAL_OR_HPP

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "expressions/logical_expression.hpp"

namespace ddbsh::engine {

class LogicalOr : public LogicalExpression {
public:
    LogicalOr(std::unique_ptr<LogicalExpression> e1, std::unique_ptr<LogicalExpression> e2) {
        m_node_type = "CLogicalOr";
        m_exprlist.push_back(std::move(e1));
        m_exprlist.push_back(std::move(e2));
    }

    explicit LogicalOr(std::unique_ptr<LogicalExpression> e1) {
        m_node_type = "CLogicalOr";
        m_exprlist.push_back(std::move(e1));
    }

    /// Flatten factory: combine two expressions under OR, splicing nested ORs
    /// where legal, appending to a non-negated OR, else wrapping in a fresh OR.
    static std::unique_ptr<LogicalExpression> make_or(std::unique_ptr<LogicalExpression> e1,
                                                      std::unique_ptr<LogicalExpression> e2) {
        if (e1->is_negated() == e2->is_negated() && e1->type() == "CLogicalOr" &&
            e2->type() == "CLogicalOr" && !e1->is_paren_group() && !e2->is_paren_group()) {
            auto* o1 = static_cast<LogicalOr*>(e1.get());
            auto* o2 = static_cast<LogicalOr*>(e2.get());
            for (auto& child : o2->m_exprlist) {
                o1->m_exprlist.push_back(std::move(child));
            }
            return e1;
        }
        if (!e1->is_negated() && e1->type() == "CLogicalOr") {
            auto* o1 = static_cast<LogicalOr*>(e1.get());
            o1->m_exprlist.push_back(std::move(e2));
            return e1;
        }
        return std::make_unique<LogicalOr>(std::move(e1), std::move(e2));
    }

    int count() const override {
        int ct = 0;
        for (const auto& child : m_exprlist) {
            ct += child->count();
        }
        return ct;
    }

    int count(const std::string& lhs) const override {
        int ct = 0;
        for (const auto& child : m_exprlist) {
            ct += child->count(lhs);
        }
        return ct;
    }

private:
    std::string do_serialize(SymbolTable* st) override {
        std::string rv = is_paren_group() ? "(" : "";
        bool first = true;
        for (const auto& child : m_exprlist) {
            if (!first) {
                rv += " OR ";
            }
            first = false;
            rv += child->serialize(st);
        }
        if (is_paren_group()) {
            rv += ")";
        }
        return rv;
    }

    std::string do_query_key_condition_expression(const std::string& /*pk*/,
                                                  const std::string& /*rk*/,
                                                  SymbolTable* /*st*/) override {
        // Nothing under an OR can be part of a KeyConditionExpression.
        return "";
    }

    std::string do_query_filter_expression(const std::string& /*pk*/, const std::string& /*rk*/,
                                           SymbolTable* st) override {
        // The entire OR subtree belongs in the FilterExpression.
        return do_serialize(st);
    }

    std::string do_update_delete_condition_check(const std::string& pk, const std::string& rk,
                                                 SymbolTable* st, bool /*top*/) override {
        std::string rv;
        bool first = true;
        for (const auto& child : m_exprlist) {
            std::string part = child->update_delete_condition_check(pk, rk, st, false);
            if (!part.empty()) {
                if (!first) {
                    rv += " OR ";
                }
                first = false;
                rv += part;
            }
        }
        if (is_paren_group()) {
            rv = "(" + rv + ")";
        }
        return rv;
    }

    bool do_query_safe(const std::string& pk, const std::string& rk) const override {
        // An OR subtree that references a key attribute forces a Scan.
        if (count(pk) > 0 || (!rk.empty() && count(rk) > 0)) {
            return false;
        }
        for (const auto& child : m_exprlist) {
            if (!child->query_safe(pk, rk)) {
                return false;
            }
        }
        return true;
    }

    std::vector<std::unique_ptr<LogicalExpression>> m_exprlist;
};

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_EXPRESSIONS_LOGICAL_OR_HPP
