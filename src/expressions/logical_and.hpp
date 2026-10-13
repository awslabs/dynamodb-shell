// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// logical_and.hpp — the AND conjunction node.
//
// PRIVATE engine header. SDK-free port of expressions/logical_and.hpp
// (CLogicalAnd). Children are owned by std::unique_ptr; the flatten factory
// make_and() merges same-type, same-negation, non-parenthesized ANDs into one
// node. Because ownership is RAII, the original's hand-written double-free
// guard (clear-then-delete the spliced child) is unnecessary — moved-from
// unique_ptrs are empty and destroying the spent node frees nothing twice.
//
// Key-condition behavior (arch §6, §8): an AND joins its children's non-empty
// KeyConditionExpression / FilterExpression / condition-check fragments with
// " AND ", and sums their GetItem/Update/Delete key contributions.

#ifndef DDBSH_SRC_EXPRESSIONS_LOGICAL_AND_HPP
#define DDBSH_SRC_EXPRESSIONS_LOGICAL_AND_HPP

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "expressions/logical_expression.hpp"

namespace ddbsh::engine {

class LogicalAnd : public LogicalExpression {
public:
    LogicalAnd(std::unique_ptr<LogicalExpression> e1, std::unique_ptr<LogicalExpression> e2) {
        m_node_type = "CLogicalAnd";
        m_exprlist.push_back(std::move(e1));
        m_exprlist.push_back(std::move(e2));
    }

    explicit LogicalAnd(std::unique_ptr<LogicalExpression> e1) {
        m_node_type = "CLogicalAnd";
        m_exprlist.push_back(std::move(e1));
    }

    /// Flatten factory: combine two expressions under AND, splicing nested
    /// ANDs where legal (same negation, both AND, neither parenthesized), or
    /// appending to a non-negated AND, else wrapping in a fresh AND.
    static std::unique_ptr<LogicalExpression> make_and(std::unique_ptr<LogicalExpression> e1,
                                                       std::unique_ptr<LogicalExpression> e2) {
        if (e1->is_negated() == e2->is_negated() && e1->type() == "CLogicalAnd" &&
            e2->type() == "CLogicalAnd" && !e1->is_paren_group() && !e2->is_paren_group()) {
            auto* a1 = static_cast<LogicalAnd*>(e1.get());
            auto* a2 = static_cast<LogicalAnd*>(e2.get());
            for (auto& child : a2->m_exprlist) {
                a1->m_exprlist.push_back(std::move(child));
            }
            return e1;  // a2 (now holding only moved-from entries) is destroyed
        }
        if (!e1->is_negated() && e1->type() == "CLogicalAnd") {
            auto* a1 = static_cast<LogicalAnd*>(e1.get());
            a1->m_exprlist.push_back(std::move(e2));
            return e1;
        }
        return std::make_unique<LogicalAnd>(std::move(e1), std::move(e2));
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

    int count_gud_key(const std::string& lhs) const override {
        int ct = 0;
        for (const auto& child : m_exprlist) {
            ct += child->count_gud_key(lhs);
        }
        return ct;
    }

    int count_query_rk(const std::string& lhs) const override {
        int ct = 0;
        for (const auto& child : m_exprlist) {
            ct += child->count_query_rk(lhs);
        }
        return ct;
    }

    KeyMap get_gud_key(const std::string& pk, const std::string& rk) const override {
        KeyMap rv;
        for (const auto& child : m_exprlist) {
            KeyMap part = child->get_gud_key(pk, rk);
            rv.insert(part.begin(), part.end());
        }
        return rv;
    }

private:
    std::string do_serialize(SymbolTable* st) override {
        std::string rv = is_paren_group() ? "(" : "";
        bool first = true;
        for (const auto& child : m_exprlist) {
            if (!first) {
                rv += " AND ";
            }
            first = false;
            rv += child->serialize(st);
        }
        if (is_paren_group()) {
            rv += ")";
        }
        return rv;
    }

    std::string do_query_key_condition_expression(const std::string& pk, const std::string& rk,
                                                  SymbolTable* st) override {
        return join(pk, rk, st, &LogicalExpression::query_key_condition_expression);
    }

    std::string do_query_filter_expression(const std::string& pk, const std::string& rk,
                                           SymbolTable* st) override {
        return join(pk, rk, st, &LogicalExpression::query_filter_expression);
    }

    std::string do_update_delete_condition_check(const std::string& pk, const std::string& rk,
                                                 SymbolTable* st, bool top) override {
        std::string rv;
        bool first = true;
        for (const auto& child : m_exprlist) {
            std::string part = child->update_delete_condition_check(pk, rk, st, top);
            if (!part.empty()) {
                if (!first) {
                    rv += " AND ";
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
        for (const auto& child : m_exprlist) {
            if (!child->query_safe(pk, rk)) {
                return false;
            }
        }
        return true;
    }

    // Shared join for key-condition / filter: concatenate non-empty child
    // fragments with " AND ", wrapping in parens for a paren group.
    using ChildExpr = std::string (LogicalExpression::*)(const std::string&, const std::string&,
                                                         SymbolTable*);
    std::string join(const std::string& pk, const std::string& rk, SymbolTable* st, ChildExpr fn) {
        std::string rv;
        bool first = true;
        for (const auto& child : m_exprlist) {
            std::string part = (child.get()->*fn)(pk, rk, st);
            if (!part.empty()) {
                if (!first) {
                    rv += " AND ";
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

    std::vector<std::unique_ptr<LogicalExpression>> m_exprlist;
};

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_EXPRESSIONS_LOGICAL_AND_HPP
