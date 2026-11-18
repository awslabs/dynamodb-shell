// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// session_internal.hpp — engine-internal seam into Session (D-P5-1).
//
// PRIVATE engine header (not installed). It exposes the narrow access the
// engine's own connection layer (src/connection/connection.cpp) needs to drive
// a lazy SELECT cursor over a Session: the injected client and the next
// per-statement alias prefix, plus a one-call helper that opens a streaming
// cursor for a row SELECT. None of this is part of the public, SDK-free
// embedder surface (ddbsh/session.hpp / ddbsh/connection.hpp); it lives here so
// the public headers stay clean and the Session internals stay encapsulated
// (Session befriends only engine::SessionAccess).

#ifndef DDBSH_SRC_SESSION_INTERNAL_HPP
#define DDBSH_SRC_SESSION_INTERNAL_HPP

#include <optional>
#include <string>

#include "ddbsh/iddbclient.hpp"
#include "ddbsh/result.hpp"
#include "ddbsh/session.hpp"
#include "ddbsh/statement.hpp"
#include "execute/select_cursor.hpp"

namespace ddbsh::engine {

/// Friend-of-Session accessor. The methods are defined in session.cpp, where
/// Session::Impl is complete.
struct SessionAccess {
    static IDDBClient& client(Session& session);
    static std::string next_alias_prefix(Session& session);
};

/// Open a lazy streaming cursor for `stmt` IF it is a non-EXPLAIN, non-vector
/// row SELECT (get_item/query/scan). Returns:
///   * a populated optional   — a cursor the caller drives with next_page();
///   * an empty optional      — `stmt` is NOT such a SELECT (EXPLAIN, a vector
///                              NEAREST search, or any non-SELECT): the caller
///                              falls back to the buffering Session::execute;
///   * an Error               — planning failed (e.g. table not found), exactly
///                              as the buffering path would have failed.
/// On the cursor path it consumes one alias prefix from the session, matching
/// the buffering path's per-statement prefix consumption so a mixed batch's
/// EXPLAIN alias numbering is unchanged.
Result<std::optional<SelectRowCursor>> open_select_cursor_for(Session& session,
                                                              const Statement& stmt);

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_SESSION_INTERNAL_HPP
