
#pragma once

#include "platform/input/types.hpp"

#include <array>
#include <cstddef>
#include <cstdio>

extern "C" {
#include <libinput.h>
}

namespace platform::input {

class SeatDeviceRegistry {
public:
    static constexpr size_t MAX_DEVICES = MAX_SEAT_DEVICES;

    SeatDeviceRegistry() noexcept = default;

    ~SeatDeviceRegistry() noexcept {
        reset();
    }

    SeatDeviceRegistry(const SeatDeviceRegistry&) = delete;
    SeatDeviceRegistry& operator=(const SeatDeviceRegistry&) = delete;

    // Допуск устройства в общий seat.
    // Возвращает индекс [0..MAX_DEVICES-1] либо -1.
    int admit_device(struct libinput_device* dev) noexcept {
        if (!dev) {
            return -1;
        }

        for (size_t i = 0; i < MAX_DEVICES; ++i) {
            if (devices_[i] == dev) {
                return static_cast<int>(i);
            }
        }

        for (size_t i = 0; i < MAX_DEVICES; ++i) {
            if (devices_[i] == nullptr) {
                devices_[i] = libinput_device_ref(dev);
                return static_cast<int>(i);
            }
        }

        std::fprintf(
            stderr,
            "Yarilo Warning: Seat device limit (%zu) reached. "
            "Device rejected.\n",
            MAX_DEVICES
        );

        return -1;
    }

    // Индекс только ранее допущенного устройства.
    [[nodiscard]] int get_device_index(
        struct libinput_device* dev
    ) const noexcept {
        if (!dev) {
            return -1;
        }

        for (size_t i = 0; i < MAX_DEVICES; ++i) {
            if (devices_[i] == dev) {
                return static_cast<int>(i);
            }
        }

        return -1;
    }

    // Освобождать только после очистки состояния устройства
    // в PointerGrabState и KeyboardFocusState.
    void release_device(size_t index) noexcept {
        if (index >= MAX_DEVICES || devices_[index] == nullptr) {
            return;
        }

        libinput_device_unref(devices_[index]);
        devices_[index] = nullptr;
    }

    void reset() noexcept {
        for (size_t i = 0; i < MAX_DEVICES; ++i) {
            release_device(i);
        }
    }

private:
    std::array<struct libinput_device*, MAX_DEVICES> devices_{};
};

// Освобождает слот при любом выходе из области обработки
// LIBINPUT_EVENT_DEVICE_REMOVED, включая выход через исключение.
class DeviceIndexReleaser {
public:
    DeviceIndexReleaser(
        SeatDeviceRegistry& registry,
        int index
    ) noexcept
        : registry_(&registry), index_(index) {}

    ~DeviceIndexReleaser() noexcept {
        if (registry_ && index_ >= 0) {
            registry_->release_device(
                static_cast<size_t>(index_)
            );
        }
    }

    DeviceIndexReleaser(const DeviceIndexReleaser&) = delete;
    DeviceIndexReleaser& operator=(const DeviceIndexReleaser&) = delete;

private:
    SeatDeviceRegistry* registry_;
    int index_;
};

} // namespace platform::input
