// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// format.cpp — the opt-in output formats (see ddbsh/format.hpp).
//
// Three writers share one shape: a result set opens on its first RowsPart,
// takes rows as they arrive, and closes on the next TextPart or at the end of
// the statement.
//   * JSON (D-P12-22): `[` + one element per line, indented two spaces; each
//     element is written as soon as its row arrives (the `,` that separates it
//     from the next goes out with the next); `]` at the close. An empty set is
//     `[]`.
//   * CSV (D-P12-23): a header when the set opens with known columns, then a
//     line per row. When later rows add attributes, a new header with every
//     column (the old ones in the same positions) goes out first.
//   * Table (D-P12-24): rows are held until an API page ends (RowsPart::
//     group_end), then printed as one box; the close prints what is left. A
//     set that printed no box but has columns prints a header-only box.

#include "ddbsh/format.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include "ddbsh/renderer.hpp"
#include "render/render_detail.hpp"

namespace ddbsh::render {

namespace {

char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) {
            return false;
        }
    }
    return true;
}

const Value* find_attribute(const ValueMap& row, std::string_view name) {
    for (const auto& [key, value] : row) {
        if (key == name) {
            return &value;
        }
    }
    return nullptr;
}

/// A map's entries in byte order of their names (the order `default` uses).
std::vector<const std::pair<std::string, Value>*> byte_ordered(const ValueMap& map) {
    std::vector<const std::pair<std::string, Value>*> entries;
    entries.reserve(map.size());
    for (const auto& e : map) {
        entries.push_back(&e);
    }
    std::sort(entries.begin(), entries.end(),
              [](const auto* a, const auto* b) { return a->first < b->first; });
    return entries;
}

void append_json_number(std::string& out, std::string_view digits) {
    if (is_json_number(digits)) {
        out += digits;
    } else {
        append_json_string(out, digits);
    }
}

/// `{"<tag>": ` ... `}` around a DynamoDB-flavor value.
void append_tagged_string(std::string& out, const char* tag, std::string_view text) {
    out += "{\"";
    out += tag;
    out += "\": ";
    append_json_string(out, text);
    out += '}';
}

void append_string_array(std::string& out, const std::vector<std::string>& items, bool numbers) {
    out += '[';
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) {
            out += ", ";
        }
        if (numbers) {
            append_json_number(out, items[i]);
        } else {
            append_json_string(out, items[i]);
        }
    }
    out += ']';
}

void append_plain(std::string& out, const Value& v);
void append_typed(std::string& out, const Value& v);

void append_plain_map(std::string& out, const ValueMap& map) {
    out += '{';
    bool first = true;
    for (const auto* e : byte_ordered(map)) {
        if (!first) {
            out += ", ";
        }
        first = false;
        append_json_string(out, e->first);
        out += ": ";
        append_plain(out, e->second);
    }
    out += '}';
}

void append_typed_map(std::string& out, const ValueMap& map) {
    out += '{';
    bool first = true;
    for (const auto* e : byte_ordered(map)) {
        if (!first) {
            out += ", ";
        }
        first = false;
        append_json_string(out, e->first);
        out += ": ";
        append_typed(out, e->second);
    }
    out += '}';
}

void append_plain(std::string& out, const Value& v) {
    switch (v.type) {
        case Value::Type::S:
        case Value::Type::B:
            append_json_string(out, v.s);
            return;
        case Value::Type::N:
            append_json_number(out, v.s);
            return;
        case Value::Type::BOOL:
            out += v.boolean ? "true" : "false";
            return;
        case Value::Type::NUL:
            out += "null";
            return;
        case Value::Type::L:
            out += '[';
            for (std::size_t i = 0; i < v.list.size(); ++i) {
                if (i > 0) {
                    out += ", ";
                }
                append_plain(out, v.list[i]);
            }
            out += ']';
            return;
        case Value::Type::M:
            append_plain_map(out, v.map);
            return;
        case Value::Type::SS:
        case Value::Type::BS:
            append_string_array(out, v.set, /*numbers=*/false);
            return;
        case Value::Type::NS:
            append_string_array(out, v.set, /*numbers=*/true);
            return;
    }
}

void append_typed(std::string& out, const Value& v) {
    switch (v.type) {
        case Value::Type::S:
            append_tagged_string(out, "S", v.s);
            return;
        case Value::Type::N:
            append_tagged_string(out, "N", v.s);
            return;
        case Value::Type::B:
            append_tagged_string(out, "B", v.s);
            return;
        case Value::Type::BOOL:
            out += v.boolean ? "{\"BOOL\": true}" : "{\"BOOL\": false}";
            return;
        case Value::Type::NUL:
            out += "{\"NULL\": true}";
            return;
        case Value::Type::L:
            out += "{\"L\": [";
            for (std::size_t i = 0; i < v.list.size(); ++i) {
                if (i > 0) {
                    out += ", ";
                }
                append_typed(out, v.list[i]);
            }
            out += "]}";
            return;
        case Value::Type::M:
            out += "{\"M\": ";
            append_typed_map(out, v.map);
            out += '}';
            return;
        case Value::Type::SS:
            out += "{\"SS\": ";
            append_string_array(out, v.set, /*numbers=*/false);
            out += '}';
            return;
        case Value::Type::NS:
            // The wire encoding carries NS members as strings.
            out += "{\"NS\": ";
            append_string_array(out, v.set, /*numbers=*/false);
            out += '}';
            return;
        case Value::Type::BS:
            out += "{\"BS\": ";
            append_string_array(out, v.set, /*numbers=*/false);
            out += '}';
            return;
    }
}

/// One JSON element for a row: an object whose keys follow the part's column
/// order when the columns are fixed (reports), else byte order.
void append_json_row(std::string& out, const RowsPart& part, const ItemRow& row,
                     JsonFlavor flavor) {
    const auto append_entry = [&](bool& first, const std::string& name, const Value& value) {
        if (!first) {
            out += ", ";
        }
        first = false;
        append_json_string(out, name);
        out += ": ";
        append_json_value(out, value, flavor);
    };
    out += '{';
    bool first = true;
    if (part.fixed_columns) {
        for (const auto& column : part.columns) {
            if (const Value* v = find_attribute(row.attributes, column); v != nullptr) {
                append_entry(first, column, *v);
            }
        }
    } else {
        for (const auto* e : byte_ordered(row.attributes)) {
            append_entry(first, e->first, e->second);
        }
    }
    out += '}';
}

/// One CSV field: quoted iff it holds `,`, `"`, CR or LF; `"` doubled.
void append_csv_field(std::string& out, std::string_view text) {
    if (text.find_first_of(",\"\r\n") == std::string_view::npos) {
        out += text;
        return;
    }
    out += '"';
    for (char c : text) {
        if (c == '"') {
            out += '"';
        }
        out += c;
    }
    out += '"';
}

/// A table cell's display text: control characters shown with their JSON
/// escapes (DEL as `\u007f`), so a newline never breaks a box.
std::string escape_controls(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text) {
        switch (c) {
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20 || c == 0x7f) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
                break;
        }
    }
    return out;
}

/// Display width in UTF-8 code points (wide and combining characters are not
/// accounted for; documented).
std::size_t display_width(std::string_view text) {
    std::size_t n = 0;
    for (unsigned char c : text) {
        if ((c & 0xC0) != 0x80) {
            ++n;
        }
    }
    return n;
}

struct TableCell {
    std::string text;
    bool right = false;  ///< numbers are right-aligned
};

/// Draw one box: a rule, the header, a rule, then (when `rows` is not null)
/// the rows and a closing rule.
std::string draw_box(const std::vector<std::string>& columns, const std::vector<ValueMap>* rows,
                     JsonFlavor flavor) {
    std::vector<std::string> header;
    std::vector<std::size_t> widths;
    header.reserve(columns.size());
    for (const auto& c : columns) {
        header.push_back(escape_controls(c));
        widths.push_back(display_width(header.back()));
    }
    std::vector<std::vector<TableCell>> cells;
    if (rows != nullptr) {
        cells.reserve(rows->size());
        for (const auto& row : *rows) {
            std::vector<TableCell> line;
            line.reserve(columns.size());
            for (std::size_t i = 0; i < columns.size(); ++i) {
                TableCell cell;
                if (const Value* v = find_attribute(row, columns[i]); v != nullptr) {
                    cell.text = escape_controls(format_cell(*v, flavor));
                    cell.right = v->type == Value::Type::N;
                }
                widths[i] = std::max(widths[i], display_width(cell.text));
                line.push_back(std::move(cell));
            }
            cells.push_back(std::move(line));
        }
    }

    std::string rule = "+";
    for (std::size_t w : widths) {
        rule.append(w + 2, '-');
        rule += '+';
    }
    rule += '\n';

    const auto draw_line = [&](const std::vector<TableCell>& line) {
        std::string out = "|";
        for (std::size_t i = 0; i < line.size(); ++i) {
            const std::size_t pad = widths[i] - display_width(line[i].text);
            out += ' ';
            if (line[i].right) {
                out.append(pad, ' ');
                out += line[i].text;
            } else {
                out += line[i].text;
                out.append(pad, ' ');
            }
            out += " |";
        }
        return out + "\n";
    };

    std::vector<TableCell> header_line;
    for (auto& h : header) {
        header_line.push_back(TableCell{std::move(h), false});
    }
    std::string out = rule + draw_line(header_line) + rule;
    if (rows != nullptr) {
        for (const auto& line : cells) {
            out += draw_line(line);
        }
        out += rule;
    }
    return out;
}

/// The name of the score column: `score`, or `score` with as many leading
/// underscores as it takes not to collide with an item attribute.
std::string score_column(const std::vector<VectorSearchOutcome::Row>& rows) {
    std::string name = "score";
    const auto taken = [&rows](const std::string& n) {
        return std::any_of(rows.begin(), rows.end(), [&n](const VectorSearchOutcome::Row& r) {
            return find_attribute(r.item.attributes, n) != nullptr;
        });
    };
    while (taken(name)) {
        name.insert(name.begin(), '_');
    }
    return name;
}

void push_text(std::vector<OutputPart>& parts, const ExecOutcome& outcome) {
    const Renderer renderer;
    TextPart text{renderer.render(outcome), renderer.render_stderr(outcome)};
    if (!text.out.empty() || !text.err.empty()) {
        parts.emplace_back(std::move(text));
    }
}

void decompose_into(std::vector<OutputPart>& parts, const ExecOutcome& outcome) {
    std::visit(
        [&parts, &outcome](const auto& payload) {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, SelectOutcome>) {
                RowsPart rows;
                for (const auto& item : payload.items) {
                    // The Renderer skips an empty item; so do the formats.
                    if (!item.attributes.empty()) {
                        rows.rows.push_back(item);
                    }
                }
                rows.columns = payload.column_hint;
                parts.emplace_back(std::move(rows));
                if (payload.consumed_capacity.has_value()) {
                    SelectOutcome capacity;
                    capacity.consumed_capacity = payload.consumed_capacity;
                    push_text(parts, ExecOutcome{std::move(capacity)});
                }
            } else if constexpr (std::is_same_v<T, VectorSearchOutcome>) {
                const std::string score = score_column(payload.rows);
                RowsPart rows;
                rows.fixed_columns = true;
                for (const auto& r : payload.rows) {
                    ItemRow item = r.item;
                    item.attributes.emplace_back(
                        score, Value::number_value(detail::format_score(r.score)));
                    rows.rows.push_back(std::move(item));
                }
                ColumnTracker tracker;
                tracker.add(rows);  // every attribute, byte order (score included)
                for (const auto& c : tracker.columns()) {
                    if (c != score) {
                        rows.columns.push_back(c);
                    }
                }
                rows.columns.push_back(score);
                parts.emplace_back(std::move(rows));
                if (payload.vector_capacity.has_value()) {
                    VectorSearchOutcome capacity;
                    capacity.vector_capacity = payload.vector_capacity;
                    push_text(parts, ExecOutcome{std::move(capacity)});
                }
            } else if constexpr (std::is_same_v<T, TransactReadOutcome>) {
                RowsPart rows;
                rows.rows = payload.items;
                parts.emplace_back(std::move(rows));
            } else if constexpr (std::is_same_v<T, WriteOutcome>) {
                for (std::size_t i = 0; i < payload.tokens.size(); ++i) {
                    push_text(parts, ExecOutcome{WriteOutcome{{payload.tokens[i]}}});
                    if (i < payload.items.size() && payload.items[i].has_value()) {
                        RowsPart rows;
                        rows.rows.push_back(*payload.items[i]);
                        parts.emplace_back(std::move(rows));
                    }
                }
            } else if constexpr (std::is_same_v<T, BulkWriteOutcome>) {
                if (!payload.returned_items.empty()) {
                    RowsPart rows;
                    rows.rows = payload.returned_items;
                    // A partial fragment is one item, not a page.
                    rows.group_end = !payload.partial;
                    parts.emplace_back(std::move(rows));
                }
                if (!payload.partial) {
                    BulkWriteOutcome accounting = payload;
                    accounting.returned_items.clear();
                    push_text(parts, ExecOutcome{std::move(accounting)});
                }
            } else if constexpr (std::is_same_v<T, ReportOutcome>) {
                if (payload.table.has_value()) {
                    RowsPart rows;
                    rows.columns = payload.table->columns;
                    rows.fixed_columns = true;
                    rows.group_end = payload.table->group_end;
                    for (const auto& r : payload.table->rows) {
                        rows.rows.push_back(ItemRow{r});
                    }
                    parts.emplace_back(std::move(rows));
                } else {
                    push_text(parts, outcome);
                }
            } else if constexpr (std::is_same_v<T, MultiOutcome>) {
                for (const auto& part : payload.parts) {
                    decompose_into(parts, part);
                }
            } else {
                push_text(parts, outcome);
            }
        },
        outcome);
}

}  // namespace

// ---- Ids ---------------------------------------------------------------------

std::optional<OutputFormat> parse_output_format(std::string_view id) {
    for (OutputFormat f :
         {OutputFormat::standard, OutputFormat::json, OutputFormat::csv, OutputFormat::table}) {
        if (iequals(id, output_format_id(f))) {
            return f;
        }
    }
    return std::nullopt;
}

std::string_view output_format_id(OutputFormat format) {
    switch (format) {
        case OutputFormat::standard:
            return "default";
        case OutputFormat::json:
            return "json";
        case OutputFormat::csv:
            return "csv";
        case OutputFormat::table:
            return "table";
    }
    return "default";
}

std::optional<JsonFlavor> parse_json_flavor(std::string_view id) {
    for (JsonFlavor f : {JsonFlavor::plain, JsonFlavor::dynamodb}) {
        if (iequals(id, json_flavor_id(f))) {
            return f;
        }
    }
    return std::nullopt;
}

std::string_view json_flavor_id(JsonFlavor flavor) {
    return flavor == JsonFlavor::dynamodb ? "dynamodb" : "plain";
}

// ---- Decomposition and columns ---------------------------------------------

std::vector<OutputPart> decompose(const ExecOutcome& outcome) {
    std::vector<OutputPart> parts;
    decompose_into(parts, outcome);
    return parts;
}

bool ColumnTracker::has(std::string_view name) const {
    return std::find(columns_.begin(), columns_.end(), name) != columns_.end();
}

std::size_t ColumnTracker::add(const RowsPart& part) {
    const std::size_t first_new = columns_.size();
    for (const auto& c : part.columns) {
        if (!has(c)) {
            columns_.push_back(c);
        }
    }
    std::vector<std::string> fresh;
    for (const auto& row : part.rows) {
        for (const auto& [name, value] : row.attributes) {
            (void)value;
            if (!has(name) && std::find(fresh.begin(), fresh.end(), name) == fresh.end()) {
                fresh.push_back(name);
            }
        }
    }
    std::sort(fresh.begin(), fresh.end());
    for (auto& name : fresh) {
        columns_.push_back(std::move(name));
    }
    return first_new;
}

// ---- Value encoding ------------------------------------------------------------

void append_json_string(std::string& out, std::string_view text) {
    out += '"';
    for (unsigned char c : text) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
                break;
        }
    }
    out += '"';
}

bool is_json_number(std::string_view text) {
    std::size_t i = 0;
    const std::size_t n = text.size();
    const auto digit = [&](std::size_t at) { return at < n && text[at] >= '0' && text[at] <= '9'; };
    if (i < n && text[i] == '-') {
        ++i;
    }
    if (!digit(i)) {
        return false;
    }
    if (text[i] == '0') {
        ++i;
    } else {
        while (digit(i)) {
            ++i;
        }
    }
    if (i < n && text[i] == '.') {
        ++i;
        if (!digit(i)) {
            return false;
        }
        while (digit(i)) {
            ++i;
        }
    }
    if (i < n && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        if (i < n && (text[i] == '+' || text[i] == '-')) {
            ++i;
        }
        if (!digit(i)) {
            return false;
        }
        while (digit(i)) {
            ++i;
        }
    }
    return i == n;
}

void append_json_value(std::string& out, const Value& value, JsonFlavor flavor) {
    if (flavor == JsonFlavor::dynamodb) {
        append_typed(out, value);
    } else {
        append_plain(out, value);
    }
}

std::string format_cell(const Value& value, JsonFlavor flavor) {
    switch (value.type) {
        case Value::Type::S:
        case Value::Type::N:
        case Value::Type::B:
            return value.s;
        case Value::Type::BOOL:
            return value.boolean ? "true" : "false";
        case Value::Type::NUL:
            return "null";
        case Value::Type::L:
        case Value::Type::M:
        case Value::Type::SS:
        case Value::Type::NS:
        case Value::Type::BS:
            break;
    }
    std::string out;
    append_json_value(out, value, flavor);
    return out;
}

// ---- FormattingSink -------------------------------------------------------------

struct FormattingSink::State {
    FormatOptions options;
    Writer writer;
    Renderer renderer;

    // The open result set, if any, and what its writer has done so far.
    bool open = false;
    ColumnTracker tracker;
    std::size_t json_elements = 0;    ///< JSON: elements written
    std::size_t csv_header_size = 0;  ///< CSV: columns in the last header written
    std::vector<ValueMap> pending;    ///< table: rows held until the page ends
    bool box_printed = false;         ///< table: a box has been printed

    State(FormatOptions o, Writer w) : options(o), writer(std::move(w)) {}

    void write(Stream stream, std::string_view text) const {
        if (!text.empty() && writer) {
            writer(stream, text);
        }
    }

    void start_set() {
        open = true;
        tracker.clear();
        json_elements = 0;
        csv_header_size = 0;
        pending.clear();
        box_printed = false;
    }

    void flush_table() {
        if (pending.empty()) {
            return;
        }
        if (!tracker.columns().empty()) {
            write(Stream::out, draw_box(tracker.columns(), &pending, options.json_flavor));
            box_printed = true;
        }
        pending.clear();
    }

    void close_set() {
        if (!open) {
            return;
        }
        switch (options.format) {
            case OutputFormat::json:
                write(Stream::out, json_elements == 0 ? "[]\n" : "\n]\n");
                break;
            case OutputFormat::table:
                flush_table();
                if (!box_printed && !tracker.columns().empty()) {
                    write(Stream::out, draw_box(tracker.columns(), nullptr, options.json_flavor));
                }
                break;
            case OutputFormat::csv:
            case OutputFormat::standard:
                break;
        }
        open = false;
        tracker.clear();
        pending.clear();
    }

    void text(const TextPart& part) {
        close_set();
        write(Stream::out, part.out);
        write(Stream::err, part.err);
    }

    void rows(const RowsPart& part) {
        if (!open) {
            start_set();
        }
        switch (options.format) {
            case OutputFormat::json:
                for (const auto& row : part.rows) {
                    std::string chunk = json_elements == 0 ? "[\n  " : ",\n  ";
                    append_json_row(chunk, part, row, options.json_flavor);
                    write(Stream::out, chunk);
                    ++json_elements;
                }
                break;
            case OutputFormat::csv: {
                tracker.add(part);
                const auto& columns = tracker.columns();
                std::string chunk;
                if (columns.size() > csv_header_size) {
                    for (std::size_t i = 0; i < columns.size(); ++i) {
                        if (i > 0) {
                            chunk += ',';
                        }
                        append_csv_field(chunk, columns[i]);
                    }
                    chunk += '\n';
                    csv_header_size = columns.size();
                }
                for (const auto& row : part.rows) {
                    for (std::size_t i = 0; i < columns.size(); ++i) {
                        if (i > 0) {
                            chunk += ',';
                        }
                        if (const Value* v = find_attribute(row.attributes, columns[i]);
                            v != nullptr) {
                            append_csv_field(chunk, format_cell(*v, options.json_flavor));
                        }
                    }
                    chunk += '\n';
                }
                write(Stream::out, chunk);
                break;
            }
            case OutputFormat::table:
                tracker.add(part);
                for (const auto& row : part.rows) {
                    pending.push_back(row.attributes);
                }
                if (part.group_end) {
                    flush_table();
                }
                break;
            case OutputFormat::standard:
                break;
        }
    }
};

FormattingSink::FormattingSink(FormatOptions options, Writer writer)
    : state_(std::make_unique<State>(options, std::move(writer))) {}

FormattingSink::~FormattingSink() = default;
FormattingSink::FormattingSink(FormattingSink&&) noexcept = default;
FormattingSink& FormattingSink::operator=(FormattingSink&&) noexcept = default;

void FormattingSink::emit(const ExecOutcome& fragment) {
    State& s = *state_;
    if (s.options.format == OutputFormat::standard) {
        s.write(Stream::out, s.renderer.render(fragment));
        s.write(Stream::err, s.renderer.render_stderr(fragment));
        return;
    }
    for (const auto& part : decompose(fragment)) {
        if (const auto* text = std::get_if<TextPart>(&part)) {
            s.text(*text);
        } else {
            s.rows(std::get<RowsPart>(part));
        }
    }
}

void FormattingSink::end_statement() {
    state_->close_set();
}

const FormatOptions& FormattingSink::options() const noexcept {
    return state_->options;
}

void FormattingSink::set_options(FormatOptions options) {
    state_->close_set();
    state_->options = options;
}

std::string render_formatted(const ExecOutcome& outcome, const FormatOptions& options) {
    std::string out;
    FormattingSink sink(options, [&out](Stream stream, std::string_view text) {
        if (stream == Stream::out) {
            out += text;
        }
    });
    sink.emit(outcome);
    sink.end_statement();
    return out;
}

}  // namespace ddbsh::render
