#pragma once

#if defined(__linux__) && defined(YARILO_B2_TEST_CONTROL)

#include "test/b2_control.hpp"
#include "core/surface.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <sys/socket.h>
#include <unistd.h>

namespace yarilo::server {

namespace b2 = yarilo::test::b2;

constexpr size_t B2_EXPECTED_SURFACES = 2;
constexpr size_t B2_DISCONNECT_BATCH_SIZE = 1 + B2_EXPECTED_SURFACES;

// Ограниченная кольцевая FIFO-очередь на фиксированном массиве (zero dynamic allocations)
template <size_t Capacity>
class B2PacketQueue {
public:
    static_assert(Capacity > 0, "B2PacketQueue Capacity must be positive");

    [[nodiscard]] bool can_enqueue(size_t count) const noexcept {
        return count <= (Capacity - count_);
    }

    [[nodiscard]] bool push(const b2::Packet& pkt) noexcept {
        if (count_ >= Capacity) return false;
        data_[tail_] = pkt;
        tail_ = (tail_ + 1) % Capacity;
        ++count_;
        return true;
    }

    // Атомарное добавление пачки пакетов: либо все, либо ни одного
    [[nodiscard]] bool push_batch(std::span<const b2::Packet> batch) noexcept {
        if (!can_enqueue(batch.size())) return false;
        for (const auto& pkt : batch) {
            data_[tail_] = pkt;
            tail_ = (tail_ + 1) % Capacity;
            ++count_;
        }
        return true;
    }

    [[nodiscard]] const b2::Packet* peek() const noexcept {
        if (count_ == 0) return nullptr;
        return &data_[head_];
    }

    void drop_head() noexcept {
        if (count_ > 0) {
            head_ = (head_ + 1) % Capacity;
            --count_;
        }
    }

    [[nodiscard]] size_t size() const noexcept { return count_; }
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    [[nodiscard]] constexpr size_t capacity() const noexcept { return Capacity; }

    void clear() noexcept {
        head_ = 0;
        tail_ = 0;
        count_ = 0;
    }

private:
    std::array<b2::Packet, Capacity> data_{};
    size_t head_{0};
    size_t tail_{0};
    size_t count_{0};
};

// Проверка Register ACK координатора против SO_PEERCRED
inline bool verify_coordinator_ack(
    const b2::Packet& ack,
    uint64_t expected_request_seq,
    pid_t coordinator_pid,
    uid_t coordinator_uid,
    gid_t coordinator_gid) noexcept
{
    return ack.type == b2::Type::Register &&
           ack.role == b2::Role::Coordinator &&
           ack.status == static_cast<uint32_t>(b2::Status::Ok) &&
           ack.message_seq == expected_request_seq &&
           ack.pid == static_cast<uint64_t>(coordinator_pid) &&
           ack.uid == static_cast<uint64_t>(coordinator_uid) &&
           ack.gid == static_cast<uint64_t>(coordinator_gid);
}

// Транзакционная подготовка батча из 3 пакетов WmDisconnected + 2x SurfaceUnmapped
inline bool prepare_disconnect_batch(
    uint64_t epoch,
    const core::WmDisconnectReport& report,
    std::span<const core::UnmappedSurfaceInfo> unmapped_records,
    uint64_t current_msg_seq,
    pid_t server_pid,
    uid_t server_uid,
    gid_t server_gid,
    std::array<b2::Packet, B2_DISCONNECT_BATCH_SIZE>& out_batch) noexcept
{
    if (report.changed_count_overflow ||
        report.changed_count != B2_EXPECTED_SURFACES ||
        report.reported_count != B2_EXPECTED_SURFACES ||
        unmapped_records.size() < B2_EXPECTED_SURFACES) {
        return false;
    }

    if (B2_DISCONNECT_BATCH_SIZE > std::numeric_limits<uint64_t>::max() - current_msg_seq) {
        return false; // Защита от переполнения sequence
    }

    // 1. WmDisconnected
    auto& d = out_batch[0];
    d = b2::Packet{};
    d.type = b2::Type::WmDisconnected;
    d.role = b2::Role::Server;
    d.status = static_cast<uint32_t>(b2::Status::Ok);
    d.message_seq = current_msg_seq + 1;
    d.pid = static_cast<uint64_t>(server_pid);
    d.uid = static_cast<uint64_t>(server_uid);
    d.gid = static_cast<uint64_t>(server_gid);
    d.server_pid = static_cast<uint64_t>(server_pid);
    d.epoch = epoch;
    d.changed_count = report.changed_count;
    d.reported_count = report.reported_count;

    // 2 & 3. SurfaceUnmapped
    for (size_t i = 0; i < B2_EXPECTED_SURFACES; ++i) {
        auto& u = out_batch[i + 1];
        u = b2::Packet{};
        u.type = b2::Type::SurfaceUnmapped;
        u.role = b2::Role::Server;
        u.status = static_cast<uint32_t>(b2::Status::Ok);
        u.message_seq = current_msg_seq + 2 + i;
        u.pid = static_cast<uint64_t>(server_pid);
        u.uid = static_cast<uint64_t>(server_uid);
        u.gid = static_cast<uint64_t>(server_gid);
        u.server_pid = static_cast<uint64_t>(server_pid);
        u.epoch = epoch;
        u.session_id = unmapped_records[i].session_id;
        u.surface_id = unmapped_records[i].surface_id;
        u.server_uid = unmapped_records[i].server_uid;
    }

    return true;
}

// Проверка строгого перехода unmapped -> mapped для конкретной unmapped поверхности
inline bool should_queue_recovery_ready(
    bool was_mapped,
    bool is_now_mapped,
    bool recovery_active,
    bool already_queued,
    uint64_t active_wm_session,
    uint64_t calling_session,
    uint64_t global_surface_id,
    uint64_t surface_server_uid,
    const core::UnmappedSurfaceInfo& unmapped_record) noexcept
{
    if (was_mapped || !is_now_mapped) return false;
    if (!recovery_active || already_queued) return false;
    if (active_wm_session != calling_session) return false;

    const uint32_t sess_id = static_cast<uint32_t>(global_surface_id >> 32);
    const uint32_t surf_id = static_cast<uint32_t>(global_surface_id & 0xFFFFFFFFULL);

    return (unmapped_record.session_id == sess_id &&
            unmapped_record.surface_id == surf_id &&
            unmapped_record.server_uid == surface_server_uid);
}

// Формирование пакета SurfaceRecoveryReady
inline bool prepare_recovery_ready_packet(
    uint64_t epoch,
    uint64_t current_msg_seq,
    pid_t server_pid,
    uid_t server_uid,
    gid_t server_gid,
    uint32_t session_id,
    uint32_t surface_id,
    uint64_t server_uid_val,
    b2::Packet& out_pkt) noexcept
{
    if (current_msg_seq == std::numeric_limits<uint64_t>::max()) {
        return false;
    }

    out_pkt = b2::Packet{};
    out_pkt.type = b2::Type::SurfaceRecoveryReady;
    out_pkt.role = b2::Role::Server;
    out_pkt.status = static_cast<uint32_t>(b2::Status::Ok);
    out_pkt.message_seq = current_msg_seq + 1;
    out_pkt.pid = static_cast<uint64_t>(server_pid);
    out_pkt.uid = static_cast<uint64_t>(server_uid);
    out_pkt.gid = static_cast<uint64_t>(server_gid);
    out_pkt.server_pid = static_cast<uint64_t>(server_pid);
    out_pkt.epoch = epoch;
    out_pkt.session_id = session_id;
    out_pkt.surface_id = surface_id;
    out_pkt.server_uid = server_uid_val;

    return true;
}

} // namespace yarilo::server

#endif // defined(__linux__) && defined(YARILO_B2_TEST_CONTROL)