// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// logical_filter_predicates.hpp — IN, contains(), attribute_type(),
// attribute_exists() / attribute_not_exists().
//
// PRIVATE engine header. SDK-free port of expressions/logical_in.hpp,
// logical_contains.hpp, logical_attribute_type.hpp, logical_attribute_exists.hpp.
// All four are ALWAYS FilterExpression / ConditionExpression content — never a
// KeyConditionExpression — so each forces a Scan when applied to a key
// attribute (query_safe is false when lhs is pk/rk). attribute_not_exists is a
// negated attribute_exists (arch §6, §14 item 4).

#ifndef DDBSH_SRC_EXPRESSIONS_LOGICAL_FILTER_PREDICATES_HPP
#define DDBSH_SRC_EXPRESSIONS_LOGICAL_FILTER_PREDICATES_HPP

#include <string>
#include <utility>
#include <vector>

#include "expressions/attribute_serialize.hpp"
#include "expressions/logical_expression.hpp"

namespace ddbsh::engine {

/// `lhs IN (v1, v2, ...)`.
class LogicalIn : public LogicalExpression {
public:
    LogicalIn(std::string lhs, std::vector<Value> rhs)
        : m_lhs(std::move(lhs)), m_rhs(std::move(rhs)) {
        m_node_type = "CLogicalIn";
    }

    int count(const std::string& lhs) const override { return m_lhs == lhs ? 1 : 0; }

private:
    std::string do_serialize(SymbolTable* st) override {
        std::string rv = (st ? st->new_attribute(m_lhs) : m_lhs) + " IN (";
        bool first = true;
        for (const auto& value : m_rhs) {
            if (!first) {
                rv += ", ";
            }
            first = false;
            rv += st ? st->new_value(value) : serialize_attribute(value);
        }
        rv += ")";
        return rv;
    }

    std::string do_query_key_condition_expression(const std::string& /*pk*/,
                                                  const std::string& /*rk*/,
                                                  SymbolTable* /*st*/) override {
        return "";
    }

    std::string do_query_filter_expression(const std::string& /*pk*/, const std::string& /*rk*/,
                                           SymbolTable* st) override {
        return do_serialize(st);
    }

    std::string do_update_delete_condition_check(const std::string& /*pk*/,
                                                 const std::string& /*rk*/, SymbolTable* st,
                                                 bool /*top*/) override {
        return do_serialize(st);
    }

    bool do_query_safe(const std::string& pk, const std::string& rk) const override {
        return m_lhs != pk && m_lhs != rk;
    }

    std::string m_lhs;
    std::vector<Value> m_rhs;
};

/// `contains(lhs, operand)`.
class LogicalContains : public LogicalExpression {
public:
    LogicalContains(std::string lhs, Value operand)
        : m_lhs(std::move(lhs)), m_operand(std::move(operand)) {
        m_node_type = "CLogicalContains";
    }

    int count(const std::string& lhs) const override { return m_lhs == lhs ? 1 : 0; }

private:
    std::string do_serialize(SymbolTable* st) override {
        std::string lhs = st ? st->new_attribute(m_lhs) : m_lhs;
        std::string rhs =
            st ? st->new_value(m_operand) : ("\"" + serialize_attribute(m_operand) + "\"");
        return "contains(" + lhs + ", " + rhs + ")";
    }

    std::string do_query_key_condition_expression(const std::string& /*pk*/,
                                                  const std::string& /*rk*/,
                                                  SymbolTable* /*st*/) override {
        return "";
    }

    std::string do_query_filter_expression(const std::string& /*pk*/, const std::string& /*rk*/,
                                           SymbolTable* st) override {
        return do_serialize(st);
    }

    std::string do_update_delete_condition_check(const std::string& /*pk*/,
                                                 const std::string& /*rk*/, SymbolTable* st,
                                                 bool /*top*/) override {
        return do_serialize(st);
    }

    bool do_query_safe(const std::string& pk, const std::string& rk) const override {
        return m_lhs != pk && m_lhs != rk;
    }

    std::string m_lhs;
    Value m_operand;
};

/// `attribute_type(lhs, type)` where `type` is one of "N"/"S"/"B"/... (the
/// DynamoDB scalar-type token, supplied by the grammar).
class LogicalAttributeType : public LogicalExpression {
public:
    LogicalAttributeType(std::string lhs, std::string type)
        : m_lhs(std::move(lhs)), m_type(std::move(type)) {
        m_node_type = "CLogicalAttributeType";
    }

    int count(const std::string& lhs) const override { return m_lhs == lhs ? 1 : 0; }

private:
    std::string do_serialize(SymbolTable* st) override {
        std::string lhs = st ? st->new_attribute(m_lhs) : m_lhs;
        std::string type = st ? st->new_value(Value::string_value(m_type)) : m_type;
        return "attribute_type(" + lhs + ", " + type + ")";
    }

    std::string do_query_key_condition_expression(const std::string& /*pk*/,
                                                  const std::string& /*rk*/,
                                                  SymbolTable* /*st*/) override {
        return "";
    }

    std::string do_query_filter_expression(const std::string& /*pk*/, const std::string& /*rk*/,
                                           SymbolTable* st) override {
        return do_serialize(st);
    }

    std::string do_update_delete_condition_check(const std::string& /*pk*/,
                                                 const std::string& /*rk*/, SymbolTable* st,
                                                 bool /*top*/) override {
        return do_serialize(st);
    }

    bool do_query_safe(const std::string& pk, const std::string& rk) const override {
        return m_lhs != pk && m_lhs != rk;
    }

    std::string m_lhs;
    std::string m_type;
};

/// `attribute_exists(lhs)` (sense=true) / `attribute_not_exists(lhs)`
/// (sense=false, stored as a negated attribute_exists).
class LogicalAttributeExists : public LogicalExpression {
public:
    LogicalAttributeExists(std::string lhs, bool sense) : m_lhs(std::move(lhs)) {
        // sense==false (attribute_not_exists) sets the negate flag.
        m_negate = !sense;
        m_node_type = "CLogicalAttributeExists";
    }

    int count(const std::string& lhs) const override { return m_lhs == lhs ? 1 : 0; }

private:
    std::string do_serialize(SymbolTable* st) override {
        std::string lhs = st ? st->new_attribute(m_lhs) : m_lhs;
        return "attribute_exists(" + lhs + ")";
    }

    std::string do_query_key_condition_expression(const std::string& /*pk*/,
                                                  const std::string& /*rk*/,
                                                  SymbolTable* /*st*/) override {
        return "";
    }

    std::string do_query_filter_expression(const std::string& /*pk*/, const std::string& /*rk*/,
                                           SymbolTable* st) override {
        return do_serialize(st);
    }

    std::string do_update_delete_condition_check(const std::string& /*pk*/,
                                                 const std::string& /*rk*/, SymbolTable* st,
                                                 bool /*top*/) override {
        return do_serialize(st);
    }

    bool do_query_safe(const std::string& pk, const std::string& rk) const override {
        return m_lhs != pk && m_lhs != rk;
    }

    std::string m_lhs;
};

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_EXPRESSIONS_LOGICAL_FILTER_PREDICATES_HPP
