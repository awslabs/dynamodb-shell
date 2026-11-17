// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// canned_transport.hpp — a scripted ddbsh::wire::Transport test double (P6b).
//
// The transport counterpart to tests/fake_ddb_client.hpp: it returns canned
// response bodies so WireClient's serialize -> send -> parse pipeline can be
// driven offline, with no AWS account and no browser. Each send() pops the next
// scripted reply (a success body or a transport-level Error) and records the
// (operation, body) it was handed, so a test can both feed responses and assert
// the exact request bytes the client produced.
//
// Header-only, SDK-free, terminal-free — part of the engine library's public
// installed surface under include/ddbsh/testing/, beside MemoryClient.

#ifndef DDBSH_TESTING_CANNED_TRANSPORT_HPP
#define DDBSH_TESTING_CANNED_TRANSPORT_HPP

#include <cstddef>
#include <deque>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ddbsh/result.hpp"
#include "ddbsh/wire/transport.hpp"

namespace ddbsh::testing {

/// A Transport that replays scripted replies in FIFO order and logs every call.
///
/// Usage:
///   CannedTransport t;
///   t.push_response(R"({"Item":{"id":{"S":"k1"}}})");   // next send() returns this body
///   WireClient client(t);
///   auto out = client.get_item(in);
///   EXPECT_EQ(t.calls().at(0).operation, "GetItem");
class CannedTransport : public ddbsh::wire::Transport {
public:
    /// A recorded send() invocation.
    struct Call {
        std::string operation;
        std::string body;
    };

    /// Enqueue a success response body (returned by a later send()).
    void push_response(std::string body) {
        replies_.push_back(Reply{std::move(body), Error{}, false});
    }

    /// Enqueue a transport-level failure (returned by a later send()).
    void push_error(Error error) {
        replies_.push_back(Reply{std::string{}, std::move(error), true});
    }

    /// The recorded calls, in order.
    [[nodiscard]] const std::vector<Call>& calls() const { return calls_; }

    /// Whether every scripted reply has been consumed.
    [[nodiscard]] bool drained() const { return replies_.empty(); }

    /// Override the descriptor reported by SHOW CONNECTION (D-P6f-9), so a test
    /// can prove WireClient delegates connection_descriptor() to its transport.
    void set_descriptor(std::string descriptor) { descriptor_ = std::move(descriptor); }

    std::string descriptor() const override { return descriptor_; }

    Result<std::string> send(std::string_view operation, std::string_view body) override {
        calls_.push_back(Call{std::string(operation), std::string(body)});
        if (replies_.empty()) {
            return fail(Errc::service_error,
                        "CannedTransport: no scripted reply for " + std::string(operation));
        }
        Reply reply = std::move(replies_.front());
        replies_.pop_front();
        if (reply.is_error) {
            return fail(std::move(reply.error));
        }
        return reply.body;
    }

private:
    struct Reply {
        std::string body;
        Error error;
        bool is_error = false;
    };

    std::deque<Reply> replies_;
    std::vector<Call> calls_;
    std::string descriptor_ = "wire transport";
};

}  // namespace ddbsh::testing

#endif  // DDBSH_TESTING_CANNED_TRANSPORT_HPP
