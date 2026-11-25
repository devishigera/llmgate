#include <gtest/gtest.h>

#include <chrono>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "llmgate/resilience/retry.h"
#include "test_helpers.h"

using namespace llmgate;
using std::chrono::milliseconds;

namespace {

// Records every wait instead of sleeping.
struct Recorder {
    std::vector<milliseconds> sleeps;
    Sleeper fn() {
        return [this](milliseconds d) { sleeps.push_back(d); };
    }
};

RetryPolicy policy(int attempts = 4, int base = 100, int cap = 1000, int budget = 100000) {
    RetryPolicy p;
    p.max_attempts = attempts;
    p.base_delay = milliseconds(base);
    p.max_delay = milliseconds(cap);
    p.total_budget = milliseconds(budget);
    return p;
}

ProviderError transient_error() { return ProviderError("temporary", true); }
ProviderError permanent_error() { return ProviderError("never works", false); }

}  // namespace

TEST(Retry, SucceedsOnFirstTry) {
    Recorder rec;
    std::mt19937_64 rng(1);
    int calls = 0;
    int result = call_with_retry(policy(), [&] { ++calls; return 42; }, rec.fn(), rng);
    EXPECT_EQ(result, 42);
    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(rec.sleeps.empty());
}

TEST(Retry, RetriesThenSucceeds) {
    Recorder rec;
    std::mt19937_64 rng(1);
    int calls = 0;
    std::string result = call_with_retry(policy(), [&] {
        if (++calls < 3) throw transient_error();
        return std::string("finally");
    }, rec.fn(), rng);
    EXPECT_EQ(result, "finally");
    EXPECT_EQ(calls, 3);
    EXPECT_EQ(rec.sleeps.size(), 2u);
}

TEST(Retry, RetriesTransientErrorsUpToMax) {
    Recorder rec;
    std::mt19937_64 rng(1);
    int calls = 0;
    EXPECT_THROW(call_with_retry(policy(4), [&]() -> int { ++calls; throw transient_error(); },
                                 rec.fn(), rng),
                 ProviderError);
    EXPECT_EQ(calls, 4);
    EXPECT_EQ(rec.sleeps.size(), 3u);  // a wait between tries, none after the last
}

TEST(Retry, TheLastErrorIsRethrown) {
    Recorder rec;
    std::mt19937_64 rng(1);
    int calls = 0;
    try {
        call_with_retry(policy(3), [&]() -> int {
            throw ProviderError("failure " + std::to_string(++calls), true);
        }, rec.fn(), rng);
        FAIL() << "expected an exception";
    } catch (const ProviderError& e) {
        EXPECT_STREQ(e.what(), "failure 3");
    }
}

TEST(Retry, DoesNotRetryNonTransient) {
    Recorder rec;
    std::mt19937_64 rng(1);
    int calls = 0;
    EXPECT_THROW(call_with_retry(policy(), [&]() -> int { ++calls; throw permanent_error(); },
                                 rec.fn(), rng),
                 ProviderError);
    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(rec.sleeps.empty());
}

TEST(Retry, OtherExceptionsAreNotRetried) {
    Recorder rec;
    std::mt19937_64 rng(1);
    int calls = 0;
    EXPECT_THROW(call_with_retry(policy(), [&]() -> int { ++calls; throw std::runtime_error("bug"); },
                                 rec.fn(), rng),
                 std::runtime_error);
    EXPECT_EQ(calls, 1);
}

TEST(Retry, MaxAttemptsOneNeverRetries) {
    Recorder rec;
    std::mt19937_64 rng(1);
    int calls = 0;
    EXPECT_THROW(call_with_retry(policy(1), [&]() -> int { ++calls; throw transient_error(); },
                                 rec.fn(), rng),
                 ProviderError);
    EXPECT_EQ(calls, 1);
}

TEST(Retry, ZeroOrNegativeAttemptsStillTryOnce) {
    Recorder rec;
    std::mt19937_64 rng(1);
    int calls = 0;
    EXPECT_THROW(call_with_retry(policy(0), [&]() -> int { ++calls; throw transient_error(); },
                                 rec.fn(), rng),
                 ProviderError);
    EXPECT_EQ(calls, 1);
}

TEST(Retry, TimeoutErrorsAreRetriedLikeOtherTransientOnes) {
    Recorder rec;
    std::mt19937_64 rng(1);
    int calls = 0;
    call_with_retry(policy(), [&] {
        if (++calls < 2) throw ProviderError("slow", true, /*timeout=*/true);
        return 1;
    }, rec.fn(), rng);
    EXPECT_EQ(calls, 2);
}

// ---- the delay calculation ---------------------------------------------------

TEST(Backoff, DelaysStayBelowTheCeilingForEachAttempt) {
    std::mt19937_64 rng(7);
    RetryPolicy p = policy(10, 100, 1000);
    const int ceilings[] = {100, 200, 400, 800, 1000, 1000, 1000};
    for (int attempt = 0; attempt < 7; ++attempt) {
        for (int i = 0; i < 2000; ++i) {
            auto d = backoff_delay(p, attempt, rng);
            EXPECT_GE(d.count(), 0);
            EXPECT_LE(d.count(), ceilings[attempt]) << "attempt " << attempt;
        }
    }
}

TEST(Backoff, CeilingGrowsWithEachAttempt) {
    // Over many samples the largest delay approaches the ceiling, which doubles.
    RetryPolicy p = policy(10, 100, 100000);
    auto biggest = [&](int attempt) {
        std::mt19937_64 rng(3);
        long long m = 0;
        for (int i = 0; i < 5000; ++i) m = std::max<long long>(m, backoff_delay(p, attempt, rng).count());
        return m;
    };
    EXPECT_GT(biggest(1), 100);  // more than attempt 0 could ever give
    EXPECT_GT(biggest(3), 400);
    EXPECT_GT(biggest(3), biggest(0));
}

TEST(Backoff, DelaysAreSpreadOutNotAllEqual) {
    std::mt19937_64 rng(5);
    RetryPolicy p = policy(10, 100, 1000);
    std::vector<long long> seen;
    for (int i = 0; i < 50; ++i) seen.push_back(backoff_delay(p, 3, rng).count());
    EXPECT_GT(*std::max_element(seen.begin(), seen.end()), *std::min_element(seen.begin(), seen.end()));
}

TEST(Backoff, HugeAttemptNumberDoesNotOverflow) {
    std::mt19937_64 rng(1);
    RetryPolicy p = policy(10, 100, 2000);
    for (int attempt : {20, 40, 62, 63, 64, 1000}) {
        auto d = backoff_delay(p, attempt, rng);
        EXPECT_LE(d.count(), 2000);
        EXPECT_GE(d.count(), 0);
    }
}

TEST(Backoff, SameSeedGivesSameDelays) {
    RetryPolicy p = policy(10, 100, 1000);
    std::mt19937_64 a(99), b(99);
    for (int attempt = 0; attempt < 8; ++attempt) {
        EXPECT_EQ(backoff_delay(p, attempt, a), backoff_delay(p, attempt, b));
    }
}

TEST(Backoff, ZeroBaseMeansNoWaiting) {
    std::mt19937_64 rng(1);
    RetryPolicy p = policy(5, 0, 1000);
    EXPECT_EQ(backoff_delay(p, 3, rng).count(), 0);
}

TEST(Retry, DelaysRecordedByTheSleeperStayBelowTheCap) {
    Recorder rec;
    std::mt19937_64 rng(11);
    int calls = 0;
    EXPECT_THROW(call_with_retry(policy(8, 100, 500), [&]() -> int { ++calls; throw transient_error(); },
                                 rec.fn(), rng),
                 ProviderError);
    ASSERT_EQ(rec.sleeps.size(), 7u);
    const int ceilings[] = {100, 200, 400, 500, 500, 500, 500};
    for (std::size_t i = 0; i < rec.sleeps.size(); ++i) EXPECT_LE(rec.sleeps[i].count(), ceilings[i]);
}

// ---- the total time budget -----------------------------------------------------

TEST(Retry, StopsWhenTotalBudgetExceeded) {
    FakeClock clock;
    std::mt19937_64 rng(1);
    int calls = 0;
    std::vector<milliseconds> sleeps;
    Sleeper sleeper = [&](milliseconds d) {
        sleeps.push_back(d);
        clock.advance_ms(static_cast<int>(d.count()));
    };
    // Every call takes 400 ms on the clock; the whole budget is 1000 ms.
    RetryPolicy p = policy(100, 100, 100, /*budget=*/1000);
    EXPECT_THROW(call_with_retry(p, [&]() -> int {
                     ++calls;
                     clock.advance_ms(400);
                     throw transient_error();
                 }, sleeper, rng, clock.fn()),
                 ProviderError);
    EXPECT_LT(calls, 100);  // we gave up long before max_attempts
    EXPECT_GE(calls, 2);
    const auto elapsed = std::chrono::duration_cast<milliseconds>(clock.now.time_since_epoch());
    EXPECT_LE(elapsed.count(), 1000 + 400);  // never far past the budget
}

TEST(Retry, SlowFailureUsesUpTheBudgetAndStopsAtOnce) {
    FakeClock clock;
    std::mt19937_64 rng(1);
    int calls = 0;
    Recorder rec;
    // One call takes 60 s, but the budget is 10 s: no point waiting and trying again.
    RetryPolicy p = policy(5, 100, 1000, /*budget=*/10000);
    EXPECT_THROW(call_with_retry(p, [&]() -> int {
                     ++calls;
                     clock.advance_ms(60000);
                     throw ProviderError("timed out", true, true);
                 }, rec.fn(), rng, clock.fn()),
                 ProviderError);
    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(rec.sleeps.empty());
}

TEST(Retry, WorksWithARealProvider) {
    // The function can be any callable, for example a lambda around Provider::chat.
    Recorder rec;
    std::mt19937_64 rng(1);
    int calls = 0;
    auto flaky = [&]() -> std::string {
        if (++calls < 3) throw ProviderError("503", true);
        return "ok";
    };
    EXPECT_EQ(call_with_retry(policy(), flaky, rec.fn(), rng), "ok");
}
