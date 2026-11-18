#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace llmgate {

// Thrown for any problem with the configuration. The message always starts
// with "config:" and names the exact field that is wrong.
class ConfigError : public std::runtime_error {
public:
    explicit ConfigError(const std::string& message) : std::runtime_error(message) {}
};

struct RateLimitConfig {
    double burst = 10;       // bucket size (largest burst)
    double per_second = 5;   // refill rate
};

struct KeyConfig {
    std::string team;
    std::string key_sha256;  // 64 lowercase hex characters; never the raw key
    RateLimitConfig rate_limit;
    long long daily_token_budget = 100000;
};

struct ProviderConfig {
    std::string name;
    std::string type;        // "mock" or "ollama"
    std::string url;         // required for "ollama"
    int timeout_ms = 30000;
    int priority = 1;        // lower number = tried first
};

struct CacheConfig {
    std::size_t capacity = 10000;
    int ttl_seconds = 600;
};

struct BreakerConfig {
    int failure_threshold = 5;
    int cool_down_seconds = 20;
};

struct Config {
    std::string listen_host = "0.0.0.0";
    int listen_port = 8080;
    std::size_t max_body_bytes = 1048576;
    CacheConfig cache;
    BreakerConfig circuit_breaker;
    std::vector<ProviderConfig> providers;
    std::vector<KeyConfig> keys;
};

// Parse and validate a JSON document. Throws ConfigError on any problem.
Config load_config_from_string(const std::string& json_text);

// Read a file and parse it. Throws ConfigError (also if the file is missing).
Config load_config_from_file(const std::string& path);

// Parses "8080" style text. Returns false unless it is a whole number 1-65535.
bool parse_port(const std::string& text, int& out);

// A short human-readable summary for the startup log. It deliberately leaves
// out key hashes and provider URLs so secrets never reach the logs.
std::string describe(const Config& config);

}  // namespace llmgate