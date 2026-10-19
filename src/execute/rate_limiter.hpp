// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// rate_limiter.hpp — a client-side token-bucket rate limiter (port of the
// original dynamodb/ratelimit.hpp CRateLimit, arch §10).
//
// PRIVATE engine header (not installed). Two independent token buckets (read,
// write), rates measured in capacity units per second. Semantics are IDENTICAL
// to the original (D-2g-6):
//   * tokens start at 0, last-refresh time = construction time;
//   * consume_reads/consume_writes subtract the TOTAL CapacityUnits of a
//     response (NOT the read/write split — matching CRateLimit::consume_*,
//     which subtract GetCapacityUnits());
//   * refresh() adds elapsed*rate to each bucket, capped AT the rate (so a
//     bucket holds at most ~1s of credit — arch §10);
//   * readlimit()/writelimit() refresh, then return immediately when the rate
//     is 0 (limiting disabled, also avoids divide-by-zero) or the bucket is
//     non-negative; otherwise sleep for deficit/rate seconds and re-refresh in
//     a loop until the bucket is non-negative.
//
// Output is UNCHANGED by the limiter — it only sleeps (arch §10). This class is
// a deliberate PORTABILITY improvement over the original's gettimeofday/usleep:
// it uses std::chrono + std::this_thread::sleep_for (cross-platform), with the
// clock and sleep INJECTABLE so the native unit test exercises the bucket math
// and the sleep DECISION (how long it would sleep) with zero real wall-clock
// delay. This changes no observable behavior (D-2g-6), so it is not a fidelity
// divergence (tenet 2).

#ifndef DDBSH_SRC_EXECUTE_RATE_LIMITER_HPP
#define DDBSH_SRC_EXECUTE_RATE_LIMITER_HPP

#include <chrono>
#include <functional>
#include <optional>

#include "ddbsh/iddbclient.hpp"

namespace ddbsh::engine {

/// A two-bucket token-bucket limiter. Construct from a WITH RATELIMIT clause's
/// (rcu, wcu); a zero rate disables that bucket.
class RateLimiter {
public:
    /// Seconds (as a double) read from a monotonic source. Injectable so tests
    /// drive time deterministically; defaults to std::chrono::steady_clock.
    using Clock = std::function<std::chrono::duration<double>()>;
    /// Sleep for a duration. Injectable so tests record the requested sleep
    /// without blocking; defaults to std::this_thread::sleep_for.
    using Sleep = std::function<void(std::chrono::duration<double>)>;

    /// rcu/wcu are the per-second refill rates (and the bucket capacity). A
    /// null `clock`/`sleep` selects the real steady-clock / sleep_for default.
    RateLimiter(double rcu, double wcu, Clock clock = {}, Sleep sleep = {});

    /// Subtract a response's TOTAL CapacityUnits from the read bucket. A missing
    /// capacity (nullopt, or no CapacityUnits) contributes 0 (no throttle) — the
    /// engine never asserts on service data (D12).
    void consume_reads(const std::optional<ConsumedCapacity>& cc);
    /// Subtract a response's TOTAL CapacityUnits from the write bucket.
    void consume_writes(const std::optional<ConsumedCapacity>& cc);

    /// Block until the read bucket is non-negative (no-op when rcu == 0).
    void readlimit();
    /// Block until the write bucket is non-negative (no-op when wcu == 0).
    void writelimit();

private:
    void refresh();

    double rcu_;
    double wcu_;
    double read_tokens_ = 0.0;
    double write_tokens_ = 0.0;
    std::chrono::duration<double> last_{};
    Clock clock_;
    Sleep sleep_;
};

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_EXECUTE_RATE_LIMITER_HPP
