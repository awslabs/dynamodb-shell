// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// param_bind.cpp — see param_bind.hpp.

#include "parser/param_bind.hpp"

#include <variant>

#include "expressions/logical_expression.hpp"  // LogicalExpression::collect_value_slots
#include "expressions/where.hpp"

namespace ddbsh::engine {

namespace {

void collect_set_operands(std::vector<UpdateSetElement>& set, std::vector<Value*>& out) {
    for (auto& element : set) {
        if (element.rhs_value.has_value()) {
            LogicalExpression::collect_value_slots(*element.rhs_value, out);
        }
        if (element.if_not_exists_value.has_value()) {
            LogicalExpression::collect_value_slots(*element.if_not_exists_value, out);
        }
    }
}

void collect_node(CommandNode& command, std::vector<Value*>& out);

struct CollectVisitor {
    std::vector<Value*>& out;

    void operator()(InsertCmd& cmd) {
        for (auto& row : cmd.rows) {
            for (auto& value : row) {
                LogicalExpression::collect_value_slots(value, out);
            }
        }
    }
    void operator()(SelectCmd& cmd) {
        if (cmd.where) {
            cmd.where->collect_param_slots(out);
        }
        if (cmd.search_vector.has_value()) {
            LogicalExpression::collect_value_slots(*cmd.search_vector, out);
        }
    }
    void operator()(UpdateCmd& cmd) {
        collect_set_operands(cmd.set, out);
        if (cmd.where) {
            cmd.where->collect_param_slots(out);
        }
    }
    void operator()(DeleteCmd& cmd) {
        if (cmd.where) {
            cmd.where->collect_param_slots(out);
        }
    }
    void operator()(TransactCmd& cmd) {
        for (auto& item : cmd.items) {
            collect_node(item, out);
        }
    }

    // Every non-DML family (and families that cannot carry a `?` value marker)
    // contributes no slots.
    template <class T>
    void operator()(T& /*other*/) {}
};

void collect_node(CommandNode& command, std::vector<Value*>& out) {
    std::visit(CollectVisitor{out}, command);
}

}  // namespace

void collect_param_slots(CommandNode& command, std::vector<Value*>& out) {
    collect_node(command, out);
}

std::size_t count_param_markers(const ParsedStatement& statement) {
    // Reuse the single mutable traversal to count markers on a const statement.
    // The const_cast is safe: collect_param_slots only reads the AST and takes
    // the addresses of marker Values — it never mutates the parsed command.
    std::vector<Value*> slots;
    collect_param_slots(const_cast<CommandNode&>(statement.command), slots);
    return slots.size();
}

}  // namespace ddbsh::engine
