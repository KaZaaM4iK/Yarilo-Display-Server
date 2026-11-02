#include "server/b2_server_transport.hpp"

#include <cassert>
#include <iostream>
#include <limits>
#include <vector>

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << ": " #cond "\n"; \
        return 1; \
    } \
} while (false)

using namespace yarilo::server;
namespace b2 = yarilo::test::b2;

int test_queue_push_peek_drop() {
    B2PacketQueue<3> q;
    CHECK(q.empty());
    CHECK(q.size() == 0);
    CHECK(q.can_enqueue(3));
    CHECK(!q.can_enqueue(4));

    b2::Packet p1{}; p1.message_seq = 101;
    b2::Packet p2{}; p2.message_seq = 102;
    b2::Packet p3{}; p3.message_seq = 103;

    CHECK(q.push(p1));
    CHECK(q.push(p2));
    CHECK(q.push(p3));
    CHECK(q.size() == 3);
    CHECK(!q.can_enqueue(1));
    CHECK(!q.push(p1)); // overflow rejected

    CHECK(q.peek() != nullptr);
    CHECK(q.peek()->message_seq == 101);
    q.drop_head();
    CHECK(q.size() == 2);
    CHECK(q.peek()->message_seq == 102);

    // wrap-around test
    b2::Packet p4{}; p4.message_seq = 104;
    CHECK(q.push(p4));
    CHECK(q.size() == 3);

    CHECK(q.peek()->message_seq == 102); q.drop_head();
    CHECK(q.peek()->message_seq == 103); q.drop_head();
    CHECK(q.peek()->message_seq == 104); q.drop_head();
    CHECK(q.empty());
    CHECK(q.peek() == nullptr);

    return 0;
}

int test_batch_atomicity() {
    B2PacketQueue<3> q;
    b2::Packet p1{}; p1.message_seq = 1;
    CHECK(q.push(p1));
    CHECK(q.size() == 1);

    std::array<b2::Packet, 3> batch{};
    batch[0].message_seq = 2;
    batch[1].message_seq = 3;
    batch[2].message_seq = 4;

    // Batch size 3 does not fit in remaining capacity 2
    CHECK(!q.can_enqueue(3));
    CHECK(!q.push_batch(batch));
    // Atomicity check: queue size must remain exactly 1, no partial enqueue
    CHECK(q.size() == 1);
    CHECK(q.peek()->message_seq == 1);

    std::array<b2::Packet, 2> fitting_batch{};
    fitting_batch[0].message_seq = 20;
    fitting_batch[1].message_seq = 21;
    CHECK(q.can_enqueue(2));
    CHECK(q.push_batch(fitting_batch));
    CHECK(q.size() == 3);

    return 0;
}

int test_verify_ack() {
    b2::Packet ack{};
    ack.type = b2::Type::Register;
    ack.role = b2::Role::Coordinator;
    ack.status = static_cast<uint32_t>(b2::Status::Ok);
    ack.message_seq = 42;
    ack.pid = 1000;
    ack.uid = 1001;
    ack.gid = 1002;

    CHECK(verify_coordinator_ack(ack, 42, 1000, 1001, 1002));
    CHECK(!verify_coordinator_ack(ack, 43, 1000, 1001, 1002)); // seq mismatch
    CHECK(!verify_coordinator_ack(ack, 42, 9999, 1001, 1002)); // pid mismatch
    CHECK(!verify_coordinator_ack(ack, 42, 1000, 9999, 1002)); // uid mismatch
    CHECK(!verify_coordinator_ack(ack, 42, 1000, 1001, 9999)); // gid mismatch

    ack.status = static_cast<uint32_t>(b2::Status::Failed);
    CHECK(!verify_coordinator_ack(ack, 42, 1000, 1001, 1002)); // status != Ok

    ack.status = static_cast<uint32_t>(b2::Status::Ok);
    ack.role = b2::Role::Server;
    CHECK(!verify_coordinator_ack(ack, 42, 1000, 1001, 1002)); // role != Coordinator

    return 0;
}

int test_disconnect_batch_preparation() {
    yarilo::core::WmDisconnectReport report{
        .changed_count = 2,
        .reported_count = 2,
        .changed_count_overflow = false
    };

    std::array<yarilo::core::UnmappedSurfaceInfo, 2> records{{
        {.session_id = 10, .surface_id = 1, .server_uid = 1001},
        {.session_id = 20, .surface_id = 2, .server_uid = 2002}
    }};

    std::array<b2::Packet, B2_DISCONNECT_BATCH_SIZE> batch{};
    CHECK(prepare_disconnect_batch(1, report, records, 10, 500, 1000, 1000, batch));

    // Check packet 0: WmDisconnected
    CHECK(batch[0].type == b2::Type::WmDisconnected);
    CHECK(batch[0].role == b2::Role::Server);
    CHECK(batch[0].epoch == 1);
    CHECK(batch[0].changed_count == 2);
    CHECK(batch[0].reported_count == 2);
    CHECK(batch[0].message_seq == 11);

    // Check packet 1: SurfaceUnmapped 0
    CHECK(batch[1].type == b2::Type::SurfaceUnmapped);
    CHECK(batch[1].session_id == 10);
    CHECK(batch[1].surface_id == 1);
    CHECK(batch[1].server_uid == 1001);
    CHECK(batch[1].message_seq == 12);

    // Check packet 2: SurfaceUnmapped 1
    CHECK(batch[2].type == b2::Type::SurfaceUnmapped);
    CHECK(batch[2].session_id == 20);
    CHECK(batch[2].surface_id == 2);
    CHECK(batch[2].server_uid == 2002);
    CHECK(batch[2].message_seq == 13);

    // Error case 1: count != 2
    report.changed_count = 1;
    CHECK(!prepare_disconnect_batch(1, report, records, 10, 500, 1000, 1000, batch));
    report.changed_count = 2;

    // Error case 2: overflow flag
    report.changed_count_overflow = true;
    CHECK(!prepare_disconnect_batch(1, report, records, 10, 500, 1000, 1000, batch));
    report.changed_count_overflow = false;

    // Error case 3: sequence overflow protection
    uint64_t near_max_seq = std::numeric_limits<uint64_t>::max() - 1;
    CHECK(!prepare_disconnect_batch(1, report, records, near_max_seq, 500, 1000, 1000, batch));

    return 0;
}

int test_recovery_ready_predicates() {
    yarilo::core::UnmappedSurfaceInfo rec{
        .session_id = 42,
        .surface_id = 7,
        .server_uid = 9999
    };

    uint64_t gid = (static_cast<uint64_t>(42) << 32) | 7;

    // Standard valid recovery transition
    CHECK(should_queue_recovery_ready(
        false, true, true, false, 1, 1, gid, 9999, rec));

    // was_mapped == true -> must be false (already mapped, not a 0->1 transition)
    CHECK(!should_queue_recovery_ready(
        true, true, true, false, 1, 1, gid, 9999, rec));

    // is_now_mapped == false -> must be false
    CHECK(!should_queue_recovery_ready(
        false, false, true, false, 1, 1, gid, 9999, rec));

    // recovery_active == false -> must be false
    CHECK(!should_queue_recovery_ready(
        false, true, false, false, 1, 1, gid, 9999, rec));

    // already_queued == true -> must be false (strictly one event per surface)
    CHECK(!should_queue_recovery_ready(
        false, true, true, true, 1, 1, gid, 9999, rec));

    // caller is not active WM
    CHECK(!should_queue_recovery_ready(
        false, true, true, false, 1, 2, gid, 9999, rec));

    // mismatched server_uid
    CHECK(!should_queue_recovery_ready(
        false, true, true, false, 1, 1, gid, 8888, rec));

    // Packet preparation
    b2::Packet pkt{};
    CHECK(prepare_recovery_ready_packet(2, 50, 100, 1000, 1000, 42, 7, 9999, pkt));
    CHECK(pkt.type == b2::Type::SurfaceRecoveryReady);
    CHECK(pkt.message_seq == 51);
    CHECK(pkt.epoch == 2);
    CHECK(pkt.session_id == 42);
    CHECK(pkt.surface_id == 7);
    CHECK(pkt.server_uid == 9999);

    // Sequence overflow check
    CHECK(!prepare_recovery_ready_packet(2, std::numeric_limits<uint64_t>::max(), 100, 1000, 1000, 42, 7, 9999, pkt));

    return 0;
}

int main() {
    std::cout << "[Test] B2 Server Transport & Batch Queue...\n";
    if (test_queue_push_peek_drop() != 0) return 1;
    if (test_batch_atomicity() != 0) return 1;
    if (test_verify_ack() != 0) return 1;
    if (test_disconnect_batch_preparation() != 0) return 1;
    if (test_recovery_ready_predicates() != 0) return 1;
    std::cout << "[PASS] All B2 Server Transport tests passed.\n";
    return 0;
}