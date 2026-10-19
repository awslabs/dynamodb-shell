// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// rate_limiter.cpp — see rate_limiter.hpp.

#include "execute/rate_limiter.hpp"

#include <algorithm>
#include <thread>
#include <utility>

namespace ddbsh::engine {

namespace {

/// Default clock: steady_clock's time since epoch as seconds (double). Monotonic
/// so a backward wall-clock step cannot make the limiter mis-refill.
std::chrono::duration<double> steady_now() {
    return std::chrono::duration_cast<std::chrono::duration<double>>(
        std::chrono::steady_clock::now().time_since_epoch());
}

}  // namespace

RateLimiter::RateLimiter(double rcu, double wcu, Clock clock, Sleep sleep)
    : rcu_(rcu),
      wcu_(wcu),
      clock_(clock ? std::move(clock) : Clock(&steady_now)),
      sleep_(sleep ? std::move(sleep) : Sleep([](std::chrono::duration<double> d) {
          std::this_thread::sleep_for(d);
      })) {
    last_ = clock_();
}

void RateLimiter::consume_reads(const std::optional<ConsumedCapacity>& cc) {
    if (cc.has_value()) {
        read_tokens_ -= cc->capacity_units.value_or(0.0);
    }
}

void RateLimiter::consume_writes(const std::optional<ConsumedCapacity>& cc) {
    if (cc.has_value()) {
        write_tokens_ -= cc->capacity_units.value_or(0.0);
    }
}

void RateLimiter::refresh() {
    const std::chrono::duration<double> now = clock_();
    const double elapsed = (now - last_).count();
    // Add elapsed*rate to each bucket, capped at the rate (bucket holds at most
    // ~1s of credit — CRateLimit::refresh).
    read_tokens_ = std::min(read_tokens_ + elapsed * rcu_, rcu_);
    write_tokens_ = std::min(write_tokens_ + elapsed * wcu_, wcu_);
    last_ = now;
}

void RateLimiter::readlimit() {
    refresh();
    // A read rate of 0 disables read limiting (and avoids divide-by-zero);
    // return immediately if the bucket is already non-negative.
    if (rcu_ == 0.0 || read_tokens_ >= 0.0) {
        return;
    }
    do {
        if (read_tokens_ < 0.0) {
            sleep_(std::chrono::duration<double>((-read_tokens_) / rcu_));
        }
        refresh();
    } while (read_tokens_ < 0.0);
}

void RateLimiter::writelimit() {
    refresh();
    if (wcu_ == 0.0 || write_tokens_ >= 0.0) {
        return;
    }
    do {
        if (write_tokens_ < 0.0) {
            sleep_(std::chrono::duration<double>((-write_tokens_) / wcu_));
        }
        refresh();
    } while (write_tokens_ < 0.0);
}

}  // namespace ddbsh::engine
