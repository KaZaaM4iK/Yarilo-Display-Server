#pragma once
#include "platform/input/types.hpp"
#include "platform/input/seat_adapter.hpp"
#include "platform/input/pointer_registry.hpp"
#include "platform/input/pointer_grab.hpp"
#include "platform/input/keyboard_focus.hpp"
#include "platform/input/time.hpp"
#include "core/types.hpp"
#include "core/error.hpp"
#include <memory>
#include <functional>
#include <optional>
#include <utility>
#include <algorithm>
#include <tuple>
#include <cstdio>
#include <cstdlib>
#include <print>

#if !defined(__linux__)
#error "Yarilo Sprint 6.1 InputSubsystem currently requires Linux with libudev. FreeBSD backend is scheduled for Sprint 7."
#endif

extern "C" {
#include <libinput.h>
#include <libudev.h>
}

namespace platform::input {

struct InputEventCallbacks {
    std::function<std::optional<HitTestResult>(double cursor_x, double cursor_y)> hit_test;
    std::function<std::optional<std::pair<int32_t, int32_t>>(uint64_t gid)> get_surface_origin;

    std::function<bool(uint32_t session_id, uint32_t surface_id, int32_t x, int32_t y, uint64_t time_us)> send_pointer_motion;
    std::function<bool(uint32_t session_id, uint32_t surface_id, uint32_t button, uint32_t state, uint64_t time_us)> send_pointer_button;
    std::function<bool(uint32_t session_id, uint32_t surface_id, uint32_t key, uint32_t state, uint64_t time_us)> send_keyboard_key;
    
    std::function<bool(const SyntheticPointerReleaseBatch& batch)> dispatch_synthetic_pointer_releases;
    std::function<bool(const SyntheticKeyReleaseBatch& batch)> dispatch_synthetic_key_releases;

    std::function<bool(uint32_t session_id)> kill_client_session;
};

struct UdevDeleter {
    void operator()(struct udev* u) const noexcept { if (u) udev_unref(u); }
};
using UniqueUdev = std::unique_ptr<struct udev, UdevDeleter>;

struct LibinputEventDeleter {
    void operator()(struct libinput_event* ev) const noexcept { if (ev) libinput_event_destroy(ev); }
};
using UniqueLibinputEvent = std::unique_ptr<struct libinput_event, LibinputEventDeleter>;

class InputSubsystem {
public:
    static core::Result<std::unique_ptr<InputSubsystem>> create(platform::Seat& seat, uint32_t screen_w, uint32_t screen_h) {
        if (screen_w == 0 || screen_h == 0) return std::unexpected(core::make_error(core::ServerError::InternalStateError));
        if (auto clk_res = MonotonicClock::initialize_epoch(); !clk_res) return std::unexpected(clk_res.error());

        UniqueUdev u(udev_new());
        if (!u) return std::unexpected(core::make_error(core::ServerError::InternalStateError));

        std::unique_ptr<InputSubsystem> sub;
        try { sub.reset(new InputSubsystem(seat.handle(), screen_w, screen_h)); } 
        catch (...) { return std::unexpected(core::make_error(core::ServerError::OutOfMemory)); }
        
        sub->udev_ = u.release();

        struct libinput* li = libinput_udev_create_context(&SeatInputAdapter::interface(), &sub->adapter_, sub->udev_);
        if (!li) return std::unexpected(core::make_error(core::ServerError::InternalStateError));
        sub->li_ = li;

        const char* seat_name = seat.name();
        if (!seat_name) seat_name = "seat0";

        if (libinput_udev_assign_seat(sub->li_, seat_name) != 0) {
            return std::unexpected(core::make_error(core::ServerError::PermissionDenied));
        }

        return sub;
    }

    ~InputSubsystem() noexcept {
        pointer_grab_.reset();
        keyboard_state_.reset();
        device_registry_.reset();
        if (li_) { libinput_unref(li_); li_ = nullptr; }
        if (udev_) { udev_unref(udev_); udev_ = nullptr; }
    }

    InputSubsystem(const InputSubsystem&) = delete;
    InputSubsystem& operator=(const InputSubsystem&) = delete;

    [[nodiscard]] int poll_fd() const noexcept { return li_ ? libinput_get_fd(li_) : -1; }

    core::Result<void> process_events(const InputEventCallbacks& cb) noexcept {
        if (!li_) return {};
        if (!validate_callbacks(cb)) return std::unexpected(core::make_error(core::ServerError::InternalStateError));

        int dispatch_ret = libinput_dispatch(li_);
        if (dispatch_ret != 0) return std::unexpected(core::make_error(core::ServerError::InternalStateError));

        while (struct libinput_event* raw_ev = libinput_get_event(li_)) {
            UniqueLibinputEvent ev(raw_ev);
            try { 
                handle_single_event(ev.get(), cb); 
            } catch (...) {
                return std::unexpected(core::make_error(core::ServerError::InternalStateError));
            }
        }
        return {};
    }

    core::Result<void> suspend(const InputEventCallbacks& cb) noexcept {
        if (!validate_callbacks(cb)) return std::unexpected(core::make_error(core::ServerError::InternalStateError));
        uint64_t now_us = MonotonicClock::now_us(last_event_time_us_);
        
        auto p_batch = pointer_grab_.cancel_grab(now_us);
        if (p_batch.has_events()) {
            safe_invoke(p_batch.session_id, cb, [&]() { return cb.dispatch_synthetic_pointer_releases(p_batch); });
        }

        auto k_batch = keyboard_state_.cancel_focus(now_us);
        if (k_batch.has_events()) {
            safe_invoke(k_batch.session_id, cb, [&]() { return cb.dispatch_synthetic_key_releases(k_batch); });
        }

        device_registry_.reset();
        if (li_) libinput_suspend(li_);
        return {};
    }

    core::Result<void> resume() noexcept {
        if (!li_) return std::unexpected(core::make_error(core::ServerError::InternalStateError));
        if (libinput_resume(li_) != 0) return std::unexpected(core::make_error(core::ServerError::PermissionDenied));
        return {};
    }

    core::Result<void> set_keyboard_focus(uint64_t gid, uint32_t local_id, uint32_t sess_id, const InputEventCallbacks& cb) noexcept {
        if (!validate_callbacks(cb)) return std::unexpected(core::make_error(core::ServerError::InternalStateError));
        if (keyboard_state_.surface_gid() == gid) return {}; 

        uint64_t now_us = MonotonicClock::now_us(last_event_time_us_);
        auto batch = keyboard_state_.cancel_focus(now_us);

        if (batch.has_events()) {
            safe_invoke(batch.session_id, cb, [&]() { return cb.dispatch_synthetic_key_releases(batch); });
        }

        if (gid != 0) keyboard_state_.set_focus(gid, local_id, sess_id);
        else keyboard_state_.reset();
        
        return {};
    }

    core::Result<void> clear_keyboard_focus_for_surface(uint64_t gid, const InputEventCallbacks& cb) noexcept {
        if (keyboard_state_.surface_gid() == gid) {
            return set_keyboard_focus(0, 0, 0, cb);
        }
        return {};
    }

    void cancel_grab_for_surface(uint64_t gid, const InputEventCallbacks& cb) noexcept {
        if (pointer_grab_.surface_gid() == gid) {
            uint64_t now_us = MonotonicClock::now_us(last_event_time_us_);
            auto batch = pointer_grab_.cancel_grab(now_us);
            if (batch.has_events()) {
                safe_invoke(batch.session_id, cb, [&]() { return cb.dispatch_synthetic_pointer_releases(batch); });
            }
        }
    }

    void cancel_grab_for_closing_session(uint32_t sess_id) noexcept {
        if (pointer_grab_.session_id() == sess_id) {
            pointer_grab_.reset();
        }
        if (keyboard_state_.session_id() == sess_id) {
            keyboard_state_.reset();
        }
    }

    core::Result<void> cancel_all_input(const InputEventCallbacks& cb) noexcept {
        auto res = set_keyboard_focus(0, 0, 0, cb);
        if (!res) return res;

        uint64_t now_us = MonotonicClock::now_us(last_event_time_us_);
        auto p_batch = pointer_grab_.cancel_grab(now_us);
        if (p_batch.has_events()) {
            safe_invoke(p_batch.session_id, cb, [&]() { return cb.dispatch_synthetic_pointer_releases(p_batch); });
        }
        return {};
    }

    [[nodiscard]] uint32_t grab_session_id() const noexcept { return pointer_grab_.session_id(); }
    [[nodiscard]] uint64_t grab_surface_gid() const noexcept { return pointer_grab_.surface_gid(); }

private:
    InputSubsystem(libseat* seat, uint32_t w, uint32_t h) noexcept 
        : adapter_(seat), screen_w_(w), screen_h_(h), cursor_x_(w / 2.0), cursor_y_(h / 2.0) {}

    bool validate_callbacks(const InputEventCallbacks& cb) const noexcept {
        return cb.kill_client_session && cb.hit_test && cb.get_surface_origin &&
               cb.send_pointer_motion && cb.send_pointer_button && cb.send_keyboard_key &&
               cb.dispatch_synthetic_pointer_releases && cb.dispatch_synthetic_key_releases;
    }

    void invoke_kill(uint32_t s_id, const InputEventCallbacks& cb) noexcept {
        try {
            if (!cb.kill_client_session(s_id)) {
                try { std::println(stderr, "Yarilo Panic: kill_client_session failed for {}. Aborting.", s_id); } catch (...) {}
                std::abort();
            }
        } catch (...) {
            try { std::println(stderr, "Yarilo Panic: kill_client_session threw exception. Aborting."); } catch (...) {}
            std::abort();
        }
    }

    template<typename Task>
    void safe_invoke(uint32_t target_sess, const InputEventCallbacks& cb, Task&& task) noexcept {
        if (target_sess == 0) {
            try { std::println(stderr, "Yarilo Panic: safe_invoke called with target_sess == 0. Aborting."); } catch (...) {}
            std::abort();
        }
        try {
            if (!task()) invoke_kill(target_sess, cb);
        } catch (...) {
            invoke_kill(target_sess, cb);
        }
    }

    void handle_single_event(struct libinput_event* ev, const InputEventCallbacks& cb) {
        auto type = libinput_event_get_type(ev);
        switch (type) {
            case LIBINPUT_EVENT_DEVICE_ADDED: {
                auto* dev = libinput_event_get_device(ev);
                if (libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_POINTER) || 
                    libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_KEYBOARD)) {
                    (void)device_registry_.admit_device(dev);
                }
                break;
            }
            case LIBINPUT_EVENT_DEVICE_REMOVED: {
                auto* dev = libinput_event_get_device(ev);
                int idx = device_registry_.get_device_index(dev);
                if (idx >= 0) {
                    DeviceIndexReleaser releaser{device_registry_, idx};
                    uint64_t now_us = MonotonicClock::now_us(last_event_time_us_);
                    
                    auto p_batch = pointer_grab_.on_device_removed(static_cast<uint8_t>(idx), now_us);
                    if (p_batch.has_events()) {
                        safe_invoke(p_batch.session_id, cb, [&]() { return cb.dispatch_synthetic_pointer_releases(p_batch); });
                    }
                    
                    auto k_batch = keyboard_state_.on_device_removed(static_cast<uint8_t>(idx), now_us);
                    if (k_batch.has_events()) {
                        safe_invoke(k_batch.session_id, cb, [&]() { return cb.dispatch_synthetic_key_releases(k_batch); });
                    }
                }
                break;
            }
            case LIBINPUT_EVENT_POINTER_MOTION: {
                auto* p_ev = libinput_event_get_pointer_event(ev);
                auto* dev = libinput_event_get_device(ev);
                if (device_registry_.get_device_index(dev) < 0) break;

                uint64_t ev_time = libinput_event_pointer_get_time_usec(p_ev);
                last_event_time_us_ = std::max(last_event_time_us_, ev_time);

                cursor_x_ = std::clamp(cursor_x_ + libinput_event_pointer_get_dx(p_ev), 0.0, static_cast<double>(screen_w_ - 1));
                cursor_y_ = std::clamp(cursor_y_ + libinput_event_pointer_get_dy(p_ev), 0.0, static_cast<double>(screen_h_ - 1));

                uint32_t target_sess = 0; uint32_t target_local = 0;
                int32_t surf_x = 0; int32_t surf_y = 0;

                if (pointer_grab_.is_active()) {
                    target_sess = pointer_grab_.session_id();
                    target_local = pointer_grab_.surface_local_id();
                    auto origin = cb.get_surface_origin(pointer_grab_.surface_gid());
                    if (origin) {
                        surf_x = static_cast<int32_t>(cursor_x_) - origin->first;
                        surf_y = static_cast<int32_t>(cursor_y_) - origin->second;
                    } else {
                        cancel_grab_for_surface(pointer_grab_.surface_gid(), cb);
                        target_sess = 0;
                    }
                } else {
                    auto hit = cb.hit_test(cursor_x_, cursor_y_);
                    if (hit && hit->global_id != 0) {
                        target_sess = hit->session_id; target_local = hit->local_id;
                        surf_x = static_cast<int32_t>(cursor_x_) - hit->window_x; 
                        surf_y = static_cast<int32_t>(cursor_y_) - hit->window_y;
                    }
                }
                if (target_sess != 0) {
                    safe_invoke(target_sess, cb, [&]() { 
                        return cb.send_pointer_motion(target_sess, target_local, surf_x, surf_y, ev_time); 
                    });
                }
                break;
            }
            case LIBINPUT_EVENT_POINTER_BUTTON: {
                auto* p_ev = libinput_event_get_pointer_event(ev);
                auto* dev = libinput_event_get_device(ev);
                int dev_idx = device_registry_.get_device_index(dev);
                if (dev_idx < 0) break;

                uint64_t ev_time = libinput_event_pointer_get_time_usec(p_ev);
                last_event_time_us_ = std::max(last_event_time_us_, ev_time);
                uint32_t button = libinput_event_pointer_get_button(p_ev);
                bool pressed = (libinput_event_pointer_get_button_state(p_ev) == LIBINPUT_BUTTON_STATE_PRESSED);

                if (pressed) {
                    uint64_t hit_gid = 0; uint32_t hit_local = 0; uint32_t hit_sess = 0;
                    if (!pointer_grab_.is_active()) {
                        auto hit = cb.hit_test(cursor_x_, cursor_y_);
                        if (hit) {
                            hit_gid = hit->global_id; 
                            hit_local = hit->local_id; 
                            hit_sess = hit->session_id;
                        }
                    }
                    auto res = pointer_grab_.on_button_press(static_cast<uint8_t>(dev_idx), button, hit_gid, hit_local, hit_sess);
                    if (res.action == ButtonPressAction::NewGrabAndLogicalPress || 
                        res.action == ButtonPressAction::LogicalPressGrabContinued) {
                        safe_invoke(res.session_id, cb, [&]() { 
                            return cb.send_pointer_button(res.session_id, res.surface_local_id, button, 1, ev_time); 
                        });
                    }
                } else {
                    auto res = pointer_grab_.on_button_release(static_cast<uint8_t>(dev_idx), button);
                    if (res.action == ButtonReleaseAction::LogicalReleaseAndGrabTerminated || 
                        res.action == ButtonReleaseAction::LogicalReleaseGrabContinued) {
                        safe_invoke(res.session_id, cb, [&]() { 
                            return cb.send_pointer_button(res.session_id, res.surface_local_id, button, 0, ev_time); 
                        });
                    }
                }
                break;
            }
            case LIBINPUT_EVENT_KEYBOARD_KEY: {
                auto* k_ev = libinput_event_get_keyboard_event(ev);
                auto* dev = libinput_event_get_device(ev);
                int dev_idx = device_registry_.get_device_index(dev);
                if (dev_idx < 0) break;

                uint64_t ev_time = libinput_event_keyboard_get_time_usec(k_ev);
                last_event_time_us_ = std::max(last_event_time_us_, ev_time);
                uint32_t key = libinput_event_keyboard_get_key(k_ev);
                bool pressed = (libinput_event_keyboard_get_key_state(k_ev) == LIBINPUT_KEY_STATE_PRESSED);

                if (pressed) {
                    auto res = keyboard_state_.on_key_press(static_cast<uint8_t>(dev_idx), key);
                    if (res.action == KeyAction::LogicalPress) {
                        safe_invoke(res.session_id, cb, [&]() { 
                            return cb.send_keyboard_key(res.session_id, res.surface_local_id, key, 1, ev_time); 
                        });
                    }
                } else {
                    auto res = keyboard_state_.on_key_release(static_cast<uint8_t>(dev_idx), key);
                    if (res.action == KeyReleaseAction::LogicalRelease) {
                        safe_invoke(res.session_id, cb, [&]() { 
                            return cb.send_keyboard_key(res.session_id, res.surface_local_id, key, 0, ev_time); 
                        });
                    }
                }
                break;
            }
            default: break;
        }
    }

    SeatInputAdapter adapter_;
    SeatDeviceRegistry device_registry_;
    PointerGrabState pointer_grab_;
    KeyboardFocusState keyboard_state_;
    struct udev* udev_{nullptr};
    struct libinput* li_{nullptr};
    uint32_t screen_w_{0}, screen_h_{0};
    double cursor_x_{0.0}, cursor_y_{0.0};
    uint64_t last_event_time_us_{0};
};

} // namespace platform::input