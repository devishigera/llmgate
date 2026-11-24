#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace llmgate {

// Calendar time. Budgets reset at UTC midnight, which is a real-world
// boundary, so this is the one place where wall-clock time is correct
// (everything else uses the steady Clock from clock.h).
using WallTime = std::chrono::system_clock::time_point;
using WallClock = std::function<WallTime()>;

inline WallClock real_wall_clock() {
    return [] { return std::chrono::system_clock::now(); };
}

// Counts the tokens each API key has used today (UTC day).
//
//   can_spend()  - would this request still fit in the daily budget?
//   record()     - add the tokens a request really used
//   used_today() - how many tokens the key has used today
//
// When the UTC day changes, a key's counter starts again from zero. This
// happens lazily: each counter remembers which day it belongs to.
//
// can_spend() and record() are separate calls, so several requests that run
// at the same moment can each pass the check and together go slightly over
// the budget. The overshoot is bounded by the number of simultaneous
// requests, which is acceptable for a budget.
//
// All methods are thread-safe.
class BudgetTracker {
public:
    explicit BudgetTracker(WallClock clock = real_wall_clock());

    bool can_spend(const std::string& key_id, long long estimated_tokens,
                   long long daily_budget) const;
    void record(const std::string& key_id, long long actual_tokens);
    long long used_today(const std::string& key_id) const;

    // Seconds until the next UTC midnight (always at least 1).
    long long seconds_until_reset() const;

private:
    struct Usage {
        long long day = 0;   // days since 1970-01-01 (UTC)
        long long used = 0;
    };

    long long today() const;
    long long used_locked(const std::string& key_id, long long day) const;

    WallClock clock_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Usage> usage_;
};

}  // namespace llmgate
