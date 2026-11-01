// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// connection.cpp — the embedding API implementation (P4d, D1/D2).
//
// Connection / Results / Row / Cell / PreparedStatement sit on top of the
// engine SPI (Session + Renderer). The engine is the only parser/planner/
// executor; this layer hides its shape (ExecOutcome variant, OutcomeSink,
// Session) behind a simple connection-handle / fetch-row / read-column surface.
// It is SDK-free: it drives whatever IDDBClient the Connection was opened over.

#include "ddbsh/connection.hpp"

#include <charconv>
#include <cstddef>
#include <limits>
#include <utility>
#include <variant>

#include "ddbsh/exec_outcome.hpp"
#include "ddbsh/renderer.hpp"
#include "ddbsh/session.hpp"

namespace ddbsh {

namespace {

constexpr std::size_t kBeforeFirst = static_cast<std::size_t>(-1);

WriteVerb to_write_verb(BulkWriteOutcome::Verb v) {
    switch (v) {
        case BulkWriteOutcome::Verb::update:
            return WriteVerb::update;
        case BulkWriteOutcome::Verb::upsert:
            return WriteVerb::upsert;
        case BulkWriteOutcome::Verb::del:
            return WriteVerb::del;
    }
    return WriteVerb::update;
}

/// Render one Value to the ddbsh source literal that re-parses to it (the
/// inverse of the lexer's literal rules). Returns an Error for a shape with no
/// literal syntax (DynamoDB sets SS/NS/BS have none in the grammar).
std::optional<Error> render_literal(const Value& v, std::string& out) {
    switch (v.type) {
        case Value::Type::S: {
            out.push_back('"');
            for (char c : v.s) {
                if (c == '"') {
                    out.push_back('"');  // SQL-style "" doubling (lexer §4)
                }
                out.push_back(c);
            }
            out.push_back('"');
            return std::nullopt;
        }
        case Value::Type::N:
            out += v.s;  // bare, exact lexeme
            return std::nullopt;
        case Value::Type::BOOL:
            out += (v.boolean ? "true" : "false");
            return std::nullopt;
        case Value::Type::NUL:
            out += "null";
            return std::nullopt;
        case Value::Type::L: {
            out.push_back('[');
            for (std::size_t i = 0; i < v.list.size(); ++i) {
                if (i != 0) {
                    out += ", ";
                }
                if (auto e = render_literal(v.list[i], out)) {
                    return e;
                }
            }
            out.push_back(']');
            return std::nullopt;
        }
        case Value::Type::M: {
            out.push_back('{');
            for (std::size_t i = 0; i < v.map.size(); ++i) {
                if (i != 0) {
                    out += ", ";
                }
                out.push_back('"');
                for (char c : v.map[i].first) {
                    if (c == '"') {
                        out.push_back('"');
                    }
                    out.push_back(c);
                }
                out += "\":";
                if (auto e = render_literal(v.map[i].second, out)) {
                    return e;
                }
            }
            out.push_back('}');
            return std::nullopt;
        }
        case Value::Type::B:
        case Value::Type::SS:
        case Value::Type::NS:
        case Value::Type::BS:
            return Error{Errc::invalid_argument,
                         "cannot bind a DynamoDB binary or set value: no literal syntax",
                         std::nullopt, std::nullopt};
    }
    return Error{Errc::invalid_argument, "cannot bind value of unknown type", std::nullopt,
                 std::nullopt};
}

bool is_name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

}  // namespace

// ============================ Cell ==========================================

std::optional<std::string> Cell::as_string() const {
    if (value_ == nullptr) {
        return std::nullopt;
    }
    if (value_->type == Value::Type::S || value_->type == Value::Type::N) {
        return value_->s;
    }
    return std::nullopt;
}

std::optional<long long> Cell::as_int() const {
    if (value_ == nullptr || value_->type != Value::Type::N) {
        return std::nullopt;
    }
    const std::string& s = value_->s;
    long long result = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), result);
    if (ec != std::errc{} || ptr != s.data() + s.size()) {
        return std::nullopt;  // not integral / overflow / trailing chars
    }
    return result;
}

std::optional<double> Cell::as_double() const {
    if (value_ == nullptr || value_->type != Value::Type::N) {
        return std::nullopt;
    }
    // Locale-independent parse: DynamoDB N values always use '.' as the decimal
    // point, so std::stod (which follows the global C++ locale and can misparse
    // under a comma-decimal locale) must not be used. std::from_chars is
    // locale-independent by contract.
    const std::string& s = value_->s;
    double result = 0.0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), result);
    if (ec != std::errc{} || ptr != s.data() + s.size()) {
        return std::nullopt;  // not a number / trailing chars / out of range
    }
    return result;
}

std::optional<bool> Cell::as_bool() const {
    if (value_ == nullptr || value_->type != Value::Type::BOOL) {
        return std::nullopt;
    }
    return value_->boolean;
}

const std::vector<std::string>* Cell::as_string_set() const {
    if (value_ == nullptr) {
        return nullptr;
    }
    if (value_->type == Value::Type::SS || value_->type == Value::Type::NS ||
        value_->type == Value::Type::BS) {
        return &value_->set;
    }
    return nullptr;
}

const ValueList* Cell::as_list() const {
    if (value_ != nullptr && value_->type == Value::Type::L) {
        return &value_->list;
    }
    return nullptr;
}

const ValueMap* Cell::as_map() const {
    if (value_ != nullptr && value_->type == Value::Type::M) {
        return &value_->map;
    }
    return nullptr;
}

std::string Cell::render() const {
    if (value_ == nullptr) {
        return {};
    }
    return render::render_value(*value_);
}

// ============================ Row ===========================================

Cell Row::cell(std::string_view name) const {
    for (const auto& [attr_name, value] : item_.attributes) {
        if (attr_name == name) {
            return Cell(&value);
        }
    }
    return Cell(nullptr);
}

// ====================== PreparedStatement ===================================

PreparedStatement& PreparedStatement::bind(int index1, Value v) {
    if (index1 < 1) {
        // A positional index is 1-based; a value <1 is caller misuse. bind()
        // returns *this for chaining and cannot return a Result, so record the
        // first such error and surface it from expand() (no silent swallow).
        if (!bind_error_.has_value()) {
            bind_error_ = Error{Errc::invalid_argument,
                                "positional bind index must be >= 1, got " +
                                    std::to_string(index1),
                                std::nullopt, std::nullopt};
        }
        return *this;
    }
    auto idx = static_cast<std::size_t>(index1 - 1);
    if (idx >= positional_.size()) {
        positional_.resize(idx + 1);
    }
    positional_[idx] = std::move(v);
    return *this;
}

PreparedStatement& PreparedStatement::bind(std::string_view name, Value v) {
    for (auto& [n, val] : named_) {
        if (n == name) {
            val = std::move(v);
            return *this;
        }
    }
    named_.emplace_back(std::string(name), std::move(v));
    return *this;
}

Result<std::string> PreparedStatement::expand() const {
    if (bind_error_.has_value()) {
        return fail(*bind_error_);
    }
    std::string out;
    out.reserve(sql_.size() + 16);
    std::size_t next_positional = 0;
    std::vector<bool> named_used(named_.size(), false);
    bool in_string = false;

    for (std::size_t i = 0; i < sql_.size(); ++i) {
        char c = sql_[i];
        if (in_string) {
            out.push_back(c);
            if (c == '"') {
                if (i + 1 < sql_.size() && sql_[i + 1] == '"') {
                    out.push_back('"');  // escaped "" — stay in string
                    ++i;
                } else {
                    in_string = false;
                }
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
            out.push_back(c);
            continue;
        }
        if (c == '?') {
            if (next_positional >= positional_.size()) {
                return fail(Errc::invalid_argument,
                            "positional placeholder #" + std::to_string(next_positional + 1) +
                                " has no bound value");
            }
            if (auto e = render_literal(positional_[next_positional], out)) {
                return fail(*e);
            }
            ++next_positional;
            continue;
        }
        if (c == ':' && i + 1 < sql_.size() && is_name_char(sql_[i + 1]) &&
            !(sql_[i + 1] >= '0' && sql_[i + 1] <= '9')) {
            std::size_t j = i + 1;
            while (j < sql_.size() && is_name_char(sql_[j])) {
                ++j;
            }
            std::string name = sql_.substr(i + 1, j - i - 1);
            bool matched = false;
            for (std::size_t k = 0; k < named_.size(); ++k) {
                if (named_[k].first == name) {
                    if (auto e = render_literal(named_[k].second, out)) {
                        return fail(*e);
                    }
                    named_used[k] = true;
                    matched = true;
                    break;
                }
            }
            if (matched) {
                i = j - 1;
                continue;
            }
            // Unbound :name — leave verbatim (the grammar deals with it).
            out.push_back(c);
            continue;
        }
        out.push_back(c);
    }

    if (next_positional < positional_.size()) {
        return fail(Errc::invalid_argument,
                    "bound positional parameter #" + std::to_string(next_positional + 1) +
                        " has no matching ? placeholder");
    }
    for (std::size_t k = 0; k < named_.size(); ++k) {
        if (!named_used[k]) {
            return fail(Errc::invalid_argument,
                        "bound parameter :" + named_[k].first + " has no matching placeholder");
        }
    }
    return out;
}

// ============================ Results =======================================

namespace {

struct ResultSetData {
    ResultKind kind = ResultKind::empty;
    std::vector<ItemRow> rows;
    std::vector<double> scores;  // parallel to rows (vector search); else empty
    std::optional<ConsumedCapacity> cc;
    std::string status;
    std::vector<std::string> diagnostics;
    long read = 0;
    long modified = 0;
    long ccf = 0;
    WriteVerb verb = WriteVerb::update;
    std::vector<std::string> lines;
    std::vector<ExplainBlock> explain_blocks;
    ExecOutcome render_source{EmptyOutcome{}};
};

void flatten(const ExecOutcome& o, std::vector<ResultSetData>& out);

void flatten_one(const StatusOutcome& s, std::vector<ResultSetData>& out) {
    ResultSetData r;
    r.kind = ResultKind::status;
    r.status = s.token;
    r.diagnostics = s.diagnostics;
    r.render_source = s;
    out.push_back(std::move(r));
}

void flatten(const ExecOutcome& o, std::vector<ResultSetData>& out) {
    if (const auto* s = std::get_if<StatusOutcome>(&o)) {
        flatten_one(*s, out);
    } else if (const auto* rep = std::get_if<ReportOutcome>(&o)) {
        ResultSetData r;
        r.kind = ResultKind::report;
        r.lines = rep->lines;
        r.render_source = *rep;
        out.push_back(std::move(r));
    } else if (std::get_if<EmptyOutcome>(&o) != nullptr) {
        ResultSetData r;
        r.kind = ResultKind::empty;
        r.render_source = EmptyOutcome{};
        out.push_back(std::move(r));
    } else if (const auto* sel = std::get_if<SelectOutcome>(&o)) {
        ResultSetData r;
        r.kind = ResultKind::rows;
        r.rows = sel->items;
        r.cc = sel->consumed_capacity;
        r.render_source = *sel;
        out.push_back(std::move(r));
    } else if (const auto* vs = std::get_if<VectorSearchOutcome>(&o)) {
        ResultSetData r;
        r.kind = ResultKind::rows;
        for (const auto& row : vs->rows) {
            r.rows.push_back(row.item);
            r.scores.push_back(row.score);
        }
        r.render_source = *vs;
        out.push_back(std::move(r));
    } else if (const auto* w = std::get_if<WriteOutcome>(&o)) {
        // One status result set per written row (arch §7 per-row token lines);
        // each render_source is a one-row WriteOutcome so render_all() is
        // byte-identical to the original's per-row emission.
        for (std::size_t i = 0; i < w->tokens.size(); ++i) {
            ResultSetData r;
            r.kind = ResultKind::status;
            r.status = w->tokens[i];
            WriteOutcome one;
            one.tokens.push_back(w->tokens[i]);
            if (i < w->items.size()) {
                one.items.push_back(w->items[i]);
            }
            r.render_source = std::move(one);
            out.push_back(std::move(r));
        }
    } else if (const auto* b = std::get_if<BulkWriteOutcome>(&o)) {
        ResultSetData r;
        r.kind = ResultKind::write;
        r.read = b->read;
        r.modified = b->modified;
        r.ccf = b->ccf;
        r.verb = to_write_verb(b->verb);
        r.cc = b->consumed_capacity;
        r.render_source = *b;
        out.push_back(std::move(r));
    } else if (const auto* t = std::get_if<TransactOutcome>(&o)) {
        ResultSetData r;
        r.kind = ResultKind::status;
        r.status = t->token;
        r.diagnostics = t->diagnostics;
        r.render_source = *t;
        out.push_back(std::move(r));
    } else if (const auto* tr = std::get_if<TransactReadOutcome>(&o)) {
        ResultSetData r;
        r.kind = ResultKind::rows;
        r.rows = tr->items;
        r.render_source = *tr;
        out.push_back(std::move(r));
    } else if (const auto* ex = std::get_if<ExplainOutcome>(&o)) {
        ResultSetData r;
        r.kind = ResultKind::explain;
        r.explain_blocks = ex->blocks;
        r.render_source = *ex;  // includes trailing_status so render() is exact
        out.push_back(std::move(r));
    } else if (const auto* m = std::get_if<MultiOutcome>(&o)) {
        for (const auto& part : m->parts) {
            flatten(part, out);
        }
    }
}

}  // namespace

struct Results::Impl {
    Session* session = nullptr;
    const render::Renderer* renderer = nullptr;
    std::vector<Statement> statements;
    std::optional<Error> parse_error;

    std::size_t stmt_index = kBeforeFirst;
    bool loaded = false;
    bool done = false;
    std::vector<ResultSetData> sets;
    std::size_t set_index = 0;
    std::optional<Error> error;

    long long row_index = -1;
    Row current_row;

    const std::vector<std::string> empty_lines{};

    void reset_row_cursor() {
        row_index = -1;
        current_row = Row{};
    }

    const ResultSetData* current() const {
        if (!loaded || set_index >= sets.size()) {
            return nullptr;
        }
        return &sets[set_index];
    }

    bool advance_statement() {
        if (done) {
            return false;
        }
        std::size_t next = (stmt_index == kBeforeFirst) ? 0 : stmt_index + 1;
        if (next >= statements.size()) {
            done = true;
            loaded = false;
            sets.clear();
            if (parse_error.has_value() && !error.has_value()) {
                error = parse_error;
            }
            return false;
        }
        stmt_index = next;
        auto r = session->execute(statements[stmt_index]);
        if (!r.has_value()) {
            error = r.error();
            done = true;
            loaded = false;
            sets.clear();
            return false;
        }
        sets.clear();
        flatten(*r, sets);
        set_index = 0;
        loaded = true;
        reset_row_cursor();
        if (sets.empty()) {
            return advance_statement();
        }
        return true;
    }

    void prime() { advance_statement(); }
};

Results::Results() : impl_(std::make_unique<Impl>()) {}
Results::Results(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Results::~Results() = default;
Results::Results(Results&&) noexcept = default;
Results& Results::operator=(Results&&) noexcept = default;

ResultKind Results::kind() const {
    const auto* c = impl_->current();
    return c != nullptr ? c->kind : ResultKind::empty;
}

bool Results::next_result() {
    if (impl_->done) {
        return false;
    }
    if (impl_->loaded && impl_->set_index + 1 < impl_->sets.size()) {
        ++impl_->set_index;
        impl_->reset_row_cursor();
        return true;
    }
    return impl_->advance_statement();
}

bool Results::next_row() {
    const auto* c = impl_->current();
    if (c == nullptr || c->kind != ResultKind::rows) {
        return false;
    }
    auto next = impl_->row_index + 1;
    if (next < 0 || static_cast<std::size_t>(next) >= c->rows.size()) {
        return false;
    }
    impl_->row_index = next;
    impl_->current_row = Row(c->rows[static_cast<std::size_t>(next)]);
    return true;
}

std::size_t Results::column_count() const { return impl_->current_row.size(); }

std::string Results::column_name(std::size_t i) const {
    if (i >= impl_->current_row.size()) {
        return {};
    }
    return impl_->current_row.name_at(i);
}

Cell Results::column(std::string_view name) const { return impl_->current_row.cell(name); }

Cell Results::column(std::size_t i) const {
    if (i >= impl_->current_row.size()) {
        return Cell(nullptr);
    }
    return impl_->current_row.cell_at(i);
}

const Row& Results::row() const { return impl_->current_row; }

double Results::score() const {
    const auto* c = impl_->current();
    if (c == nullptr || impl_->row_index < 0) {
        return 0.0;
    }
    auto idx = static_cast<std::size_t>(impl_->row_index);
    if (idx < c->scores.size()) {
        return c->scores[idx];
    }
    return 0.0;
}

std::string Results::status_token() const {
    const auto* c = impl_->current();
    return c != nullptr ? c->status : std::string{};
}

long Results::read() const {
    const auto* c = impl_->current();
    return c != nullptr ? c->read : 0;
}
long Results::modified() const {
    const auto* c = impl_->current();
    return c != nullptr ? c->modified : 0;
}
long Results::ccf() const {
    const auto* c = impl_->current();
    return c != nullptr ? c->ccf : 0;
}
WriteVerb Results::verb() const {
    const auto* c = impl_->current();
    return c != nullptr ? c->verb : WriteVerb::update;
}

const std::vector<std::string>& Results::lines() const {
    const auto* c = impl_->current();
    return c != nullptr ? c->lines : impl_->empty_lines;
}

std::string Results::explain_text() const {
    const auto* c = impl_->current();
    if (c == nullptr || c->kind != ResultKind::explain || impl_->renderer == nullptr) {
        return {};
    }
    ExplainOutcome blocks_only;
    blocks_only.blocks = c->explain_blocks;
    return impl_->renderer->render(blocks_only);
}

std::optional<ConsumedCapacity> Results::consumed_capacity() const {
    const auto* c = impl_->current();
    return c != nullptr ? c->cc : std::nullopt;
}

const std::vector<std::string>& Results::diagnostics() const {
    const auto* c = impl_->current();
    return c != nullptr ? c->diagnostics : impl_->empty_lines;
}

std::optional<Error> Results::error() const { return impl_->error; }

std::string Results::render() const {
    const auto* c = impl_->current();
    if (c == nullptr || impl_->renderer == nullptr) {
        return {};
    }
    return impl_->renderer->render(c->render_source);
}

std::string Results::render_all() {
    std::string out;
    if (impl_->renderer == nullptr) {
        return out;
    }
    // Render the current result set, then every remaining one in order.
    const auto* c = impl_->current();
    if (c != nullptr) {
        out += impl_->renderer->render(c->render_source);
        out += impl_->renderer->render_stderr(c->render_source);
    }
    while (next_result()) {
        const auto* cur = impl_->current();
        if (cur != nullptr) {
            out += impl_->renderer->render(cur->render_source);
            out += impl_->renderer->render_stderr(cur->render_source);
        }
    }
    return out;
}

// ============================ Connection ====================================

struct Connection::Impl {
    SessionConfig cfg;
    std::unique_ptr<IDDBClient> owned;  // non-null only for the owning ctor
    IDDBClient* client = nullptr;
    std::unique_ptr<Session> session;
    render::Renderer renderer;
    bool closed = false;

    void make_session() { session = std::make_unique<Session>(cfg, *client); }
};

Connection::Connection(SessionConfig cfg, IDDBClient& client) : impl_(std::make_unique<Impl>()) {
    impl_->cfg = std::move(cfg);
    impl_->client = &client;
    impl_->make_session();
}

Connection::Connection(SessionConfig cfg, std::unique_ptr<IDDBClient> client)
    : impl_(std::make_unique<Impl>()) {
    impl_->cfg = std::move(cfg);
    impl_->owned = std::move(client);
    impl_->client = impl_->owned.get();
    impl_->make_session();
}

Connection::~Connection() = default;
Connection::Connection(Connection&&) noexcept = default;
Connection& Connection::operator=(Connection&&) noexcept = default;

Result<Results> Connection::exec(std::string_view sql) {
    if (impl_->closed) {
        return fail(Errc::invalid_argument, "connection is closed");
    }
    ParsedBatch batch = impl_->session->parse_batch(sql);
    if (batch.statements.empty() && batch.error.has_value()) {
        return fail(*batch.error);  // parse error on the first statement
    }

    auto rimpl = std::make_unique<Results::Impl>();
    rimpl->session = impl_->session.get();
    rimpl->renderer = &impl_->renderer;
    rimpl->statements = std::move(batch.statements);
    rimpl->parse_error = std::move(batch.error);
    rimpl->prime();

    Results out(std::move(rimpl));
    return out;
}

Result<Results> Connection::exec(const PreparedStatement& stmt) {
    auto expanded = stmt.expand();
    if (!expanded.has_value()) {
        return fail(expanded.error());
    }
    return exec(*expanded);
}

Result<RowSet> Connection::query(std::string_view sql) {
    auto r = exec(sql);
    if (!r.has_value()) {
        return fail(r.error());
    }
    Results& results = *r;
    if (results.kind() != ResultKind::rows) {
        std::string kind_name;
        switch (results.kind()) {
            case ResultKind::status:
                kind_name = "status";
                break;
            case ResultKind::write:
                kind_name = "write";
                break;
            case ResultKind::report:
                kind_name = "report";
                break;
            case ResultKind::explain:
                kind_name = "explain";
                break;
            case ResultKind::empty:
                kind_name = "empty";
                break;
            case ResultKind::rows:
                kind_name = "rows";
                break;
        }
        if (auto e = results.error()) {
            return fail(*e);
        }
        return fail(Errc::invalid_argument,
                    "query() requires a row-returning statement; the result kind is " + kind_name);
    }
    std::vector<Row> rows;
    while (results.next_row()) {
        rows.push_back(results.row());
    }
    if (auto e = results.error()) {
        return fail(*e);
    }
    return RowSet(std::move(rows));
}

Result<RowSet> Connection::query(const PreparedStatement& stmt) {
    auto expanded = stmt.expand();
    if (!expanded.has_value()) {
        return fail(expanded.error());
    }
    return query(*expanded);
}

Result<void> Connection::reconnect(const std::string& region, const std::string& endpoint) {
    if (impl_->closed) {
        return fail(Errc::invalid_argument, "connection is closed");
    }
    auto r = impl_->client->connect(region, endpoint);
    if (!r.has_value()) {
        return r;
    }
    // Keep the session's config in step so connection_hint()/region() reflect
    // the new target (mirrors the CONNECT statement's effect on the session).
    impl_->cfg.region = region;
    impl_->cfg.endpoint = endpoint;
    impl_->make_session();
    return {};
}

std::string Connection::region() const {
    if (impl_->session == nullptr) {
        return impl_->cfg.region;
    }
    return impl_->session->connection_hint().region;
}

bool Connection::endpoint_overridden() const {
    if (impl_->session == nullptr) {
        return !impl_->cfg.endpoint.empty();
    }
    return impl_->session->connection_hint().endpoint_overridden;
}

void Connection::close() {
    impl_->session.reset();
    impl_->owned.reset();
    impl_->client = nullptr;
    impl_->closed = true;
}

}  // namespace ddbsh
