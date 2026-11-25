#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "llmgate/app/app.h"
#include "llmgate/budget/budget_tracker.h"
#include "llmgate/handlers/chat_handler.h"
#include "llmgate/provider/mock.h"
#include "llmgate/util/sha256.h"

using namespace llmgate;
using json = nlohmann::json;
using std::chrono::hours;
using std::chrono::seconds;

namespace {

// A wall clock that only moves when the test moves it. It starts at
// 10:00 UTC on day 20000 since 1970, so midnight is 14 hours away.
struct FakeWall {
    WallTime now = WallTime{} + hours(24 * 20000 + 10);
    WallClock fn() { return [this] { return now; }; }
};

// ---- BudgetTracker on its own -------------------------------------------

TEST(BudgetTracker, StartsAtZero) {
    FakeWall wall;
    BudgetTracker b(wall.fn());
    EXPECT_EQ(b.used_today("k"), 0);
}

TEST(BudgetTracker, AllowsSpendWithinBudget) {
    FakeWall wall;
    BudgetTracker b(wall.fn());
    b.record("k", 400);
    EXPECT_TRUE(b.can_spend("k", 500, 1000));
    EXPECT_EQ(b.used_today("k"), 400);  // asking does not spend
}

TEST(BudgetTracker, AllowsExactlyTheBudget) {
    FakeWall wall;
    BudgetTracker b(wall.fn());
    b.record("k", 400);
    EXPECT_TRUE(b.can_spend("k", 600, 1000));
    EXPECT_FALSE(b.can_spend("k", 601, 1000));
}

TEST(BudgetTracker, RejectsOverBudget) {
    FakeWall wall;
    BudgetTracker b(wall.fn());
    b.record("k", 900);
    EXPECT_FALSE(b.can_spend("k", 200, 1000));
}

TEST(BudgetTracker, RecordAddsUp) {
    FakeWall wall;
    BudgetTracker b(wall.fn());
    b.record("k", 10);
    b.record("k", 20);
    b.record("k", 30);
    EXPECT_EQ(b.used_today("k"), 60);
}

TEST(BudgetTracker, KeysAreIndependent) {
    FakeWall wall;
    BudgetTracker b(wall.fn());
    b.record("a", 900);
    EXPECT_FALSE(b.can_spend("a", 200, 1000));
    EXPECT_TRUE(b.can_spend("b", 200, 1000));
    EXPECT_EQ(b.used_today("b"), 0);
}

TEST(BudgetTracker, IgnoresZeroAndNegativeRecords) {
    FakeWall wall;
    BudgetTracker b(wall.fn());
    b.record("k", 50);
    b.record("k", 0);
    b.record("k", -30);
    EXPECT_EQ(b.used_today("k"), 50);
}

TEST(BudgetTracker, DoesNotResetBeforeMidnight) {
    FakeWall wall;
    BudgetTracker b(wall.fn());
    b.record("k", 700);
    wall.now += hours(13) + std::chrono::minutes(59);  // 23:59 UTC
    EXPECT_EQ(b.used_today("k"), 700);
}

TEST(BudgetTracker, ResetsAtUtcMidnight) {
    FakeWall wall;
    BudgetTracker b(wall.fn());
    b.record("k", 1000);
    EXPECT_FALSE(b.can_spend("k", 1, 1000));

    wall.now += hours(14);  // exactly 00:00 UTC the next day
    EXPECT_EQ(b.used_today("k"), 0);
    EXPECT_TRUE(b.can_spend("k", 1000, 1000));

    b.record("k", 5);  // the new day starts counting from zero
    EXPECT_EQ(b.used_today("k"), 5);
}

TEST(BudgetTracker, SecondsUntilReset) {
    FakeWall wall;  // 10:00 UTC
    BudgetTracker b(wall.fn());
    EXPECT_EQ(b.seconds_until_reset(), 14 * 3600);
    wall.now += hours(13) + std::chrono::minutes(59) + seconds(59);  // 23:59:59
    EXPECT_EQ(b.seconds_until_reset(), 1);
    wall.now += seconds(1);  // 00:00:00
    EXPECT_EQ(b.seconds_until_reset(), 24 * 3600);
}

TEST(BudgetTracker, ConcurrentRecordsAddUpExactly) {
    FakeWall wall;
    BudgetTracker b(wall.fn());
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&b] {
            for (int i = 0; i < 1000; ++i) {
                b.record("k", 3);
                b.can_spend("k", 1, 1000000);
                b.used_today("k");
            }
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_EQ(b.used_today("k"), 8 * 1000 * 3);
}

// ---- Budgets inside the whole gateway ----------------------------------------

const char* kRawKey = "lg_live_budgetkey";
const char* kOtherKey = "lg_live_otherkey";

Config budget_config(long long budget) {
    Config c;
    for (auto [team, raw] : {std::pair<const char*, const char*>{"t1", kRawKey}, {"t2", kOtherKey}}) {
        KeyConfig k;
        k.team = team;
        k.key_sha256 = sha256_hex(raw);
        k.rate_limit.burst = 100000;
        k.rate_limit.per_second = 100000;
        k.daily_token_budget = budget;
        c.keys.push_back(k);
    }
    ProviderConfig p;
    p.name = "mock";
    p.type = "mock";
    c.providers.push_back(p);
    return c;
}

HttpRequest chat_request(const std::string& prompt, const std::string& key = kRawKey) {
    HttpRequest r;
    r.method = "POST";
    r.path = "/v1/chat";
    r.headers["authorization"] = std::string("Bearer ") + key;
    r.body = json{{"model", "m"}, {"messages", {{{"role", "user"}, {"content", prompt}}}}}.dump();
    return r;
}

HttpRequest usage_request(const std::string& key = kRawKey, const std::string& method = "GET") {
    HttpRequest r;
    r.method = method;
    r.path = "/v1/usage";
    if (!key.empty()) r.headers["authorization"] = "Bearer " + key;
    return r;
}

long long used(const App& app, const std::string& key = kRawKey) {
    return json::parse(app.router().dispatch(usage_request(key)).body)["used_today"];
}

TEST(Budget, RequestsWithinBudgetSucceed) {
    FakeWall wall;
    App app(budget_config(100000), real_clock(), wall.fn());
    EXPECT_EQ(app.router().dispatch(chat_request("hello")).status, 200);
}

TEST(Budget, KeyWithTinyBudgetGets429AfterAFewRequests) {
    FakeWall wall;
    App app(budget_config(100), real_clock(), wall.fn());

    int ok = 0;
    HttpResponse last;
    for (int i = 0; i < 50; ++i) {
        last = app.router().dispatch(chat_request("question number " + std::to_string(i)));
        if (last.status != 200) break;
        ++ok;
    }
    EXPECT_GE(ok, 1);
    EXPECT_LT(ok, 50);
    EXPECT_EQ(last.status, 429);
    EXPECT_EQ(json::parse(last.body)["error"]["code"], "budget_exceeded");

    const long long retry = std::stoll(last.headers.at("retry-after"));
    EXPECT_EQ(retry, 14 * 3600);  // it is 10:00 UTC
}

TEST(Budget, BigPromptIsRefusedBeforeTheCall) {
    FakeWall wall;
    App app(budget_config(100), real_clock(), wall.fn());
    // about 4000 characters is about 1000 tokens, far over a budget of 100
    HttpResponse r = app.router().dispatch(chat_request(std::string(4000, 'x')));
    EXPECT_EQ(r.status, 429);
    EXPECT_EQ(used(app), 0);  // nothing was spent
}

TEST(Budget, CacheHitDoesNotSpend) {
    FakeWall wall;
    App app(budget_config(100000), real_clock(), wall.fn());

    HttpResponse first = app.router().dispatch(chat_request("same question"));
    ASSERT_EQ(first.status, 200);
    const long long after_first = used(app);
    EXPECT_EQ(after_first, json::parse(first.body)["usage"]["total_tokens"].get<long long>());
    EXPECT_GT(after_first, 0);

    for (int i = 0; i < 5; ++i) {
        HttpResponse again = app.router().dispatch(chat_request("same question"));
        ASSERT_EQ(again.status, 200);
        EXPECT_EQ(json::parse(again.body)["cached"], true);
    }
    EXPECT_EQ(used(app), after_first);
}

TEST(Budget, RejectedRequestsDoNotSpend) {
    FakeWall wall;
    App app(budget_config(100), real_clock(), wall.fn());
    for (int i = 0; i < 50; ++i) app.router().dispatch(chat_request("q" + std::to_string(i)));
    const long long before = used(app);
    EXPECT_EQ(app.router().dispatch(chat_request("one more")).status, 429);
    EXPECT_EQ(used(app), before);
}

TEST(Budget, EachKeyHasItsOwnBudget) {
    FakeWall wall;
    App app(budget_config(100), real_clock(), wall.fn());
    for (int i = 0; i < 50; ++i) app.router().dispatch(chat_request("q" + std::to_string(i)));
    EXPECT_EQ(app.router().dispatch(chat_request("again")).status, 429);
    EXPECT_EQ(app.router().dispatch(chat_request("fresh", kOtherKey)).status, 200);
    EXPECT_EQ(used(app, kOtherKey) > 0, true);
}

TEST(Budget, BudgetComesBackAfterUtcMidnight) {
    FakeWall wall;
    App app(budget_config(100), real_clock(), wall.fn());
    for (int i = 0; i < 50; ++i) app.router().dispatch(chat_request("q" + std::to_string(i)));
    ASSERT_EQ(app.router().dispatch(chat_request("blocked")).status, 429);

    wall.now += hours(14);
    EXPECT_EQ(app.router().dispatch(chat_request("blocked")).status, 200);
    EXPECT_GT(used(app), 0);
}

TEST(Budget, FailedProviderCallDoesNotSpend) {
    FakeWall wall;
    BudgetTracker budget(wall.fn());
    KeyStore keys({[] {
        KeyConfig k;
        k.team = "t";
        k.key_sha256 = sha256_hex(kRawKey);
        k.rate_limit.burst = 100;
        k.rate_limit.per_second = 100;
        return k;
    }()});
    RateLimiter limiter(real_clock());
    ChatCache cache(10, seconds(60), real_clock());
    MockOptions opts;
    opts.fail_next_n = 1;
    MockProvider provider(opts);
    CircuitBreaker breaker(5, std::chrono::milliseconds(1000), 1, real_clock());
    RetryPolicy one_try;
    one_try.max_attempts = 1;
    ProviderRouter router(one_try, [](std::chrono::milliseconds) {}, real_clock());
    router.add_ref(provider, breaker, 1, std::chrono::milliseconds(1000));
    ChatHandler handler(keys, limiter, cache, router, &budget);

    EXPECT_EQ(handler.handle(chat_request("will fail")).status, 502);
    EXPECT_EQ(budget.used_today(sha256_hex(kRawKey)), 0);
    EXPECT_EQ(handler.handle(chat_request("will work")).status, 200);
    EXPECT_GT(budget.used_today(sha256_hex(kRawKey)), 0);
}

// ---- GET /v1/usage ---------------------------------------------------------

TEST(Usage, UsageEndpointReportsCounters) {
    FakeWall wall;
    App app(budget_config(5000), real_clock(), wall.fn());

    HttpResponse before = app.router().dispatch(usage_request());
    ASSERT_EQ(before.status, 200);
    EXPECT_EQ(json::parse(before.body)["used_today"], 0);
    EXPECT_EQ(json::parse(before.body)["daily_budget"], 5000);

    HttpResponse chat = app.router().dispatch(chat_request("count me"));
    const long long tokens = json::parse(chat.body)["usage"]["total_tokens"];

    HttpResponse after = app.router().dispatch(usage_request());
    EXPECT_EQ(json::parse(after.body)["used_today"], tokens);
    EXPECT_EQ(json::parse(after.body)["daily_budget"], 5000);
}

TEST(Usage, UsageIsPerKey) {
    FakeWall wall;
    App app(budget_config(5000), real_clock(), wall.fn());
    app.router().dispatch(chat_request("only mine"));
    EXPECT_GT(used(app, kRawKey), 0);
    EXPECT_EQ(used(app, kOtherKey), 0);
}

TEST(Usage, UsageEndpointNeedsAValidKey) {
    FakeWall wall;
    App app(budget_config(5000), real_clock(), wall.fn());
    EXPECT_EQ(app.router().dispatch(usage_request("")).status, 401);
    EXPECT_EQ(app.router().dispatch(usage_request("lg_live_wrong")).status, 401);
}

TEST(Usage, UsageEndpointIsGetOnly) {
    FakeWall wall;
    App app(budget_config(5000), real_clock(), wall.fn());
    EXPECT_EQ(app.router().dispatch(usage_request(kRawKey, "POST")).status, 405);
}

TEST(Usage, UsageDoesNotUseUpTheRateLimit) {
    FakeWall wall;
    Config c = budget_config(5000);
    c.keys[0].rate_limit.burst = 1;
    c.keys[0].rate_limit.per_second = 0.001;
    App app(c, real_clock(), wall.fn());
    for (int i = 0; i < 5; ++i) EXPECT_EQ(app.router().dispatch(usage_request()).status, 200);
    EXPECT_EQ(app.router().dispatch(chat_request("first")).status, 200);
}

}  // namespace
