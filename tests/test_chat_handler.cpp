#include <gtest/gtest.h>

#include <chrono>
#include <nlohmann/json.hpp>
#include <stdexcept>

#include "llmgate/handlers/chat_handler.h"
#include "llmgate/provider/mock.h"
#include "llmgate/util/sha256.h"
#include "test_helpers.h"

using namespace llmgate;
using std::chrono::milliseconds;
using std::chrono::seconds;
using json = nlohmann::json;

namespace {

const char* kBody = R"({"model":"m","messages":[{"role":"user","content":"hello"}]})";

KeyConfig make_key(const std::string& team, const std::string& raw, double burst, double per_second) {
    KeyConfig k;
    k.team = team;
    k.key_sha256 = sha256_hex(raw);
    k.rate_limit.burst = burst;
    k.rate_limit.per_second = per_second;
    return k;
}

HttpRequest make_request(const std::string& body = kBody,
                         const std::string& auth = "Bearer lg_live_a") {
    HttpRequest r;
    r.method = "POST";
    r.path = "/v1/chat";
    r.body = body;
    if (!auth.empty()) r.headers["authorization"] = auth;
    return r;
}

std::string code_of(const HttpResponse& r) { return json::parse(r.body)["error"]["code"]; }

// A complete set of fake dependencies. Time only moves when a test moves it.
struct Env {
    FakeClock clock;
    KeyStore keys;
    RateLimiter limiter{clock.fn()};
    ChatCache cache{100, seconds(60), clock.fn()};
    MockProvider provider;
    CircuitBreaker breaker{3, milliseconds(1000), 1, clock.fn()};
    ChatHandler handler;

    explicit Env(double burst = 100, double per_second = 100, MockOptions opts = {},
                 milliseconds timeout = milliseconds(1000))
        : keys({make_key("team-a", "lg_live_a", burst, per_second),
                make_key("team-b", "lg_live_b", burst, per_second)}),
          provider(opts),
          handler(keys, limiter, cache, provider, breaker, timeout) {}

    HttpResponse send(const std::string& body = kBody, const std::string& auth = "Bearer lg_live_a") {
        return handler.handle(make_request(body, auth));
    }
};

MockOptions failing(int n) {
    MockOptions o;
    o.fail_next_n = n;
    return o;
}

// A provider that throws something that is not a ProviderError.
class ExplodingProvider : public Provider {
public:
    ChatResponse chat(const ChatRequest&, milliseconds) override {
        throw std::runtime_error("secret database password");
    }
    std::string name() const override { return "exploding"; }
};

}  // namespace

// ---- authentication ---------------------------------------------------------

TEST(ChatHandler, MissingAuthGives401) {
    Env env;
    HttpResponse r = env.send(kBody, "");
    EXPECT_EQ(r.status, 401);
    EXPECT_EQ(code_of(r), "unauthorized");
    EXPECT_EQ(r.headers.at("www-authenticate"), "Bearer");
}

TEST(ChatHandler, MalformedAuthGives401) {
    Env env;
    EXPECT_EQ(env.send(kBody, "lg_live_a").status, 401);
    EXPECT_EQ(env.send(kBody, "bearer lg_live_a").status, 401);
    EXPECT_EQ(env.send(kBody, "Bearer").status, 401);
}

TEST(ChatHandler, InvalidKeyGives401) {
    Env env;
    EXPECT_EQ(env.send(kBody, "Bearer lg_live_wrong").status, 401);
}

TEST(ChatHandler, AuthIsCheckedBeforeBodyIsRead) {
    Env env;
    EXPECT_EQ(env.send("not json", "").status, 401);
}

TEST(ChatHandler, UnauthenticatedRequestsNeverReachProvider) {
    Env env;
    env.send(kBody, "");
    env.send(kBody, "Bearer nope");
    EXPECT_EQ(env.provider.calls(), 0);
}

// ---- validation -------------------------------------------------------------

TEST(ChatHandler, BadJsonGives400) {
    Env env;
    HttpResponse r = env.send("{ not json");
    EXPECT_EQ(r.status, 400);
    EXPECT_EQ(code_of(r), "bad_request");
}

TEST(ChatHandler, InvalidBodiesGive400) {
    Env env;
    const char* bad[] = {
        "[]",
        R"({})",
        R"({"messages":[{"role":"user","content":"x"}]})",
        R"({"model":"","messages":[{"role":"user","content":"x"}]})",
        R"({"model":1,"messages":[{"role":"user","content":"x"}]})",
        R"({"model":"m"})",
        R"({"model":"m","messages":[]})",
        R"({"model":"m","messages":"hi"})",
        R"({"model":"m","messages":["hi"]})",
        R"({"model":"m","messages":[{"role":"robot","content":"x"}]})",
        R"({"model":"m","messages":[{"content":"x"}]})",
        R"({"model":"m","messages":[{"role":"user"}]})",
        R"({"model":"m","messages":[{"role":"user","content":5}]})",
        R"({"model":"m","messages":[{"role":"user","content":"x"}],"max_tokens":0})",
        R"({"model":"m","messages":[{"role":"user","content":"x"}],"max_tokens":99999})",
        R"({"model":"m","messages":[{"role":"user","content":"x"}],"max_tokens":1.5})",
        R"({"model":"m","messages":[{"role":"user","content":"x"}],"max_tokens":"5"})",
        R"({"model":"m","messages":[{"role":"user","content":"x"}],"stream":"yes"})",
    };
    for (const char* body : bad) {
        EXPECT_EQ(env.send(body).status, 400) << body;
    }
    EXPECT_EQ(env.provider.calls(), 0);
}

TEST(ChatHandler, StreamingIsRejectedForNow) {
    Env env;
    HttpResponse r = env.send(
        R"({"model":"m","messages":[{"role":"user","content":"x"}],"stream":true})");
    EXPECT_EQ(r.status, 400);
    EXPECT_NE(json::parse(r.body)["error"]["message"].get<std::string>().find("streaming"),
              std::string::npos);
}

TEST(ChatHandler, ErrorMessageNamesTheBadField) {
    Env env;
    HttpResponse r = env.send(R"({"model":"m","messages":[{"role":"robot","content":"x"}]})");
    EXPECT_NE(json::parse(r.body)["error"]["message"].get<std::string>().find("messages[0].role"),
              std::string::npos);
}

TEST(ChatHandler, InvalidBodiesDoNotUseRateLimit) {
    Env env(/*burst=*/1, /*per_second=*/0.001);
    for (int i = 0; i < 5; ++i) EXPECT_EQ(env.send("{ bad").status, 400);
    EXPECT_EQ(env.send().status, 200);  // the one token is still there
}

// ---- success path -----------------------------------------------------------

TEST(ChatHandler, ValidRequestReturnsAnswer) {
    MockOptions o;
    o.canned_reply = "12345678";
    Env env(100, 100, o);
    HttpResponse r = env.send();
    ASSERT_EQ(r.status, 200);
    EXPECT_EQ(r.headers.at("content-type"), "application/json");
    json j = json::parse(r.body);
    EXPECT_EQ(j["content"], "12345678");
    EXPECT_EQ(j["model"], "m");
    EXPECT_EQ(j["provider"], "mock");
    EXPECT_EQ(j["cached"], false);
    EXPECT_EQ(j["usage"]["prompt_tokens"], 2);      // "hello" = 5 chars -> 2
    EXPECT_EQ(j["usage"]["completion_tokens"], 2);  // 8 chars -> 2
    EXPECT_EQ(j["usage"]["total_tokens"], 4);
}

// ---- cache ------------------------------------------------------------------

TEST(ChatHandler, SecondIdenticalRequestIsCached) {
    Env env;
    EXPECT_EQ(json::parse(env.send().body)["cached"], false);
    HttpResponse second = env.send();
    EXPECT_EQ(second.status, 200);
    EXPECT_EQ(json::parse(second.body)["cached"], true);
    EXPECT_EQ(env.provider.calls(), 1);
}

TEST(ChatHandler, DifferentPromptIsNotCached) {
    Env env;
    env.send();
    env.send(R"({"model":"m","messages":[{"role":"user","content":"other"}]})");
    EXPECT_EQ(env.provider.calls(), 2);
}

TEST(ChatHandler, DifferentModelOrMaxTokensIsNotCached) {
    Env env;
    env.send();
    env.send(R"({"model":"other","messages":[{"role":"user","content":"hello"}]})");
    env.send(R"({"model":"m","messages":[{"role":"user","content":"hello"}],"max_tokens":10})");
    EXPECT_EQ(env.provider.calls(), 3);
}

TEST(ChatHandler, KeyOrderInJsonDoesNotMatter) {
    Env env;
    env.send(R"({"model":"m","messages":[{"role":"user","content":"hello"}]})");
    env.send(R"({"messages":[{"content":"hello","role":"user"}],"model":"m"})");
    EXPECT_EQ(env.provider.calls(), 1);
}

TEST(ChatHandler, TeamsDoNotShareCache) {
    Env env;
    env.send(kBody, "Bearer lg_live_a");
    HttpResponse b = env.send(kBody, "Bearer lg_live_b");
    EXPECT_EQ(json::parse(b.body)["cached"], false);
    EXPECT_EQ(env.provider.calls(), 2);
}

TEST(ChatHandler, CachedAnswerExpires) {
    Env env;
    env.send();
    env.clock.advance_ms(61000);  // the cache TTL is 60 s
    EXPECT_EQ(json::parse(env.send().body)["cached"], false);
    EXPECT_EQ(env.provider.calls(), 2);
}

TEST(ChatHandler, FailedAnswersAreNotCached) {
    Env env(100, 100, failing(1));
    EXPECT_EQ(env.send().status, 502);
    HttpResponse r = env.send();
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(json::parse(r.body)["cached"], false);
}

// ---- rate limiting ----------------------------------------------------------

TEST(ChatHandler, RateLimitedGives429WithRetryAfter) {
    Env env(/*burst=*/2, /*per_second=*/1);
    EXPECT_EQ(env.send().status, 200);
    EXPECT_EQ(env.send().status, 200);
    HttpResponse r = env.send();
    EXPECT_EQ(r.status, 429);
    EXPECT_EQ(code_of(r), "rate_limited");
    EXPECT_EQ(r.headers.at("retry-after"), "1");
}

TEST(ChatHandler, RetryAfterRoundsUpToWholeSeconds) {
    Env env(/*burst=*/1, /*per_second=*/0.4);  // needs 2.5 s for a new token
    env.send();
    EXPECT_EQ(env.send().headers.at("retry-after"), "3");
}

TEST(ChatHandler, RateLimitRefillsOverTime) {
    Env env(/*burst=*/1, /*per_second=*/1);
    EXPECT_EQ(env.send().status, 200);
    EXPECT_EQ(env.send().status, 429);
    env.clock.advance_ms(1000);
    EXPECT_EQ(env.send().status, 200);
}

TEST(ChatHandler, RateLimitedRequestsNeverReachProvider) {
    Env env(/*burst=*/1, /*per_second=*/0.001);
    env.send();
    env.send(R"({"model":"m","messages":[{"role":"user","content":"different"}]})");
    EXPECT_EQ(env.provider.calls(), 1);
}

TEST(ChatHandler, CachedAnswersStillCountAgainstRateLimit) {
    Env env(/*burst=*/2, /*per_second=*/0.001);
    env.send();
    EXPECT_EQ(env.send().status, 200);  // cache hit, second token
    EXPECT_EQ(env.send().status, 429);
}

TEST(ChatHandler, EachKeyHasItsOwnBucket) {
    Env env(/*burst=*/1, /*per_second=*/0.001);
    EXPECT_EQ(env.send(kBody, "Bearer lg_live_a").status, 200);
    EXPECT_EQ(env.send(kBody, "Bearer lg_live_a").status, 429);
    EXPECT_EQ(env.send(kBody, "Bearer lg_live_b").status, 200);
}

// ---- provider failures and the circuit breaker -------------------------------

TEST(ChatHandler, ProviderFailureGives502) {
    Env env(100, 100, failing(1));
    HttpResponse r = env.send();
    EXPECT_EQ(r.status, 502);
    EXPECT_EQ(code_of(r), "provider_error");
}

TEST(ChatHandler, ProviderTimeoutGives504) {
    MockOptions o;
    o.latency_ms = 200;
    Env env(100, 100, o, milliseconds(20));
    HttpResponse r = env.send();
    EXPECT_EQ(r.status, 504);
    EXPECT_EQ(code_of(r), "provider_timeout");
}

TEST(ChatHandler, UnexpectedProviderExceptionGives502WithoutLeakingDetails) {
    FakeClock clock;
    KeyStore keys({make_key("team-a", "lg_live_a", 100, 100)});
    RateLimiter limiter(clock.fn());
    ChatCache cache(10, seconds(60), clock.fn());
    ExplodingProvider provider;
    CircuitBreaker breaker(3, milliseconds(1000), 1, clock.fn());
    ChatHandler handler(keys, limiter, cache, provider, breaker, milliseconds(100));

    HttpResponse r = handler.handle(make_request());
    EXPECT_EQ(r.status, 502);
    EXPECT_EQ(r.body.find("secret"), std::string::npos);
    EXPECT_EQ(breaker.state(), BreakerState::Closed);  // 1 failure < threshold 3
}

TEST(ChatHandler, OpenBreakerGives502WithoutCallingProvider) {
    Env env(100, 100, failing(3));
    for (int i = 0; i < 3; ++i) EXPECT_EQ(env.send().status, 502);
    ASSERT_EQ(env.breaker.state(), BreakerState::Open);
    ASSERT_EQ(env.provider.calls(), 3);

    HttpResponse r = env.send();
    EXPECT_EQ(r.status, 502);
    EXPECT_EQ(code_of(r), "provider_unavailable");
    EXPECT_EQ(env.provider.calls(), 3);  // not called again
}

TEST(ChatHandler, BreakerRecoversAfterCooldown) {
    Env env(100, 100, failing(3));
    for (int i = 0; i < 3; ++i) env.send();
    EXPECT_EQ(code_of(env.send()), "provider_unavailable");
    env.clock.advance_ms(1000);
    EXPECT_EQ(env.send().status, 200);  // trial call succeeds, breaker closes
    EXPECT_EQ(env.breaker.state(), BreakerState::Closed);
}

TEST(ChatHandler, CachedAnswersAreServedEvenWhenBreakerIsOpen) {
    Env env;
    env.send();  // fills the cache
    env.provider.set_fail_rate(1.0);
    env.send(R"({"model":"m","messages":[{"role":"user","content":"a"}]})");
    env.send(R"({"model":"m","messages":[{"role":"user","content":"b"}]})");
    env.send(R"({"model":"m","messages":[{"role":"user","content":"c"}]})");
    ASSERT_EQ(env.breaker.state(), BreakerState::Open);

    HttpResponse r = env.send();  // the cached question
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(json::parse(r.body)["cached"], true);
}

TEST(ChatHandler, SuccessKeepsBreakerClosed) {
    Env env;
    for (int i = 0; i < 10; ++i) env.send();
    EXPECT_EQ(env.breaker.state(), BreakerState::Closed);
}