// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// statement.cpp — public Statement method bodies (PIMPL).
//
// Defined here where engine::ParsedStatement is complete (the public header
// only forward-declares it), so the unique_ptr member's destructor and the
// kind()/explain() accessors compile against the full AST type. The ctors,
// dtor, and move operations are out-of-line for the same reason (a
// header-defaulted dtor would fail against the incomplete Impl).

#include "ddbsh/statement.hpp"

#include <utility>

#include "parser/ast.hpp"

namespace ddbsh {

Statement::Statement() = default;

Statement::Statement(std::unique_ptr<engine::ParsedStatement> ast) : ast_(std::move(ast)) {}

Statement::~Statement() = default;
Statement::Statement(Statement&&) noexcept = default;
Statement& Statement::operator=(Statement&&) noexcept = default;

StatementKind Statement::kind() const {
    return ast_ ? ast_->kind() : StatementKind::empty;
}

bool Statement::explain() const {
    return ast_ ? ast_->explain : false;
}

}  // namespace ddbsh
