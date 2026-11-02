#include "server/orchestration.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <system_error>
#include <vector>
#include <iostream>

#define CHECK(condition) do { if (!(condition)) { std::cerr << "[FAIL] " << __FILE__ << ':' << __LINE__ << ": " #condition "\n"; return 1; } } while (false)

int main() {
    using yarilo::server::prepare_session_id;
    using yarilo::server::SessionPreparationStatus;
    using yarilo::server::process_wm_disconnect;
    using yarilo::server::try_advance_recovery_epoch;
    using yarilo::server::try_advance_session_id;

    uint32_t next_session = 99;
    const bool advanced_last_session = try_advance_session_id(UINT32_MAX - 1, next_session);
    CHECK(advanced_last_session);
    CHECK(next_session == UINT32_MAX);
    next_session = 99;
    const bool advanced_past_max_session = try_advance_session_id(UINT32_MAX, next_session);
    CHECK(!advanced_past_max_session);
    CHECK(next_session == 99);

    next_session = 77;
    bool credential_check_called = false;
    const auto rejected_credentials = prepare_session_id(10, next_session, [&] {
        credential_check_called = true;
        return false;
    });
    CHECK(rejected_credentials == SessionPreparationStatus::CredentialsRejected);
    CHECK(credential_check_called && next_session == 77);
    const auto prepared = prepare_session_id(10, next_session, [] { return true; });
    CHECK(prepared == SessionPreparationStatus::Ready && next_session == 11);
    next_session = 77;
    const auto exhausted = prepare_session_id(UINT32_MAX, next_session, [] { return true; });
    CHECK(exhausted == SessionPreparationStatus::SessionIdExhausted && next_session == 77);

    uint64_t next_epoch = 99;
    const bool advanced_last_epoch = try_advance_recovery_epoch(UINT64_MAX - 1, next_epoch);
    CHECK(advanced_last_epoch);
    CHECK(next_epoch == UINT64_MAX);
    next_epoch = 99;
    const bool advanced_past_max_epoch = try_advance_recovery_epoch(UINT64_MAX, next_epoch);
    CHECK(!advanced_past_max_epoch);
    CHECK(next_epoch == 99);

    std::optional<uint32_t> owner = 7;
    uint64_t epoch = 4;
    bool test_failed = false;
    std::vector<int> order;
    int core_calls = 0;
    int input_calls = 0;
    int repaint_calls = 0;
    bool not_owner_epoch_called = false;
    const auto not_owner = process_wm_disconnect(
        owner, 8,
        [&] { not_owner_epoch_called = true; order.push_back(1); return false; },
        [&] { ++core_calls; order.push_back(2); },
        [&] { ++input_calls; order.push_back(3); return std::error_code{}; },
        [&] { ++repaint_calls; order.push_back(4); });
    CHECK(!not_owner.was_active_owner);
    CHECK(!not_owner_epoch_called);
    CHECK(owner == 7 && epoch == 4);
    CHECK(order.empty() && core_calls == 0 && input_calls == 0 && repaint_calls == 0);

    bool owner_reset_before_epoch = false;
    bool owner_reset_before_core = false;
    bool core_before_input = false;
    bool input_before_repaint = false;
    const auto active = process_wm_disconnect(
        owner, 7,
        [&] {
            owner_reset_before_epoch = !owner.has_value();
            order.push_back(1);
            ++epoch; // Represents the B2-only epoch policy supplied by main.cpp.
            return false;
        },
        [&] {
            owner_reset_before_core = !owner.has_value();
            ++core_calls;
            order.push_back(2);
        },
        [&] {
            core_before_input = core_calls == 1;
            ++input_calls;
            order.push_back(3);
            return std::error_code{};
        },
        [&] {
            input_before_repaint = input_calls == 1;
            ++repaint_calls;
            order.push_back(4);
        });
    CHECK(owner_reset_before_epoch && owner_reset_before_core && core_before_input && input_before_repaint);
    CHECK(active.was_active_owner && !active.epoch_overflow && !active.input_error);
    CHECK(!owner && epoch == 5);
    CHECK((order == std::vector<int>{1, 2, 3, 4}));

    order.clear();
    const auto repeated = process_wm_disconnect(
        owner, 7,
        [&] { order.push_back(1); return false; },
        [&] { order.push_back(2); },
        [&] { order.push_back(3); return std::error_code{}; },
        [&] { order.push_back(4); });
    CHECK(!repeated.was_active_owner && order.empty() && epoch == 5);

    owner = 12;
    uint64_t overflow_epoch = UINT64_MAX;
    bool overflow_counter_rejected = false;
    bool overflow_owner_was_reset = false;
    bool overflow_core_preceded_input = false;
    bool overflow_input_preceded_repaint = false;
    const auto overflow = process_wm_disconnect(
        owner, 12,
        [&] {
            overflow_owner_was_reset = !owner.has_value();
            uint64_t next = 0;
            overflow_counter_rejected = !try_advance_recovery_epoch(overflow_epoch, next) &&
                                        next == 0 && overflow_epoch == UINT64_MAX;
            test_failed = overflow_counter_rejected;
            order.push_back(1);
            return overflow_counter_rejected;
        },
        [&] { ++core_calls; order.push_back(2); },
        [&] {
            overflow_core_preceded_input = core_calls == 2;
            ++input_calls;
            order.push_back(3);
            return std::make_error_code(std::errc::io_error);
        },
        [&] { overflow_input_preceded_repaint = input_calls == 2; ++repaint_calls; order.push_back(4); });
    CHECK(overflow_owner_was_reset && overflow_core_preceded_input && overflow_input_preceded_repaint);
    CHECK(overflow.was_active_owner && overflow.epoch_overflow);
    CHECK(overflow_counter_rejected && overflow_epoch == UINT64_MAX);
    CHECK(test_failed); // Locally latched B2 failure; cleanup still completed.
    CHECK(overflow.input_error == std::make_error_code(std::errc::io_error));
    CHECK(!owner);
    CHECK((order == std::vector<int>{1, 2, 3, 4}));
    CHECK(core_calls == 2 && input_calls == 2 && repaint_calls == 2);
}
