#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <vector>

#include "llmgate/resilience/circuit_breaker.h"
#include "test_helpers.h"

using llmgate::BreakerState;
using llmgate::CircuitBreaker;
using std::chrono::milliseconds;

namespace {

// threshold 3, cool-down 1000 ms, 2 trial calls in half-open
struct Fixture {
    FakeClock clock;
    CircuitBreaker breaker{3, milliseconds(1000), 2, clock.fn()};

    void fail(int n) {
        for (int i = 0; i < n; ++i) {
            ASSERT_TRUE(breaker.allow_request());
            breaker.record_failure();
        }
    }
    // Open the breaker, wait out the cool-down and enter half-open.
    void enter_half_open() {
        fail(3);
        clock.advance_ms(1000);
    }
};

}  // namespace

TEST(CircuitBreaker, StartsClosed) {
    Fixture f;
    EXPECT_EQ(f.breaker.state(), BreakerState::Closed);
    EXPECT_TRUE(f.breaker.allow_request());
}

TEST(CircuitBreaker, StaysClosedBelowThreshold) {
    Fixture f;
    f.fail(2);
    EXPECT_EQ(f.breaker.state(), BreakerState::Closed);
    EXPECT_TRUE(f.breaker.allow_request());
}

TEST(CircuitBreaker, OpensAfterThresholdFailures) {
    Fixture f;
    f.fail(3);
    EXPECT_EQ(f.breaker.state(), BreakerState::Open);
}

TEST(CircuitBreaker, OpenRejectsImmediately) {
    Fixture f;
    f.fail(3);
    EXPECT_FALSE(f.breaker.allow_request());
    f.clock.advance_ms(999);
    EXPECT_FALSE(f.breaker.allow_request());
}

TEST(CircuitBreaker, SuccessResetsFailureCount) {
    Fixture f;
    f.fail(2);
    f.breaker.record_success();
    f.fail(2);  // would be 4 in a row without the reset
    EXPECT_EQ(f.breaker.state(), BreakerState::Closed);
    f.fail(1);
    EXPECT_EQ(f.breaker.state(), BreakerState::Open);
}

TEST(CircuitBreaker, MovesToHalfOpenAfterCooldown) {
    Fixture f;
    f.fail(3);
    f.clock.advance_ms(1000);
    EXPECT_TRUE(f.breaker.allow_request());
    EXPECT_EQ(f.breaker.state(), BreakerState::HalfOpen);
}

TEST(CircuitBreaker, HalfOpenSuccessCloses) {
    Fixture f;
    f.enter_half_open();
    ASSERT_TRUE(f.breaker.allow_request());
    ASSERT_TRUE(f.breaker.allow_request());
    f.breaker.record_success();
    EXPECT_EQ(f.breaker.state(), BreakerState::HalfOpen);  // needs both trials
    f.breaker.record_success();
    EXPECT_EQ(f.breaker.state(), BreakerState::Closed);
    EXPECT_TRUE(f.breaker.allow_request());
}

TEST(CircuitBreaker, HalfOpenFailureReopens) {
    Fixture f;
    f.enter_half_open();
    ASSERT_TRUE(f.breaker.allow_request());
    f.breaker.record_failure();
    EXPECT_EQ(f.breaker.state(), BreakerState::Open);
    EXPECT_FALSE(f.breaker.allow_request());
}

TEST(CircuitBreaker, ReopenStartsANewCooldown) {
    Fixture f;
    f.enter_half_open();
    ASSERT_TRUE(f.breaker.allow_request());
    f.breaker.record_failure();
    f.clock.advance_ms(999);
    EXPECT_FALSE(f.breaker.allow_request());
    f.clock.advance_ms(1);
    EXPECT_TRUE(f.breaker.allow_request());
}

TEST(CircuitBreaker, HalfOpenLimitsTrialRequests) {
    Fixture f;
    f.enter_half_open();
    EXPECT_TRUE(f.breaker.allow_request());
    EXPECT_TRUE(f.breaker.allow_request());
    EXPECT_FALSE(f.breaker.allow_request());  // only 2 trials in flight
}

TEST(CircuitBreaker, ClosedAgainNeedsFullThresholdToReopen) {
    Fixture f;
    f.enter_half_open();
    ASSERT_TRUE(f.breaker.allow_request());
    ASSERT_TRUE(f.breaker.allow_request());
    f.breaker.record_success();
    f.breaker.record_success();
    ASSERT_EQ(f.breaker.state(), BreakerState::Closed);
    f.fail(2);
    EXPECT_EQ(f.breaker.state(), BreakerState::Closed);
    f.fail(1);
    EXPECT_EQ(f.breaker.state(), BreakerState::Open);
}

TEST(CircuitBreaker, LateResultsWhileOpenAreIgnored) {
    Fixture f;
    f.fail(3);
    f.breaker.record_success();
    f.breaker.record_failure();
    EXPECT_EQ(f.breaker.state(), BreakerState::Open);
    f.clock.advance_ms(1000);
    EXPECT_TRUE(f.breaker.allow_request());  // cool-down unchanged by late results
}

TEST(CircuitBreaker, StateNamesForMetrics) {
    EXPECT_STREQ(llmgate::to_string(BreakerState::Closed), "closed");
    EXPECT_STREQ(llmgate::to_string(BreakerState::Open), "open");
    EXPECT_STREQ(llmgate::to_string(BreakerState::HalfOpen), "half_open");
}

TEST(CircuitBreaker, RejectsBadSettings) {
    FakeClock c;
    EXPECT_THROW(CircuitBreaker(0, milliseconds(10), 1, c.fn()), std::invalid_argument);
    EXPECT_THROW(CircuitBreaker(1, milliseconds(0), 1, c.fn()), std::invalid_argument);
    EXPECT_THROW(CircuitBreaker(1, milliseconds(10), 0, c.fn()), std::invalid_argument);
    EXPECT_THROW(CircuitBreaker(1, milliseconds(10), 1, llmgate::Clock{}), std::invalid_argument);
}

TEST(CircuitBreaker, SafeUnderConcurrentUse) {
    CircuitBreaker breaker(5, milliseconds(1), 2, llmgate::real_clock());
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&breaker, t] {
            for (int i = 0; i < 2000; ++i) {
                if (breaker.allow_request()) {
                    if ((i + t) % 3 == 0) breaker.record_failure();
                    else breaker.record_success();
                }
                breaker.state();
            }
        });
    }
    for (auto& th : threads) th.join();
    SUCCEED();
}