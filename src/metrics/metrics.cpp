#include "llmgate/metrics/metrics.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <stdexcept>

namespace llmgate {

namespace {

// Prints a number the way Prometheus likes: no trailing zeros, no exponent
// for ordinary values ("0.35", "2", "0.005").
std::string format_double(double v) {
    if (std::isinf(v)) return v > 0 ? "+Inf" : "-Inf";
    if (std::isnan(v)) return "NaN";
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.10g", v);
    return buffer;
}

// Escapes a label value: backslash, double quote and newline.
std::string escape_label_value(const std::string& value) {
    std::string out;
    for (char c : value) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

// Escapes HELP text: backslash and newline.
std::string escape_help(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

// name{labels} or just name when there are no labels.
std::string series_name(const std::string& name, const std::string& label_text) {
    return label_text.empty() ? name : name + "{" + label_text + "}";
}

// For histogram buckets: name_bucket{labels,le="0.1"}.
std::string bucket_name(const std::string& name, const std::string& label_text,
                        const std::string& le) {
    std::string inside = label_text.empty() ? "" : label_text + ",";
    inside += "le=\"" + le + "\"";
    return name + "_bucket{" + inside + "}";
}

}  // namespace

// ---- Gauge, Histogram ------------------------------------------------------

void Gauge::add(double delta) {
    double current = value_.load(std::memory_order_relaxed);
    while (!value_.compare_exchange_weak(current, current + delta, std::memory_order_relaxed)) {
    }
}

Histogram::Histogram(std::vector<double> bounds)
    : bounds_(std::move(bounds)),
      buckets_(new std::atomic<std::uint64_t>[bounds_.size() + 1]) {
    for (std::size_t i = 0; i <= bounds_.size(); ++i) buckets_[i].store(0);
}

void Histogram::observe(double value) {
    if (std::isnan(value)) return;
    // The first bucket whose bound is >= value ("le" means less than or equal).
    const auto it = std::lower_bound(bounds_.begin(), bounds_.end(), value);
    const std::size_t index = static_cast<std::size_t>(it - bounds_.begin());  // == size() is +Inf
    buckets_[index].fetch_add(1, std::memory_order_relaxed);

    double current = sum_.load(std::memory_order_relaxed);
    while (!sum_.compare_exchange_weak(current, current + value, std::memory_order_relaxed)) {
    }
}

std::vector<std::uint64_t> Histogram::bucket_counts() const {
    std::vector<std::uint64_t> out(bounds_.size() + 1);
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = buckets_[i].load(std::memory_order_relaxed);
    return out;
}

std::uint64_t Histogram::count() const {
    std::uint64_t total = 0;
    for (std::uint64_t c : bucket_counts()) total += c;
    return total;
}

const std::vector<double>& default_duration_bounds() {
    static const std::vector<double> bounds = {0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25,
                                               0.5,   1,     2.5,  5,     10,   30,  60,  120};
    return bounds;
}

// ---- labels ------------------------------------------------------------------

std::string label(const std::string& key, const std::string& value) {
    return key + "=\"" + escape_label_value(value) + "\"";
}

std::string labels(const std::vector<std::pair<std::string, std::string>>& pairs) {
    std::string out;
    for (const auto& p : pairs) {
        if (!out.empty()) out += ",";
        out += label(p.first, p.second);
    }
    return out;
}

// ---- registry ------------------------------------------------------------------

MetricsRegistry::Family& MetricsRegistry::family_locked(const std::string& name,
                                                        const std::string& help, Kind kind) {
    auto it = families_.find(name);
    if (it == families_.end()) {
        Family f;
        f.kind = kind;
        f.help = help;
        it = families_.emplace(name, std::move(f)).first;
    } else if (it->second.kind != kind) {
        throw std::logic_error("metric '" + name + "' was already created as a different kind");
    }
    return it->second;
}

Counter& MetricsRegistry::counter(const std::string& name, const std::string& help,
                                  const std::string& label_text) {
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);  // the common case: it exists already
        auto f = families_.find(name);
        if (f != families_.end() && f->second.kind == Kind::Counter) {
            auto s = f->second.counters.find(label_text);
            if (s != f->second.counters.end()) return *s->second;
        }
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    Family& family = family_locked(name, help, Kind::Counter);
    auto& slot = family.counters[label_text];
    if (!slot) slot = std::make_unique<Counter>();
    return *slot;
}

Gauge& MetricsRegistry::gauge(const std::string& name, const std::string& help,
                              const std::string& label_text) {
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto f = families_.find(name);
        if (f != families_.end() && f->second.kind == Kind::Gauge) {
            auto s = f->second.gauges.find(label_text);
            if (s != f->second.gauges.end()) return *s->second;
        }
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    Family& family = family_locked(name, help, Kind::Gauge);
    auto& slot = family.gauges[label_text];
    if (!slot) slot = std::make_unique<Gauge>();
    return *slot;
}

Histogram& MetricsRegistry::histogram(const std::string& name, const std::string& help,
                                      const std::string& label_text,
                                      const std::vector<double>& bounds) {
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto f = families_.find(name);
        if (f != families_.end() && f->second.kind == Kind::Histogram) {
            auto s = f->second.histograms.find(label_text);
            if (s != f->second.histograms.end()) return *s->second;
        }
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    Family& family = family_locked(name, help, Kind::Histogram);
    auto& slot = family.histograms[label_text];
    if (!slot) slot = std::make_unique<Histogram>(bounds);
    return *slot;
}

std::string MetricsRegistry::render() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::ostringstream out;

    for (const auto& [name, family] : families_) {
        out << "# HELP " << name << " " << escape_help(family.help) << "\n";

        if (family.kind == Kind::Counter) {
            out << "# TYPE " << name << " counter\n";
            for (const auto& [label_text, counter] : family.counters) {
                out << series_name(name, label_text) << " " << counter->value() << "\n";
            }
        } else if (family.kind == Kind::Gauge) {
            out << "# TYPE " << name << " gauge\n";
            for (const auto& [label_text, gauge] : family.gauges) {
                out << series_name(name, label_text) << " " << format_double(gauge->value()) << "\n";
            }
        } else {
            out << "# TYPE " << name << " histogram\n";
            for (const auto& [label_text, histogram] : family.histograms) {
                // Prometheus wants CUMULATIVE buckets: each one includes all the smaller ones.
                const std::vector<std::uint64_t> counts = histogram->bucket_counts();
                std::uint64_t running = 0;
                for (std::size_t i = 0; i < histogram->bounds().size(); ++i) {
                    running += counts[i];
                    out << bucket_name(name, label_text, format_double(histogram->bounds()[i]))
                        << " " << running << "\n";
                }
                running += counts.back();
                out << bucket_name(name, label_text, "+Inf") << " " << running << "\n";
                out << series_name(name + "_sum", label_text) << " "
                    << format_double(histogram->sum()) << "\n";
                out << series_name(name + "_count", label_text) << " " << running << "\n";
            }
        }
    }
    return out.str();
}

}  // namespace llmgate
