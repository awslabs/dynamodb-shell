// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// logical_expression.hpp — abstract base for the WHERE-clause expression tree.
//
// PRIVATE engine header (not installed). SDK-free port of the original
// expressions/logical_expression.hpp (CLogicalExpression). The tree is both
// the parsed representation of a WHERE clause (built by the grammar in W3
// part 2) and the planner input that decides GetItem/Query/Scan and renders
// the KeyConditionExpression / FilterExpression / ConditionExpression strings
// (arch §6, §8).
//
// Ownership: children are held by std::unique_ptr (RAII), replacing the
// original's manual `new`/`delete` and its NEW/FREE leak tracker (D13,
// P2a-02). This also removes the double-free hazard the original guarded
// against by hand in the AND/OR flatten factories.
//
// Template-method pattern (preserved from the original): the public
// serialize() / query_*_expression() / update_delete_condition_check()
// methods wrap the private do_* virtuals and wrap the result in `NOT (...)`
// when the node is negated and the inner result is non-empty.
//
// D12: no assert()/abort()/exit() on user- or data-derived conditions.

#ifndef DDBSH_SRC_EXPRESSIONS_LOGICAL_EXPRESSION_HPP
#define DDBSH_SRC_EXPRESSIONS_LOGICAL_EXPRESSION_HPP

#include <map>
#include <string>
#include <vector>

#include "ddbsh/value.hpp"
#include "expressions/symbol_table.hpp"

namespace ddbsh::engine {

/// A primary-key map (attribute name -> Value) for GetItem/Update/Delete keys.
using KeyMap = std::map<std::string, Value>;

/// The key attributes a WHERE tree is evaluated against for a Query (phase 4a
/// generalizes the original's scalar pk/rk pair to ordered lists for composite
/// GSI keys). `pk` is every partition-key attribute; `sk` is the gap-free
/// sort-key PREFIX that is key-condition-eligible for this clause (computed by
/// Where::plan_query). A leaf renders into the KeyConditionExpression iff its
/// lhs is in pk ∪ sk, else into the FilterExpression. A single-attribute key
/// reduces to `pk == {p}`, `sk == {}` or `{r}` — byte-identical to the old
/// scalar path.
struct KeySchemaView {
    const std::vector<std::string>& pk;
    const std::vector<std::string>& sk;
};

/// Membership test: is `name` one of the attributes in `list`?
inline bool key_contains(const std::vector<std::string>& list, const std::string& name) {
    for (const auto& entry : list) {
        if (entry == name) {
            return true;
        }
    }
    return false;
}

class LogicalExpression {
public:
    LogicalExpression() = default;
    virtual ~LogicalExpression() = default;

    LogicalExpression(const LogicalExpression&) = delete;
    LogicalExpression& operator=(const LogicalExpression&) = delete;

    // ---- serialization (template methods wrapping the do_* virtuals) -------

    /// Serialize to an expression string. A null symbol table renders literal
    /// values (arch §6 no-symbol-table path); a non-null table collects
    /// `#a`/`:v` aliases and the string carries the tokens.
    std::string serialize(SymbolTable* st) {
        return m_negate ? "NOT (" + do_serialize(st) + ")" : do_serialize(st);
    }

    std::string query_key_condition_expression(const KeySchemaView& ks, SymbolTable* st) {
        std::string rv = do_query_key_condition_expression(ks, st);
        if (!rv.empty()) {
            return m_negate ? "NOT (" + rv + ")" : rv;
        }
        return rv;
    }

    std::string query_filter_expression(const KeySchemaView& ks, SymbolTable* st) {
        std::string rv = do_query_filter_expression(ks, st);
        if (!rv.empty()) {
            return m_negate ? "NOT (" + rv + ")" : rv;
        }
        return rv;
    }

    std::string update_delete_condition_check(const std::string& pk, const std::string& rk,
                                              SymbolTable* st, bool top) {
        std::string rv = do_update_delete_condition_check(pk, rk, st, top);
        if (!rv.empty()) {
            return m_negate ? "NOT (" + rv + ")" : rv;
        }
        return rv;
    }

    // ---- negation / grouping state -----------------------------------------

    bool is_negated() const { return m_negate; }
    void negate() { m_negate = !m_negate; }
    void set_paren_group() { m_parens = true; }
    bool is_paren_group() const { return m_parens; }

    /// The concrete node kind, used by the AND/OR flatten factories. Set by
    /// each subclass constructor.
    const std::string& type() const { return m_node_type; }

    // ---- capability / counting probes --------------------------------------

    /// Count of non-conjunction nodes (AND/OR override to recurse).
    virtual int count() const { return 1; }
    /// Count of nodes whose left-hand side equals `lhs`.
    virtual int count(const std::string& lhs) const = 0;
    /// Count of nodes that contribute a GetItem/Update/Delete key component for
    /// `lhs` (a non-negated `=`). Overridden in comparison and AND.
    virtual int count_gud_key(const std::string& /*lhs*/) const { return 0; }
    /// The key map for a GetItem/Update/Delete (comparison and AND override).
    virtual KeyMap get_gud_key(const std::string& /*pk*/, const std::string& /*rk*/) const {
        return {};
    }

    /// True if the whole subtree is legal inside a Query for the given key
    /// schema (arch §8); AND/OR descend, leaves answer for their own lhs. The
    /// view carries the partition-key list and the eligible sort-key prefix
    /// (phase 4a).
    bool query_safe(const KeySchemaView& ks) const { return do_query_safe(ks); }

protected:
    bool m_negate = false;
    bool m_parens = false;
    std::string m_node_type;

private:
    virtual std::string do_serialize(SymbolTable* st) = 0;
    virtual std::string do_query_key_condition_expression(const KeySchemaView& ks,
                                                          SymbolTable* st) = 0;
    virtual std::string do_query_filter_expression(const KeySchemaView& ks, SymbolTable* st) = 0;
    virtual std::string do_update_delete_condition_check(const std::string& pk,
                                                         const std::string& rk, SymbolTable* st,
                                                         bool top) = 0;
    virtual bool do_query_safe(const KeySchemaView& ks) const = 0;
};

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_EXPRESSIONS_LOGICAL_EXPRESSION_HPP
