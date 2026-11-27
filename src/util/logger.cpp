#include "llmgate/util/logger.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <random>

namespace llmgate {

const char* to_string(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "debug";
        case LogLevel::Info: return "info";
        case LogLevel::Warn: return "warn";
        case LogLevel::Error: return "error";
        case LogLevel::Off: return "off";
    }
    return "info";
}

bool parse_log_level(const std::string& text, LogLevel& out) {
    for (LogLevel level : {LogLevel::Debug, LogLevel::Info, LogLevel::Warn, LogLevel::Error,
                           LogLevel::Off}) {
        if (text == to_string(level)) {
            out = level;
            return true;
        }
    }
    return false;
}

std::string iso8601_utc(WallTime time) {
    using namespace std::chrono;
    const long long total_ms = duration_cast<milliseconds>(time.time_since_epoch()).count();

    // Floor division, so times before 1970 also round the right way.
    long long seconds = total_ms / 1000;
    long long ms = total_ms % 1000;
    if (ms < 0) {
        ms += 1000;
        --seconds;
    }

    const std::time_t t = static_cast<std::time_t>(seconds);
    std::tm utc{};
    gmtime_r(&t, &utc);

    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min,
                  utc.tm_sec, static_cast<int>(ms % 1000));
    return buffer;
}

Logger::Logger(std::ostream& out, LogLevel min_level, WallClock clock)
    : out_(out), min_level_(min_level), clock_(std::move(clock)) {}

void Logger::log(LogLevel level, const std::string& message, const LogFields& fields) {
    if (!enabled(level)) return;

    LogFields line = LogFields::object();
    line["ts"] = iso8601_utc(clock_());
    line["level"] = to_string(level);
    line["msg"] = message;
    if (fields.is_object()) {
        for (auto it = fields.begin(); it != fields.end(); ++it) {
            const std::string& key = it.key();
            if (key == "ts" || key == "level" || key == "msg") continue;  // reserved
            line[key] = it.value();
        }
    }

    // "replace" turns invalid UTF-8 into U+FFFD instead of throwing: logging must never crash a request.
    const std::string text = line.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);

    std::lock_guard<std::mutex> lock(mutex_);
    out_ << text << '\n';
    out_.flush();
}

std::string generate_request_id() {
    // One engine per thread: no locking. Each is seeded from the operating
    // system's randomness, mixed with a counter so two threads never share a seed.
    static std::atomic<std::uint64_t> counter{0};
    thread_local std::mt19937_64 engine = [] {
        std::random_device device;
        const std::uint64_t seed = (static_cast<std::uint64_t>(device()) << 32) ^ device() ^
                                   (counter.fetch_add(1) * 0x9E3779B97F4A7C15ULL);
        return std::mt19937_64(seed);
    }();

    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llx",
                  static_cast<unsigned long long>(engine()));
    return buffer;
}

}  // namespace llmgate
