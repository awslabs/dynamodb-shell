// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// result.hpp — the library's Result<T> alias.
//
// D12 (ratified): the engine error model is std::expected<T, Error> (C++23).
// No tl::expected fallback — the toolchain matrix ships std::expected
// (gcc >= 12 / clang >= 16 / MSVC 19.3x). See coding-standards.md.

#ifndef DDBSH_RESULT_HPP
#define DDBSH_RESULT_HPP

#include <expected>

#include "ddbsh/error.hpp"

namespace ddbsh {

/// The uniform library result type. A successful call yields a T; a failure
/// yields an Error (never an exception, never a process exit).
template <class T>
using Result = std::expected<T, Error>;

/// Convenience for constructing the error alternative.
inline std::unexpected<Error> fail(Error e) {
    return std::unexpected<Error>(std::move(e));
}

inline std::unexpected<Error> fail(Errc code, std::string message) {
    return std::unexpected<Error>(Error{code, std::move(message), std::nullopt, std::nullopt});
}

}  // namespace ddbsh

#endif  // DDBSH_RESULT_HPP
