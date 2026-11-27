#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "llmgate/app/app.h"
#include "llmgate/util/logger.h"
#include "llmgate/util/sha256.h"

using namespace llmgate;
using json = nlohmann::json;

namespace {

// 2026-11-27T08:41:12Z
WallTime fixed_time(int extra_ms = 0) {
    return WallTime{} + std::chrono::seconds(1795768872) + std::chrono::milliseconds(extra_ms);
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) lines.push_back(line);
    return lines;
}

bool is_request_id(const std::string& id) {
    return id.size() == 16 &&
           std::all_of(id.begin(), id.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

}  // namespace

// ======================= the logger =======================

TEST(Logger, WritesOneJsonObjectPerLine) {
    std::ostringstream out;
    Logger logger(out, LogLevel::Info, [] { return fixed_time(); });
    logger.info("first");
    logger.warn("second", {{"n", 2}});
    logger.error("third");

    const auto lines = split_lines(out.str());
    ASSERT_EQ(lines.size(), 3u);
    for (const auto& line : lines) {
        json parsed = json::parse(line);  // every line parses on its own
        EXPECT_TRUE(parsed.is_object());
    }
}

TEST(Logger, EveryLineHasTimestampLevelAndMessage) {
    std::ostringstream out;
    Logger logger(out, LogLevel::Debug, [] { return fixed_time(345); });
    logger.warn("something happened");
    json line = json::parse(out.str());
    EXPECT_EQ(line["ts"], "2026-11-27T08:41:12.345Z");
    EXPECT_EQ(line["level"], "warn");
    EXPECT_EQ(line["msg"], "something happened");
}

TEST(Logger, FieldsKeepTheirTypes) {
    std::ostringstream out;
    Logger logger(out, LogLevel::Info, [] { return fixed_time(); });
    logger.info("typed", {{"count", 7}, {"ratio", 0.5}, {"flag", true}, {"name", "x"}, {"none", nullptr}});
    json line = json::parse(out.str());
    EXPECT_EQ(line["count"], 7);
    EXPECT_EQ(line["ratio"], 0.5);
    EXPECT_EQ(line["flag"], true);
    EXPECT_EQ(line["name"], "x");
    EXPECT_TRUE(line["none"].is_null());
}

TEST(Logger, TimestampLevelAndMessageComeFirst) {
    std::ostringstream out;
    Logger logger(out, LogLevel::Info, [] { return fixed_time(); });
    logger.info("hello", {{"zebra", 1}, {"apple", 2}});
    const std::string line = out.str();
    EXPECT_EQ(line.rfind("{\"ts\":", 0), 0u) << line;
    EXPECT_LT(line.find("\"level\""), line.find("\"msg\""));
    EXPECT_LT(line.find("\"msg\""), line.find("\"zebra\""));
    EXPECT_LT(line.find("\"zebra\""), line.find("\"apple\""));  // fields keep the order they were added
}

TEST(Logger, FieldsCannotOverwriteTheReservedKeys) {
    std::ostringstream out;
    Logger logger(out, LogLevel::Info, [] { return fixed_time(); });
    logger.info("real message", {{"ts", "fake"}, {"level", "fake"}, {"msg", "fake"}});
    json line = json::parse(out.str());
    EXPECT_EQ(line["msg"], "real message");
    EXPECT_EQ(line["level"], "info");
    EXPECT_NE(line["ts"], "fake");
}

TEST(Logger, NewlinesInValuesDoNotBreakTheLine) {
    std::ostringstream out;
    Logger logger(out, LogLevel::Info, [] { return fixed_time(); });
    logger.info("multi\nline", {{"text", "a\nb\r\nc\t\"quoted\""}});
    const auto lines = split_lines(out.str());
    ASSERT_EQ(lines.size(), 1u);  // an attacker cannot forge a second log line with a newline
    EXPECT_EQ(json::parse(lines[0])["text"], "a\nb\r\nc\t\"quoted\"");
}

TEST(Logger, InvalidUtf8DoesNotThrow) {
    std::ostringstream out;
    Logger logger(out, LogLevel::Info, [] { return fixed_time(); });
    EXPECT_NO_THROW(logger.info("bad bytes", {{"text", std::string("ok\xff\xfe")}}));
    EXPECT_NO_THROW(json::parse(out.str()));
}

TEST(Logger, LevelsBelowTheMinimumAreDropped) {
    std::ostringstream out;
    Logger logger(out, LogLevel::Warn, [] { return fixed_time(); });
    logger.log(LogLevel::Debug, "d");
    logger.info("i");
    logger.warn("w");
    logger.error("e");
    EXPECT_EQ(split_lines(out.str()).size(), 2u);
}

TEST(Logger, OffDropsEverything) {
    std::ostringstream out;
    Logger logger(out, LogLevel::Off, [] { return fixed_time(); });
    logger.error("even errors");
    EXPECT_EQ(out.str(), "");
    EXPECT_FALSE(logger.enabled(LogLevel::Error));
}

TEST(Logger, ParsesLevelNames) {
    LogLevel level = LogLevel::Info;
    EXPECT_TRUE(parse_log_level("debug", level));
    EXPECT_EQ(level, LogLevel::Debug);
    EXPECT_TRUE(parse_log_level("off", level));
    EXPECT_EQ(level, LogLevel::Off);
    EXPECT_FALSE(parse_log_level("verbose", level));
    EXPECT_FALSE(parse_log_level("", level));
    EXPECT_FALSE(parse_log_level("INFO", level));
}

TEST(Logger, ConcurrentLinesNeverInterleave) {
    std::ostringstream out;
    Logger logger(out, LogLevel::Info);
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&logger, t] {
            for (int i = 0; i < 500; ++i) {
                logger.info("event", {{"thread", t}, {"i", i}, {"padding", std::string(200, 'x')}});
            }
        });
    }
    for (auto& th : threads) th.join();
    const auto lines = split_lines(out.str());
    ASSERT_EQ(lines.size(), 4000u);
    for (const auto& line : lines) ASSERT_NO_THROW(json::parse(line)) << line;
}

TEST(Logger, FormatsTimestampsInUtc) {
    EXPECT_EQ(iso8601_utc(WallTime{}), "1970-01-01T00:00:00.000Z");
    EXPECT_EQ(iso8601_utc(fixed_time(7)), "2026-11-27T08:41:12.007Z");
    EXPECT_EQ(iso8601_utc(WallTime{} + std::chrono::hours(24 * 365) + std::chrono::milliseconds(999)),
              "1971-01-01T00:00:00.999Z");
}

// ======================= request ids =======================

TEST(RequestId, IsSixteenLowerCaseHexCharacters) {
    for (int i = 0; i < 100; ++i) EXPECT_TRUE(is_request_id(generate_request_id()));
}

TEST(RequestId, IdsAreUnique) {
    std::set<std::string> seen;
    for (int i = 0; i < 100000; ++i) seen.insert(generate_request_id());
    EXPECT_EQ(seen.size(), 100000u);
}

TEST(RequestId, IdsAreUniqueAcrossThreads) {
    std::vector<std::vector<std::string>> per_thread(8);
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&per_thread, t] {
            for (int i = 0; i < 20000; ++i) per_thread[t].push_back(generate_request_id());
        });
    }
    for (auto& th : threads) th.join();
    std::set<std::string> all;
    for (const auto& ids : per_thread) all.insert(ids.begin(), ids.end());
    EXPECT_EQ(all.size(), 8u * 20000u);
}

// ======================= logging inside the gateway =======================

namespace {

const char* kKey = "lg_live_loggingkey";
const char* kSecretPrompt = "my very secret prompt about quarterly layoffs";

Config gateway_config(double burst = 1000) {
    Config c;
    KeyConfig k;
    k.team = "team-one";
    k.key_sha256 = sha256_hex(kKey);
    k.rate_limit.burst = burst;
    k.rate_limit.per_second = burst >= 1000 ? 1000 : 0.001;
    c.keys.push_back(k);
    ProviderConfig p;
    p.name = "mock";
    p.type = "mock";
    c.providers.push_back(p);
    return c;
}

Config dead_ollama_config() {
    Config c = gateway_config();
    c.providers.clear();
    ProviderConfig p;
    p.name = "local";
    p.type = "ollama";
    p.url = "http://127.0.0.1:1";  // nothing listens there
    p.timeout_ms = 500;
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

void no_sleep(std::chrono::milliseconds) {}

// A gateway that logs into a string.
struct LoggedApp {
    std::ostringstream out;
    Logger logger;
    App app;

    explicit LoggedApp(const Config& config = gateway_config())
        : logger(out), app(config, real_clock(), real_wall_clock(), no_sleep, &logger) {}

    std::vector<json> lines() const {
        std::vector<json> parsed;
        for (const auto& line : split_lines(out.str())) parsed.push_back(json::parse(line));
        return parsed;
    }
    HttpResponse send(const HttpRequest& r) { return app.router().dispatch(r); }
};

}  // namespace

TEST(RequestLog, EveryResponseCarriesARequestId) {
    LoggedApp g(gateway_config(/*burst=*/1));
    HttpRequest bad = chat("x");
    bad.body = "not json";

    std::set<std::string> ids;
    for (const HttpRequest& r : {chat("one"), chat("two"), chat("x", "lg_live_wrong"), bad}) {
        HttpResponse res = g.send(r);
        ASSERT_TRUE(res.headers.count("x-request-id")) << res.status;
        EXPECT_TRUE(is_request_id(res.headers.at("x-request-id")));
        ids.insert(res.headers.at("x-request-id"));
    }
    EXPECT_EQ(ids.size(), 4u);  // 200, 429, 401 and 400 each got their own
}

TEST(RequestLog, RequestIdIsThereEvenWithoutALogger) {
    App app(gateway_config());
    HttpResponse res = app.router().dispatch(chat("hello"));
    EXPECT_TRUE(is_request_id(res.headers.at("x-request-id")));
}

TEST(RequestLog, OneLinePerChatRequestWithTheRightFields) {
    LoggedApp g;
    HttpResponse res = g.send(chat("hello"));
    const auto lines = g.lines();
    ASSERT_EQ(lines.size(), 1u);
    const json& line = lines[0];

    EXPECT_EQ(line["msg"], "request");
    EXPECT_EQ(line["level"], "info");
    EXPECT_EQ(line["request_id"], res.headers.at("x-request-id"));  // the log matches the header
    EXPECT_EQ(line["team"], "team-one");
    EXPECT_EQ(line["route"], "/v1/chat");
    EXPECT_EQ(line["status"], 200);
    EXPECT_TRUE(line["latency_ms"].is_number());
    EXPECT_GE(line["latency_ms"].get<double>(), 0.0);
    EXPECT_EQ(line["cache_hit"], false);
    EXPECT_EQ(line["provider"], "mock");
    EXPECT_TRUE(line["ts"].is_string());
}

TEST(RequestLog, CacheHitsAreMarked) {
    LoggedApp g;
    g.send(chat("same"));
    g.send(chat("same"));
    const auto lines = g.lines();
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0]["cache_hit"], false);
    EXPECT_EQ(lines[1]["cache_hit"], true);
    EXPECT_EQ(lines[1]["provider"], "mock");  // who produced the cached answer
}

TEST(RequestLog, UnknownKeyHasNoTeamAndIsAWarning) {
    LoggedApp g;
    g.send(chat("hi", "lg_live_wrong"));
    const auto lines = g.lines();
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0]["status"], 401);
    EXPECT_EQ(lines[0]["level"], "warn");
    EXPECT_TRUE(lines[0]["team"].is_null());
    EXPECT_TRUE(lines[0]["provider"].is_null());
}

TEST(RequestLog, RateLimitedRequestsAreLoggedWithTheirTeam) {
    LoggedApp g(gateway_config(/*burst=*/1));
    g.send(chat("a"));
    g.send(chat("b"));
    const auto lines = g.lines();
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[1]["status"], 429);
    EXPECT_EQ(lines[1]["level"], "warn");
    EXPECT_EQ(lines[1]["team"], "team-one");
}

TEST(RequestLog, ServerSideFailuresAreErrors) {
    LoggedApp g(dead_ollama_config());
    EXPECT_EQ(g.send(chat("hi")).status, 502);
    json request_line;
    for (const auto& line : g.lines()) {
        if (line["msg"] == "request") request_line = line;
    }
    EXPECT_EQ(request_line["status"], 502);
    EXPECT_EQ(request_line["level"], "error");
}

TEST(RequestLog, NeverContainsKeysOrPrompts) {
    LoggedApp g;
    g.send(chat(kSecretPrompt));
    g.send(chat(kSecretPrompt));                          // cached
    g.send(chat(kSecretPrompt, "lg_live_wrongkey999"));   // rejected
    HttpRequest bad = chat(kSecretPrompt);
    bad.body = std::string("{\"model\":\"m\",\"messages\":\"") + kSecretPrompt;  // broken JSON
    g.send(bad);

    const std::string all = g.out.str();
    EXPECT_FALSE(all.empty());
    EXPECT_EQ(all.find(kKey), std::string::npos);
    EXPECT_EQ(all.find("lg_live_wrongkey999"), std::string::npos);
    EXPECT_EQ(all.find(sha256_hex(kKey)), std::string::npos);
    EXPECT_EQ(all.find("secret prompt"), std::string::npos);
    EXPECT_EQ(all.find("layoffs"), std::string::npos);
    EXPECT_EQ(all.find("Bearer"), std::string::npos);
}

TEST(RequestLog, FailedProviderCallsAreLoggedWithoutTheirMessage) {
    Config c = dead_ollama_config();
    ProviderConfig backup;
    backup.name = "backup";
    backup.type = "mock";
    backup.priority = 2;
    c.providers.push_back(backup);
    LoggedApp g(c);

    EXPECT_EQ(g.send(chat("hello")).status, 200);  // failed over to the backup

    bool saw_failure = false;
    for (const auto& line : g.lines()) {
        if (line["msg"] == "provider_failed") {
            saw_failure = true;
            EXPECT_EQ(line["provider"], "local");
            EXPECT_EQ(line["level"], "warn");
            EXPECT_EQ(line["transient"], true);
            EXPECT_EQ(line["timeout"], false);
        }
    }
    EXPECT_TRUE(saw_failure);
    EXPECT_EQ(g.out.str().find("127.0.0.1"), std::string::npos);  // no provider text or URLs
}

TEST(RequestLog, HealthAndMetricsChecksAreNotLogged) {
    LoggedApp g;
    for (const char* path : {"/health", "/metrics"}) {
        HttpRequest r;
        r.method = "GET";
        r.path = path;
        g.send(r);
    }
    EXPECT_EQ(g.out.str(), "");
}

TEST(RequestLog, ManyThreadsProduceOnlyValidLines) {
    LoggedApp g;
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&g, t] {
            for (int i = 0; i < 100; ++i) g.send(chat("q" + std::to_string(t) + "-" + std::to_string(i)));
        });
    }
    for (auto& th : threads) th.join();
    const auto lines = split_lines(g.out.str());
    ASSERT_EQ(lines.size(), 400u);
    std::set<std::string> ids;
    for (const auto& line : lines) ids.insert(json::parse(line)["request_id"].get<std::string>());
    EXPECT_EQ(ids.size(), 400u);
}
