#pragma once

#include <mutex>
#include <nlohmann/json.hpp>
#include <ostream>
#include <string>

#include "llmgate/budget/budget_tracker.h"  // WallClock: logs carry calendar time

namespace llmgate {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3, Off = 4 };

const char* to_string(LogLevel level);                   // "debug", "info", ...
bool parse_log_level(const std::string& text, LogLevel& out);  // false if not a level name

// The fields of a log line, in the order they were added.
using LogFields = nlohmann::ordered_json;

// Writes one JSON object per line ("JSON lines"), for example:
//
//   {"ts":"2026-11-27T08:41:12.345Z","level":"info","msg":"request","request_id":"9f3a...", ...}
//
// Machines can read this: every line parses on its own, so tools such as jq,
// Loki or Elasticsearch can filter and count without guessing at the text.
//
// A mutex makes each line a single write, so lines from different threads
// never mix. A line is never longer than its content: newlines inside values
// are escaped by the JSON encoder.
//
// SECURITY RULE: callers must never put API keys, key hashes or prompt text in
// the fields. The request logger only ever passes ids, team names, numbers and
// provider names.
class Logger {
public:
    Logger(std::ostream& out, LogLevel min_level = LogLevel::Info,
           WallClock clock = real_wall_clock());

    void log(LogLevel level, const std::string& message,
             const LogFields& fields = LogFields::object());

    void info(const std::string& message, const LogFields& fields = LogFields::object()) {
        log(LogLevel::Info, message, fields);
    }
    void warn(const std::string& message, const LogFields& fields = LogFields::object()) {
        log(LogLevel::Warn, message, fields);
    }
    void error(const std::string& message, const LogFields& fields = LogFields::object()) {
        log(LogLevel::Error, message, fields);
    }

    bool enabled(LogLevel level) const { return level >= min_level_ && min_level_ != LogLevel::Off; }

private:
    std::ostream& out_;
    LogLevel min_level_;
    WallClock clock_;
    std::mutex mutex_;
};

// "2026-11-27T08:41:12.345Z" (UTC, millisecond precision).
std::string iso8601_utc(WallTime time);

// 16 random lower-case hex characters, different for every call (and thread-safe).
// Used to follow one request through the logs and the x-request-id header.
std::string generate_request_id();

}  // namespace llmgate
