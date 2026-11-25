#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <random>
#include <thread>
#include <stdexcept>

#include "llmgate/provider/provider.h"
#include "llmgate/util/clock.h"

namespace llmgate {

// How to retry a call that failed in a way that might go away (a transient
// ProviderError).
struct RetryPolicy {
    int max_attempts = 3;                           // tries in total, including the first
    std::chrono::milliseconds base_delay{100};      // wait scale before the 2nd try
    std::chrono::milliseconds max_delay{2000};      // no single wait is longer than this
    std::chrono::milliseconds total_budget{10000};  // give up when this much time has passed
};

// Pauses the calling thread. Production passes a real sleep; tests pass a
// function that only records the delay, so they run instantly.
using Sleeper = std::function<void(std::chrono::milliseconds)>;

inline Sleeper real_sleeper() {
    return [](std::chrono::milliseconds d) { std::this_thread::sleep_for(d); };
}

// The wait before retry number `attempt` (0 = the wait after the first failure).
//
// "Full jitter": pick a random time between 0 and min(max_delay, base * 2^attempt).
// The ceiling doubles every time, so a struggling provider gets more room, and
// the randomness spreads clients out so they do not all retry at the same instant.
inline std::chrono::milliseconds backoff_delay(const RetryPolicy& policy, int attempt,
                                               std::mt19937_64& rng) {
    const std::int64_t base = std::max<std::int64_t>(0, policy.base_delay.count());
    const std::int64_t cap_limit = std::max<std::int64_t>(0, policy.max_delay.count());

    // base * 2^attempt, but stop doubling once we are past the cap (no overflow).
    std::int64_t ceiling = base;
    for (int i = 0; i < attempt && ceiling < cap_limit; ++i) ceiling *= 2;
    ceiling = std::min(ceiling, cap_limit);

    std::uniform_int_distribution<std::int64_t> pick(0, ceiling);
    return std::chrono::milliseconds(pick(rng));
}

// Calls fn(). If it throws a ProviderError that is transient, waits (with
// backoff) and tries again, up to policy.max_attempts tries and policy.total_budget
// of elapsed time. The last error is rethrown when we give up.
//
//   - a non-transient ProviderError (a bad request, an unknown model) is rethrown
//     at once: retrying can never help;
//   - any other exception is rethrown at once as well;
//   - a wait that would carry us past total_budget is not started, so one request
//     cannot hang for minutes. This also stops us retrying after a slow timeout.
//
// `sleeper`, `rng` and `clock` are passed in so that tests are instant and repeatable.
template <typename Fn>
auto call_with_retry(const RetryPolicy& policy, Fn&& fn, const Sleeper& sleeper,
                     std::mt19937_64& rng, const Clock& clock = real_clock()) -> decltype(fn()) {
    const int attempts = std::max(1, policy.max_attempts);
    const TimePoint start = clock();

    for (int attempt = 0;; ++attempt) {
        try {
            return fn();
        } catch (const ProviderError& e) {
            if (!e.transient()) throw;
            if (attempt + 1 >= attempts) throw;

            const auto delay = backoff_delay(policy, attempt, rng);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(clock() - start);
            if (elapsed + delay > policy.total_budget) throw;

            sleeper(delay);
        }
    }
}

}  // namespace llmgate
