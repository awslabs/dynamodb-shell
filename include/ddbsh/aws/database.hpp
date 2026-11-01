// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// database.hpp — the one-object AWS-backed embedding entry point (P4d, D2/D3).
//
// `ddbsh::aws::open(region)` returns a ready-to-use Database: it resolves
// region/endpoint/credentials, manages the AWS SDK lifecycle internally
// (reference-counted — the embedder never writes AwsApiGuard), constructs the
// shared AwsDDBClient, and owns a ddbsh::Connection over it. The Database
// forwards the full connection-handle surface (exec / query / reconnect /
// options), so a third party runs a statement and walks rows with ONE object:
//
//     auto db = ddbsh::aws::open("us-east-1");
//     if (!db) { std::fputs(db.error().message.c_str(), stderr); return 1; }
//     for (const ddbsh::Row& r : *db->query("select * from t;"))
//         use(r.get_string("title"));
//
// SDK BOUNDARY (D26 / D-P4d-13): this is the SDK-dependent zone — it includes
// <aws/core/client/ClientConfiguration.h> for the client-config passthrough.
// The SDK-free engine handle (ddbsh/connection.hpp) never sees an Aws:: type;
// the config is consumed here, before the Connection is built over the client.

#ifndef DDBSH_AWS_DATABASE_HPP
#define DDBSH_AWS_DATABASE_HPP

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <aws/core/client/ClientConfiguration.h>

#include "ddbsh/connection.hpp"
#include "ddbsh/result.hpp"

namespace ddbsh::aws {

/// Options for open(). The two convenience overloads cover the common cases;
/// the struct form adds the full client-configuration passthrough (R-P4d-3).
struct OpenOptions {
    std::string region;                  ///< "" => SDK default region resolution
    std::string endpoint;                ///< "" => regional endpoint (no override)
    std::optional<std::string> profile;  ///< unset => default credential chain

    /// Full SDK client configuration (timeouts, retry strategy, max
    /// connections, proxy, …). When set, it is the BASE the AWS client builds
    /// on; ddbsh overlays region/endpoint and always the ddbsh user-agent
    /// (D24/D25). Unset => a default-constructed ClientConfiguration.
    std::optional<Aws::Client::ClientConfiguration> client_config;

    /// Poll-loop budget for the CREATE/DROP ACTIVE/gone waiters (D27). This is
    /// a loop-iteration bound, distinct from an SDK per-request timeout (set
    /// that via client_config). Unset => the original's unbounded backoff loop.
    std::optional<std::chrono::milliseconds> waiter_timeout;
};

/// A Database is a Connection plus the AWS SDK lifecycle and client it owns.
/// Move-only; closure is RAII (dropping the last Database shuts the SDK down).
/// It forwards the connection-handle surface, so `db->exec(...)` /
/// `db->query(...)` work directly. Not thread-safe for concurrent use (one
/// Database per thread); multiple Databases coexist safely.
class Database {
public:
    ~Database();
    Database(Database&&) noexcept;
    Database& operator=(Database&&) noexcept;
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    Result<Results> exec(std::string_view sql) { return conn_->exec(sql); }
    Result<Results> exec(const PreparedStatement& stmt) { return conn_->exec(stmt); }
    Result<RowSet> query(std::string_view sql) { return conn_->query(sql); }
    Result<RowSet> query(const PreparedStatement& stmt) { return conn_->query(stmt); }

    Result<void> reconnect(const std::string& region, const std::string& endpoint = "") {
        return conn_->reconnect(region, endpoint);
    }
    std::string region() const { return conn_->region(); }
    bool endpoint_overridden() const { return conn_->endpoint_overridden(); }
    void close() { conn_->close(); }

    /// The underlying engine handle, for an embedder that wants it directly.
    Connection& connection() { return *conn_; }

private:
    friend Result<Database> open(const OpenOptions&);
    Database(std::shared_ptr<void> sdk_token, std::unique_ptr<Connection> conn)
        : sdk_token_(std::move(sdk_token)), conn_(std::move(conn)) {}

    std::shared_ptr<void> sdk_token_;     // reference-counted Aws::InitAPI/Shutdown
    std::unique_ptr<Connection> conn_;    // owns the AwsDDBClient
};

/// Open an AWS-backed Database. Resolves region/endpoint/credentials through
/// the standard AWS chain (~/.aws, AWS_PROFILE, AWS_REGION), overridden by the
/// non-empty option fields, and constructs the client. It performs no network
/// probe and no credential validation, so it does not fail on an unreachable
/// endpoint or bad credentials — those surface on the first exec()/query().
/// The Result is for future-proofing (and any construction-time failure the SDK
/// client setup could raise); a successful open() does not imply a live
/// connection. Run a cheap statement for a liveness check.
Result<Database> open(const OpenOptions& opts);

/// Convenience: open with a region and optional endpoint override.
Result<Database> open(const std::string& region, const std::string& endpoint = "");

}  // namespace ddbsh::aws

#endif  // DDBSH_AWS_DATABASE_HPP
