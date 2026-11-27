#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "llmgate/app/app.h"
#include "llmgate/metrics/metrics.h"
#include "llmgate/util/sha256.h"

using namespace llmgate;
using json = nlohmann::json;

// ======================= the building blocks =======================

TEST(Metrics, CounterStartsAtZeroAndIncrements) {
    Counter c;
    EXPECT_EQ(c.value(), 0u);
    c.inc();
    c.inc();
    c.inc(5);
    EXPECT_EQ(c.value(), 7u);
}

TEST(Metrics, GaugeSetsAndAdds) {
    Gauge g;
    EXPECT_EQ(g.value(), 0.0);
    g.set(2.5);
    EXPECT_EQ(g.value(), 2.5);
    g.add(1.0);
    g.add(-0.5);
    EXPECT_EQ(g.value(), 3.0);
}

TEST(Metrics, HistogramPutsValuesInTheRightBucket) {
    Histogram h({0.1, 0.5, 1.0});
    h.observe(0.05);  // <= 0.1
    h.observe(0.1);   // exactly on the bound: still the 0.1 bucket ("le" = less or equal)
    h.observe(0.3);   // <= 0.5
    h.observe(2.0);   // above every bound: the +Inf bucket
    EXPECT_EQ(h.bucket_counts(), (std::vector<std::uint64_t>{2, 1, 0, 1}));
    EXPECT_EQ(h.count(), 4u);
}

TEST(Metrics, HistogramSumAddsEveryObservation) {
    Histogram h({1.0});
    h.observe(0.25);
    h.observe(0.5);
    EXPECT_DOUBLE_EQ(h.sum(), 0.75);
}

TEST(Metrics, HistogramIgnoresNaN) {
    Histogram h({1.0});
    h.observe(std::nan(""));
    EXPECT_EQ(h.count(), 0u);
    EXPECT_EQ(h.sum(), 0.0);
}

TEST(Metrics, HistogramBucketsAreCumulativeWhenRendered) {
    MetricsRegistry reg;
    Histogram& h = reg.histogram("t_seconds", "Test", "", {0.1, 0.5, 1.0});
    h.observe(0.05);
    h.observe(0.3);
    h.observe(0.3);
    h.observe(2.0);
    const std::string text = reg.render();
    // Each bucket includes everything below it: 1, 3, 3, then all 4.
    EXPECT_NE(text.find("t_seconds_bucket{le=\"0.1\"} 1\n"), std::string::npos) << text;
    EXPECT_NE(text.find("t_seconds_bucket{le=\"0.5\"} 3\n"), std::string::npos) << text;
    EXPECT_NE(text.find("t_seconds_bucket{le=\"1\"} 3\n"), std::string::npos) << text;
    EXPECT_NE(text.find("t_seconds_bucket{le=\"+Inf\"} 4\n"), std::string::npos) << text;
    EXPECT_NE(text.find("t_seconds_count 4\n"), std::string::npos) << text;
}

// ======================= the text format =======================

TEST(Metrics, RendersExactPrometheusText) {
    MetricsRegistry reg;
    reg.counter("llmgate_requests_total", "Total requests", label("status", "200")).inc(1042);
    reg.counter("llmgate_requests_total", "Total requests", label("status", "429")).inc(7);
    reg.gauge("llmgate_breaker_state", "Breaker state", label("provider", "ollama")).set(1);
    Histogram& h = reg.histogram("llmgate_request_duration_seconds", "Request time", "",
                                 {0.1, 0.5, 1.0});
    h.observe(0.05);
    h.observe(0.3);
    h.observe(0.3);
    h.observe(2.0);

    const std::string expected =
        "# HELP llmgate_breaker_state Breaker state\n"
        "# TYPE llmgate_breaker_state gauge\n"
        "llmgate_breaker_state{provider=\"ollama\"} 1\n"
        "# HELP llmgate_request_duration_seconds Request time\n"
        "# TYPE llmgate_request_duration_seconds histogram\n"
        "llmgate_request_duration_seconds_bucket{le=\"0.1\"} 1\n"
        "llmgate_request_duration_seconds_bucket{le=\"0.5\"} 3\n"
        "llmgate_request_duration_seconds_bucket{le=\"1\"} 3\n"
        "llmgate_request_duration_seconds_bucket{le=\"+Inf\"} 4\n"
        "llmgate_request_duration_seconds_sum 2.65\n"
        "llmgate_request_duration_seconds_count 4\n"
        "# HELP llmgate_requests_total Total requests\n"
        "# TYPE llmgate_requests_total counter\n"
        "llmgate_requests_total{status=\"200\"} 1042\n"
        "llmgate_requests_total{status=\"429\"} 7\n";
    EXPECT_EQ(reg.render(), expected);
}

TEST(Metrics, UnlabelledSeriesHaveNoBraces) {
    MetricsRegistry reg;
    reg.counter("hits_total", "Hits").inc(3);
    EXPECT_EQ(reg.render(), "# HELP hits_total Hits\n# TYPE hits_total counter\nhits_total 3\n");
}

TEST(Metrics, HistogramWithLabelsPutsLeLast) {
    MetricsRegistry reg;
    reg.histogram("d_seconds", "D", label("provider", "p"), {1.0}).observe(0.5);
    const std::string text = reg.render();
    EXPECT_NE(text.find("d_seconds_bucket{provider=\"p\",le=\"1\"} 1\n"), std::string::npos) << text;
    EXPECT_NE(text.find("d_seconds_sum{provider=\"p\"} 0.5\n"), std::string::npos) << text;
    EXPECT_NE(text.find("d_seconds_count{provider=\"p\"} 1\n"), std::string::npos) << text;
}

TEST(Metrics, LabelledSeriesAreIndependent) {
    MetricsRegistry reg;
    reg.counter("c_total", "C", label("k", "a")).inc();
    reg.counter("c_total", "C", label("k", "b")).inc(5);
    EXPECT_EQ(reg.counter("c_total", "C", label("k", "a")).value(), 1u);
    EXPECT_EQ(reg.counter("c_total", "C", label("k", "b")).value(), 5u);
}

TEST(Metrics, AskingTwiceGivesTheSameSeries) {
    MetricsRegistry reg;
    Counter& first = reg.counter("c_total", "C", label("k", "a"));
    Counter& again = reg.counter("c_total", "C", label("k", "a"));
    EXPECT_EQ(&first, &again);
}

TEST(Metrics, OutputOrderDoesNotDependOnCreationOrder) {
    MetricsRegistry a, b;
    a.counter("z_total", "Z").inc();
    a.counter("a_total", "A", label("k", "2")).inc();
    a.counter("a_total", "A", label("k", "1")).inc();
    b.counter("a_total", "A", label("k", "1")).inc();
    b.counter("a_total", "A", label("k", "2")).inc();
    b.counter("z_total", "Z").inc();
    EXPECT_EQ(a.render(), b.render());
}

TEST(Metrics, LabelValuesAreEscaped) {
    EXPECT_EQ(label("k", "plain"), "k=\"plain\"");
    EXPECT_EQ(label("k", "a\"b"), "k=\"a\\\"b\"");
    EXPECT_EQ(label("k", "a\\b"), "k=\"a\\\\b\"");
    EXPECT_EQ(label("k", "a\nb"), "k=\"a\\nb\"");
    EXPECT_EQ(labels({{"a", "1"}, {"b", "2"}}), "a=\"1\",b=\"2\"");
}

TEST(Metrics, HelpTextIsEscaped) {
    MetricsRegistry reg;
    reg.counter("c_total", "line one\nline two \\ end").inc();
    EXPECT_NE(reg.render().find("# HELP c_total line one\\nline two \\\\ end\n"), std::string::npos);
}

TEST(Metrics, UsingANameAsADifferentKindThrows) {
    MetricsRegistry reg;
    reg.counter("thing", "A thing");
    EXPECT_THROW(reg.gauge("thing", "A thing"), std::logic_error);
    EXPECT_THROW(reg.histogram("thing", "A thing"), std::logic_error);
}

TEST(Metrics, EmptyRegistryRendersNothing) {
    MetricsRegistry reg;
    EXPECT_EQ(reg.render(), "");
}

// ======================= many threads =======================

TEST(Metrics, ConcurrentIncrementsAreExact) {
    MetricsRegistry reg;
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&reg] {
            for (int i = 0; i < 20000; ++i) reg.counter("c_total", "C", label("k", "x")).inc();
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_EQ(reg.counter("c_total", "C", label("k", "x")).value(), 8u * 20000u);
}

TEST(Metrics, ConcurrentHistogramObservationsAddUp) {
    Histogram h({1.0, 2.0});
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&h] {
            for (int i = 0; i < 10000; ++i) h.observe(0.5);
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_EQ(h.count(), 80000u);
    EXPECT_DOUBLE_EQ(h.sum(), 40000.0);
}

TEST(Metrics, CreatingSeriesWhileRenderingIsSafe) {
    MetricsRegistry reg;
    std::atomic<bool> done{false};
    std::thread writer([&] {
        for (int i = 0; i < 300; ++i) reg.counter("c_total", "C", label("k", std::to_string(i))).inc();
        done = true;
    });
    std::thread reader([&] {
        while (!done) reg.render();
    });
    writer.join();
    reader.join();
    // 300 series: one HELP, one TYPE and 300 value lines
    const std::string text = reg.render();
    EXPECT_EQ(std::count(text.begin(), text.end(), '\n'), 302);
}

// ======================= the gateway's own metrics =======================

namespace {

const char* kKey = "lg_live_metricskey";

Config gateway_config(double burst = 1000, long long budget = 100000) {
    Config c;
    KeyConfig k;
    k.team = "team-one";
    k.key_sha256 = sha256_hex(kKey);
    k.rate_limit.burst = burst;
    k.rate_limit.per_second = burst >= 1000 ? 1000 : 0.001;
    k.daily_token_budget = budget;
    c.keys.push_back(k);
    ProviderConfig p;
    p.name = "mock";
    p.type = "mock";
    c.providers.push_back(p);
    return c;
}

HttpRequest chat(const std::string& prompt, const std::string& key = kKey) {
    HttpRequest r;
    r.method = "POST";
    r.path = "/v1/chat";
    if (!key.empty()) r.headers["authorization"] = "Bearer " + key;
    r.body = json{{"model", "m"}, {"messages", {{{"role", "user"}, {"content", prompt}}}}}.dump();
    return r;
}

HttpResponse get_metrics(const App& app) {
    HttpRequest r;
    r.method = "GET";
    r.path = "/metrics";
    return app.router().dispatch(r);
}

// True if `line` appears as a whole line of the page.
bool has_line(const std::string& page, const std::string& line) {
    return page.find("\n" + line + "\n") != std::string::npos || page.rfind(line + "\n", 0) == 0;
}

void no_sleep(std::chrono::milliseconds) {}

}  // namespace

TEST(GatewayMetrics, EndpointNeedsNoKeyAndUsesThePrometheusContentType) {
    App app(gateway_config());
    HttpResponse r = get_metrics(app);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.headers.at("content-type"), "text/plain; version=0.0.4; charset=utf-8");
    EXPECT_NE(r.body.find("# TYPE llmgate_cache_hits_total counter"), std::string::npos) << r.body;
}

TEST(GatewayMetrics, EndpointIsGetOnly) {
    App app(gateway_config());
    HttpRequest r;
    r.method = "POST";
    r.path = "/metrics";
    EXPECT_EQ(app.router().dispatch(r).status, 405);
}

TEST(GatewayMetrics, EverythingStartsAtZero) {
    App app(gateway_config());
    const std::string page = get_metrics(app).body;
    EXPECT_TRUE(has_line(page, "llmgate_cache_hits_total 0")) << page;
    EXPECT_TRUE(has_line(page, "llmgate_cache_misses_total 0")) << page;
    EXPECT_TRUE(has_line(page, "llmgate_rate_limited_total 0")) << page;
    EXPECT_TRUE(has_line(page, "llmgate_budget_rejected_total 0")) << page;
    EXPECT_TRUE(has_line(page, "llmgate_failovers_total 0")) << page;
    EXPECT_TRUE(has_line(page, "llmgate_request_duration_seconds_count 0")) << page;
}

TEST(GatewayMetrics, RequestsAreCountedByStatus) {
    App app(gateway_config());
    EXPECT_EQ(app.router().dispatch(chat("hello")).status, 200);
    EXPECT_EQ(app.router().dispatch(chat("hello", "lg_live_wrong")).status, 401);
    HttpRequest bad = chat("x");
    bad.body = "not json";
    EXPECT_EQ(app.router().dispatch(bad).status, 400);

    const std::string page = get_metrics(app).body;
    EXPECT_TRUE(has_line(page, "llmgate_requests_total{status=\"200\"} 1")) << page;
    EXPECT_TRUE(has_line(page, "llmgate_requests_total{status=\"401\"} 1")) << page;
    EXPECT_TRUE(has_line(page, "llmgate_requests_total{status=\"400\"} 1")) << page;
}

TEST(GatewayMetrics, DurationHistogramCountsEveryRequest) {
    App app(gateway_config());
    for (int i = 0; i < 3; ++i) app.router().dispatch(chat("q" + std::to_string(i)));
    const std::string page = get_metrics(app).body;
    EXPECT_TRUE(has_line(page, "llmgate_request_duration_seconds_count 3")) << page;
    EXPECT_TRUE(has_line(page, "llmgate_request_duration_seconds_bucket{le=\"+Inf\"} 3")) << page;
}

TEST(GatewayMetrics, CacheHitsAndMissesAreCounted) {
    App app(gateway_config());
    app.router().dispatch(chat("same"));
    app.router().dispatch(chat("same"));
    app.router().dispatch(chat("same"));
    const std::string page = get_metrics(app).body;
    EXPECT_TRUE(has_line(page, "llmgate_cache_misses_total 1")) << page;
    EXPECT_TRUE(has_line(page, "llmgate_cache_hits_total 2")) << page;
}

TEST(GatewayMetrics, RateLimitedRequestsAreCounted) {
    App app(gateway_config(/*burst=*/1));
    EXPECT_EQ(app.router().dispatch(chat("a")).status, 200);
    EXPECT_EQ(app.router().dispatch(chat("b")).status, 429);
    const std::string page = get_metrics(app).body;
    EXPECT_TRUE(has_line(page, "llmgate_rate_limited_total 1")) << page;
    EXPECT_TRUE(has_line(page, "llmgate_requests_total{status=\"429\"} 1")) << page;
    EXPECT_TRUE(has_line(page, "llmgate_cache_misses_total 1")) << page;  // the refused one never got that far
}

TEST(GatewayMetrics, BudgetRejectedRequestsAreCounted) {
    App app(gateway_config(1000, /*budget=*/10));
    EXPECT_EQ(app.router().dispatch(chat(std::string(400, 'x'))).status, 429);  // ~100 tokens > 10
    const std::string page = get_metrics(app).body;
    EXPECT_TRUE(has_line(page, "llmgate_budget_rejected_total 1")) << page;
    EXPECT_TRUE(has_line(page, "llmgate_rate_limited_total 0")) << page;
}

TEST(GatewayMetrics, TokensAreCountedPerTeamAndNotForCacheHits) {
    App app(gateway_config());
    HttpResponse first = app.router().dispatch(chat("count my tokens"));
    const long long tokens = json::parse(first.body)["usage"]["total_tokens"];
    app.router().dispatch(chat("count my tokens"));  // cache hit: costs nothing

    const std::string page = get_metrics(app).body;
    EXPECT_TRUE(has_line(page, "llmgate_tokens_total{team=\"team-one\"} " + std::to_string(tokens)))
        << page;
}

TEST(GatewayMetrics, ProviderCallsAreTimedPerProvider) {
    App app(gateway_config());
    app.router().dispatch(chat("hi"));
    const std::string page = get_metrics(app).body;
    EXPECT_TRUE(has_line(page, "llmgate_provider_duration_seconds_count{provider=\"mock\"} 1")) << page;
    EXPECT_EQ(page.find("llmgate_provider_errors_total"), std::string::npos) << page;  // none yet
}

TEST(GatewayMetrics, BreakerStateStartsClosed) {
    App app(gateway_config());
    EXPECT_TRUE(has_line(get_metrics(app).body, "llmgate_breaker_state{provider=\"mock\"} 0"));
}

TEST(GatewayMetrics, DeadProviderShowsErrorsAndAnOpenBreaker) {
    Config c = gateway_config();
    c.providers.clear();
    ProviderConfig p;
    p.name = "local";
    p.type = "ollama";
    p.url = "http://127.0.0.1:1";  // nothing listens there
    p.timeout_ms = 500;
    c.providers.push_back(p);
    c.circuit_breaker.failure_threshold = 2;
    App app(c, real_clock(), real_wall_clock(), no_sleep);

    EXPECT_EQ(app.router().dispatch(chat("one")).status, 502);
    EXPECT_EQ(app.router().dispatch(chat("two")).status, 502);
    EXPECT_EQ(app.router().dispatch(chat("three")).status, 502);  // refused by the open breaker

    const std::string page = get_metrics(app).body;
    EXPECT_TRUE(has_line(page, "llmgate_breaker_state{provider=\"local\"} 1")) << page;
    EXPECT_TRUE(has_line(page, "llmgate_provider_errors_total{provider=\"local\"} 2")) << page;
    EXPECT_TRUE(has_line(page, "llmgate_requests_total{status=\"502\"} 3")) << page;
}

TEST(GatewayMetrics, FailoverIsCounted) {
    Config c = gateway_config();
    c.providers.clear();
    ProviderConfig dead;
    dead.name = "local";
    dead.type = "ollama";
    dead.url = "http://127.0.0.1:1";
    dead.timeout_ms = 500;
    dead.priority = 1;
    c.providers.push_back(dead);
    ProviderConfig backup;
    backup.name = "backup";
    backup.type = "mock";
    backup.priority = 2;
    c.providers.push_back(backup);
    App app(c, real_clock(), real_wall_clock(), no_sleep);

    EXPECT_EQ(app.router().dispatch(chat("hello")).status, 200);

    const std::string page = get_metrics(app).body;
    EXPECT_TRUE(has_line(page, "llmgate_failovers_total 1")) << page;
    EXPECT_TRUE(has_line(page, "llmgate_provider_errors_total{provider=\"local\"} 1")) << page;
    EXPECT_TRUE(has_line(page, "llmgate_provider_duration_seconds_count{provider=\"local\"} 1")) << page;
    EXPECT_TRUE(has_line(page, "llmgate_provider_duration_seconds_count{provider=\"backup\"} 1")) << page;
}

TEST(GatewayMetrics, ProviderNamesAreEscapedInLabels) {
    Config c = gateway_config();
    c.providers[0].name = "we\"ird\\name";
    App app(c);
    app.router().dispatch(chat("hi"));
    const std::string page = get_metrics(app).body;
    EXPECT_NE(page.find("provider=\"we\\\"ird\\\\name\""), std::string::npos) << page;
}

TEST(GatewayMetrics, SecretsNeverAppearOnThePage) {
    App app(gateway_config());
    app.router().dispatch(chat("my super secret prompt text"));
    app.router().dispatch(chat("x", "lg_live_wrongkey123"));
    const std::string page = get_metrics(app).body;
    EXPECT_EQ(page.find(kKey), std::string::npos);
    EXPECT_EQ(page.find("lg_live_wrongkey123"), std::string::npos);
    EXPECT_EQ(page.find(sha256_hex(kKey)), std::string::npos);
    EXPECT_EQ(page.find("super secret prompt"), std::string::npos);
}

TEST(GatewayMetrics, ScrapingDuringTrafficIsSafe) {
    App app(gateway_config());
    std::atomic<bool> stop{false};
    std::thread scraper([&] {
        while (!stop) get_metrics(app);
    });
    std::vector<std::thread> clients;
    for (int t = 0; t < 4; ++t) {
        clients.emplace_back([&app, t] {
            for (int i = 0; i < 100; ++i) {
                app.router().dispatch(chat("q" + std::to_string(t) + "-" + std::to_string(i % 7)));
            }
        });
    }
    for (auto& c : clients) c.join();
    stop = true;
    scraper.join();

    const std::string page = get_metrics(app).body;
    EXPECT_TRUE(has_line(page, "llmgate_requests_total{status=\"200\"} 400")) << page;
    EXPECT_TRUE(has_line(page, "llmgate_request_duration_seconds_count 400")) << page;
}
