// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// renderer.cpp — byte-exact formatting of engine results.
//
// The Renderer owns ALL output formatting so byte-compat lives in one place
// (P2a-03 §5). This implementation covers StatusOutcome / ReportOutcome /
// EmptyOutcome, the error path, (W5) SelectOutcome, and (W5 part 2)
// WriteOutcome — the item format
// (alpha-sorted attributes, quote_if_required, TRUE/FALSE/NULL, the top-level
// `: ` vs nested `:` asymmetry, lists/maps/empties), the
// `SELECT (table, cu, rcu, wcu)` consumed-capacity line (arch §9), and the
// per-row INSERT/REPLACE status lines. The EXPLAIN block and remaining
// write/report refinements land with their outcome arms (W6).

#include "ddbsh/renderer.hpp"

#include <algorithm>
#include <cstring>
#include <regex>
#include <string>
#include <variant>
#include <vector>

#include "ddbsh/value.hpp"

namespace ddbsh::render {

namespace {

/// A status token renders as the token followed by a newline (arch §7/§9).
std::string render_status(const StatusOutcome& s) {
    return s.token + "\n";
}

/// A report renders its lines joined by newlines, each terminated by a
/// newline (arch §9 multi-line reports).
std::string render_report(const ReportOutcome& r) {
    std::string out;
    for (const auto& line : r.lines) {
        out += line;
        out += '\n';
    }
    return out;
}

// ---- SELECT item formatting (port of select_helper.cpp, arch §9) -----------

/// A string renders bare unless it contains a space/tab/`,`/`:`/newline or any
/// bracket/brace, in which case it is double-quoted (CSelectHelper::
/// quote_if_required). strcspn returns the length of the leading run with none
/// of those chars; if that run spans the whole string, no quoting is needed.
std::string quote_if_required(const std::string& input) {
    if (std::strcspn(input.c_str(), " \t,:\n[]{}") != input.size()) {
        return "\"" + input + "\"";
    }
    return input;
}

/// Serialize one attribute Value (CSelectHelper::serialize_value). Nested maps
/// use `:` with NO space after the key (contrast the top-level item `: `), and
/// map keys are emitted alphabetically — the original iterated an Aws::Map
/// (std::map, sorted); our ValueMap keeps source order, so we sort here.
std::string serialize_value(const Value& v) {
    switch (v.type) {
        case Value::Type::S:
            return quote_if_required(v.s);
        case Value::Type::N:
            return v.s;
        case Value::Type::BOOL:
            return v.boolean ? "TRUE" : "FALSE";
        case Value::Type::NUL:
            // A stored null attribute renders NULL (the original's "NOT NULL"
            // branch is for the IS NOT NULL display path, not a stored value).
            return "NULL";
        case Value::Type::L: {
            std::string out;
            for (const auto& e : v.list) {
                out += (out.empty() ? "[" : ", ") + serialize_value(e);
            }
            return out.empty() ? "[]" : out + "]";
        }
        case Value::Type::M: {
            std::vector<const std::pair<std::string, Value>*> entries;
            entries.reserve(v.map.size());
            for (const auto& e : v.map) {
                entries.push_back(&e);
            }
            std::sort(entries.begin(), entries.end(),
                      [](const auto* a, const auto* b) { return a->first < b->first; });
            std::string out;
            for (const auto* e : entries) {
                out += (out.empty() ? "{" : ", ") + quote_if_required(e->first) + ":" +
                       serialize_value(e->second);
            }
            return out.empty() ? "{}" : out + "}";
        }
        default:
            // Sets (SS/NS/BS) and binary (B) are not reachable via the in-scope
            // commands; the original likewise rendered them "Unknown Type".
            return "Unknown Type";
    }
}

/// Render one item (CSelectHelper::show_item): `{name: value, ...}` with
/// attribute names sorted alphabetically and the top-level `: ` separator.
/// An empty item renders to the empty string (and is skipped by the caller).
std::string show_item(const ItemRow& item) {
    if (item.attributes.empty()) {
        return {};
    }
    std::vector<const std::pair<std::string, Value>*> attrs;
    attrs.reserve(item.attributes.size());
    for (const auto& a : item.attributes) {
        attrs.push_back(&a);
    }
    std::sort(attrs.begin(), attrs.end(),
              [](const auto* a, const auto* b) { return a->first < b->first; });
    std::string out;
    for (const auto* a : attrs) {
        out += (out.empty() ? "{" : ", ") + quote_if_required(a->first) + ": " +
               serialize_value(a->second);
    }
    return out + "}";
}

/// Trim trailing zeros from a double's string form (util/trim_double.cpp): the
/// consumed-capacity figures print without trailing `.000000`.
std::string trim_double(const std::string& input) {
    static const std::regex reg(R"(^([\d,]+)$|^([\d,]+)\.0*$|^([\d,]+\.[0-9]*?)0*$)");
    return std::regex_replace(input, reg, "$1$2$3");
}

/// Format a totalized consumed-capacity figure as the `SELECT (...)` line
/// (select.cpp display_consumed_capacity). Printed only when a table name and
/// at least one capacity figure are set; each missing numeric field prints 0.
std::string render_consumed_capacity(const ConsumedCapacity& cc) {
    const bool any_capacity = cc.capacity_units.has_value() || cc.read_capacity_units.has_value() ||
                              cc.write_capacity_units.has_value();
    if (!cc.table_name.has_value() || !any_capacity) {
        return {};
    }
    auto field = [](const std::optional<double>& v) {
        return v.has_value() ? trim_double(std::to_string(*v)) : std::string{"0"};
    };
    std::string capacity = *cc.table_name;
    capacity += ", " + field(cc.capacity_units);
    capacity += ", " + field(cc.read_capacity_units);
    capacity += ", " + field(cc.write_capacity_units);
    return "SELECT (" + capacity + ")\n";
}

std::string render_select(const SelectOutcome& s) {
    std::string out;
    for (const auto& item : s.items) {
        std::string line = show_item(item);
        if (!line.empty()) {
            out += line;
            out += '\n';
        }
    }
    if (s.consumed_capacity.has_value()) {
        out += render_consumed_capacity(*s.consumed_capacity);
    }
    return out;
}

/// INSERT / REPLACE render one status token per row written, each on its own
/// line (arch §7 — the original printf("%s\n", op) per PutItem).
std::string render_write(const WriteOutcome& w) {
    std::string out;
    for (const auto& token : w.tokens) {
        out += token;
        out += '\n';
    }
    return out;
}

}  // namespace

std::string Renderer::render(const ExecOutcome& outcome) const {
    return std::visit(
        [](const auto& payload) -> std::string {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, StatusOutcome>) {
                return render_status(payload);
            } else if constexpr (std::is_same_v<T, ReportOutcome>) {
                return render_report(payload);
            } else if constexpr (std::is_same_v<T, EmptyOutcome>) {
                return {};
            } else if constexpr (std::is_same_v<T, SelectOutcome>) {
                return render_select(payload);
            } else if constexpr (std::is_same_v<T, WriteOutcome>) {
                return render_write(payload);
            } else {
                static_assert(sizeof(T) == 0, "unhandled ExecOutcome arm");
            }
        },
        outcome);
}

std::string Renderer::render_error(const Error& error) const {
    // Byte-exact user string (tenet 5); the CLI writes this to stderr. The
    // per-Errc mapping to the original's exact strings is completed as each
    // command family lands; the message field already carries that string.
    return error.message + "\n";
}

}  // namespace ddbsh::render
