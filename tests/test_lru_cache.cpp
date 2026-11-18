#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "llmgate/cache/lru_cache.h"
#include "test_helpers.h"

using llmgate::LruCache;
using std::chrono::milliseconds;

TEST(LruCache, PutThenGetReturnsValue) {
    FakeClock clock;
    LruCache<std::string, int> cache(2, milliseconds(1000), clock.fn());
    cache.put("a", 1);
    auto v = cache.get("a");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 1);
}

TEST(LruCache, MissingKeyReturnsNothing) {
    FakeClock clock;
    LruCache<std::string, int> cache(2, milliseconds(1000), clock.fn());
    EXPECT_FALSE(cache.get("nope").has_value());
}

TEST(LruCache, EvictsLeastRecentlyUsedFirst) {
    FakeClock clock;
    LruCache<std::string, int> cache(2, milliseconds(1000), clock.fn());
    cache.put("a", 1);
    cache.put("b", 2);
    cache.put("c", 3);  // full: "a" is the oldest, so it goes
    EXPECT_FALSE(cache.get("a").has_value());
    EXPECT_TRUE(cache.get("b").has_value());
    EXPECT_TRUE(cache.get("c").has_value());
}

TEST(LruCache, GetRefreshesRecency) {
    FakeClock clock;
    LruCache<std::string, int> cache(2, milliseconds(1000), clock.fn());
    cache.put("a", 1);
    cache.put("b", 2);
    ASSERT_TRUE(cache.get("a").has_value());  // "a" is now the newest
    cache.put("c", 3);                        // so "b" is evicted instead
    EXPECT_TRUE(cache.get("a").has_value());
    EXPECT_FALSE(cache.get("b").has_value());
    EXPECT_TRUE(cache.get("c").has_value());
}

TEST(LruCache, PutUpdatesExistingKeyAndRefreshesRecency) {
    FakeClock clock;
    LruCache<std::string, int> cache(2, milliseconds(1000), clock.fn());
    cache.put("a", 1);
    cache.put("b", 2);
    cache.put("a", 10);  // update, and "a" becomes the newest
    EXPECT_EQ(cache.size(), 2u);
    cache.put("c", 3);   // evicts "b", not "a"
    auto a = cache.get("a");
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(*a, 10);
    EXPECT_FALSE(cache.get("b").has_value());
}

TEST(LruCache, EntryExpiresAfterTtl) {
    FakeClock clock;
    LruCache<std::string, int> cache(2, milliseconds(1000), clock.fn());
    cache.put("a", 1);
    clock.advance_ms(999);
    EXPECT_TRUE(cache.get("a").has_value());   // 1 ms before expiry
    clock.advance_ms(1);
    EXPECT_FALSE(cache.get("a").has_value());  // exactly at the TTL: expired
}

TEST(LruCache, GetDoesNotExtendTtl) {
    FakeClock clock;
    LruCache<std::string, int> cache(2, milliseconds(1000), clock.fn());
    cache.put("a", 1);
    clock.advance_ms(600);
    ASSERT_TRUE(cache.get("a").has_value());   // reading must not reset the timer
    clock.advance_ms(400);                     // total 1000 ms since put
    EXPECT_FALSE(cache.get("a").has_value());
}

TEST(LruCache, PutResetsTtl) {
    FakeClock clock;
    LruCache<std::string, int> cache(2, milliseconds(1000), clock.fn());
    cache.put("a", 1);
    clock.advance_ms(800);
    cache.put("a", 2);                         // timer restarts here
    clock.advance_ms(800);                     // 1600 ms after first put
    auto a = cache.get("a");
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(*a, 2);
}

TEST(LruCache, ExpiredEntryIsRemovedWhenTouched) {
    FakeClock clock;
    LruCache<std::string, int> cache(2, milliseconds(1000), clock.fn());
    cache.put("a", 1);
    EXPECT_EQ(cache.size(), 1u);
    clock.advance_ms(1000);
    EXPECT_FALSE(cache.get("a").has_value());
    EXPECT_EQ(cache.size(), 0u);               // lazily removed on the miss
}

TEST(LruCache, CapacityOne) {
    FakeClock clock;
    LruCache<std::string, int> cache(1, milliseconds(1000), clock.fn());
    cache.put("a", 1);
    cache.put("b", 2);
    EXPECT_FALSE(cache.get("a").has_value());
    EXPECT_TRUE(cache.get("b").has_value());
    EXPECT_EQ(cache.size(), 1u);
}

TEST(LruCache, SizeNeverExceedsCapacity) {
    FakeClock clock;
    LruCache<int, int> cache(5, milliseconds(1000), clock.fn());
    for (int i = 0; i < 100; ++i) {
        cache.put(i, i);
        EXPECT_LE(cache.size(), 5u);
    }
    EXPECT_EQ(cache.size(), 5u);
    // the 5 most recent keys survive
    for (int i = 95; i < 100; ++i) {
        EXPECT_TRUE(cache.get(i).has_value());
    }
    EXPECT_FALSE(cache.get(94).has_value());
}

TEST(LruCache, RejectsInvalidArguments) {
    FakeClock clock;
    using Cache = LruCache<int, int>;
    EXPECT_THROW(Cache(0, milliseconds(1000), clock.fn()), std::invalid_argument);
    EXPECT_THROW(Cache(1, milliseconds(0), clock.fn()), std::invalid_argument);
    EXPECT_THROW(Cache(1, milliseconds(-5), clock.fn()), std::invalid_argument);
}

TEST(LruCache, ConcurrentReadersAndWriters) {
    FakeClock clock;                            // frozen time: nothing expires
    const std::size_t kCapacity = 16;
    LruCache<int, int> cache(kCapacity, milliseconds(1000), clock.fn());

    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&cache, t] {
            for (int i = 0; i < 2000; ++i) {
                int key = (t * 7 + i) % 40;
                cache.put(key, key * 2);
                auto v = cache.get((key + 3) % 40);
                if (v.has_value()) {
                    // a value must always match its key: no torn or mixed data
                    EXPECT_EQ(*v % 2, 0);
                }
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    EXPECT_LE(cache.size(), kCapacity);
}