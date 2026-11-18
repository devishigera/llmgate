#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <limits>
#include <thread>
#include <vector>

#include "llmgate/limiter/token_bucket.h"
#include "test_helpers.h"

using llmgate::RateLimiter;
using llmgate::TokenBucket;

// ---------------------------------------------------------------- TokenBucket

TEST(TokenBucket, AllowsBurstUpToCapacity) {
    FakeClock clock;
    TokenBucket bucket(3, 1, clock.fn());
    EXPECT_TRUE(bucket.try_acquire());
    EXPECT_TRUE(bucket.try_acquire());
    EXPECT_TRUE(bucket.try_acquire());
}

TEST(TokenBucket, RejectsWhenEmpty) {
    FakeClock clock;
    TokenBucket bucket(3, 1, clock.fn());
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(bucket.try_acquire());
    }
    EXPECT_FALSE(bucket.try_acquire());
}

TEST(TokenBucket, RefillsOverTime) {
    FakeClock clock;
    TokenBucket bucket(3, 1, clock.fn());  // 1 token per second
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(bucket.try_acquire());
    }
    ASSERT_FALSE(bucket.try_acquire());

    clock.advance_ms(1000);                // one second passes
    EXPECT_TRUE(bucket.try_acquire());     // exactly one token came back
    EXPECT_FALSE(bucket.try_acquire());
}

TEST(TokenBucket, PartialRefillAccumulates) {
    FakeClock clock;
    TokenBucket bucket(1, 1, clock.fn());
    ASSERT_TRUE(bucket.try_acquire());

    clock.advance_ms(500);
    EXPECT_FALSE(bucket.try_acquire());    // only half a token so far
    clock.advance_ms(500);
    EXPECT_TRUE(bucket.try_acquire());     // now a whole token
}

TEST(TokenBucket, NeverExceedsCapacityAfterLongIdle) {
    FakeClock clock;
    TokenBucket bucket(3, 1, clock.fn());
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(bucket.try_acquire());
    }
    clock.advance_ms(60 * 60 * 1000);      // one hour idle
    EXPECT_TRUE(bucket.try_acquire());
    EXPECT_TRUE(bucket.try_acquire());
    EXPECT_TRUE(bucket.try_acquire());
    EXPECT_FALSE(bucket.try_acquire());    // capped at 3, not 3600
}

TEST(TokenBucket, RetryAfterIsCorrect) {
    FakeClock clock;
    TokenBucket bucket(1, 2, clock.fn());  // 2 tokens per second
    ASSERT_TRUE(bucket.try_acquire());
    EXPECT_NEAR(bucket.retry_after_seconds(), 0.5, 1e-9);
    clock.advance_ms(250);
    EXPECT_NEAR(bucket.retry_after_seconds(), 0.25, 1e-9);
}

TEST(TokenBucket, RetryAfterIsZeroWhenAvailable) {
    FakeClock clock;
    TokenBucket bucket(2, 1, clock.fn());
    EXPECT_EQ(bucket.retry_after_seconds(), 0.0);
}

TEST(TokenBucket, RequestLargerThanCapacityNeverSucceeds) {
    FakeClock clock;
    TokenBucket bucket(3, 1, clock.fn());
    EXPECT_FALSE(bucket.try_acquire(5));
    EXPECT_TRUE(std::isinf(bucket.retry_after_seconds(5)));
}

TEST(TokenBucket, RejectedRequestDoesNotConsumeTokens) {
    FakeClock clock;
    TokenBucket bucket(2, 1, clock.fn());
    EXPECT_FALSE(bucket.try_acquire(3));   // too big, must not drain the bucket
    EXPECT_TRUE(bucket.try_acquire());
    EXPECT_TRUE(bucket.try_acquire());
}

TEST(TokenBucket, RejectsInvalidArguments) {
    FakeClock clock;
    EXPECT_THROW(TokenBucket(0, 1, clock.fn()), std::invalid_argument);
    EXPECT_THROW(TokenBucket(1, 0, clock.fn()), std::invalid_argument);
    EXPECT_THROW(TokenBucket(-1, 1, clock.fn()), std::invalid_argument);
}

TEST(TokenBucket, ConcurrentAcquireNeverExceedsCapacity) {
    FakeClock clock;                       // frozen: no refill during the test
    const int kCapacity = 50;
    TokenBucket bucket(kCapacity, 1, clock.fn());

    std::atomic<int> granted{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 100; ++i) {
                if (bucket.try_acquire()) {
                    ++granted;
                }
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    EXPECT_EQ(granted.load(), kCapacity);  // 800 attempts, exactly 50 allowed
}

// ---------------------------------------------------------------- RateLimiter

TEST(RateLimiter, SeparateKeysHaveSeparateBuckets) {
    FakeClock clock;
    RateLimiter limiter(clock.fn());
    EXPECT_TRUE(limiter.allow("team-a", 1, 1));
    EXPECT_FALSE(limiter.allow("team-a", 1, 1));  // team-a is now empty
    EXPECT_TRUE(limiter.allow("team-b", 1, 1));   // team-b is unaffected
}

TEST(RateLimiter, SameKeySharesOneBucket) {
    FakeClock clock;
    RateLimiter limiter(clock.fn());
    EXPECT_TRUE(limiter.allow("k", 2, 1));
    EXPECT_TRUE(limiter.allow("k", 2, 1));
    EXPECT_FALSE(limiter.allow("k", 2, 1));
}

TEST(RateLimiter, RefillsOverTimePerKey) {
    FakeClock clock;
    RateLimiter limiter(clock.fn());
    ASSERT_TRUE(limiter.allow("k", 1, 1));
    ASSERT_FALSE(limiter.allow("k", 1, 1));
    clock.advance_ms(1000);
    EXPECT_TRUE(limiter.allow("k", 1, 1));
}

TEST(RateLimiter, RetryAfterForRejectedKey) {
    FakeClock clock;
    RateLimiter limiter(clock.fn());
    ASSERT_TRUE(limiter.allow("k", 1, 2));
    ASSERT_FALSE(limiter.allow("k", 1, 2));
    EXPECT_NEAR(limiter.retry_after_seconds("k"), 0.5, 1e-9);
}

TEST(RateLimiter, RetryAfterForUnknownKeyIsZero) {
    FakeClock clock;
    RateLimiter limiter(clock.fn());
    EXPECT_EQ(limiter.retry_after_seconds("never-seen"), 0.0);
}

TEST(RateLimiter, ConcurrentKeysAndRequests) {
    FakeClock clock;
    RateLimiter limiter(clock.fn());
    const int kBurst = 20;
    std::atomic<int> granted{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 100; ++i) {
                if (limiter.allow("shared-key", kBurst, 1)) {
                    ++granted;
                }
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    EXPECT_EQ(granted.load(), kBurst);
}