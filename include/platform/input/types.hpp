#pragma once
#include <cstdint>
#include <array>
#include <cstddef>

namespace platform::input {

constexpr size_t MAX_SEAT_DEVICES = 16;
constexpr size_t MAX_POINTER_BUTTONS = 16;
constexpr size_t EVDEV_MAX_KEY = 768; // KEY_MAX (0x2ff) + 1

struct SyntheticPointerReleaseBatch {
    uint32_t session_id{0};
    uint32_t surface_local_id{0};
    uint64_t timestamp_us{0};
    std::array<uint32_t, MAX_POINTER_BUTTONS> buttons{};
    size_t count{0};

    [[nodiscard]] bool has_events() const noexcept { return count > 0; }
    void add_unique(uint32_t button) noexcept {
        for (size_t i = 0; i < count; ++i) {
            if (buttons[i] == button) return;
        }
        if (count < buttons.size()) {
            buttons[count++] = button;
        }
    }
};

struct SyntheticKeyReleaseBatch {
    uint32_t session_id{0};
    uint32_t surface_local_id{0};
    uint64_t timestamp_us{0};
    std::array<uint32_t, EVDEV_MAX_KEY> keys{};
    size_t count{0};

    [[nodiscard]] bool has_events() const noexcept { return count > 0; }
    void add_unique(uint32_t key) noexcept {
        for (size_t i = 0; i < count; ++i) {
            if (keys[i] == key) return;
        }
        if (count < keys.size()) {
            keys[count++] = key;
        }
    }
};

struct HitTestResult {
    uint64_t global_id{0};
    uint32_t local_id{0};
    uint32_t session_id{0};
    int32_t  window_x{0};
    int32_t  window_y{0};
};

} // namespace platform::input