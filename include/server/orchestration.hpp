#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <system_error>
#include <utility>

namespace yarilo::server {

inline bool try_advance_session_id(uint32_t current, uint32_t& next) noexcept {
    if (current == std::numeric_limits<uint32_t>::max()) return false;
    next = current + 1;
    return true;
}

inline bool try_advance_recovery_epoch(uint64_t current, uint64_t& next) noexcept {
    if (current == std::numeric_limits<uint64_t>::max()) return false;
    next = current + 1;
    return true;
}

enum class SessionPreparationStatus {
    Ready,
    CredentialsRejected,
    SessionIdExhausted,
};

template <typename CredentialsCheck>
SessionPreparationStatus prepare_session_id(
    uint32_t current_session_id,
    uint32_t& candidate_session_id,
    CredentialsCheck&& credentials_check)
{
    if (!std::forward<CredentialsCheck>(credentials_check)())
        return SessionPreparationStatus::CredentialsRejected;
    if (!try_advance_session_id(current_session_id, candidate_session_id))
        return SessionPreparationStatus::SessionIdExhausted;
    return SessionPreparationStatus::Ready;
}

struct WmDisconnectOutcome {
    bool was_active_owner{false};
    bool epoch_overflow{false};
    std::error_code input_error{};
};

template <typename EpochStep, typename CoreCleanup, typename InputCleanup, typename Repaint>
WmDisconnectOutcome process_wm_disconnect(
    std::optional<uint32_t>& active_wm_session_id,
    uint32_t closing_session_id,
    EpochStep&& advance_epoch,
    CoreCleanup&& core_cleanup,
    InputCleanup&& input_cleanup,
    Repaint&& repaint)
{
    WmDisconnectOutcome outcome{};
    if (!active_wm_session_id || *active_wm_session_id != closing_session_id) return outcome;

    outcome.was_active_owner = true;
    active_wm_session_id.reset();
    outcome.epoch_overflow = std::forward<EpochStep>(advance_epoch)();
    std::forward<CoreCleanup>(core_cleanup)();
    outcome.input_error = std::forward<InputCleanup>(input_cleanup)();
    std::forward<Repaint>(repaint)();
    return outcome;
}

} // namespace yarilo::server
