#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "llmgate/provider/mock.h"
#include "llmgate/provider/router.h"
#include "test_helpers.h"

using namespace llmgate;
using std::chrono::milliseconds;

namespace {

ChatRequest request() {
    ChatRequest r;
    r.model = "m";
    r.messages.push_back({"user", "hello"});
    r.max_tokens = 32;
    return r;
}

MockOptions named(const std::string& name) {
    MockOptions o;
    o.name = name;
    return o;
}

RetryPolicy attempts(int n) {
    RetryPolicy p;
    p.max_attempts = n;
    return p;
}

// A provider that behaves exactly as told, to produce errors the mock cannot.
class ScriptedProvider : public Provider {
public:
    enum class Mode { Ok, Transient, Permanent, Timeout, Explode };

    ScriptedProvider(std::string name, Mode mode) : name_(std::move(name)), mode_(mode) {}

    ChatResponse chat(const ChatRequest&, milliseconds) override {
        ++calls_;
        switch (mode_) {
            case Mode::Transient: throw ProviderError("busy", true);
            case Mode::Permanent: throw ProviderError("bad model", false);
            case Mode::Timeout: throw ProviderError("slow", true, true);
            case Mode::Explode: throw std::runtime_error("secret database password");
            case Mode::Ok: break;
        }
        ChatResponse r;
        r.provider = name_;
        r.content = "ok";
        return r;
    }
    std::string name() const override { return name_; }
    int calls() const { return calls_.load(); }

private:
    std::string name_;
    Mode mode_;
    std::atomic<int> calls_{0};
};

// Two mock providers, "primary" (priority 1) and "secondary" (priority 2),
// each with its own breaker (opens after 3 failures, 1 s cool-down).
struct Rig {
    FakeClock clock;
    std::vector<milliseconds> sleeps;
    MockProvider primary{named("primary")};
    MockProvider secondary{named("secondary")};
    CircuitBreaker primary_breaker{3, milliseconds(1000), 1, clock.fn()};
    CircuitBreaker secondary_breaker{3, milliseconds(1000), 1, clock.fn()};
    ProviderRouter router;

    explicit Rig(int max_attempts = 1)
        : router(attempts(max_attempts), [this](milliseconds d) { sleeps.push_back(d); },
                 clock.fn(), /*seed=*/42) {
        router.add_ref(primary, primary_breaker, 1, milliseconds(500));
        router.add_ref(secondary, secondary_breaker, 2, milliseconds(500));
    }
};

}  // namespace

TEST(ProviderRouter, UsesPrimaryWhenHealthy) {
    Rig rig;
    EXPECT_EQ(rig.router.chat(request()).provider, "primary");
    EXPECT_EQ(rig.primary.calls(), 1);
    EXPECT_EQ(rig.secondary.calls(), 0);
    EXPECT_EQ(rig.router.failover_count(), 0);
}

TEST(ProviderRouter, FailsOverWhenPrimaryFails) {
    Rig rig;
    rig.primary.set_fail_rate(1.0);
    EXPECT_EQ(rig.router.chat(request()).provider, "secondary");
    EXPECT_EQ(rig.primary.calls(), 1);
    EXPECT_EQ(rig.secondary.calls(), 1);
    EXPECT_EQ(rig.router.failover_count(), 1);
}

TEST(ProviderRouter, TriesLowestPriorityNumberFirstWhateverTheAddOrder) {
    MockProvider first(named("first")), second(named("second")), third(named("third"));
    CircuitBreaker b1(3, milliseconds(1000), 1, real_clock());
    CircuitBreaker b2(3, milliseconds(1000), 1, real_clock());
    CircuitBreaker b3(3, milliseconds(1000), 1, real_clock());
    ProviderRouter router(attempts(1), [](milliseconds) {});
    router.add_ref(third, b3, 30, milliseconds(100));
    router.add_ref(first, b1, 10, milliseconds(100));
    router.add_ref(second, b2, 20, milliseconds(100));

    auto status = router.status();
    ASSERT_EQ(status.size(), 3u);
    EXPECT_EQ(status[0].name, "first");
    EXPECT_EQ(status[1].name, "second");
    EXPECT_EQ(status[2].name, "third");
    EXPECT_EQ(router.chat(request()).provider, "first");
}

TEST(ProviderRouter, SkipsProviderWithOpenBreaker) {
    Rig rig;
    rig.primary.set_fail_rate(1.0);
    for (int i = 0; i < 3; ++i) EXPECT_EQ(rig.router.chat(request()).provider, "secondary");
    ASSERT_EQ(rig.primary_breaker.state(), BreakerState::Open);
    ASSERT_EQ(rig.primary.calls(), 3);

    EXPECT_EQ(rig.router.chat(request()).provider, "secondary");
    EXPECT_EQ(rig.primary.calls(), 3);  // not called again while its breaker is open
}

TEST(ProviderRouter, ReturnsErrorWhenAllFail) {
    Rig rig;
    rig.primary.set_fail_rate(1.0);
    rig.secondary.set_fail_rate(1.0);
    try {
        rig.router.chat(request());
        FAIL() << "expected an exception";
    } catch (const NoProviderAvailable&) {
        FAIL() << "providers were tried; this is not 'no provider available'";
    } catch (const ProviderError& e) {
        EXPECT_TRUE(e.transient());
    }
    EXPECT_EQ(rig.primary.calls(), 1);
    EXPECT_EQ(rig.secondary.calls(), 1);
}

TEST(ProviderRouter, ThrowsNoProviderAvailableWhenEveryBreakerIsOpen) {
    Rig rig;
    rig.primary.set_fail_rate(1.0);
    rig.secondary.set_fail_rate(1.0);
    for (int i = 0; i < 3; ++i) EXPECT_THROW(rig.router.chat(request()), ProviderError);
    ASSERT_EQ(rig.primary_breaker.state(), BreakerState::Open);
    ASSERT_EQ(rig.secondary_breaker.state(), BreakerState::Open);

    EXPECT_THROW(rig.router.chat(request()), NoProviderAvailable);
    EXPECT_EQ(rig.primary.calls(), 3);  // nobody was called the fourth time
    EXPECT_EQ(rig.secondary.calls(), 3);
}

TEST(ProviderRouter, EmptyRouterThrowsNoProviderAvailable) {
    ProviderRouter router;
    EXPECT_THROW(router.chat(request()), NoProviderAvailable);
}

TEST(ProviderRouter, RecoversAfterCooldown) {
    Rig rig;
    rig.primary.set_fail_rate(1.0);
    for (int i = 0; i < 3; ++i) rig.router.chat(request());
    ASSERT_EQ(rig.primary_breaker.state(), BreakerState::Open);

    rig.primary.set_fail_rate(0.0);  // the provider is healthy again
    EXPECT_EQ(rig.router.chat(request()).provider, "secondary");  // still cooling down

    rig.clock.advance_ms(1000);
    EXPECT_EQ(rig.router.chat(request()).provider, "primary");  // the trial call succeeds
    EXPECT_EQ(rig.primary_breaker.state(), BreakerState::Closed);
}

TEST(ProviderRouter, TheLastErrorIsThrownSoATimeoutStaysATimeout) {
    ScriptedProvider busy("busy", ScriptedProvider::Mode::Transient);
    ScriptedProvider slow("slow", ScriptedProvider::Mode::Timeout);
    CircuitBreaker b1(3, milliseconds(1000), 1, real_clock());
    CircuitBreaker b2(3, milliseconds(1000), 1, real_clock());
    ProviderRouter router(attempts(1), [](milliseconds) {});
    router.add_ref(busy, b1, 1, milliseconds(100));
    router.add_ref(slow, b2, 2, milliseconds(100));
    try {
        router.chat(request());
        FAIL();
    } catch (const ProviderError& e) {
        EXPECT_TRUE(e.timeout());
    }
}

TEST(ProviderRouter, IfTheLastErrorIsNotATimeoutItIsNotReportedAsOne) {
    ScriptedProvider slow("slow", ScriptedProvider::Mode::Timeout);
    ScriptedProvider busy("busy", ScriptedProvider::Mode::Transient);
    CircuitBreaker b1(3, milliseconds(1000), 1, real_clock());
    CircuitBreaker b2(3, milliseconds(1000), 1, real_clock());
    ProviderRouter router(attempts(1), [](milliseconds) {});
    router.add_ref(slow, b1, 1, milliseconds(100));
    router.add_ref(busy, b2, 2, milliseconds(100));
    try {
        router.chat(request());
        FAIL();
    } catch (const ProviderError& e) {
        EXPECT_FALSE(e.timeout());
    }
}

// ---- retries inside the router --------------------------------------------------

TEST(ProviderRouter, RetriesATransientFailureBeforeFailingOver) {
    Rig rig(/*max_attempts=*/3);
    rig.primary.set_fail_next(2);  // fails twice, then works
    EXPECT_EQ(rig.router.chat(request()).provider, "primary");
    EXPECT_EQ(rig.primary.calls(), 3);
    EXPECT_EQ(rig.secondary.calls(), 0);
    EXPECT_EQ(rig.router.failover_count(), 0);
    EXPECT_EQ(rig.sleeps.size(), 2u);  // waited between the tries
}

TEST(ProviderRouter, FailsOverAfterRetriesAreUsedUp) {
    Rig rig(3);
    rig.primary.set_fail_rate(1.0);
    EXPECT_EQ(rig.router.chat(request()).provider, "secondary");
    EXPECT_EQ(rig.primary.calls(), 3);
    EXPECT_EQ(rig.sleeps.size(), 2u);
    EXPECT_EQ(rig.router.failover_count(), 1);
}

TEST(ProviderRouter, PermanentErrorIsNotRetriedButStillFailsOver) {
    ScriptedProvider picky("picky", ScriptedProvider::Mode::Permanent);
    MockProvider backup(named("backup"));
    CircuitBreaker b1(3, milliseconds(1000), 1, real_clock());
    CircuitBreaker b2(3, milliseconds(1000), 1, real_clock());
    std::vector<milliseconds> sleeps;
    ProviderRouter router(attempts(3), [&](milliseconds d) { sleeps.push_back(d); });
    router.add_ref(picky, b1, 1, milliseconds(100));
    router.add_ref(backup, b2, 2, milliseconds(100));

    EXPECT_EQ(router.chat(request()).provider, "backup");
    EXPECT_EQ(picky.calls(), 1);  // retrying a permanent error is pointless
    EXPECT_TRUE(sleeps.empty());
}

TEST(ProviderRouter, BreakerCountsOneFailurePerRequestNotPerRetry) {
    Rig rig(3);  // 3 tries per request, breaker threshold 3
    rig.primary.set_fail_rate(1.0);
    rig.router.chat(request());
    rig.router.chat(request());
    EXPECT_EQ(rig.primary.calls(), 6);  // 2 requests x 3 tries
    EXPECT_EQ(rig.primary_breaker.state(), BreakerState::Closed);  // but only 2 failures counted
    rig.router.chat(request());
    EXPECT_EQ(rig.primary_breaker.state(), BreakerState::Open);
}

// ---- unusual failures ------------------------------------------------------------

TEST(ProviderRouter, UnexpectedExceptionCountsAsFailureAndFailsOver) {
    ScriptedProvider bomb("bomb", ScriptedProvider::Mode::Explode);
    MockProvider backup(named("backup"));
    CircuitBreaker b1(3, milliseconds(1000), 1, real_clock());
    CircuitBreaker b2(3, milliseconds(1000), 1, real_clock());
    ProviderRouter router(attempts(3), [](milliseconds) {});
    router.add_ref(bomb, b1, 1, milliseconds(100));
    router.add_ref(backup, b2, 2, milliseconds(100));
    EXPECT_EQ(router.chat(request()).provider, "backup");
    EXPECT_EQ(bomb.calls(), 1);  // a bug is not retried
}

TEST(ProviderRouter, UnexpectedExceptionTextIsNeverPassedOn) {
    ScriptedProvider bomb("bomb", ScriptedProvider::Mode::Explode);
    CircuitBreaker b1(3, milliseconds(1000), 1, real_clock());
    ProviderRouter router(attempts(1), [](milliseconds) {});
    router.add_ref(bomb, b1, 1, milliseconds(100));
    try {
        router.chat(request());
        FAIL();
    } catch (const ProviderError& e) {
        EXPECT_EQ(std::string(e.what()).find("secret"), std::string::npos);
        EXPECT_FALSE(e.transient());
    }
}

// ---- bookkeeping ------------------------------------------------------------------

TEST(ProviderRouter, StatusReportsNamesPrioritiesAndBreakerStates) {
    Rig rig;
    auto before = rig.router.status();
    ASSERT_EQ(before.size(), 2u);
    EXPECT_EQ(before[0].name, "primary");
    EXPECT_EQ(before[0].priority, 1);
    EXPECT_EQ(before[0].breaker, BreakerState::Closed);

    rig.primary.set_fail_rate(1.0);
    for (int i = 0; i < 3; ++i) rig.router.chat(request());
    auto after = rig.router.status();
    EXPECT_EQ(after[0].breaker, BreakerState::Open);
    EXPECT_EQ(after[1].breaker, BreakerState::Closed);
}

TEST(ProviderRouter, FailoverCountAddsUpAcrossRequests) {
    Rig rig;
    rig.primary.set_fail_rate(1.0);
    for (int i = 0; i < 2; ++i) rig.router.chat(request());
    EXPECT_EQ(rig.router.failover_count(), 2);
}

TEST(ProviderRouter, OwnedProvidersWork) {
    ProviderRouter router(attempts(1), [](milliseconds) {});
    router.add(std::make_unique<MockProvider>(named("owned")),
               std::make_unique<CircuitBreaker>(3, milliseconds(1000), 1, real_clock()), 1,
               milliseconds(100));
    EXPECT_EQ(router.size(), 1u);
    EXPECT_EQ(router.chat(request()).provider, "owned");
}

TEST(ProviderRouter, SameSeedGivesTheSameRetryDelays) {
    auto delays_for = [](std::uint64_t seed) {
        std::vector<milliseconds> sleeps;
        MockProvider p(named("p"));
        p.set_fail_rate(1.0);
        CircuitBreaker b(100, milliseconds(1000), 1, real_clock());
        ProviderRouter router(attempts(5), [&](milliseconds d) { sleeps.push_back(d); },
                              real_clock(), seed);
        router.add_ref(p, b, 1, milliseconds(100));
        EXPECT_THROW(router.chat(request()), ProviderError);
        return sleeps;
    };
    EXPECT_EQ(delays_for(7), delays_for(7));
}

TEST(ProviderRouter, ManyThreadsCanShareOneRouter) {
    FakeClock clock;  // only read after this point, never moved
    MockOptions flaky = named("flaky");
    flaky.fail_rate = 0.5;
    MockProvider primary(flaky);
    MockProvider secondary(named("secondary"));
    CircuitBreaker b1(1000000, milliseconds(1000), 1, clock.fn());
    CircuitBreaker b2(1000000, milliseconds(1000), 1, clock.fn());
    ProviderRouter router(attempts(2), [](milliseconds) {}, clock.fn(), 5);
    router.add_ref(primary, b1, 1, milliseconds(100));
    router.add_ref(secondary, b2, 2, milliseconds(100));

    std::atomic<int> ok{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 200; ++i) {
                try {
                    router.chat(request());
                    ++ok;
                } catch (const ProviderError&) {
                }
            }
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_EQ(ok.load(), 8 * 200);  // the secondary is healthy, so every request is answered
    EXPECT_GT(router.failover_count(), 0);
}
