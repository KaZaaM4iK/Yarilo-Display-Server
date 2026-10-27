#include "platform/seat.hpp"
#include "core/error.hpp"
#include <poll.h>
#include <cerrno>
#include <chrono>
#include <print>

namespace platform {

Seat::Seat(libseat* seat, ActivationCallback callback) noexcept
    : seat_(seat), callback_(std::move(callback)) {}

Seat::~Seat() {
    if (seat_) {
        libseat_close_seat(seat_);
    }
}

void Seat::on_enable_seat(libseat*, void* userdata) noexcept {
    try {
        auto* self = static_cast<Seat*>(userdata);
        self->is_active_ = true;
        if (self->callback_) self->callback_(true);
    } catch (...) {
        // Запрещаем выход C++ исключений через границу C ABI libseat
    }
}

void Seat::on_disable_seat(libseat* seat, void* userdata) noexcept {
    try {
        auto* self = static_cast<Seat*>(userdata);
        self->is_active_ = false;
        if (self->callback_) self->callback_(false);
        libseat_disable_seat(seat);
    } catch (...) {
        libseat_disable_seat(seat);
    }
}

core::Result<std::unique_ptr<Seat>> Seat::open(ActivationCallback callback) {
    static libseat_seat_listener listener = {
        .enable_seat = &Seat::on_enable_seat,
        .disable_seat = &Seat::on_disable_seat,
    };

    // Создаем объект Seat заранее, чтобы передать его указатель как userdata в libseat_open_seat
    auto seat = std::unique_ptr<Seat>(new Seat(nullptr, std::move(callback)));

    libseat* s = libseat_open_seat(&listener, seat.get());
    if (!s) {
        return std::unexpected(core::make_error(core::ServerError::PermissionDenied));
    }

    seat->seat_ = s;
    return seat;
}

core::Result<void> Seat::wait_for_activation(int timeout_ms) {
    if (is_active_) return {};

    pollfd pfd{
        .fd = poll_fd(),
        .events = POLLIN,
        .revents = 0,
    };

    auto start = std::chrono::steady_clock::now();
    while (!is_active_) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= timeout_ms) {
            return std::unexpected(core::make_error(core::ServerError::SeatActivationTimeout));
        }

        int remaining = timeout_ms - static_cast<int>(elapsed);
        int ret = poll(&pfd, 1, remaining);
        if (ret < 0) {
            if (errno == EINTR) continue;
            return std::unexpected(core::make_error(core::ServerError::PermissionDenied));
        }
        if (ret > 0) {
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                return std::unexpected(core::make_error(core::ServerError::PermissionDenied));
            }
            if (pfd.revents & POLLIN) {
                if (dispatch(0) < 0) {
                    return std::unexpected(core::make_error(core::ServerError::PermissionDenied));
                }
            }
        }
    }
    return {};
}

core::Result<core::SeatDevice> Seat::open_device(const char* path) {
    if (!seat_ || !path) {
        return std::unexpected(core::make_error(core::ServerError::PermissionDenied));
    }

    errno = 0;
    int fd = -1;
    int device_id = libseat_open_device(seat_, path, &fd);
    int err = (errno != 0) ? errno : EACCES;

    // Транзакционный откат при частичном открытии
    if (device_id < 0 || fd < 0) {
        if (device_id >= 0) {
            libseat_close_device(seat_, device_id);
        }
        if (fd >= 0) {
            ::close(fd);
        }
        try {
            std::println(stderr, "Yarilo Error: libseat_open_device failed for {}: {}", path, std::strerror(err));
        } catch (...) {}
        return std::unexpected(core::make_error(core::ServerError::PermissionDenied));
    }

    return core::SeatDevice(seat_, device_id, fd);
}

int Seat::dispatch(int timeout_ms) {
    return seat_ ? libseat_dispatch(seat_, timeout_ms) : -1;
}

int Seat::poll_fd() const noexcept {
    return libseat_get_fd(seat_);
}

} // namespace platform