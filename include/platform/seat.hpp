#pragma once
#include "core/types.hpp"
extern "C" {
#include <libseat.h>
}
#include <functional>
#include <memory>

namespace platform {

class Seat {
public:
    using ActivationCallback = std::function<void(bool active)>;

    static core::Result<std::unique_ptr<Seat>> open(ActivationCallback callback);
    ~Seat();

    core::Result<core::SeatDevice> open_device(const char* path);

    int dispatch(int timeout_ms = 0);
    [[nodiscard]] int poll_fd() const noexcept;
    [[nodiscard]] bool is_active() const noexcept { return is_active_; }

    core::Result<void> wait_for_activation(int timeout_ms = 2000);

private:
    Seat(libseat* seat, ActivationCallback callback) noexcept;

    // Спецификации noexcept согласованы с реализацией в seat.cpp
    static void on_enable_seat(libseat* seat, void* userdata) noexcept;
    static void on_disable_seat(libseat* seat, void* userdata) noexcept;

    libseat* seat_{nullptr};
    ActivationCallback callback_;
    bool is_active_{false};
};

} // namespace platform