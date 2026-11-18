#include <gtest/gtest.h>

#include <cctype>
#include <string>

#include "llmgate/config/config.h"

using llmgate::Config;
using llmgate::ConfigError;
using llmgate::load_config_from_string;

namespace {

const char* kHash = "456acec14fd9a742e5306de8bc0aaaeaf57d506f7d0bf46b44326cfbbe6fcea0";

// Smallest valid config; tests tweak it by string replacement.
std::string minimal(const std::string& extra_top = "") {
    return std::string(R"({
  )") + extra_top + R"(
  "providers": [ { "name": "mock", "type": "mock" } ],
  "keys": [ { "team": "t", "key_sha256": ")" + kHash + R"(" } ]
})";
}

std::string error_of(const std::string& text) {
    try {
        load_config_from_string(text);
    } catch (const ConfigError& e) {
        return e.what();
    }
    return "";
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST(Config, ParsesExampleConfig) {
    Config c = llmgate::load_config_from_file(std::string(LLMGATE_SOURCE_DIR) +
                                              "/config/llmgate.example.json");
    EXPECT_EQ(c.listen_host, "0.0.0.0");
    EXPECT_EQ(c.listen_port, 8080);
    EXPECT_EQ(c.max_body_bytes, 1048576u);
    EXPECT_EQ(c.cache.capacity, 10000u);
    EXPECT_EQ(c.cache.ttl_seconds, 600);
    EXPECT_EQ(c.circuit_breaker.failure_threshold, 5);
    EXPECT_EQ(c.circuit_breaker.cool_down_seconds, 20);
    ASSERT_EQ(c.providers.size(), 2u);
    EXPECT_EQ(c.providers[0].name, "ollama");
    EXPECT_EQ(c.providers[0].url, "http://localhost:11434");
    EXPECT_EQ(c.providers[1].type, "mock");
    ASSERT_EQ(c.keys.size(), 1u);
    EXPECT_EQ(c.keys[0].team, "claims-team");
    EXPECT_DOUBLE_EQ(c.keys[0].rate_limit.burst, 20);
    EXPECT_DOUBLE_EQ(c.keys[0].rate_limit.per_second, 5);
    EXPECT_EQ(c.keys[0].daily_token_budget, 200000);
}

TEST(Config, UsesDefaultsForOptionalFields) {
    Config c = load_config_from_string(minimal());
    EXPECT_EQ(c.listen_host, "0.0.0.0");
    EXPECT_EQ(c.listen_port, 8080);
    EXPECT_EQ(c.max_body_bytes, 1048576u);
    EXPECT_EQ(c.cache.capacity, 10000u);
    EXPECT_EQ(c.cache.ttl_seconds, 600);
    EXPECT_EQ(c.circuit_breaker.failure_threshold, 5);
    EXPECT_EQ(c.providers[0].timeout_ms, 30000);
    EXPECT_EQ(c.providers[0].priority, 1);
    EXPECT_DOUBLE_EQ(c.keys[0].rate_limit.burst, 10);
    EXPECT_EQ(c.keys[0].daily_token_budget, 100000);
}

TEST(Config, MissingKeysArrayGivesClearError) {
    const std::string text = R"({ "providers": [ { "name": "m", "type": "mock" } ] })";
    EXPECT_TRUE(contains(error_of(text), "missing required field \"keys\""));
}

TEST(Config, MissingProvidersGivesClearError) {
    const std::string text = std::string(R"({ "keys": [ { "team": "t", "key_sha256": ")") + kHash + R"(" } ] })";
    EXPECT_TRUE(contains(error_of(text), "missing required field \"providers\""));
}

TEST(Config, BadJsonGivesClearError) {
    const std::string err = error_of("{ not json");
    EXPECT_TRUE(contains(err, "config: invalid JSON"));
}

TEST(Config, TopLevelMustBeObject) {
    EXPECT_TRUE(contains(error_of("[1,2]"), "top level must be a JSON object"));
}

TEST(Config, NegativeBudgetRejected) {
    std::string text = minimal();
    text.replace(text.find("\"key_sha256\""), 0, "\"daily_token_budget\": -5, ");
    EXPECT_TRUE(contains(error_of(text), "keys[0].daily_token_budget must be greater than 0"));
}

TEST(Config, ZeroRateLimitRejected) {
    std::string text = minimal();
    text.replace(text.find("\"key_sha256\""), 0, "\"rate_limit\": {\"burst\": 0}, ");
    EXPECT_TRUE(contains(error_of(text), "rate_limit.burst must be greater than 0"));
}

TEST(Config, UnknownFieldRejectedAsTypo) {
    const std::string err = error_of(minimal("\"max_body_byts\": 5,"));
    EXPECT_TRUE(contains(err, "unknown field \"max_body_byts\""));
}

TEST(Config, UnknownNestedFieldRejected) {
    const std::string err = error_of(minimal("\"cache\": {\"capacty\": 5},"));
    EXPECT_TRUE(contains(err, "unknown field \"cache.capacty\""));
}

TEST(Config, WrongTypeNamesTheField) {
    const std::string err = error_of(minimal("\"max_body_bytes\": \"big\","));
    EXPECT_TRUE(contains(err, "max_body_bytes must be a whole number"));
}

TEST(Config, FractionIsNotAWholeNumber) {
    EXPECT_TRUE(contains(error_of(minimal("\"cache\": {\"ttl_seconds\": 1.5},")),
                         "cache.ttl_seconds must be a whole number"));
}

TEST(Config, ListenParsesHostAndPort) {
    Config c = load_config_from_string(minimal("\"listen\": \"127.0.0.1:9090\","));
    EXPECT_EQ(c.listen_host, "127.0.0.1");
    EXPECT_EQ(c.listen_port, 9090);
}

TEST(Config, ListenRejectsBadValues) {
    for (const char* bad : {"8080", ":8080", "host:", "host:0", "host:70000", "host:abc", "host:-1"}) {
        const std::string err = error_of(minimal(std::string("\"listen\": \"") + bad + "\","));
        EXPECT_TRUE(contains(err, "listen must look like")) << bad;
    }
}

TEST(Config, EmptyProvidersRejected) {
    std::string text = std::string(R"({ "providers": [], "keys": [ { "team": "t", "key_sha256": ")") + kHash + R"(" } ] })";
    EXPECT_TRUE(contains(error_of(text), "providers must contain at least one"));
}

TEST(Config, EmptyKeysRejected) {
    const std::string text = R"({ "providers": [ { "name": "m", "type": "mock" } ], "keys": [] })";
    EXPECT_TRUE(contains(error_of(text), "keys must contain at least one"));
}

TEST(Config, UnknownProviderTypeRejected) {
    const std::string text = std::string(R"({ "providers": [ { "name": "m", "type": "openai" } ], "keys": [ { "team": "t", "key_sha256": ")") + kHash + R"(" } ] })";
    EXPECT_TRUE(contains(error_of(text), "providers[0].type must be \"mock\" or \"ollama\""));
}

TEST(Config, OllamaRequiresUrl) {
    const std::string text = std::string(R"({ "providers": [ { "name": "o", "type": "ollama" } ], "keys": [ { "team": "t", "key_sha256": ")") + kHash + R"(" } ] })";
    EXPECT_TRUE(contains(error_of(text), "providers[0].url is required for an ollama provider"));
}

TEST(Config, DuplicateProviderNameRejected) {
    const std::string text = std::string(R"({ "providers": [ { "name": "m", "type": "mock" }, { "name": "m", "type": "mock" } ], "keys": [ { "team": "t", "key_sha256": ")") + kHash + R"(" } ] })";
    EXPECT_TRUE(contains(error_of(text), "duplicate provider name \"m\""));
}

TEST(Config, BadKeyHashRejected) {
    for (const char* bad : {"abc", "zz6acec14fd9a742e5306de8bc0aaaeaf57d506f7d0bf46b44326cfbbe6fcea0"}) {
        const std::string text = std::string(R"({ "providers": [ { "name": "m", "type": "mock" } ], "keys": [ { "team": "t", "key_sha256": ")") + bad + R"(" } ] })";
        EXPECT_TRUE(contains(error_of(text), "keys[0].key_sha256 must be exactly 64 hexadecimal")) << bad;
    }
}

TEST(Config, DuplicateKeyHashRejected) {
    const std::string k = std::string(R"({ "team": "t", "key_sha256": ")") + kHash + R"(" })";
    const std::string text = R"({ "providers": [ { "name": "m", "type": "mock" } ], "keys": [ )" + k + ", " + k + " ] }";
    EXPECT_TRUE(contains(error_of(text), "keys[1].key_sha256 is a duplicate"));
}

TEST(Config, KeyHashIsLowercased) {
    std::string upper = kHash;
    for (auto& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    const std::string text = std::string(R"({ "providers": [ { "name": "m", "type": "mock" } ], "keys": [ { "team": "t", "key_sha256": ")") + upper + R"(" } ] })";
    EXPECT_EQ(load_config_from_string(text).keys[0].key_sha256, kHash);
}

TEST(Config, SecondEntryErrorsUseItsIndex) {
    const std::string text = std::string(R"({ "providers": [ { "name": "a", "type": "mock" }, { "name": "b", "type": "nope" } ], "keys": [ { "team": "t", "key_sha256": ")") + kHash + R"(" } ] })";
    EXPECT_TRUE(contains(error_of(text), "providers[1].type"));
}

TEST(Config, MissingFileGivesClearError) {
    try {
        llmgate::load_config_from_file("/definitely/not/here.json");
        FAIL() << "expected ConfigError";
    } catch (const ConfigError& e) {
        EXPECT_TRUE(contains(e.what(), "cannot open file"));
    }
}

TEST(Config, ParsePort) {
    int p = 0;
    EXPECT_TRUE(llmgate::parse_port("1", p));
    EXPECT_EQ(p, 1);
    EXPECT_TRUE(llmgate::parse_port("65535", p));
    EXPECT_EQ(p, 65535);
    for (const char* bad : {"", "0", "65536", "-1", "80a", " 80", "123456", "8.0"}) {
        EXPECT_FALSE(llmgate::parse_port(bad, p)) << bad;
    }
}

TEST(Config, DescribeNeverLeaksSecrets) {
    Config c = llmgate::load_config_from_file(std::string(LLMGATE_SOURCE_DIR) +
                                              "/config/llmgate.example.json");
    const std::string text = llmgate::describe(c);
    EXPECT_TRUE(contains(text, "claims-team"));
    EXPECT_FALSE(contains(text, kHash));
    EXPECT_FALSE(contains(text, "localhost:11434"));
}