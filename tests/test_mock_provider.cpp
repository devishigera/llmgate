#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <vector>

#include "llmgate/provider/mock.h"

using llmgate::ChatRequest;
using llmgate::MockOptions;
using llmgate::MockProvider;
using llmgate::ProviderError;
using std::chrono::milliseconds;

namespace {

ChatRequest request(const std::string& text = "hello there") {
    ChatRequest r;
    r.model = "test-model";
    r.messages.push_back({"user", text});
    return r;
}

const milliseconds kTimeout{1000};

}  // namespace

TEST(MockProvider, ReturnsCannedReply) {
    MockOptions o;
    o.canned_reply = "hi from mock";
    MockProvider p(o);
    auto r = p.chat(request(), kTimeout);
    EXPECT_EQ(r.content, "hi from mock");
    EXPECT_EQ(r.provider, "mock");
    EXPECT_EQ(r.model, "test-model");
}

TEST(MockProvider, NameComesFromOptions) {
    MockOptions o;
    o.name = "primary";
    EXPECT_EQ(MockProvider(o).name(), "primary");
}

TEST(MockProvider, FailsNextNCalls) {
    MockOptions o;
    o.fail_next_n = 2;
    MockProvider p(o);
    EXPECT_THROW(p.chat(request(), kTimeout), ProviderError);
    EXPECT_THROW(p.chat(request(), kTimeout), ProviderError);
    EXPECT_NO_THROW(p.chat(request(), kTimeout));
    EXPECT_NO_THROW(p.chat(request(), kTimeout));
}

TEST(MockProvider, InjectedFailureIsTransientNotTimeout) {
    MockOptions o;
    o.fail_next_n = 1;
    MockProvider p(o);
    try {
        p.chat(request(), kTimeout);
        FAIL() << "expected ProviderError";
    } catch (const ProviderError& e) {
        EXPECT_TRUE(e.transient());
        EXPECT_FALSE(e.timeout());
    }
}

TEST(MockProvider, FailRateOneAlwaysFails) {
    MockOptions o;
    o.fail_rate = 1.0;
    MockProvider p(o);
    for (int i = 0; i < 50; ++i) EXPECT_THROW(p.chat(request(), kTimeout), ProviderError);
}

TEST(MockProvider, FailRateZeroNeverFails) {
    MockProvider p;
    for (int i = 0; i < 50; ++i) EXPECT_NO_THROW(p.chat(request(), kTimeout));
}

TEST(MockProvider, FailRateHalfFailsSometimes) {
    MockOptions o;
    o.fail_rate = 0.5;
    MockProvider p(o);
    int failures = 0;
    for (int i = 0; i < 400; ++i) {
        try {
            p.chat(request(), kTimeout);
        } catch (const ProviderError&) {
            ++failures;
        }
    }
    EXPECT_GT(failures, 120);
    EXPECT_LT(failures, 280);
}

TEST(MockProvider, SameSeedGivesSameFailurePattern) {
    auto pattern = [](unsigned seed) {
        MockOptions o;
        o.fail_rate = 0.5;
        o.seed = seed;
        MockProvider p(o);
        std::vector<bool> out;
        for (int i = 0; i < 40; ++i) {
            try {
                p.chat(request(), kTimeout);
                out.push_back(false);
            } catch (const ProviderError&) {
                out.push_back(true);
            }
        }
        return out;
    };
    EXPECT_EQ(pattern(7), pattern(7));
    EXPECT_NE(pattern(7), pattern(8));
}

TEST(MockProvider, ReportsTokenUsage) {
    MockOptions o;
    o.canned_reply = "12345678";  // 8 chars -> 2 tokens
    MockProvider p(o);
    auto r = p.chat(request("abcdefgh"), kTimeout);  // 8 chars -> 2 tokens
    EXPECT_EQ(r.usage.prompt_tokens, 2);
    EXPECT_EQ(r.usage.completion_tokens, 2);
    EXPECT_EQ(r.usage.total(), 4);
}

TEST(MockProvider, UsageAddsUpAllMessages) {
    MockProvider p;
    ChatRequest r;
    r.messages.push_back({"system", "abcd"});  // 1
    r.messages.push_back({"user", "abcde"});   // 2 (rounded up)
    EXPECT_EQ(p.chat(r, kTimeout).usage.prompt_tokens, 3);
}

TEST(MockProvider, SlowCallBeyondTimeoutTimesOut) {
    MockOptions o;
    o.latency_ms = 200;
    MockProvider p(o);
    try {
        p.chat(request(), milliseconds(20));
        FAIL() << "expected timeout";
    } catch (const ProviderError& e) {
        EXPECT_TRUE(e.timeout());
        EXPECT_TRUE(e.transient());
    }
}

TEST(MockProvider, SlowCallWithinTimeoutSucceeds) {
    MockOptions o;
    o.latency_ms = 30;
    MockProvider p(o);
    auto start = std::chrono::steady_clock::now();
    EXPECT_NO_THROW(p.chat(request(), milliseconds(1000)));
    auto took = std::chrono::steady_clock::now() - start;
    EXPECT_GE(took, milliseconds(25));
}

TEST(MockProvider, CountsCalls) {
    MockOptions o;
    o.fail_next_n = 1;
    MockProvider p(o);
    try { p.chat(request(), kTimeout); } catch (const ProviderError&) {}
    p.chat(request(), kTimeout);
    EXPECT_EQ(p.calls(), 2);
}

TEST(MockProvider, BehaviourCanChangeWhileRunning) {
    MockProvider p;
    EXPECT_NO_THROW(p.chat(request(), kTimeout));
    p.set_fail_rate(1.0);
    EXPECT_THROW(p.chat(request(), kTimeout), ProviderError);
    p.set_fail_rate(0.0);
    p.set_fail_next(1);
    EXPECT_THROW(p.chat(request(), kTimeout), ProviderError);
    EXPECT_NO_THROW(p.chat(request(), kTimeout));
}

TEST(MockProvider, SafeUnderConcurrentCalls) {
    MockOptions o;
    o.fail_rate = 0.3;
    MockProvider p(o);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&p] {
            for (int i = 0; i < 200; ++i) {
                try { p.chat(request(), kTimeout); } catch (const ProviderError&) {}
            }
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(p.calls(), 800);
}