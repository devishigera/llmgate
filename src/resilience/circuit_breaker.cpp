#include "llmgate/resilience/circuit_breaker.h"

#include <stdexcept>

namespace llmgate {

CircuitBreaker::CircuitBreaker(int failure_threshold,
                               std::chrono::milliseconds cool_down,
                               int half_open_trials, Clock clock)
    : failure_threshold_(failure_threshold),
      cool_down_(cool_down),
      half_open_trials_(half_open_trials),
      clock_(std::move(clock)) {
    if (failure_threshold < 1) throw std::invalid_argument("failure_threshold must be >= 1");
    if (cool_down <= std::chrono::milliseconds(0)) throw std::invalid_argument("cool_down must be > 0");
    if (half_open_trials < 1) throw std::invalid_argument("half_open_trials must be >= 1");
    if (!clock_) throw std::invalid_argument("clock must not be empty");
}

void CircuitBreaker::open_locked() {
    state_ = BreakerState::Open;
    opened_at_ = clock_();
    consecutive_failures_ = 0;
    trials_in_flight_ = 0;
    trial_successes_ = 0;
}

bool CircuitBreaker::allow_request() {
    std::lock_guard<std::mutex> lock(mutex_);
    switch (state_) {
        case BreakerState::Closed:
            return true;
        case BreakerState::Open:
            if (clock_() - opened_at_ < cool_down_) return false;
            state_ = BreakerState::HalfOpen;
            trials_in_flight_ = 0;
            trial_successes_ = 0;
            [[fallthrough]];
        case BreakerState::HalfOpen:
            if (trials_in_flight_ >= half_open_trials_) return false;
            ++trials_in_flight_;
            return true;
    }
    return false;
}

void CircuitBreaker::record_success() {
    std::lock_guard<std::mutex> lock(mutex_);
    switch (state_) {
        case BreakerState::Closed:
            consecutive_failures_ = 0;
            break;
        case BreakerState::HalfOpen:
            if (trials_in_flight_ > 0) --trials_in_flight_;
            ++trial_successes_;
            if (trial_successes_ >= half_open_trials_) {
                state_ = BreakerState::Closed;
                consecutive_failures_ = 0;
                trials_in_flight_ = 0;
                trial_successes_ = 0;
            }
            break;
        case BreakerState::Open:
            break;  // a late result from before the breaker opened: ignore it
    }
}

void CircuitBreaker::record_failure() {
    std::lock_guard<std::mutex> lock(mutex_);
    switch (state_) {
        case BreakerState::Closed:
            if (++consecutive_failures_ >= failure_threshold_) open_locked();
            break;
        case BreakerState::HalfOpen:
            open_locked();  // any failed trial reopens the breaker
            break;
        case BreakerState::Open:
            break;  // already open
    }
}

BreakerState CircuitBreaker::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

const char* to_string(BreakerState state) {
    switch (state) {
        case BreakerState::Closed: return "closed";
        case BreakerState::Open: return "open";
        case BreakerState::HalfOpen: return "half_open";
    }
    return "unknown";
}

}  // namespace llmgate