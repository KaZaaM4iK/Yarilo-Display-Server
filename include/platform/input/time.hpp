#pragma once
#include "core/types.hpp"
#include "core/error.hpp"
#include <cstdint>
#include <time.h>
#include <algorithm>

namespace platform::input {

class MonotonicClock {
public:
    static core::Result<void> initialize_epoch() noexcept {
        struct timespec ts{};
        if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
            return std::unexpected(core::make_error(core::ServerError::InternalStateError));
        }
        last_time_us_ = (static_cast<uint64_t>(ts.tv_sec) * 1'000'000ULL) +
                        (static_cast<uint64_t>(ts.tv_nsec) / 1000ULL);
        if (last_time_us_ == 0) last_time_us_ = 1;
        return {};
    }

    static uint64_t now_us(uint64_t last_event_high_water_mark) noexcept {
        struct timespec ts{};
        uint64_t current = 0;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
            current = (static_cast<uint64_t>(ts.tv_sec) * 1'000'000ULL) +
                      (static_cast<uint64_t>(ts.tv_nsec) / 1000ULL);
        }

        // Гарантия 1: время не идет назад относительно системных часов
        if (current > last_time_us_) {
            last_time_us_ = current;
        } else {
            last_time_us_++;
        }

        // Гарантия 2: синтетическое время строго >= последнему аппаратному событию
        if (last_event_high_water_mark >= last_time_us_) {
            last_time_us_ = last_event_high_water_mark + 1;
        }

        return last_time_us_;
    }

private:
    static inline uint64_t last_time_us_{0};
};

} // namespace platform::input