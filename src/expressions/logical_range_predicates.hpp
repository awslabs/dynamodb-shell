// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// logical_range_predicates.hpp — BETWEEN and begins_with().
//
// PRIVATE engine header. SDK-free port of expressions/logical_between.hpp and
// expressions/logical_begins_with.hpp. Both are range-key-legal: a
// non-negated predicate on the range key belongs in the
// KeyConditionExpression; anything else is a FilterExpression (arch §6, §8).

#ifndef DDBSH_SRC_EXPRESSIONS_LOGICAL_RANGE_PREDICATES_HPP
#define DDBSH_SRC_EXPRESSIONS_LOGICAL_RANGE_PREDICATES_HPP

#include <string>
#include <utility>

#include "expressions/attribute_serialize.hpp"
#include "expressions/logical_expression.hpp"

namespace ddbsh::engine {

/// `lhs BETWEEN lo AND hi`.
class LogicalBetween : public LogicalExpression {
public:
    LogicalBetween(std::string lhs, Value lo, Value hi)
        : m_lhs(std::move(lhs)), m_lo(std::move(lo)), m_hi(std::move(hi)) {
        m_node_type = "CLogicalBetween";
    }

    int count(const std::string& lhs) const override { return m_lhs == lhs ? 1 : 0; }

    int count_query_rk(const std::string& lhs) const override {
        return is_negated() ? 0 : count(lhs);
    }

private:
    std::string do_serialize(SymbolTable* st) override {
        std::string lhs = st ? st->new_attribute(m_lhs) : m_lhs;
        std::string lo = st ? st->new_value(m_lo) : serialize_attribute(m_lo);
        std::string hi = st ? st->new_value(m_hi) : serialize_attribute(m_hi);
        return lhs + " BETWEEN " + lo + " AND " + hi;
    }

    std::string do_query_key_condition_expression(const std::string& /*pk*/, const std::string& rk,
                                                  SymbolTable* st) override {
        if (m_lhs == rk && !is_negated()) {
            return do_serialize(st);
        }
        return "";
    }

    std::string do_query_filter_expression(const std::string& /*pk*/, const std::string& rk,
                                           SymbolTable* st) override {
        if (m_lhs != rk) {
            return do_serialize(st);
        }
        return "";
    }

    std::string do_update_delete_condition_check(const std::string& pk, const std::string& rk,
                                                 SymbolTable* st, bool top) override {
        if (!top) {
            return do_serialize(st);
        }
        if ((m_lhs == pk || m_lhs == rk) && !is_negated()) {
            return "";
        }
        return do_serialize(st);
    }

    bool do_query_safe(const std::string& pk, const std::string& rk) const override {
        if (m_lhs != pk && m_lhs != rk) {
            return true;
        }
        return m_lhs == rk && !is_negated();
    }

    std::string m_lhs;
    Value m_lo;
    Value m_hi;
};

/// `begins_with(lhs, prefix)`.
class LogicalBeginsWith : public LogicalExpression {
public:
    LogicalBeginsWith(std::string lhs, Value prefix)
        : m_lhs(std::move(lhs)), m_prefix(std::move(prefix)) {
        m_node_type = "CLogicalBeginsWith";
    }

    int count(const std::string& lhs) const override { return m_lhs == lhs ? 1 : 0; }

    int count_query_rk(const std::string& lhs) const override {
        return is_negated() ? 0 : count(lhs);
    }

private:
    std::string do_serialize(SymbolTable* st) override {
        std::string lhs = st ? st->new_attribute(m_lhs) : m_lhs;
        std::string rhs =
            st ? st->new_value(m_prefix) : ("\"" + serialize_attribute(m_prefix) + "\"");
        return "begins_with(" + lhs + ", " + rhs + ")";
    }

    std::string do_query_key_condition_expression(const std::string& /*pk*/, const std::string& rk,
                                                  SymbolTable* st) override {
        if (m_lhs == rk && !is_negated()) {
            return do_serialize(st);
        }
        return "";
    }

    std::string do_query_filter_expression(const std::string& /*pk*/, const std::string& rk,
                                           SymbolTable* st) override {
        if (m_lhs != rk) {
            return do_serialize(st);
        }
        return "";
    }

    std::string do_update_delete_condition_check(const std::string& pk, const std::string& rk,
                                                 SymbolTable* st, bool top) override {
        if (!top) {
            return do_serialize(st);
        }
        if ((m_lhs == pk || m_lhs == rk) && !is_negated()) {
            return "";
        }
        return do_serialize(st);
    }

    bool do_query_safe(const std::string& pk, const std::string& rk) const override {
        if (m_lhs != pk && m_lhs != rk) {
            return true;
        }
        return m_lhs == rk && !is_negated();
    }

    std::string m_lhs;
    Value m_prefix;
};

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_EXPRESSIONS_LOGICAL_RANGE_PREDICATES_HPP
