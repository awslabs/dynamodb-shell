// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// error.hpp — the library's public error type.
//
// Part of the engine library's SDK-free public API surface (D11): no AWS SDK
// type appears here. The library never throws across this boundary and never
// calls exit()/abort()/assert() on caller- or data-derived conditions (D12);
// all such failures are carried by value in an Error and returned via
// Result<T> (see result.hpp).

#ifndef DDBSH_ERROR_HPP
#define DDBSH_ERROR_HPP

#include <optional>
#include <string>

namespace ddbsh {

/// Categorical error code. The byte-exact user-facing string lives in
/// Error::message (tenet 5); code is for programmatic dispatch (e.g. CLI exit
/// codes). Extended per sub-phase as more command families land.
enum class Errc {
    parse_error,
    no_such_table,
    table_already_exists,
    conditional_check_failed,
    service_error,
    invalid_argument,
    cancelled,
    not_reducible_to_getitem,
    not_implemented,
    internal_error,
};

/// A library error, carried by value. `message` is the byte-exact string the
/// original ddbsh prints for the corresponding failure, so golden `.exp`
/// output is unchanged when the CLI/Renderer emits it. `service_error_name`
/// and `request_id` are populated from IDDBClient responses when available.
struct Error {
    Errc code = Errc::internal_error;
    std::string message;
    std::optional<std::string> service_error_name;
    std::optional<std::string> request_id;
};

}  // namespace ddbsh

#endif  // DDBSH_ERROR_HPP
