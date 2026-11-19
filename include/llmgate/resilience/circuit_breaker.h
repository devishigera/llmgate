#pragma once

#include <chrono>
#include <mutex>

#include "llmgate/util/clock.h"

namespace llmgate {

enum class BreakerState { Closed, Open, HalfOpen };

// Stops calling a provider that keeps failing, then tests it carefully.
//
//   Closed   -> normal. Too many failures in a row open the breaker.
//   Open     -> fail fast: no calls reach the provider until the cool-down ends.
//   HalfOpen -> after the cool-down, let a few trial calls through. If they all
//               succeed the breaker closes; if any fails it opens again.
//
// Typical use:
//   if (!breaker.allow_request()) { /* skip this provider */ }
//   else { try { call(); breaker.record_success(); }
//          catch (...) { breaker.record_failure(); } }
class CircuitBreaker {
public:
    // failure_threshold: consecutive failures that open the breaker (>= 1)
    // cool_down:         how long to stay open before trying again (> 0)
    // half_open_trials:  trial calls allowed, and needed, in half-open (>= 1)
    CircuitBreaker(int failure_threshold, std::chrono::milliseconds cool_down,
                   int half_open_trials, Clock clock);

    bool allow_request();
    void record_success();
    void record_failure();

    // The stored state. Open turns into HalfOpen when allow_request() is
    // called after the cool-down, not on its own.
    BreakerState state() const;

private:
    void open_locked();

    const int failure_threshold_;
    const std::chrono::milliseconds cool_down_;
    const int half_open_trials_;
    Clock clock_;

    mutable std::mutex mutex_;
    BreakerState state_ = BreakerState::Closed;
    int consecutive_failures_ = 0;
    int trials_in_flight_ = 0;
    int trial_successes_ = 0;
    TimePoint opened_at_{};
};

const char* to_string(BreakerState state);

}  // namespace llmgate