#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "llmgate/util/clock.h"

namespace llmgate {

// A token bucket allows short bursts but enforces an average rate.
//   capacity           = the largest burst (bucket size)
//   refill_per_second  = how fast tokens come back
// The bucket starts full. Thread-safe.
class TokenBucket {
public:
    TokenBucket(double capacity, double refill_per_second, Clock clock);

    // Takes `tokens` from the bucket if enough are available.
    // Returns true if the request is allowed, false if it must be rejected.
    bool try_acquire(double tokens = 1.0);

    // Seconds until `tokens` would be available.
    // 0 if available now; infinity if it can never be satisfied.
    double retry_after_seconds(double tokens = 1.0);

private:
    void refill_locked();  // caller must already hold mu_

    double capacity_;
    double rate_;
    double tokens_;
    TimePoint last_refill_;
    Clock clock_;
    std::mutex mu_;
};

// One bucket per API key id. std::mutex cannot be copied or moved, so each
// bucket lives behind a unique_ptr and never moves in memory.
// Note: a key's burst and rate are fixed when its bucket is first created.
class RateLimiter {
public:
    explicit RateLimiter(Clock clock);

    // True if `key_id` may make a request now.
    bool allow(const std::string& key_id, double burst, double per_second);

    // Seconds until `key_id` could make another request (0 if unknown key
    // or a token is available).
    double retry_after_seconds(const std::string& key_id);

private:
    TokenBucket* get_or_create(const std::string& key_id, double burst,
                               double per_second);

    Clock clock_;
    std::mutex map_mu_;  // protects buckets_ (the map), not the buckets inside
    std::unordered_map<std::string, std::unique_ptr<TokenBucket>> buckets_;
};

}  // namespace llmgate