#include "llmgate/budget/budget_tracker.h"

#include <algorithm>

namespace llmgate {

namespace {
constexpr long long kSecondsPerDay = 24 * 60 * 60;
}

BudgetTracker::BudgetTracker(WallClock clock) : clock_(std::move(clock)) {}

long long BudgetTracker::today() const {
    const auto hours = std::chrono::duration_cast<std::chrono::hours>(clock_().time_since_epoch());
    return hours.count() / 24;
}

// Caller holds the mutex. A counter from an earlier day counts as zero.
long long BudgetTracker::used_locked(const std::string& key_id, long long day) const {
    auto it = usage_.find(key_id);
    if (it == usage_.end() || it->second.day != day) return 0;
    return it->second.used;
}

bool BudgetTracker::can_spend(const std::string& key_id, long long estimated_tokens,
                              long long daily_budget) const {
    const long long day = today();
    std::lock_guard<std::mutex> lock(mutex_);
    return used_locked(key_id, day) + std::max(0LL, estimated_tokens) <= daily_budget;
}

void BudgetTracker::record(const std::string& key_id, long long actual_tokens) {
    if (actual_tokens <= 0) return;
    const long long day = today();
    std::lock_guard<std::mutex> lock(mutex_);
    Usage& u = usage_[key_id];
    if (u.day != day) {  // a new UTC day: start again from zero
        u.day = day;
        u.used = 0;
    }
    u.used += actual_tokens;
}

long long BudgetTracker::used_today(const std::string& key_id) const {
    const long long day = today();
    std::lock_guard<std::mutex> lock(mutex_);
    return used_locked(key_id, day);
}

long long BudgetTracker::seconds_until_reset() const {
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(clock_().time_since_epoch()).count();
    return std::max(1LL, kSecondsPerDay - (seconds % kSecondsPerDay));
}

}  // namespace llmgate
