// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// format.hpp — selectable output formats: JSON, CSV and a text table (P12).
//
// The Renderer (renderer.hpp) produces ddbsh's own output, byte for byte the
// original's. This header adds the opt-in formats a customer can choose
// instead (D-P12-4, tenet 8): JSON in two flavors (plain values, or the
// DynamoDB-typed `{"N": "1"}` encoding), RFC 4180 CSV, and a MySQL-style text
// table drawn with `+`, `-` and `|`. Without a choice the format is
// `standard`, which writes exactly what the Renderer writes.
//
// A format applies only to rows: the items of a SELECT, a read transaction and
// RETURN VALUES, and the reports that have natural columns (SHOW TABLES, SHOW
// BACKUPS, SHOW LIMITS). Every other output (status tokens, DESCRIBE, EXPLAIN,
// HELP, errors) stays text in every format (D-P12-21). decompose() makes that
// split: it turns one ExecOutcome into an ordered list of TextParts and
// RowsParts.
//
// FormattingSink is the streaming consumer: an OutcomeSink that writes each
// fragment through a caller-supplied writer as it arrives. It holds at most
// one open result set. A RowsPart opens one (or continues the open one); a
// TextPart closes it first; the end of the statement closes it. So a
// multi-page SELECT is one JSON array and one CSV header, and a status line
// never lands inside a JSON array. A run of several statements is a sequence
// of documents, one per result set, with the text output between them.
//
// Everything here is a pure function of its input or of the sink's own state:
// no globals, no locale, no terminal, no AWS SDK (tenet 3).

#ifndef DDBSH_FORMAT_HPP
#define DDBSH_FORMAT_HPP

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ddbsh/exec_outcome.hpp"
#include "ddbsh/iddbclient.hpp"
#include "ddbsh/outcome_sink.hpp"
#include "ddbsh/value.hpp"

namespace ddbsh::render {

/// The output format. `standard` (the id "default") is ddbsh's own output,
/// byte-identical to the Renderer's.
enum class OutputFormat { standard, json, csv, table };

/// How the JSON format encodes a value, and how CSV and table cells encode a
/// nested map, list or set (D-P12-5, D-P12-7). `plain` writes JSON values
/// (`{"pk": 1}`); `dynamodb` writes the AWS wire encoding (`{"pk": {"N": "1"}}`).
enum class JsonFlavor { plain, dynamodb };

/// A format selection. The flavor is ignored by `standard`.
struct FormatOptions {
    OutputFormat format = OutputFormat::standard;
    JsonFlavor json_flavor = JsonFlavor::plain;
};

/// Parse a format id: "default", "json", "csv" or "table", ASCII
/// case-insensitive. nullopt for anything else.
std::optional<OutputFormat> parse_output_format(std::string_view id);

/// The id of a format ("default", "json", "csv", "table").
std::string_view output_format_id(OutputFormat format);

/// Parse a JSON flavor id: "plain" or "dynamodb", ASCII case-insensitive.
/// nullopt for anything else.
std::optional<JsonFlavor> parse_json_flavor(std::string_view id);

/// The id of a JSON flavor ("plain", "dynamodb").
std::string_view json_flavor_id(JsonFlavor flavor);

// ---- Decomposition -----------------------------------------------------------

/// Output that is text in every format: exactly what Renderer::render (`out`)
/// and Renderer::render_stderr (`err`) produce for that piece of an outcome.
struct TextPart {
    std::string out;
    std::string err;
};

/// Rows a format lays out: items, or report rows.
struct RowsPart {
    /// The rows, in output order. A read transaction's miss is an empty row,
    /// so the rows stay positional.
    std::vector<ItemRow> rows;
    /// Columns known before the rows are seen: a report's complete column list
    /// (fixed_columns), or a SELECT's hint (key attributes for `SELECT *`, the
    /// projection's attributes for an explicit projection). May be empty.
    std::vector<std::string> columns;
    /// True for reports (and vector search): `columns` is complete and
    /// ordered, and a JSON element keeps that order instead of byte order.
    bool fixed_columns = false;
    /// An API page ended with this part. The table format prints its buffered
    /// rows as one box here (D-P12-24).
    bool group_end = true;
};

/// One piece of an outcome, in output order.
using OutputPart = std::variant<TextPart, RowsPart>;

/// Split an outcome into its text and its rows (D-P12-21):
///   * SelectOutcome: its rows (always, even when empty, so an empty result
///     still opens a result set), then the `SELECT (...)` capacity line as
///     text when it carries one;
///   * VectorSearchOutcome: its rows with the similarity score as a last
///     column named `score` (`_score`, `__score`, ... when an item already has
///     an attribute of that name), then the capacity line as text;
///   * TransactReadOutcome: its rows, a miss as an empty row;
///   * WriteOutcome: per written row, the status token as text, then the
///     RETURN VALUES item as a one-row part when the row carries one;
///   * BulkWriteOutcome: the RETURN VALUES items, then the accounting line as
///     text (not for a partial streaming fragment);
///   * ReportOutcome with a table: its rows, with the report's fixed columns;
///   * MultiOutcome: its parts' parts, in order;
///   * everything else: one TextPart.
/// A piece whose text renders empty on both streams produces no part.
std::vector<OutputPart> decompose(const ExecOutcome& outcome);

/// The column list of one result set for CSV, the table and the web's HTML
/// table (design §4.6). Columns are only ever appended: they never reorder or
/// disappear within a result set.
class ColumnTracker {
public:
    /// Add a part's columns, then every attribute of its rows not yet present,
    /// in byte order. Returns the index of the first column this call added
    /// (== the previous size when it added none).
    std::size_t add(const RowsPart& part);

    /// The current columns, in order.
    const std::vector<std::string>& columns() const noexcept { return columns_; }

    /// Forget every column (a new result set).
    void clear() noexcept { columns_.clear(); }

private:
    bool has(std::string_view name) const;
    std::vector<std::string> columns_;
};

// ---- Value encoding ----------------------------------------------------------

/// Append `text` as a JSON string: `"`, `\` and U+0000..U+001F escaped
/// (`\b \f \n \r \t` by name, the rest as lowercase `\u00xx`), every other byte
/// (UTF-8 included) copied as is.
void append_json_string(std::string& out, std::string_view text);

/// True when `text` matches the RFC 8259 number grammar
/// (`-? (0 | [1-9][0-9]*) (. [0-9]+)? ([eE] [+-]? [0-9]+)?`). A DynamoDB number
/// that does not is written as a JSON string, so no digit is ever lost.
bool is_json_number(std::string_view text);

/// Append one value as JSON in the given flavor. Plain: S and B as strings, N
/// as a number (the exact digits; a string when not an RFC 8259 number), BOOL,
/// NULL as null, L as an array, M as an object with byte-ordered keys, SS and
/// BS as arrays of strings, NS as an array of numbers. DynamoDB: the AWS wire
/// encoding (`{"S": ...}`, `{"N": "1"}`, `{"M": {...}}`, `{"NS": ["1"]}`, ...).
/// Arrays and objects use `", "` and `": "` separators.
void append_json_value(std::string& out, const Value& value, JsonFlavor flavor);

/// The text of one CSV or table cell: S, N and B (base64) as they are, BOOL as
/// `true`/`false`, NULL as `null`; a map, list or set as JSON in the flavor.
std::string format_cell(const Value& value, JsonFlavor flavor);

// ---- Streaming -------------------------------------------------------------

/// Which stream a piece of formatted output belongs on.
enum class Stream { out, err };

/// An OutcomeSink that formats each fragment as it arrives and hands the
/// bytes to a writer (the CLI's writes to two FILE*s and flushes; the web's
/// appends to strings). In `standard` it writes exactly Renderer::render (to
/// `out`) and Renderer::render_stderr (to `err`) per fragment, and nothing at
/// end_statement(). In the other formats it writes each row as it arrives
/// (the table format, per API page), closes an open result set before any
/// text, and closes it at end_statement(), so the closing bytes precede the
/// error the caller prints. Not thread-safe; one statement at a time.
class FormattingSink : public OutcomeSink {
public:
    /// Receives each piece of output, in order. Never called with empty text.
    using Writer = std::function<void(Stream, std::string_view)>;

    FormattingSink(FormatOptions options, Writer writer);
    ~FormattingSink() override;
    FormattingSink(FormattingSink&&) noexcept;
    FormattingSink& operator=(FormattingSink&&) noexcept;
    FormattingSink(const FormattingSink&) = delete;
    FormattingSink& operator=(const FormattingSink&) = delete;

    void emit(const ExecOutcome& fragment) override;
    void end_statement() override;

    /// The current selection.
    const FormatOptions& options() const noexcept;

    /// Change the selection for the statements that follow. An open result
    /// set is closed first, in the old format.
    void set_options(FormatOptions options);

private:
    struct State;
    std::unique_ptr<State> state_;
};

/// Format one buffered outcome (Session::execute(stmt)'s result) as a whole:
/// the stdout bytes a FormattingSink would write for it as one statement. The
/// stderr bytes do not depend on the format; they are Renderer::render_stderr.
std::string render_formatted(const ExecOutcome& outcome, const FormatOptions& options);

}  // namespace ddbsh::render

#endif  // DDBSH_FORMAT_HPP
