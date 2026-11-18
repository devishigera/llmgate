#include "llmgate/config/config.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <climits>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <set>
#include <sstream>

namespace llmgate {

namespace {

using json = nlohmann::json;

[[noreturn]] void fail(const std::string& message) {
    throw ConfigError("config: " + message);
}

std::string join(const std::string& base, const std::string& key) {
    return base.empty() ? key : base + "." + key;
}

// ---- typed readers: each one checks the type and names the field on error --

void expect_object(const json& v, const std::string& path) {
    if (!v.is_object()) fail(path + " must be an object");
}

void expect_array(const json& v, const std::string& path) {
    if (!v.is_array()) fail(path + " must be an array");
}

std::string as_string(const json& v, const std::string& path) {
    if (!v.is_string()) fail(path + " must be a string");
    return v.get<std::string>();
}

long long as_int(const json& v, const std::string& path) {
    if (!v.is_number_integer()) fail(path + " must be a whole number");
    if (v.is_number_unsigned() &&
        v.get<unsigned long long>() > static_cast<unsigned long long>(LLONG_MAX)) {
        fail(path + " is too large");
    }
    return v.get<long long>();
}

double as_number(const json& v, const std::string& path) {
    if (!v.is_number()) fail(path + " must be a number");
    return v.get<double>();
}

// Optional field: returns nullptr when the key is absent.
const json* find(const json& obj, const std::string& key) {
    auto it = obj.find(key);
    return it == obj.end() ? nullptr : &*it;
}

// Catches typos: any key that is not in `allowed` is an error.
void reject_unknown(const json& obj, std::initializer_list<const char*> allowed,
                    const std::string& path) {
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        bool known = false;
        for (const char* name : allowed) {
            if (it.key() == name) known = true;
        }
        if (!known) {
            fail("unknown field \"" + join(path, it.key()) + "\" (check the spelling)");
        }
    }
}

const json& require(const json& obj, const std::string& key, const std::string& base) {
    const json* v = find(obj, key);
    if (v == nullptr) fail("missing required field \"" + join(base, key) + "\"");
    return *v;
}

long long int_in_range(const json& v, const std::string& path, long long lo, long long hi) {
    const long long n = as_int(v, path);
    if (n < lo || n > hi) {
        fail(path + " must be between " + std::to_string(lo) + " and " + std::to_string(hi));
    }
    return n;
}

// ---- section parsers --------------------------------------------------------

void parse_listen(const std::string& text, Config& cfg) {
    const auto pos = text.rfind(':');
    int port = 0;
    if (pos == std::string::npos || pos == 0 || pos + 1 >= text.size() ||
        !parse_port(text.substr(pos + 1), port)) {
        fail("listen must look like \"host:port\" with a port from 1 to 65535 "
             "(for example \"0.0.0.0:8080\")");
    }
    cfg.listen_host = text.substr(0, pos);
    cfg.listen_port = port;
}

void parse_cache(const json& v, Config& cfg) {
    expect_object(v, "cache");
    const json& obj = v;
    reject_unknown(obj, {"capacity", "ttl_seconds"}, "cache");
    if (const json* x = find(obj, "capacity")) {
        cfg.cache.capacity = static_cast<std::size_t>(
            int_in_range(*x, "cache.capacity", 1, 100000000));
    }
    if (const json* x = find(obj, "ttl_seconds")) {
        cfg.cache.ttl_seconds = static_cast<int>(
            int_in_range(*x, "cache.ttl_seconds", 1, 31536000));
    }
}

void parse_breaker(const json& v, Config& cfg) {
    expect_object(v, "circuit_breaker");
    const json& obj = v;
    reject_unknown(obj, {"failure_threshold", "cool_down_seconds"}, "circuit_breaker");
    if (const json* x = find(obj, "failure_threshold")) {
        cfg.circuit_breaker.failure_threshold = static_cast<int>(
            int_in_range(*x, "circuit_breaker.failure_threshold", 1, 1000000));
    }
    if (const json* x = find(obj, "cool_down_seconds")) {
        cfg.circuit_breaker.cool_down_seconds = static_cast<int>(
            int_in_range(*x, "circuit_breaker.cool_down_seconds", 1, 86400));
    }
}

void parse_providers(const json& v, Config& cfg) {
    expect_array(v, "providers");
    const json& arr = v;
    if (arr.empty()) fail("providers must contain at least one provider");

    std::set<std::string> names;
    for (std::size_t i = 0; i < arr.size(); ++i) {
        const std::string base = "providers[" + std::to_string(i) + "]";
        expect_object(arr[i], base);
        const json& obj = arr[i];
        reject_unknown(obj, {"name", "type", "url", "timeout_ms", "priority"}, base);

        ProviderConfig p;
        p.name = as_string(require(obj, "name", base), join(base, "name"));
        if (p.name.empty()) fail(join(base, "name") + " must not be empty");
        if (!names.insert(p.name).second) {
            fail("duplicate provider name \"" + p.name + "\"");
        }

        p.type = as_string(require(obj, "type", base), join(base, "type"));
        if (p.type != "mock" && p.type != "ollama") {
            fail(join(base, "type") + " must be \"mock\" or \"ollama\", got \"" + p.type + "\"");
        }

        if (const json* x = find(obj, "url")) p.url = as_string(*x, join(base, "url"));
        if (p.type == "ollama" && p.url.empty()) {
            fail(join(base, "url") + " is required for an ollama provider");
        }
        if (const json* x = find(obj, "timeout_ms")) {
            p.timeout_ms = static_cast<int>(int_in_range(*x, join(base, "timeout_ms"), 1, 600000));
        }
        if (const json* x = find(obj, "priority")) {
            p.priority = static_cast<int>(int_in_range(*x, join(base, "priority"), -1000000, 1000000));
        }
        cfg.providers.push_back(std::move(p));
    }
}

bool is_sha256_hex(const std::string& s) {
    return s.size() == 64 &&
           std::all_of(s.begin(), s.end(),
                       [](unsigned char c) { return std::isxdigit(c) != 0; });
}

void parse_keys(const json& v, Config& cfg) {
    expect_array(v, "keys");
    const json& arr = v;
    if (arr.empty()) fail("keys must contain at least one key");

    std::set<std::string> hashes;
    for (std::size_t i = 0; i < arr.size(); ++i) {
        const std::string base = "keys[" + std::to_string(i) + "]";
        expect_object(arr[i], base);
        const json& obj = arr[i];
        reject_unknown(obj, {"team", "key_sha256", "rate_limit", "daily_token_budget"}, base);

        KeyConfig k;
        k.team = as_string(require(obj, "team", base), join(base, "team"));
        if (k.team.empty()) fail(join(base, "team") + " must not be empty");

        k.key_sha256 = as_string(require(obj, "key_sha256", base), join(base, "key_sha256"));
        if (!is_sha256_hex(k.key_sha256)) {
            fail(join(base, "key_sha256") + " must be exactly 64 hexadecimal characters "
                 "(the SHA-256 of the key, never the key itself)");
        }
        std::transform(k.key_sha256.begin(), k.key_sha256.end(), k.key_sha256.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!hashes.insert(k.key_sha256).second) {
            fail(join(base, "key_sha256") + " is a duplicate of an earlier key");
        }

        if (const json* x = find(obj, "rate_limit")) {
            const std::string rl = join(base, "rate_limit");
            expect_object(*x, rl);
            const json& rlobj = *x;
            reject_unknown(rlobj, {"burst", "per_second"}, rl);
            if (const json* b = find(rlobj, "burst")) {
                k.rate_limit.burst = as_number(*b, join(rl, "burst"));
            }
            if (const json* p = find(rlobj, "per_second")) {
                k.rate_limit.per_second = as_number(*p, join(rl, "per_second"));
            }
        }
        if (!(k.rate_limit.burst > 0)) fail(join(base, "rate_limit.burst") + " must be greater than 0");
        if (!(k.rate_limit.per_second > 0)) fail(join(base, "rate_limit.per_second") + " must be greater than 0");

        if (const json* x = find(obj, "daily_token_budget")) {
            k.daily_token_budget = as_int(*x, join(base, "daily_token_budget"));
        }
        if (k.daily_token_budget <= 0) fail(join(base, "daily_token_budget") + " must be greater than 0");

        cfg.keys.push_back(std::move(k));
    }
}

Config parse_config(const json& root) {
    if (!root.is_object()) fail("the top level must be a JSON object { ... }");
    reject_unknown(root, {"listen", "max_body_bytes", "cache", "providers",
                          "circuit_breaker", "keys"}, "");

    Config cfg;
    if (const json* x = find(root, "listen")) parse_listen(as_string(*x, "listen"), cfg);
    if (const json* x = find(root, "max_body_bytes")) {
        cfg.max_body_bytes = static_cast<std::size_t>(
            int_in_range(*x, "max_body_bytes", 1, 1073741824));
    }
    if (const json* x = find(root, "cache")) parse_cache(*x, cfg);
    if (const json* x = find(root, "circuit_breaker")) parse_breaker(*x, cfg);
        parse_providers(require(root, "providers", ""), cfg);
    parse_keys(require(root, "keys", ""), cfg);
    return cfg;
}

}  // namespace

bool parse_port(const std::string& text, int& out) {
    if (text.empty() || text.size() > 5) return false;
    for (unsigned char c : text) {
        if (!std::isdigit(c)) return false;
    }
    const int value = std::stoi(text);
    if (value < 1 || value > 65535) return false;
    out = value;
    return true;
}

Config load_config_from_string(const std::string& json_text) {
    json root;
    try {
        root = json::parse(json_text);
    } catch (const json::parse_error& e) {
        throw ConfigError(std::string("config: invalid JSON: ") + e.what());
    }
    return parse_config(root);
}

Config load_config_from_file(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) fail("cannot open file \"" + path + "\"");
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    return load_config_from_string(text);
}

std::string describe(const Config& c) {
    std::ostringstream out;
    out << "config: listen " << c.listen_host << ":" << c.listen_port << "\n";
    out << "config: max_body_bytes " << c.max_body_bytes << "\n";
    out << "config: cache capacity=" << c.cache.capacity << " ttl=" << c.cache.ttl_seconds << "s\n";
    out << "config: circuit_breaker threshold=" << c.circuit_breaker.failure_threshold
        << " cool_down=" << c.circuit_breaker.cool_down_seconds << "s\n";
    for (const auto& p : c.providers) {
        out << "config: provider " << p.name << " type=" << p.type
            << " priority=" << p.priority << " timeout=" << p.timeout_ms << "ms\n";
    }
    for (const auto& k : c.keys) {
        out << "config: key team=" << k.team << " burst=" << k.rate_limit.burst
            << " per_second=" << k.rate_limit.per_second
            << " daily_budget=" << k.daily_token_budget << "\n";
    }
    return out.str();
}

}  // namespace llmgate