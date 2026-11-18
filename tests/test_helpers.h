#pragma once
#include <chrono>
#include "llmgate/util/clock.h"

struct FakeClock {
    llmgate::TimePoint now{};
    llmgate::Clock fn() { return [this] { return now; }; }
    void advance_ms(int ms) { now += std::chrono::milliseconds(ms); }
};