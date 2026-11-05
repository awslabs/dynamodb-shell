// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// connection.hpp — the embedding API (P4d, D1/D2).
//
// A simple, ODBC / Sybase-DB-Library-style C++ client surface: open a
// Connection over an injected IDDBClient, submit a statement with exec(), walk
// result sets with next_result(), walk rows with next_row(), and read a typed,
// null-aware column with column()/Cell. This is THE embedding surface an
// application uses; the engine SPI (Session / ExecOutcome / OutcomeSink /
// Renderer) remains the internal/injection interface the CLI and tests drive.
//
// SDK-FREE (D11 / D-P4d-13): this header is part of the engine library's
// SDK-free public surface — no AWS SDK type appears here. The AWS-backed,
// region/credential-resolving convenience (ddbsh::aws::open -> Database) lives
// in the opt-in component header include/ddbsh/aws/database.hpp. A Connection
// works over ANY IDDBClient, so an embedder can drive it against the real AWS
// client, a ddbsh::testing::MemoryClient, or their own backend, all offline
// unit-testable (the engine_boundary_check asserts this header pulls no
// <aws/...>).
//
// The engine is materialized-fetch (D11): a Query/Scan is paged internally and
// its rows buffered, so a large scan buffers in memory. Row-at-a-time lazy
// pagination is a future enhancement (TODO(phase-N)); result-SET advancement
// (next_result) IS lazy — the next statement in a batch runs only when the
// caller advances to it.

#ifndef DDBSH_CONNECTION_HPP
#define DDBSH_CONNECTION_HPP

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ddbsh/config.hpp"
#include "ddbsh/error.hpp"
#include "ddbsh/iddbclient.hpp"  // ItemRow, ConsumedCapacity (SDK-free)
#include "ddbsh/result.hpp"
#include "ddbsh/value.hpp"

namespace ddbsh {

/// The kind of a single result set (one outcome fragment). A batch or a
/// multi-row write emits several result sets; walk them with
/// Results::next_result().
enum class ResultKind {
    rows,     ///< a SELECT / transact-read / vector-search row set (walk with next_row)
    status,   ///< a terse status token line (CREATE/INSERT/ALTER/ABORT/...)
    write,    ///< a bulk UPDATE/UPSERT/DELETE accounting result (read/modified/ccf)
    report,   ///< a multi-line human report (DESCRIBE, SHOW ...)
    explain,  ///< an EXPLAIN block group (the serialized request text)
    empty,    ///< blank / comment-only input
};

/// The bulk-write verb a `write`-kind result set reports.
enum class WriteVerb { update, upsert, del };

/// A read-side view of one attribute cell of the current row. It cleanly
/// separates the three DynamoDB states a DB-Library `dbdatlen` distinguishes:
/// the attribute is ABSENT (missing()), PRESENT and of type NUL (is_null()),
/// or PRESENT with a value (the typed accessors).
///
/// Lifetime (D16): a Cell is a non-owning view into the current row's Value and
/// is valid until the row advances (Results::next_row()) or the owning
/// Row/Results is destroyed. The copy-returning accessors (as_string/as_int/…)
/// return std::optional copies that outlive advancement; the view accessors
/// (as_list/as_map/as_string_set/value()) point into the current row and share
/// its lifetime.
class Cell {
public:
    Cell() = default;
    explicit Cell(const Value* value) : value_(value) {}

    /// True when the current row has no such attribute.
    bool missing() const { return value_ == nullptr; }
    /// True when the attribute is present and of DynamoDB NULL type.
    bool is_null() const { return value_ != nullptr && value_->type == Value::Type::NUL; }
    /// The attribute's DynamoDB type. Only valid when !missing().
    Value::Type type() const { return value_ != nullptr ? value_->type : Value::Type::NUL; }

    /// S (or N as its exact text) else nullopt (missing/null/other type).
    std::optional<std::string> as_string() const;
    /// N parsed as a signed integer; nullopt if not N, overflow, or not integral.
    std::optional<long long> as_int() const;
    /// N parsed as a double; nullopt if not N or unparseable.
    std::optional<double> as_double() const;
    /// BOOL else nullopt.
    std::optional<bool> as_bool() const;

    /// SS / NS / BS element view, else nullptr. Shares the row's lifetime.
    const std::vector<std::string>* as_string_set() const;
    /// L element view, else nullptr. Shares the row's lifetime.
    const ValueList* as_list() const;
    /// M element view, else nullptr. Shares the row's lifetime.
    const ValueMap* as_map() const;

    /// The raw SDK-free Value (full fidelity). Shares the row's lifetime.
    /// Precondition: !missing().
    const Value& value() const { return *value_; }

    /// This cell's byte-exact ddbsh text (the same formatter the CLI uses).
    /// A missing cell renders the empty string.
    std::string render() const;

private:
    const Value* value_ = nullptr;
};

/// A whole row: the current item's attributes, read by name with typed,
/// null-aware getters. The get_* convenience getters fold ALL of {attribute
/// absent, attribute present with type NUL, attribute present with a different
/// type} into std::nullopt (lossy by design — the common case wants "the value
/// or nothing" in one line that never throws). An embedder that must
/// distinguish the three states drops to cell(): missing() vs is_null() vs a
/// typed accessor returning nullopt with type() naming the actual type (D20).
class Row {
public:
    Row() = default;
    explicit Row(ItemRow item) : item_(std::move(item)) {}

    /// The three-state cell view for `name` (missing cell if absent).
    Cell cell(std::string_view name) const;

    std::optional<std::string> get_string(std::string_view name) const {
        return cell(name).as_string();
    }
    std::optional<long long> get_int(std::string_view name) const { return cell(name).as_int(); }
    std::optional<double> get_double(std::string_view name) const { return cell(name).as_double(); }
    std::optional<bool> get_bool(std::string_view name) const { return cell(name).as_bool(); }

    /// Attribute count.
    std::size_t size() const { return item_.attributes.size(); }
    /// The i-th attribute's name (source order).
    const std::string& name_at(std::size_t i) const { return item_.attributes[i].first; }
    /// The i-th attribute's cell (source order).
    Cell cell_at(std::size_t i) const { return Cell(&item_.attributes[i].second); }

    /// The underlying SDK-free item (full fidelity).
    const ItemRow& item() const { return item_; }

    // Range-for over the attributes as (name, Value) pairs.
    ValueMap::const_iterator begin() const { return item_.attributes.begin(); }
    ValueMap::const_iterator end() const { return item_.attributes.end(); }

private:
    ItemRow item_;
};

/// An iterable set of rows returned by Connection::query() (the single-SELECT
/// shortcut). Rows are materialized and stable, so a range-for and the Cells
/// read from each Row are valid for the RowSet's whole lifetime.
class RowSet {
public:
    RowSet() = default;
    explicit RowSet(std::vector<Row> rows) : rows_(std::move(rows)) {}

    std::vector<Row>::const_iterator begin() const { return rows_.begin(); }
    std::vector<Row>::const_iterator end() const { return rows_.end(); }
    std::size_t size() const { return rows_.size(); }
    bool empty() const { return rows_.empty(); }
    const Row& operator[](std::size_t i) const { return rows_[i]; }

private:
    std::vector<Row> rows_;
};

/// The result of exec(): a sequence of result sets (DB-Library `dbresults`),
/// each walked for rows (`dbnextrow`) and read by column (`dbdata`). After
/// exec() the Results is positioned AT the first result set (the single-SELECT
/// case never calls next_result()). next_result() advances to the next result
/// set — executing the next statement in a batch lazily — and returns false
/// when the batch is exhausted; a per-statement failure then stops the batch
/// and is readable via error().
///
/// Results is move-only and borrows its Connection's engine state; it is valid
/// until the next exec() on the same Connection or until the Connection is
/// destroyed (D16).
class Results {
public:
    Results();
    ~Results();
    Results(Results&&) noexcept;
    Results& operator=(Results&&) noexcept;
    Results(const Results&) = delete;
    Results& operator=(const Results&) = delete;

    /// The current result set's kind.
    ResultKind kind() const;

    /// Advance to the next result set (running the next statement lazily if
    /// needed). Returns false when none remain; on a per-statement failure it
    /// returns false and the Error is readable via error().
    bool next_result();

    // ---- rows-kind ----------------------------------------------------------
    /// Advance within the current rows result set. Returns false at end of set
    /// and on a non-rows set. After a true return, column()/row()/score() read
    /// the new current row.
    bool next_row();
    /// Attribute count of the current row.
    std::size_t column_count() const;
    /// The i-th attribute's name (source order) of the current row.
    std::string column_name(std::size_t i) const;
    /// The named column of the current row (missing cell if absent).
    Cell column(std::string_view name) const;
    /// The i-th column of the current row (source order).
    Cell column(std::size_t i) const;
    /// The whole current row.
    const Row& row() const;
    /// The similarity score of the current row on a vector-search result set
    /// (0.0 on any other rows set).
    double score() const;

    // ---- status-kind --------------------------------------------------------
    /// The status token (CREATE/INSERT/ALTER/ABORT/...); empty on other kinds.
    std::string status_token() const;

    // ---- write-kind ---------------------------------------------------------
    long read() const;      ///< items enumerated (0 on the single-write path)
    long modified() const;  ///< successful writes
    long ccf() const;       ///< conditional-check failures (data, not error)
    WriteVerb verb() const;

    // ---- report-kind --------------------------------------------------------
    /// The report lines (DESCRIBE / SHOW ...); empty on other kinds.
    const std::vector<std::string>& lines() const;

    // ---- explain-kind -------------------------------------------------------
    /// The serialized request text of an EXPLAIN block group; empty otherwise.
    std::string explain_text() const;

    // ---- common -------------------------------------------------------------
    /// Consumed capacity for the current result set, when the user asked for it
    /// (RETURN CONSUMED CAPACITY); nullopt otherwise.
    std::optional<ConsumedCapacity> consumed_capacity() const;

    /// Byte-exact ddbsh stderr diagnostic lines carried by the current result
    /// set (a transaction's non-reducible-SELECT message, a combined-ALTER
    /// note). DATA on a success outcome — it does NOT set error() (D17). Empty
    /// for every other result set.
    const std::vector<std::string>& diagnostics() const;

    /// The error that stopped the batch (a per-statement execution failure or a
    /// trailing parse error), or nullopt on a clean end. A conditional-check
    /// failure on a bulk write is DATA (see ccf()), not an error (D17).
    std::optional<Error> error() const;

    // ---- exact-ddbsh-bytes path (D10) ---------------------------------------
    /// The byte-exact CLI stdout bytes for the current result set.
    std::string render() const;
    /// The byte-exact CLI stdout bytes for every result set of the batch,
    /// concatenated in order (runs any not-yet-executed statements).
    std::string render_all();

private:
    friend class Connection;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit Results(std::unique_ptr<Impl> impl);
};

/// A prepared statement with client-side parameter binding (D6). The caller
/// writes positional `?` (1-based) and/or named `:name` placeholders and binds
/// Values; the API substitutes correctly-escaped ddbsh literals into the SQL
/// text BEFORE it reaches the parser, so EXPLAIN/alias/golden fidelity is
/// untouched (binding is purely an embedding convenience). This is the
/// injection-safe path: an embedder building a statement from untrusted input
/// SHOULD bind rather than concatenate — the escaping is the API's job.
/// Binding operates on the pre-parse text, so a placeholder anywhere in a
/// multi-statement `BEGIN; … COMMIT;` body is substituted the same way (D21).
class PreparedStatement {
public:
    explicit PreparedStatement(std::string sql) : sql_(std::move(sql)) {}

    PreparedStatement& bind(int index1, Value v);
    PreparedStatement& bind(std::string_view name, Value v);

    // Convenience overloads.
    PreparedStatement& bind(int index1, std::string s) {
        return bind(index1, Value::string_value(std::move(s)));
    }
    PreparedStatement& bind(int index1, long long n) {
        return bind(index1, Value::number_value(std::to_string(n)));
    }
    PreparedStatement& bind(int index1, bool b) { return bind(index1, Value::bool_value(b)); }
    PreparedStatement& bind(std::string_view name, std::string s) {
        return bind(name, Value::string_value(std::move(s)));
    }
    PreparedStatement& bind(std::string_view name, long long n) {
        return bind(name, Value::number_value(std::to_string(n)));
    }
    PreparedStatement& bind(std::string_view name, bool b) {
        return bind(name, Value::bool_value(b));
    }

    /// Substitute the bound values into the SQL, producing literal SQL ready to
    /// parse. An unknown placeholder, an out-of-range index, or a value that
    /// cannot be rendered to a literal is an Errc::invalid_argument error.
    Result<std::string> expand() const;

private:
    std::string sql_;
    std::vector<Value> positional_;
    std::vector<std::pair<std::string, Value>> named_;
    std::optional<Error> bind_error_;  ///< first bind misuse; surfaced by expand()
};

/// The embedding handle: open over an injected IDDBClient, submit statements,
/// walk results. Move-only; closure is RAII (D16). A single Connection is NOT
/// thread-safe for concurrent use (it holds the engine's per-session alias
/// counter and schema cache); the contract is N threads / N connections.
/// Multiple Connections coexist safely.
class Connection {
public:
    /// Open over a BORROWED client: the caller guarantees `client` outlives the
    /// Connection.
    Connection(SessionConfig cfg, IDDBClient& client);
    /// Open over an OWNED client: the Connection takes ownership (used by
    /// ddbsh::aws::open). `client` must be non-null.
    Connection(SessionConfig cfg, std::unique_ptr<IDDBClient> client);

    ~Connection();
    Connection(Connection&&) noexcept;
    Connection& operator=(Connection&&) noexcept;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    /// Submit a batch of one or more `;`-separated statements. The whole batch
    /// is parsed up front (a parse error is surfaced at the result boundary via
    /// Results::error()); statements then execute lazily as the caller advances
    /// through result sets. A parse error on the FIRST statement (no statement
    /// parsed) is returned here directly.
    Result<Results> exec(std::string_view sql);
    /// Submit a prepared statement (client-side binding expanded first).
    Result<Results> exec(const PreparedStatement& stmt);

    /// Execute a single row-returning statement and iterate its rows directly.
    /// A non-row-returning statement (or a batch) is an Errc::invalid_argument
    /// error naming the actual kind. The returned RowSet owns its rows.
    Result<RowSet> query(std::string_view sql);
    Result<RowSet> query(const PreparedStatement& stmt);

    // ---- connection options (D9) --------------------------------------------
    /// Re-point the handle at a new region/endpoint (and, for an AWS client,
    /// re-resolve credentials — the cross-account path). The programmatic form
    /// of the CONNECT statement.
    Result<void> reconnect(const std::string& region, const std::string& endpoint = "");
    /// The current region.
    std::string region() const;
    /// Whether an endpoint override is in effect (for an embedder rendering a
    /// prompt).
    bool endpoint_overridden() const;

    /// Deterministic teardown before scope exit. Makes the handle invalid;
    /// further use returns an Errc::invalid_argument error (no crash). Idempotent.
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ddbsh

#endif  // DDBSH_CONNECTION_HPP
