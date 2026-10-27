#pragma once
#include "core/types.hpp"
extern "C" {
#include <libseat.h>
#include <libinput.h>
}
#include <unordered_map>
#include <cerrno>
#include <unistd.h>
#include <cstring>
#include <print>

namespace platform::input {

class SeatInputAdapter {
public:
    explicit SeatInputAdapter(libseat* seat) noexcept : seat_(seat) {}
    ~SeatInputAdapter() = default;

    SeatInputAdapter(const SeatInputAdapter&) = delete;
    SeatInputAdapter& operator=(const SeatInputAdapter&) = delete;

    int open_restricted(const char* path, int flags) noexcept {
        (void)flags;
        if (!seat_ || !path) return -EINVAL;

        errno = 0;
        int fd = -1;
        int device_id = libseat_open_device(seat_, path, &fd);
        int err = (errno != 0) ? errno : EACCES;

        // Откат при асимметричном сбое открытия
        if (device_id < 0 || fd < 0) {
            if (device_id >= 0) {
                libseat_close_device(seat_, device_id);
            }
            if (fd >= 0) {
                ::close(fd);
            }
            return -err;
        }

        // Защита от OOM: исключение не должно выйти за границу C ABI libinput
        try {
            fd_to_device_id_[fd] = device_id;
            return fd;
        } catch (...) {
            // Транзакционный откат при сбое аллокации хеш-таблицы
            libseat_close_device(seat_, device_id);
            ::close(fd);
            return -ENOMEM;
        }
    }

    void close_restricted(int fd) noexcept {
        if (fd < 0) return;

        // Если seat_ валиден, пробуем найти соответствие и освободить слот в seatd
        if (seat_) {
            auto it = fd_to_device_id_.find(fd);
            if (it != fd_to_device_id_.end()) {
                int device_id = it->second;
                fd_to_device_id_.erase(it);

                int ret = libseat_close_device(seat_, device_id);
                if (ret != 0) {
                    try {
                        std::println(stderr, "Yarilo Warning: libseat_close_device failed for device {}", device_id);
                    } catch (...) {}
                }
            } else {
                try {
                    std::println(stderr, "Yarilo Warning: close_restricted called for unregistered fd {}", fd);
                } catch (...) {}
            }
        }

        // Инвариант libinput: переданный дескриптор закрывается безусловно
        ::close(fd);
    }

    static const struct libinput_interface& interface() noexcept {
        static const struct libinput_interface s_interface = {
            .open_restricted = [](const char* path, int flags, void* user_data) noexcept -> int {
                return static_cast<SeatInputAdapter*>(user_data)->open_restricted(path, flags);
            },
            .close_restricted = [](int fd, void* user_data) noexcept {
                static_cast<SeatInputAdapter*>(user_data)->close_restricted(fd);
            }
        };
        return s_interface;
    }

private:
    libseat* seat_{nullptr};
    std::unordered_map<int, int> fd_to_device_id_;
};

} // namespace platform::input