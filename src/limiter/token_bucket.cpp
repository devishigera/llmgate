#include "llmgate/limiter/token_bucket.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <utility>

namespace llmgate {

// ---------------------------------------------------------------- TokenBucket

TokenBucket::TokenBucket(double capacity, double refill_per_second, Clock clock)
    : capacity_(capacity),
      rate_(refill_per_second),
      tokens_(capacity),
      clock_(std::move(clock)) {
    if (capacity <= 0.0) {
        throw std::invalid_argument("TokenBucket: capacity must be > 0");
    }
    if (refill_per_second <= 0.0) {
        throw std::invalid_argument("TokenBucket: refill rate must be > 0");
    }
    last_refill_ = clock_();
}

void TokenBucket::refill_locked() {
    const TimePoint now = clock_();
    const double elapsed =
        std::chrono::duration<double>(now - last_refill_).count();
    if (elapsed > 0.0) {  // ignore a clock that went backwards
        tokens_ = std::min(capacity_, tokens_ + elapsed * rate_);
        last_refill_ = now;
    }
}

bool TokenBucket::try_acquire(double tokens) {
    std::lock_guard<std::mutex> lock(mu_);
    refill_locked();
    if (tokens_ >= tokens) {
        tokens_ -= tokens;
        return true;
    }
    return false;
}

double TokenBucket::retry_after_seconds(double tokens) {
    std::lock_guard<std::mutex> lock(mu_);
    refill_locked();
    if (tokens_ >= tokens) {
        return 0.0;
    }
    if (tokens > capacity_) {
        return std::numeric_limits<double>::infinity();  // can never be met
    }
    return (tokens - tokens_) / rate_;
}

// ---------------------------------------------------------------- RateLimiter

RateLimiter::RateLimiter(Clock clock) : clock_(std::move(clock)) {}

TokenBucket* RateLimiter::get_or_create(const std::string& key_id, double burst,
                                        double per_second) {
    std::lock_guard<std::mutex> lock(map_mu_);
    auto it = buckets_.find(key_id);
    if (it == buckets_.end()) {
        it = buckets_
                 .emplace(key_id,
                          std::make_unique<TokenBucket>(burst, per_second, clock_))
                 .first;
    }
    return it->second.get();  // stable: the TokenBucket never moves
}

bool RateLimiter::allow(const std::string& key_id, double burst,
                        double per_second) {
    // The map lock is released before touching the bucket, so one slow key
    // never blocks lookups for other keys.
    return get_or_create(key_id, burst, per_second)->try_acquire();
}

double RateLimiter::retry_after_seconds(const std::string& key_id) {
    TokenBucket* bucket = nullptr;
    {
        std::lock_guard<std::mutex> lock(map_mu_);
        auto it = buckets_.find(key_id);
        if (it == buckets_.end()) {
            return 0.0;
        }
        bucket = it->second.get();
    }
    return bucket->retry_after_seconds();
}

}  // namespace llmgate