#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

namespace llmgate {

// A number that only goes up (requests served, cache hits, ...).
// inc() is lock-free, so any thread can call it without slowing the others.
class Counter {
public:
    void inc(std::uint64_t n = 1) { value_.fetch_add(n, std::memory_order_relaxed); }
    std::uint64_t value() const { return value_.load(std::memory_order_relaxed); }

private:
    std::atomic<std::uint64_t> value_{0};
};

// A number that can go up and down (the state of a circuit breaker, ...).
class Gauge {
public:
    void set(double v) { value_.store(v, std::memory_order_relaxed); }
    void add(double delta);
    double value() const { return value_.load(std::memory_order_relaxed); }

private:
    std::atomic<double> value_{0.0};
};

// Counts observations (for example request durations in seconds) in fixed
// buckets. Bucket "le=0.1" means "observations that were <= 0.1".
// observe() is lock-free.
class Histogram {
public:
    // `bounds` must be sorted ascending. There is always an extra "+Inf" bucket.
    explicit Histogram(std::vector<double> bounds);

    void observe(double value);

    const std::vector<double>& bounds() const { return bounds_; }

    // How many observations fell in each bucket, NOT cumulative:
    // bounds().size() + 1 numbers, the last one is the "+Inf" bucket.
    std::vector<std::uint64_t> bucket_counts() const;

    std::uint64_t count() const;  // all observations
    double sum() const { return sum_.load(std::memory_order_relaxed); }

private:
    std::vector<double> bounds_;
    std::unique_ptr<std::atomic<std::uint64_t>[]> buckets_;
    std::atomic<double> sum_{0.0};
};

// Bucket bounds for durations in seconds, from 1 ms to 2 minutes.
const std::vector<double>& default_duration_bounds();

// Builds the text of one label, escaped as Prometheus requires:
// label("status", "200") gives   status="200"
// Quotes, backslashes and newlines in the value are escaped.
std::string label(const std::string& key, const std::string& value);

// Several labels joined with commas: labels({{"a","1"},{"b","2"}}) gives a="1",b="2"
std::string labels(const std::vector<std::pair<std::string, std::string>>& pairs);

// Holds every metric and renders them in the Prometheus text format
// (version 0.0.4), the format a Prometheus server reads from GET /metrics.
//
// counter()/gauge()/histogram() find the series with this name and label
// text, creating it the first time. The returned reference stays valid for
// the life of the registry, so hot code can keep it and call inc() directly.
// Only the lookup takes a lock (shared for finding, exclusive for creating);
// updating a metric never does.
//
// Use the SAME `help` text each time for a name. Asking for a name as a
// different kind (counter vs gauge) than before throws std::logic_error.
class MetricsRegistry {
public:
    Counter& counter(const std::string& name, const std::string& help,
                     const std::string& label_text = "");
    Gauge& gauge(const std::string& name, const std::string& help,
                 const std::string& label_text = "");
    Histogram& histogram(const std::string& name, const std::string& help,
                         const std::string& label_text = "",
                         const std::vector<double>& bounds = default_duration_bounds());

    // The whole page. Metrics are sorted by name and series by label text,
    // so the output is always in the same order.
    std::string render() const;

private:
    enum class Kind { Counter, Gauge, Histogram };

    struct Family {
        Kind kind = Kind::Counter;
        std::string help;
        std::map<std::string, std::unique_ptr<Counter>> counters;
        std::map<std::string, std::unique_ptr<Gauge>> gauges;
        std::map<std::string, std::unique_ptr<Histogram>> histograms;
    };

    Family& family_locked(const std::string& name, const std::string& help, Kind kind);

    mutable std::shared_mutex mutex_;
    std::map<std::string, Family> families_;
};

}  // namespace llmgate
