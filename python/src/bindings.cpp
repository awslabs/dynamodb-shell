// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// bindings.cpp — the nanobind extension module `ddbsh._core` (P13 T3,
// D-P13-5..8).
//
// The Python package's native half. It owns the SDK-free engine stack and
// exposes two thin classes the pure-Python layer (python/ddbsh/) wraps:
//
//   _Engine   one per ddbsh.Connection. It owns the DynamoDB client — a
//             WireClient over a Python transport (PyTransport, below; the
//             transport is python/ddbsh/transport.py, botocore SigV4 + raw
//             HTTP) or a ddbsh::testing::MemoryClient — plus a ddbsh::Session
//             for run(), which drives the CLI's own terminal-free driver
//             (cli/repl.hpp run_source) and returns the CLI's exact bytes.
//   _Cursor   one per ddbsh.Cursor. It owns a ddbsh::Connection (the
//             embedding API: exec / prepare / bind / next_result / next_row)
//             that borrows its engine's client, so each cursor has its own
//             result position and prepared statement.
//
// No engine logic lives here or in Python (tenet 3): parsing, planning,
// execution, rendering and the wire codec are the library's. This file only
// converts values (ddbsh::Value <-> Python objects), maps errors, and manages
// the GIL.
//
// Threads and the GIL. Every engine call releases the GIL, so two Python
// threads with two connections run concurrently (D3 reentrancy). The Python
// transport and the retry callback re-acquire it. One connection (its client
// and every cursor's ddbsh::Connection) is guarded by one mutex, taken only
// after the GIL is released, so a second thread using the same connection
// waits instead of racing; a callback that calls back into its own connection
// gets an error instead of a deadlock. Python objects are built only while the
// GIL is held, after the mutex is dropped.
//
// Errors. A ddbsh::Error is raised as `_core.EngineError(code, message,
// service_error)`, where `message` is the CLI's byte-exact error text
// (Renderer::render_error, trailing newline removed) and `code` names the
// Errc. python/ddbsh/_errors.py maps it to the PEP 249 hierarchy. An exception
// the Python transport raised is kept and attached as the EngineError's
// __cause__.

#include <atomic>
#include <cmath>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <nanobind/nanobind.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>

#include "ddbsh/config.hpp"
#include "ddbsh/connection.hpp"
#include "ddbsh/error.hpp"
#include "ddbsh/format.hpp"
#include "ddbsh/renderer.hpp"
#include "ddbsh/retry_event.hpp"
#include "ddbsh/session.hpp"
#include "ddbsh/testing/memory_client.hpp"
#include "ddbsh/value.hpp"
#include "ddbsh/version.hpp"
#include "ddbsh/wire/transport.hpp"
#include "ddbsh/wire/wire_client.hpp"
#include "repl.hpp"

namespace nb = nanobind;
using namespace nb::literals;

namespace {

// ---- errors -----------------------------------------------------------------

// The module's EngineError type (created in NB_MODULE; owned by the module).
PyObject* g_engine_error = nullptr;

std::string errc_name(ddbsh::Errc code) {
    switch (code) {
        case ddbsh::Errc::parse_error:
            return "parse_error";
        case ddbsh::Errc::no_such_table:
            return "no_such_table";
        case ddbsh::Errc::table_already_exists:
            return "table_already_exists";
        case ddbsh::Errc::conditional_check_failed:
            return "conditional_check_failed";
        case ddbsh::Errc::service_error:
            return "service_error";
        case ddbsh::Errc::invalid_argument:
            return "invalid_argument";
        case ddbsh::Errc::cancelled:
            return "cancelled";
        case ddbsh::Errc::not_reducible_to_getitem:
            return "not_reducible_to_getitem";
        case ddbsh::Errc::not_implemented:
            return "not_implemented";
        case ddbsh::Errc::unbound_parameter:
            return "unbound_parameter";
        case ddbsh::Errc::internal_error:
            return "internal_error";
    }
    return "internal_error";
}

// Raise EngineError(code, message, service_error) with `cause` (may be
// invalid) as its __cause__. GIL held.
[[noreturn]] void raise_engine_error(const std::string& code, std::string message,
                                     const std::optional<std::string>& service_error,
                                     nb::object cause = nb::object()) {
    while (!message.empty() && message.back() == '\n') {
        message.pop_back();
    }
    nb::object service = service_error ? nb::object(nb::str(service_error->c_str())) : nb::none();
    nb::object exc = nb::borrow(g_engine_error)(nb::str(code.c_str()),
                                                nb::str(message.data(), message.size()), service);
    if (cause.is_valid() && !cause.is_none()) {
        PyException_SetCause(exc.ptr(), cause.release().ptr());  // steals the reference
    }
    PyErr_SetObject(g_engine_error, exc.ptr());
    throw nb::python_error();
}

// The text naming a Python exception: "TypeName: str(value)". GIL held.
std::string describe_exception(const nb::python_error& e) {
    std::string name = "Exception";
    std::string text;
    try {
        name = nb::cast<std::string>(nb::handle(e.type()).attr("__name__"));
        text = nb::cast<std::string>(nb::str(e.value()));
    } catch (...) {
        // Fall back to the type name alone.
    }
    return text.empty() ? name : name + ": " + text;
}

// ---- values -------------------------------------------------------------------

// Cached module attributes (imported once; the references are deliberately
// kept for the life of the process).
nb::handle decimal_type() {
    static nb::handle h = nb::object(nb::module_::import_("decimal").attr("Decimal")).release();
    return h;
}
nb::handle a2b_base64() {
    static nb::handle h = nb::object(nb::module_::import_("binascii").attr("a2b_base64")).release();
    return h;
}
nb::handle b2a_base64() {
    static nb::handle h = nb::object(nb::module_::import_("binascii").attr("b2a_base64")).release();
    return h;
}

nb::str to_str(const std::string& s) {
    return nb::str(s.data(), s.size());
}

nb::object to_bytes(const std::string& base64) {
    return nb::borrow(a2b_base64())(to_str(base64));
}

nb::object to_decimal(const std::string& exact) {
    return nb::borrow(decimal_type())(to_str(exact));
}

// ddbsh::Value -> Python (S str, N Decimal, B bytes, BOOL bool, NULL None,
// L list, M dict, SS/NS/BS set). GIL held.
nb::object to_py(const ddbsh::Value& v) {
    using T = ddbsh::Value::Type;
    switch (v.type) {
        case T::S:
            return to_str(v.s);
        case T::N:
            return to_decimal(v.s);
        case T::B:
            return to_bytes(v.s);
        case T::BOOL:
            return nb::bool_(v.boolean);
        case T::NUL:
            return nb::none();
        case T::L: {
            nb::list out;
            for (const auto& e : v.list) {
                out.append(to_py(e));
            }
            return out;
        }
        case T::M: {
            nb::dict out;
            for (const auto& [k, e] : v.map) {
                out[to_str(k)] = to_py(e);
            }
            return out;
        }
        case T::SS:
        case T::NS:
        case T::BS: {
            nb::object out = nb::steal(PySet_New(nullptr));
            for (const auto& e : v.set) {
                nb::object item = v.type == T::SS   ? nb::object(to_str(e))
                                  : v.type == T::NS ? to_decimal(e)
                                                    : to_bytes(e);
                if (PySet_Add(out.ptr(), item.ptr()) != 0) {
                    throw nb::python_error();
                }
            }
            return out;
        }
    }
    return nb::none();
}

nb::dict row_to_py(const ddbsh::ItemRow& row) {
    nb::dict out;
    for (const auto& [k, v] : row.attributes) {
        out[to_str(k)] = to_py(v);
    }
    return out;
}

[[noreturn]] void bind_error(const std::string& message) {
    raise_engine_error("invalid_argument", message, std::nullopt);
}

std::string py_type_name(nb::handle h) {
    return nb::cast<std::string>(nb::handle(Py_TYPE(h.ptr())).attr("__name__"));
}

bool is_number(nb::handle h) {
    return !PyBool_Check(h.ptr()) &&
           (PyLong_Check(h.ptr()) || PyFloat_Check(h.ptr()) || nb::isinstance(h, decimal_type()));
}

bool is_binary(nb::handle h) {
    return PyBytes_Check(h.ptr()) || PyByteArray_Check(h.ptr()) || PyMemoryView_Check(h.ptr());
}

// A Python number -> its exact DynamoDB N text.
std::string number_text(nb::handle h) {
    if (PyFloat_Check(h.ptr())) {
        const double d = PyFloat_AsDouble(h.ptr());
        if (!std::isfinite(d)) {
            bind_error("cannot bind a non-finite float: DynamoDB numbers are finite");
        }
        return nb::cast<std::string>(nb::repr(h));
    }
    if (nb::isinstance(h, decimal_type())) {
        if (!nb::cast<bool>(h.attr("is_finite")())) {
            bind_error("cannot bind a non-finite Decimal: DynamoDB numbers are finite");
        }
    }
    return nb::cast<std::string>(nb::str(h));
}

std::string binary_base64(nb::handle h) {
    nb::object encoded = nb::borrow(b2a_base64())(h, "newline"_a = false);
    return nb::cast<std::string>(encoded.attr("decode")("ascii"));
}

// Python -> ddbsh::Value, the reverse of to_py (D-P13 T3 type mapping). bool
// is tested before int (bool is an int subclass). A float binds as its repr
// (shortest round-trip text); Decimal is the exact form. GIL held.
ddbsh::Value from_py(nb::handle h) {
    using ddbsh::Value;
    if (h.is_none()) {
        return Value::null_value();
    }
    if (PyBool_Check(h.ptr())) {
        return Value::bool_value(h.ptr() == Py_True);
    }
    if (is_number(h)) {
        return Value::number_value(number_text(h));
    }
    if (PyUnicode_Check(h.ptr())) {
        return Value::string_value(nb::cast<std::string>(h));
    }
    if (is_binary(h)) {
        return Value::binary_value(binary_base64(h));
    }
    if (PyList_Check(h.ptr()) || PyTuple_Check(h.ptr())) {
        Value out;
        out.type = Value::Type::L;
        for (nb::handle e : h) {
            out.list.push_back(from_py(e));
        }
        return out;
    }
    if (PyDict_Check(h.ptr())) {
        Value out;
        out.type = Value::Type::M;
        for (auto [k, e] : nb::borrow<nb::dict>(h)) {
            if (!PyUnicode_Check(k.ptr())) {
                bind_error("cannot bind a dict with a " + py_type_name(k) +
                           " key: map keys must be str");
            }
            out.map.emplace_back(nb::cast<std::string>(k), from_py(e));
        }
        return out;
    }
    if (PyAnySet_Check(h.ptr())) {
        std::vector<std::string> elements;
        int kind = 0;  // 1 string, 2 number, 3 binary
        for (nb::handle e : h) {
            const int k = PyUnicode_Check(e.ptr()) ? 1 : is_number(e) ? 2 : is_binary(e) ? 3 : 0;
            if (k == 0 || (kind != 0 && k != kind)) {
                bind_error(
                    "cannot bind a set: its elements must be all str, all numbers or all bytes");
            }
            kind = k;
            elements.push_back(k == 1   ? nb::cast<std::string>(e)
                               : k == 2 ? number_text(e)
                                        : binary_base64(e));
        }
        if (elements.empty()) {
            bind_error("cannot bind an empty set: DynamoDB sets are never empty");
        }
        return kind == 1   ? Value::string_set(std::move(elements))
               : kind == 2 ? Value::number_set(std::move(elements))
                           : Value::binary_set(std::move(elements));
    }
    bind_error("cannot bind a value of type " + py_type_name(h));
}

// ---- the Python transport -------------------------------------------------------

// A ddbsh::wire::Transport over a Python object with send(operation, body) ->
// str, on_connect(region, endpoint) and descriptor() -> str (D-P13-5). Called
// with the GIL released (inside an engine call); each method re-acquires it.
// An exception the Python side raises becomes a transport-level Error and is
// kept so the caller can attach it as __cause__ (or re-raise a
// KeyboardInterrupt).
class PyTransport : public ddbsh::wire::Transport {
public:
    explicit PyTransport(nb::object impl) : impl_(std::move(impl)) {}

    ~PyTransport() override {
        nb::gil_scoped_acquire gil;
        impl_.reset();
        pending_.reset();
    }

    ddbsh::Result<std::string> send(std::string_view operation, std::string_view body) override {
        nb::gil_scoped_acquire gil;
        try {
            nb::object r = impl_.attr("send")(nb::str(operation.data(), operation.size()),
                                              nb::str(body.data(), body.size()));
            if (!PyUnicode_Check(r.ptr())) {
                return failure("send() returned " + py_type_name(r) + ", expected str");
            }
            return nb::cast<std::string>(r);
        } catch (nb::python_error& e) {
            return keep(e);
        }
    }

    ddbsh::Result<void> on_connect(const std::string& region,
                                   const std::string& endpoint) override {
        nb::gil_scoped_acquire gil;
        try {
            impl_.attr("on_connect")(to_str(region), to_str(endpoint));
            return {};
        } catch (nb::python_error& e) {
            auto r = keep(e);
            return std::unexpected(r.error());
        }
    }

    std::string descriptor() const override {
        nb::gil_scoped_acquire gil;
        try {
            return nb::cast<std::string>(nb::str(impl_.attr("descriptor")()));
        } catch (nb::python_error&) {
            // Informational only; keep a generic label.
            return "wire/python";
        } catch (const std::exception&) {
            return "wire/python";
        }
    }

    /// The exception the last failing call raised, or an invalid object; the
    /// slot is cleared. GIL held.
    nb::object take_pending() { return std::exchange(pending_, nb::object()); }

    /// Cyclic-GC support (see engine_tp_traverse). GIL held.
    int traverse(visitproc visit, void* arg) {
        Py_VISIT(impl_.ptr());
        Py_VISIT(pending_.ptr());
        return 0;
    }
    void clear() {
        impl_ = nb::none();
        pending_.reset();
    }

private:
    static ddbsh::Result<std::string> failure(const std::string& what) {
        return std::unexpected(
            ddbsh::Error{ddbsh::Errc::service_error, "wire transport: " + what, {}, {}});
    }

    ddbsh::Result<std::string> keep(nb::python_error& e) {
        std::string what = describe_exception(e);
        pending_ = nb::borrow(e.value());
        return failure(what);
    }

    nb::object impl_;
    nb::object pending_;
};

// ---- retry events ---------------------------------------------------------------

/// The Python view of one ddbsh::RetryEvent (P12, D-P12-18).
struct PyRetryEvent {
    std::string operation;
    int attempt = 0;
    int max_attempts = 0;
    std::string error_code;
    bool throttled = false;
    std::optional<double> delay_ms;
};

// Calls a Python callable with a RetryEvent before each retry. Its exceptions
// go to sys.unraisablehook (the retry still happens, retry_event.hpp).
class PyRetryObserver : public ddbsh::RetryObserver {
public:
    explicit PyRetryObserver(nb::object fn) : fn_(std::move(fn)) {}
    ~PyRetryObserver() override {
        nb::gil_scoped_acquire gil;
        fn_.reset();
    }

    void on_retry(const ddbsh::RetryEvent& event) override {
        PyRetryEvent ev{event.operation,  event.attempt,   event.max_attempts,
                        event.error_code, event.throttled, std::nullopt};
        if (event.delay) {
            ev.delay_ms = static_cast<double>(event.delay->count());
        }
        nb::gil_scoped_acquire gil;
        try {
            fn_(nb::cast(ev));
        } catch (nb::python_error& e) {
            e.discard_as_unraisable("ddbsh retry callback");
        }
    }

    /// Cyclic-GC support (see engine_tp_traverse). GIL held.
    int traverse(visitproc visit, void* arg) {
        Py_VISIT(fn_.ptr());
        return 0;
    }
    void clear() { fn_ = nb::none(); }

private:
    nb::object fn_;
};

// ---- the engine ---------------------------------------------------------------------

// The per-connection call guard: one mutex for the connection's client and
// every cursor's ddbsh::Connection, the thread inside an engine call (to turn a
// re-entrant call from a callback into an error, not a deadlock), and the
// closed flag.
struct Guard {
    std::mutex mu;
    std::atomic<std::thread::id> owner{};
    std::atomic<bool> closed{false};
};

// One engine call: check the connection is usable (GIL held), then run `f`
// with the GIL released and the guard's mutex held. The mutex is taken only
// after the GIL is released, so a thread waiting for it never holds the GIL
// the owner's transport needs. `f` must not touch Python objects.
template <class F>
auto locked(Guard& guard, F&& f) {
    if (guard.closed) {
        raise_engine_error("interface_error", "connection is closed", std::nullopt);
    }
    if (guard.owner.load() == std::this_thread::get_id()) {
        raise_engine_error("reentrant_call",
                           "a ddbsh connection was used from inside its own transport or "
                           "retry callback",
                           std::nullopt);
    }
    nb::gil_scoped_release release;
    std::lock_guard<std::mutex> lock(guard.mu);
    guard.owner.store(std::this_thread::get_id());
    struct Reset {
        Guard& g;
        ~Reset() { g.owner.store(std::thread::id()); }
    } reset{guard};
    return f();
}

class Engine {
public:
    /// A wire engine over a Python transport.
    Engine(nb::object transport, std::string region, std::string endpoint)
        : cfg_{std::move(region), std::move(endpoint)} {
        auto t = std::make_unique<PyTransport>(std::move(transport));
        client_ = std::make_unique<ddbsh::wire::WireClient>(*t);
        transport_ = std::move(t);
        session_ = std::make_unique<ddbsh::Session>(cfg_, *client_);
    }

    /// An in-memory engine (ddbsh::testing::MemoryClient).
    explicit Engine(std::string region) : cfg_{std::move(region), ""} {
        client_ = std::make_unique<ddbsh::testing::MemoryClient>();
        session_ = std::make_unique<ddbsh::Session>(cfg_, *client_);
    }

    ~Engine() {
        // Members borrow each other: session -> client -> transport. Destroy in
        // that order (the declaration order below gives the same; explicit for
        // the reader).
        session_.reset();
        if (client_) {
            client_->set_retry_observer(nullptr);
        }
        client_.reset();
        observer_.reset();
        transport_.reset();
    }

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    /// Run `text` exactly as the CLI's -c runs it (run_source: terminator
    /// lines, `set output`, stop-on-quit, abort-on-error). Returns
    /// ([(stream, text), ...], status): the output in emission order, adjacent
    /// same-stream text coalesced, stream "stdout" or "stderr", and the CLI's
    /// exit status. When `on_output` is not None it is also called as
    /// on_output(stream, text) for each piece as the engine emits it (timely
    /// output, P12); the first exception it raises stops further calls and is
    /// raised when the run ends.
    nb::tuple run(const std::string& text, const std::string& output, const std::string& flavor,
                  bool abort_on_error, nb::object on_output) {
        ddbsh::render::FormatOptions options;
        if (auto f = ddbsh::render::parse_output_format(output)) {
            options.format = *f;
        } else {
            std::string m = ddbsh::cli::unknown_output_format_message(output);
            m.pop_back();
            throw nb::value_error(m.c_str());
        }
        if (auto f = ddbsh::render::parse_json_flavor(flavor)) {
            options.json_flavor = *f;
        } else {
            std::string m = ddbsh::cli::unknown_json_flavor_message(flavor);
            m.pop_back();
            throw nb::value_error(m.c_str());
        }

        std::vector<std::pair<bool, std::string>> segments;  // (is_stderr, text)
        nb::object callback_error;
        const bool has_callback = !on_output.is_none();
        auto append = [&](bool err, std::string_view piece) {
            if (piece.empty()) {
                return;
            }
            if (!segments.empty() && segments.back().first == err) {
                segments.back().second += piece;
            } else {
                segments.emplace_back(err, std::string(piece));
            }
            if (has_callback) {
                nb::gil_scoped_acquire gil;
                if (callback_error.is_valid()) {
                    return;
                }
                try {
                    on_output(err ? "stderr" : "stdout", nb::str(piece.data(), piece.size()));
                } catch (nb::python_error& e) {
                    callback_error = nb::borrow(e.value());
                }
            }
        };
        const int status = locked(guard_, [&] {
            ddbsh::render::FormattingSink sink(
                options, [&](ddbsh::render::Stream stream, std::string_view piece) {
                    append(stream == ddbsh::render::Stream::err, piece);
                });
            std::istringstream in(text);
            ddbsh::cli::StreamLineSource src(in);
            ddbsh::cli::Terminator term;  // the CLI default (`go`)
            return ddbsh::cli::run_source(
                *session_, renderer_, src, term, abort_on_error, sink,
                [&append](const std::string& rendered) { append(true, rendered); });
        });
        reraise_interrupt();
        if (callback_error.is_valid()) {
            PyErr_SetObject(reinterpret_cast<PyObject*>(Py_TYPE(callback_error.ptr())),
                            callback_error.ptr());
            throw nb::python_error();
        }

        nb::list out;
        for (const auto& [err, piece] : segments) {
            out.append(nb::make_tuple(err ? "stderr" : "stdout", to_str(piece)));
        }
        return nb::make_tuple(out, status);
    }

    /// The current region (CONNECT changes it).
    std::string region() {
        return locked(guard_, [&] { return session_->connection_hint().region; });
    }

    /// Whether an endpoint override is in effect.
    bool endpoint_overridden() {
        return locked(guard_, [&] { return session_->connection_hint().endpoint_overridden; });
    }

    /// Register `fn(event)` to run before each retry, or clear it with None.
    void set_retry_callback(nb::object fn) {
        std::unique_ptr<PyRetryObserver> next;
        if (!fn.is_none()) {
            next = std::make_unique<PyRetryObserver>(std::move(fn));
        }
        // Under the engine mutex, so no retry is running while the old observer
        // is destroyed.
        std::unique_ptr<PyRetryObserver> old = locked(guard_, [&] {
            client_->set_retry_observer(next.get());
            auto prev = std::move(observer_);
            observer_ = std::move(next);
            return prev;
        });
        old.reset();  // GIL held: the observer releases its callable
    }

    void close() { guard_.closed = true; }
    bool closed() const { return guard_.closed; }
    Guard& guard() { return guard_; }

    /// After an engine call (GIL held): drop the transport's kept exception,
    /// and re-raise it when it is a KeyboardInterrupt or SystemExit (anything
    /// that is not an Exception), so Ctrl-C is never swallowed.
    void reraise_interrupt() { (void)take_cause(); }

    /// Raise `error` (GIL held), with the transport's kept exception, if any,
    /// as its __cause__.
    [[noreturn]] void raise(const ddbsh::Error& error) {
        nb::object cause = take_cause();
        raise_engine_error(errc_name(error.code), renderer_.render_error(error),
                           error.service_error_name, std::move(cause));
    }

    /// Cyclic-GC support: the Python objects the engine holds (the transport
    /// object and the retry callback) can refer back to the Connection, for
    /// example a transport whose handler closes over it. GIL held.
    int traverse(visitproc visit, void* arg) {
        if (transport_) {
            if (int r = transport_->traverse(visit, arg)) {
                return r;
            }
        }
        if (observer_) {
            if (int r = observer_->traverse(visit, arg)) {
                return r;
            }
        }
        return 0;
    }
    void clear() {
        if (transport_) {
            transport_->clear();
        }
        if (observer_) {
            observer_->clear();
        }
    }

    ddbsh::IDDBClient& client() { return *client_; }
    ddbsh::SessionConfig config() const { return cfg_; }
    const ddbsh::render::Renderer& renderer() const { return renderer_; }

private:
    /// Take the transport's kept exception (GIL held). A KeyboardInterrupt or
    /// SystemExit is re-raised as itself; an ordinary exception is returned.
    nb::object take_cause() {
        nb::object e = transport_ ? transport_->take_pending() : nb::object();
        if (e.is_valid() && !PyObject_IsInstance(e.ptr(), PyExc_Exception)) {
            PyErr_SetObject(reinterpret_cast<PyObject*>(Py_TYPE(e.ptr())), e.ptr());
            throw nb::python_error();
        }
        return e;
    }

    ddbsh::SessionConfig cfg_;
    std::unique_ptr<PyTransport> transport_;
    std::unique_ptr<ddbsh::IDDBClient> client_;
    std::unique_ptr<ddbsh::Session> session_;
    std::unique_ptr<PyRetryObserver> observer_;
    ddbsh::render::Renderer renderer_;
    Guard guard_;
};

// ---- the cursor ---------------------------------------------------------------------

std::string kind_name(ddbsh::ResultKind k) {
    switch (k) {
        case ddbsh::ResultKind::rows:
            return "rows";
        case ddbsh::ResultKind::status:
            return "status";
        case ddbsh::ResultKind::write:
            return "write";
        case ddbsh::ResultKind::report:
            return "report";
        case ddbsh::ResultKind::explain:
            return "explain";
        case ddbsh::ResultKind::empty:
            return "empty";
    }
    return "empty";
}

class Cursor {
public:
    explicit Cursor(Engine& engine)
        : engine_(engine),
          conn_(std::make_unique<ddbsh::Connection>(engine.config(), engine.client())) {}

    ~Cursor() = default;
    Cursor(const Cursor&) = delete;
    Cursor& operator=(const Cursor&) = delete;

    /// Execute a batch of one or more statements; positioned at the first
    /// result set. Raises when no statement could run (a parse error, or the
    /// first statement failed).
    void execute(const std::string& sql) {
        auto r = locked(engine_.guard(), [&] {
            results_.reset();
            return conn_->exec(sql);
        });
        adopt(std::move(r));
    }

    /// Prepare `sql` (one statement; cached while the text is unchanged),
    /// bind `params` to its `?` markers in order, and execute it.
    void execute_params(const std::string& sql, nb::sequence params) {
        std::vector<ddbsh::Value> values;
        for (nb::handle p : params) {
            values.push_back(from_py(p));
        }
        auto r = locked(engine_.guard(), [&]() -> ddbsh::Result<ddbsh::Results> {
            results_.reset();
            if (!prepared_ || prepared_sql_ != sql) {
                prepared_.reset();
                auto p = conn_->prepare(sql);
                if (!p.has_value()) {
                    return std::unexpected(p.error());
                }
                prepared_.emplace(std::move(*p));
                prepared_sql_ = sql;
            }
            const int count = prepared_->parameter_count();
            if (count != static_cast<int>(values.size())) {
                return std::unexpected(ddbsh::Error{
                    ddbsh::Errc::invalid_argument,
                    "the statement has " + std::to_string(count) + " parameter marker" +
                        (count == 1 ? "" : "s") + " but " + std::to_string(values.size()) +
                        " parameter" + (values.size() == 1 ? " was" : "s were") + " given",
                    {},
                    {}});
            }
            for (std::size_t i = 0; i < values.size(); ++i) {
                prepared_->bind(static_cast<int>(i + 1), std::move(values[i]));
            }
            return conn_->exec(*prepared_);
        });
        adopt(std::move(r));
    }

    /// The current result set's kind, or None before any execute().
    std::optional<std::string> kind() {
        return locked(engine_.guard(), [&]() -> std::optional<std::string> {
            if (!results_) {
                return std::nullopt;
            }
            return kind_name(results_->kind());
        });
    }

    /// Advance to the next result set. False at the end; raises the error that
    /// stopped the batch.
    bool next_result() {
        const bool more =
            locked(engine_.guard(), [&] { return results_ && results_->next_result(); });
        if (!more) {
            check_error();
        }
        return more;
    }

    /// Up to `n` rows (all when n < 0) of the current rows result set, as
    /// dicts. Fewer than asked means the set is exhausted.
    nb::list fetch(long n) {
        std::optional<std::string> wrong_kind;
        std::vector<ddbsh::ItemRow> rows;
        locked(engine_.guard(), [&] {
            if (!results_) {
                wrong_kind = "no statement has been executed";
                return;
            }
            if (results_->kind() != ddbsh::ResultKind::rows) {
                wrong_kind = "the current result set returns no rows (kind " +
                             kind_name(results_->kind()) + ")";
                return;
            }
            while ((n < 0 || static_cast<long>(rows.size()) < n) && results_->next_row()) {
                rows.push_back(results_->row().item());
            }
        });
        if (wrong_kind) {
            raise_engine_error("no_result_set", *wrong_kind, std::nullopt);
        }
        nb::list out;
        for (const auto& row : rows) {
            out.append(row_to_py(row));
        }
        if (n < 0 || static_cast<long>(rows.size()) < n) {
            check_error();
        }
        return out;
    }

    std::string status_token() {
        return locked(engine_.guard(), [&] { return results_ ? results_->status_token() : ""; });
    }

    nb::tuple write_counts() {
        long read = 0;
        long modified = 0;
        long ccf = 0;
        std::string verb;
        locked(engine_.guard(), [&] {
            if (results_ && results_->kind() == ddbsh::ResultKind::write) {
                read = results_->read();
                modified = results_->modified();
                ccf = results_->ccf();
                switch (results_->verb()) {
                    case ddbsh::WriteVerb::update:
                        verb = "UPDATE";
                        break;
                    case ddbsh::WriteVerb::upsert:
                        verb = "UPSERT";
                        break;
                    case ddbsh::WriteVerb::del:
                        verb = "DELETE";
                        break;
                }
            }
        });
        if (verb.empty()) {
            return nb::make_tuple(nb::none(), nb::none(), nb::none(), nb::none());
        }
        return nb::make_tuple(verb, read, modified, ccf);
    }

    nb::list lines() {
        std::vector<std::string> copy;
        locked(engine_.guard(), [&] {
            if (results_) {
                copy = results_->lines();
            }
        });
        nb::list out;
        for (const auto& l : copy) {
            out.append(to_str(l));
        }
        return out;
    }

    nb::list diagnostics() {
        std::vector<std::string> copy;
        locked(engine_.guard(), [&] {
            if (results_) {
                copy = results_->diagnostics();
            }
        });
        nb::list out;
        for (const auto& l : copy) {
            out.append(to_str(l));
        }
        return out;
    }

    std::string explain_text() {
        return locked(engine_.guard(), [&] { return results_ ? results_->explain_text() : ""; });
    }

    /// The current result set's exact CLI stdout bytes (drains a lazy SELECT).
    std::string render() {
        std::string out =
            locked(engine_.guard(), [&] { return results_ ? results_->render() : std::string(); });
        check_error();
        return out;
    }

    void close() {
        locked(engine_.guard(), [&] {
            results_.reset();
            prepared_.reset();
            conn_->close();
        });
    }

private:
    void adopt(ddbsh::Result<ddbsh::Results> r) {
        if (!r.has_value()) {
            engine_.raise(r.error());
        }
        results_.emplace(std::move(*r));
        error_raised_ = false;
        // When the first statement failed nothing ran, so execute() raises.
        check_error();
    }

    /// Raise the batch's error once (GIL held); otherwise re-raise a kept
    /// KeyboardInterrupt and drop any other kept transport exception.
    void check_error() {
        std::optional<ddbsh::Error> e =
            locked(engine_.guard(), [&]() -> std::optional<ddbsh::Error> {
                if (!results_ || error_raised_) {
                    return std::nullopt;
                }
                return results_->error();
            });
        if (e.has_value()) {
            error_raised_ = true;
            engine_.raise(*e);
        }
        engine_.reraise_interrupt();
    }

    Engine& engine_;
    std::unique_ptr<ddbsh::Connection> conn_;
    std::optional<ddbsh::Results> results_;
    std::optional<ddbsh::PreparedStatement> prepared_;
    std::string prepared_sql_;
    bool error_raised_ = false;
};

// tp_traverse / tp_clear for _Engine, so a reference cycle through the engine
// (Connection -> engine -> transport -> ... -> Connection) is collectable.
int engine_tp_traverse(PyObject* self, visitproc visit, void* arg) {
#if PY_VERSION_HEX >= 0x03090000
    Py_VISIT(Py_TYPE(self));
#endif
    if (!nb::inst_ready(self)) {
        return 0;
    }
    return nb::inst_ptr<Engine>(self)->traverse(visit, arg);
}

int engine_tp_clear(PyObject* self) {
    if (nb::inst_ready(self)) {
        nb::inst_ptr<Engine>(self)->clear();
    }
    return 0;
}

PyType_Slot engine_slots[] = {
    {Py_tp_traverse, reinterpret_cast<void*>(engine_tp_traverse)},
    {Py_tp_clear, reinterpret_cast<void*>(engine_tp_clear)},
    {0, nullptr},
};

}  // namespace

NB_MODULE(_core, m) {
    m.doc() = "ddbsh native engine (internal; use the ddbsh package)";

    g_engine_error = PyErr_NewException("ddbsh._core.EngineError", PyExc_Exception, nullptr);
    m.attr("EngineError") = nb::handle(g_engine_error);  // the module keeps a reference

    m.def("version", &ddbsh::version, "The ddbsh library version (SHOW VERSION's number).");

    nb::class_<PyRetryEvent>(m, "RetryEvent", "One retry, raised before its backoff.")
        .def_ro("operation", &PyRetryEvent::operation)
        .def_ro("attempt", &PyRetryEvent::attempt)
        .def_ro("max_attempts", &PyRetryEvent::max_attempts)
        .def_ro("error_code", &PyRetryEvent::error_code)
        .def_ro("throttled", &PyRetryEvent::throttled)
        .def_ro("delay_ms", &PyRetryEvent::delay_ms)
        .def("__repr__", [](const PyRetryEvent& e) {
            return "RetryEvent(operation=" + e.operation +
                   ", attempt=" + std::to_string(e.attempt) +
                   ", max_attempts=" + std::to_string(e.max_attempts) +
                   ", error_code=" + e.error_code +
                   ", throttled=" + (e.throttled ? "True" : "False") + ")";
        });

    nb::class_<Engine>(m, "_Engine", nb::type_slots(engine_slots))
        .def(nb::init<nb::object, std::string, std::string>(), "transport"_a, "region"_a,
             "endpoint"_a)
        .def(nb::init<std::string>(), "region"_a)
        .def("run", &Engine::run, "text"_a, "output"_a, "json_flavor"_a, "abort_on_error"_a,
             "on_output"_a.none())
        .def("region", &Engine::region)
        .def("endpoint_overridden", &Engine::endpoint_overridden)
        .def("set_retry_callback", &Engine::set_retry_callback, "fn"_a.none())
        .def("close", &Engine::close)
        .def_prop_ro("closed", &Engine::closed)
        .def(
            "cursor", [](Engine& e) { return new Cursor(e); }, nb::keep_alive<0, 1>(),
            nb::rv_policy::take_ownership);

    nb::class_<Cursor>(m, "_Cursor")
        .def("execute", &Cursor::execute, "sql"_a)
        .def("execute_params", &Cursor::execute_params, "sql"_a, "params"_a)
        .def("kind", &Cursor::kind)
        .def("next_result", &Cursor::next_result)
        .def("fetch", &Cursor::fetch, "n"_a)
        .def("status_token", &Cursor::status_token)
        .def("write_counts", &Cursor::write_counts)
        .def("lines", &Cursor::lines)
        .def("diagnostics", &Cursor::diagnostics)
        .def("explain_text", &Cursor::explain_text)
        .def("render", &Cursor::render)
        .def("close", &Cursor::close);
}
