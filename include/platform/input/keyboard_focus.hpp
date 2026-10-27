#pragma once
#include "platform/input/types.hpp"
#include <cstdint>
#include <cstddef>
#include <array>
#include <cstdio>
#include <print>

namespace platform::input {

enum class KeyAction {
    LogicalPress,
    PhysicalPressOnly,
    IgnoredDuplicate,
    RejectedInvalidKey,
    RejectedNoFocus,
    RejectedDeviceIndex
};

enum class KeyReleaseAction {
    LogicalRelease,
    PhysicalReleaseOnly,
    UntrackedIgnored
};

struct KeyPressResult { KeyAction action; uint32_t session_id; uint32_t surface_local_id; };
struct KeyReleaseResult { KeyReleaseAction action; uint32_t session_id; uint32_t surface_local_id; };

class KeyboardFocusState {
public:
    KeyboardFocusState() noexcept = default;
    ~KeyboardFocusState() noexcept { reset(); }

    void set_focus(uint64_t gid, uint32_t local_id, uint32_t sess_id) noexcept {
        surface_gid_ = gid;
        surface_local_id_ = local_id;
        session_id_ = sess_id;
    }

    [[nodiscard]] uint64_t surface_gid() const noexcept { return surface_gid_; }
    [[nodiscard]] uint32_t session_id() const noexcept { return session_id_; }

    KeyPressResult on_key_press(uint8_t dev_idx, uint32_t key) noexcept {
        if (dev_idx >= MAX_SEAT_DEVICES) return {KeyAction::RejectedDeviceIndex, 0, 0};
        if (key >= EVDEV_MAX_KEY) return {KeyAction::RejectedInvalidKey, 0, 0};
        if (surface_gid_ == 0) return {KeyAction::RejectedNoFocus, 0, 0};

        uint16_t& mask = device_masks_[key];

        if (mask & (1U << dev_idx)) {
            return {KeyAction::IgnoredDuplicate, 0, 0};
        }

        bool was_idle = (mask == 0);
        mask |= static_cast<uint16_t>(1U << dev_idx);

        if (was_idle) {
            active_logical_keys_++;
            return {KeyAction::LogicalPress, session_id_, surface_local_id_};
        }

        return {KeyAction::PhysicalPressOnly, session_id_, surface_local_id_};
    }

    KeyReleaseResult on_key_release(uint8_t dev_idx, uint32_t key) noexcept {
        if (dev_idx >= MAX_SEAT_DEVICES || key >= EVDEV_MAX_KEY) {
            return {KeyReleaseAction::UntrackedIgnored, 0, 0};
        }

        uint16_t& mask = device_masks_[key];
        if (!(mask & (1U << dev_idx))) {
            return {KeyReleaseAction::UntrackedIgnored, 0, 0};
        }

        mask &= static_cast<uint16_t>(~(1U << dev_idx));

        if (mask == 0) {
            active_logical_keys_--;
            return {KeyReleaseAction::LogicalRelease, session_id_, surface_local_id_};
        }

        return {KeyReleaseAction::PhysicalReleaseOnly, session_id_, surface_local_id_};
    }

    SyntheticKeyReleaseBatch on_device_removed(uint8_t dev_idx, uint64_t timestamp_us) noexcept {
        SyntheticKeyReleaseBatch batch;
        if (dev_idx >= MAX_SEAT_DEVICES || active_logical_keys_ == 0) return batch;

        batch.session_id = session_id_;
        batch.surface_local_id = surface_local_id_;
        batch.timestamp_us = timestamp_us;

        for (size_t key = 0; key < EVDEV_MAX_KEY; ++key) {
            uint16_t& mask = device_masks_[key];
            if (mask & (1U << dev_idx)) {
                mask &= static_cast<uint16_t>(~(1U << dev_idx));
                if (mask == 0) {
                    batch.add_unique(static_cast<uint32_t>(key));
                    active_logical_keys_--; // [P1 FIX]: Корректный декремент логических клавиш
                }
            }
        }
        return batch;
    }

    SyntheticKeyReleaseBatch cancel_focus(uint64_t timestamp_us) noexcept {
        SyntheticKeyReleaseBatch batch;
        if (active_logical_keys_ == 0) return batch;

        batch.session_id = session_id_;
        batch.surface_local_id = surface_local_id_;
        batch.timestamp_us = timestamp_us;

        for (size_t key = 0; key < EVDEV_MAX_KEY; ++key) {
            if (device_masks_[key] != 0) {
                batch.add_unique(static_cast<uint32_t>(key));
                device_masks_[key] = 0;
            }
        }
        active_logical_keys_ = 0;
        return batch;
    }

    void reset() noexcept {
        (void)cancel_focus(0);
        surface_gid_ = 0;
        surface_local_id_ = 0;
        session_id_ = 0;
    }

private:
    uint64_t surface_gid_{0};
    uint32_t surface_local_id_{0};
    uint32_t session_id_{0};
    std::array<uint16_t, EVDEV_MAX_KEY> device_masks_{};
    uint32_t active_logical_keys_{0};
};

} // namespace platform::input