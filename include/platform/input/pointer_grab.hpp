#pragma once
#include "platform/input/time.hpp"
#include <cstdint>
#include <array>
#include <cstddef>
#include <cstdio>
#include <print>

namespace platform::input {

constexpr size_t MAX_SEAT_POINTERS = 16;
constexpr size_t MAX_UNIQUE_BUTTONS = 16;

enum class ButtonPressAction {
    NewGrabAndLogicalPress,     // Первая кнопка: активирует захват, отправить клиенту (state = 1)
    LogicalPressGrabContinued,  // Новая уникальная кнопка: отправить клиенту (state = 1)
    PhysicalPressOnly,          // Кнопка уже зажата другой мышью: учесть физически, клиенту НЕ отправлять
    IgnoredDuplicate,           // Та же кнопка на том же устройстве: дребезг, игнорировать
    RejectedOverflow,           // Лимит уникальных кнопок (16) исчерпан: отклонить
    RejectedInvalidSurface,     // Клик в пустой фон без активного захвата: отклонить
    RejectedDeviceIndex         // Индекс устройства превышает лимит места ввода: отклонить
};

enum class ButtonReleaseAction {
    LogicalReleaseAndGrabTerminated, // Последняя кнопка отпущена: снять захват, отправить клиенту (state = 0)
    LogicalReleaseGrabContinued,     // Логическая кнопка отпущена: отправить клиенту (state = 0), захват активен
    PhysicalReleaseOnly,             // Кнопку еще удерживает другая мышь: клиенту НЕ отправлять
    UntrackedIgnored                 // Кнопка не отслеживалась (была отклонена): клиенту НЕ отправлять
};

struct ButtonPressResult {
    ButtonPressAction action{ButtonPressAction::RejectedInvalidSurface};
    uint32_t session_id{0};
    uint32_t surface_local_id{0};
};

struct ButtonReleaseResult {
    ButtonReleaseAction action{ButtonReleaseAction::UntrackedIgnored};
    uint32_t session_id{0};
    uint32_t surface_local_id{0};
};

// Слот уникальной логической кнопки с битовой маской удерживающих устройств
struct GrabbedButtonSlot {
    uint32_t button{0};
    uint16_t device_mask{0}; // Бит N = 1, если устройство с device_index == N удерживает кнопку

    [[nodiscard]] bool has_device(uint8_t dev_idx) const noexcept {
        return (device_mask & static_cast<uint16_t>(1U << dev_idx)) != 0;
    }

    void add_device(uint8_t dev_idx) noexcept {
        device_mask |= static_cast<uint16_t>(1U << dev_idx);
    }

    void remove_device(uint8_t dev_idx) noexcept {
        device_mask &= static_cast<uint16_t>(~(1U << dev_idx));
    }

    [[nodiscard]] bool is_held() const noexcept {
        return device_mask != 0;
    }
};

// Пакет дедуплицированных синтетических отпусканий с монотонным временем CLOCK_MONOTONIC
struct SyntheticReleaseBatch {
    uint32_t session_id{0};
    uint32_t surface_local_id{0};
    uint64_t timestamp_us{0};
    std::array<uint32_t, MAX_UNIQUE_BUTTONS> buttons{};
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

class PointerGrabState {
public:
    PointerGrabState() noexcept = default;
    ~PointerGrabState() noexcept { reset(); }

    PointerGrabState(const PointerGrabState&) = delete;
    PointerGrabState& operator=(const PointerGrabState&) = delete;

    [[nodiscard]] bool is_active() const noexcept {
        return active_slots_count_ > 0 && surface_gid_ != 0;
    }

    [[nodiscard]] uint64_t surface_gid() const noexcept { return surface_gid_; }
    [[nodiscard]] uint32_t surface_local_id() const noexcept { return surface_local_id_; }
    [[nodiscard]] uint32_t session_id() const noexcept { return session_id_; }

    ButtonPressResult on_button_press(uint8_t dev_index, uint32_t button,
                                      uint64_t hit_gid, uint32_t hit_local_id, uint32_t hit_sess_id) noexcept 
    {
        if (dev_index >= MAX_SEAT_POINTERS) {
            return ButtonPressResult{ButtonPressAction::RejectedDeviceIndex, 0, 0};
        }

        bool was_idle = (active_slots_count_ == 0);

        if (was_idle && hit_gid == 0) {
            return ButtonPressResult{ButtonPressAction::RejectedInvalidSurface, 0, 0};
        }

        // 1. Поиск существующего слота логической кнопки
        GrabbedButtonSlot* target_slot = nullptr;
        for (size_t i = 0; i < active_slots_count_; ++i) {
            if (slots_[i].button == button) {
                target_slot = &slots_[i];
                break;
            }
        }

        // 2. Если кнопка уже удерживается в системе:
        if (target_slot != nullptr) {
            if (target_slot->has_device(dev_index)) {
                return ButtonPressResult{ButtonPressAction::IgnoredDuplicate, 0, 0};
            }

            // Устройство выставляет свой бит. Переполнение физически невозможно!
            target_slot->add_device(dev_index);
            return ButtonPressResult{ButtonPressAction::PhysicalPressOnly, session_id_, surface_local_id_};
        }

        // 3. Новая уникальная логическая кнопка
        if (active_slots_count_ >= MAX_UNIQUE_BUTTONS) {
            try {
                std::println(stderr, "Yarilo Warning: Max unique grab buttons (16) exceeded. Dropping button 0x{:x}", button);
            } catch (...) {}
            return ButtonPressResult{ButtonPressAction::RejectedOverflow, 0, 0};
        }

        if (was_idle) {
            surface_gid_ = hit_gid;
            surface_local_id_ = hit_local_id;
            session_id_ = hit_sess_id;
        }

        auto& new_slot = slots_[active_slots_count_++];
        new_slot.button = button;
        new_slot.device_mask = 0;
        new_slot.add_device(dev_index);

        return ButtonPressResult{
            was_idle ? ButtonPressAction::NewGrabAndLogicalPress : ButtonPressAction::LogicalPressGrabContinued,
            session_id_,
            surface_local_id_
        };
    }

    ButtonReleaseResult on_button_release(uint8_t dev_index, uint32_t button) noexcept {
        if (dev_index >= MAX_SEAT_POINTERS) {
            return ButtonReleaseResult{ButtonReleaseAction::UntrackedIgnored, 0, 0};
        }

        for (size_t i = 0; i < active_slots_count_; ++i) {
            if (slots_[i].button == button) {
                if (!slots_[i].has_device(dev_index)) {
                    return ButtonReleaseResult{ButtonReleaseAction::UntrackedIgnored, 0, 0};
                }

                slots_[i].remove_device(dev_index);

                uint32_t target_sess = session_id_;
                uint32_t target_local = surface_local_id_;

                // Если другие устройства еще физически удерживают кнопку:
                if (slots_[i].is_held()) {
                    return ButtonReleaseResult{ButtonReleaseAction::PhysicalReleaseOnly, target_sess, target_local};
                }

                // Кнопку отпустило последнее физическое устройство -> освобождаем слот
                for (size_t j = i; j + 1 < active_slots_count_; ++j) {
                    slots_[j] = slots_[j + 1];
                }
                slots_[--active_slots_count_] = GrabbedButtonSlot{};

                if (active_slots_count_ == 0) {
                    surface_gid_ = 0;
                    surface_local_id_ = 0;
                    session_id_ = 0;
                    return ButtonReleaseResult{ButtonReleaseAction::LogicalReleaseAndGrabTerminated, target_sess, target_local};
                }

                return ButtonReleaseResult{ButtonReleaseAction::LogicalReleaseGrabContinued, target_sess, target_local};
            }
        }

        return ButtonReleaseResult{ButtonReleaseAction::UntrackedIgnored, 0, 0};
    }

    // Обработка удаления физического устройства по его индексу
    SyntheticReleaseBatch on_device_removed(uint8_t dev_index, uint64_t timestamp_us) noexcept {
        SyntheticReleaseBatch batch;
        if (dev_index >= MAX_SEAT_POINTERS || active_slots_count_ == 0) return batch;

        batch.session_id = session_id_;
        batch.surface_local_id = surface_local_id_;
        batch.timestamp_us = timestamp_us;

        for (size_t i = 0; i < active_slots_count_; ) {
            if (slots_[i].has_device(dev_index)) {
                slots_[i].remove_device(dev_index);

                // Если это устройство было единственным держателем кнопки:
                if (!slots_[i].is_held()) {
                    batch.add_unique(slots_[i].button);

                    for (size_t j = i; j + 1 < active_slots_count_; ++j) {
                        slots_[j] = slots_[j + 1];
                    }
                    slots_[--active_slots_count_] = GrabbedButtonSlot{};
                    continue;
                }
            }
            ++i;
        }

        if (active_slots_count_ == 0) {
            surface_gid_ = 0;
            surface_local_id_ = 0;
            session_id_ = 0;
        }

        return batch;
    }

    // Runtime-отмена захвата (смена VT, уничтожение поверхности)
    SyntheticReleaseBatch cancel_grab(uint64_t timestamp_us) noexcept {
        SyntheticReleaseBatch batch;
        if (active_slots_count_ == 0) return batch;

        batch.session_id = session_id_;
        batch.surface_local_id = surface_local_id_;
        batch.timestamp_us = timestamp_us;

        for (size_t i = 0; i < active_slots_count_; ++i) {
            batch.add_unique(slots_[i].button);
        }

        slots_.fill(GrabbedButtonSlot{});
        active_slots_count_ = 0;
        surface_gid_ = 0;
        surface_local_id_ = 0;
        session_id_ = 0;

        return batch;
    }

    // Терминальный деструкторный сброс
    void reset() noexcept {
        slots_.fill(GrabbedButtonSlot{});
        active_slots_count_ = 0;
        surface_gid_ = 0;
        surface_local_id_ = 0;
        session_id_ = 0;
    }

private:
    uint64_t surface_gid_{0};
    uint32_t surface_local_id_{0};
    uint32_t session_id_{0};
    std::array<GrabbedButtonSlot, MAX_UNIQUE_BUTTONS> slots_{};
    size_t active_slots_count_{0};
};

} // namespace platform::input