#pragma once
#include <chrono>
#include <functional>

namespace llmgate {
using TimePoint = std::chrono::steady_clock::time_point;
using Clock = std::function<TimePoint()>;

inline Clock real_clock() {
    return [] { return std::chrono::steady_clock::now(); };
}
}  // namespace llmgate