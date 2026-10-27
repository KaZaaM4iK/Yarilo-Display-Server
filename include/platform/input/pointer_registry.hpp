#pragma once
#include <cstdint>
#include <array>
#include <cstddef>
#include <print>

extern "C" {
#include <libinput.h>
}

namespace platform::input {

class SeatPointerRegistry {
public:
    static constexpr size_t MAX_POINTERS = 16;

    SeatPointerRegistry() noexcept = default;
    
    ~SeatPointerRegistry() noexcept {
        reset();
    }

    SeatPointerRegistry(const SeatPointerRegistry&) = delete;
    SeatPointerRegistry& operator=(const SeatPointerRegistry&) = delete;

    // Регистрация устройства при LIBINPUT_EVENT_DEVICE_ADDED.
    // Возвращает индекс [0..15] при успехе.
    // Возвращает -1, если пул мест исчерпан (отказ в допуске).
    int admit_device(struct libinput_device* dev) noexcept {
        if (!dev) return -1;

        // Проверяем, не зарегистрировано ли уже
        for (size_t i = 0; i < MAX_POINTERS; ++i) {
            if (devices_[i] == dev) {
                return static_cast<int>(i);
            }
        }

        // Ищем свободный слот
        for (size_t i = 0; i < MAX_POINTERS; ++i) {
            if (devices_[i] == nullptr) {
                devices_[i] = dev;
                libinput_device_ref(dev);
                return static_cast<int>(i);
            }
        }

        try {
            std::println(stderr, "Yarilo Warning: Seat pointer limit (16) reached. Device rejected.");
        } catch (...) {}
        return -1; // Шлюз закрыт: отказ в допуске
    }

    // Поиск индекса допущенного устройства при событиях ввода.
    // Возвращает -1, если устройство не допущено к сеансу.
    [[nodiscard]] int get_device_index(struct libinput_device* dev) const noexcept {
        if (!dev) return -1;
        for (size_t i = 0; i < MAX_POINTERS; ++i) {
            if (devices_[i] == dev) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    // Освобождение слота устройства.
    // Вызывается СТРОГО ПОСЛЕ зачистки битовой маски в PointerGrabState!
    void release_device(size_t index) noexcept {
        if (index < MAX_POINTERS && devices_[index] != nullptr) {
            libinput_device_unref(devices_[index]);
            devices_[index] = nullptr;
        }
    }

    void reset() noexcept {
        for (size_t i = 0; i < MAX_POINTERS; ++i) {
            if (devices_[i] != nullptr) {
                libinput_device_unref(devices_[i]);
                devices_[i] = nullptr;
            }
        }
    }

private:
    std::array<struct libinput_device*, MAX_POINTERS> devices_{};
};

} // namespace platform::input