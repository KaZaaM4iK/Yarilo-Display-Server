#include "platform/seat.hpp"
#include "platform/drm/output.hpp"
#include "platform/input/input_subsystem.hpp"
#include "ipc/server_socket.hpp"
#include "core/surface.hpp"
#include "core/geometry.hpp"
#include "server/orchestration.hpp"
#include "shm/validator.hpp"
#if defined(__linux__) && defined(YARILO_B2_TEST_CONTROL)
#include "test/b2_control.hpp"
#endif
#include <xf86drm.h>
#include <print>
#include <csignal>
#include <poll.h>
#include <chrono>
#include <thread>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <vector>
#include <deque>
#include <unordered_map>
#include <string>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <optional>
#include <array>
#include <span>
#include <algorithm>
#include <sys/stat.h>
#include <utility>
#include <unistd.h>
#include <new>
#include <exception>

#if defined(__linux__) && defined(YARILO_B2_TEST_CONTROL)
namespace b2 = yarilo::test::b2;
#endif

static volatile std::sig_atomic_t g_running = 1;
static volatile std::sig_atomic_t g_shutdown_requested = 0;
static volatile std::sig_atomic_t g_drm_handler_error = 0;

static void sig_handler(int) {
    g_shutdown_requested = 1;
    g_running = 0;
}

static core::Result<std::pair<core::SeatDevice, std::unique_ptr<platform::drm::DrmOutput>>> 
setup_active_pipeline(platform::Seat& seat) {
    constexpr int max_devices = 16;
    drmDevicePtr devices[max_devices];
    int num_devices = drmGetDevices2(0, devices, max_devices);
    if (num_devices <= 0) {
        return std::unexpected(core::make_error(core::ServerError::DeviceNotFound));
    }

    int device_count = std::min(num_devices, max_devices);
    std::vector<std::string> candidates;
    for (int i = 0; i < device_count; ++i) {
        if (devices[i] && (devices[i]->available_nodes & (1U << DRM_NODE_PRIMARY))) {
            candidates.emplace_back(devices[i]->nodes[DRM_NODE_PRIMARY]);
        }
    }
    drmFreeDevices(devices, device_count);

    for (const auto& path : candidates) {
        auto dev_res = seat.open_device(path.c_str());
        if (!dev_res) continue;

        auto output_res = platform::drm::DrmOutput::create_and_bind(dev_res->fd());
        if (output_res) {
            return std::make_pair(std::move(*dev_res), std::move(*output_res));
        }
    }
    return std::unexpected(core::make_error(core::ServerError::NoConnectedOutput));
}

enum class ClientRole {
    Unauthenticated,
    Application,
    WindowManager
};

#if defined(__linux__) && defined(YARILO_B2_TEST_CONTROL)
struct PeerCredentials {
    pid_t pid{0};
    uid_t uid{0};
    gid_t gid{0};
};
#endif

struct PendingOutPacket {
    yarilo::ipc::MessageType type;
    uint64_t sequence{0};
    std::vector<std::byte> payload;
    uint64_t map_req_global_id{0};
    uint64_t map_req_server_uid{0};
};

struct ClientSession {
    uint32_t session_id{0};
#if defined(__linux__) && defined(YARILO_B2_TEST_CONTROL)
    PeerCredentials peer_credentials{};
#endif
    core::UniqueFd socket_fd;
    ClientRole role{ClientRole::Unauthenticated};
    std::deque<PendingOutPacket> out_queue;
    std::deque<core::MapRequestInfo> pending_wm_maps;
    bool is_closing{false};
    bool close_after_flush{false};
};

struct PresentationTarget {
    uint32_t session_id{0};
    uint32_t surface_id{0};
    uint64_t commit_seq{0};
    uint64_t global_surface_id{0};
};

struct PreparedFlip {
    std::vector<PresentationTarget> targets;
};

struct SnapshotLeaseDrainer {
    core::SurfaceManager& sm;
    std::span<const core::RenderNode> nodes;
    size_t next_idx{0};

    SnapshotLeaseDrainer(core::SurfaceManager& m, std::span<const core::RenderNode> n) noexcept
        : sm(m), nodes(n) {}

    ~SnapshotLeaseDrainer() noexcept {
        drain();
    }

    std::optional<core::ReleaseInfo> release_current() noexcept {
        if (next_idx >= nodes.size()) return std::nullopt;
        uint64_t id = nodes[next_idx++].lease_id;
        return sm.release_read_lease(id);
    }

    void drain() noexcept {
        while (next_idx < nodes.size()) {
            (void)sm.release_read_lease(nodes[next_idx++].lease_id);
        }
    }
};

static void page_flip_handler(int /*fd*/, unsigned int sequence, unsigned int tv_sec, unsigned int tv_usec, void* user_data) noexcept {
    try {
        auto* output = static_cast<platform::drm::DrmOutput*>(user_data);
        if (output) {
            output->handle_page_flip_event(sequence, tv_sec, tv_usec);
        }
    } catch (...) {
        g_drm_handler_error = 1;
    }
}

int main() {
    ::umask(0077);

    std::signal(SIGINT, sig_handler);
    std::signal(SIGTERM, sig_handler);

    int exit_code = 0;
    bool drm_fault = false;

    std::println("=================================================");
    std::println("   Yarilo Display Server v0.6 (Input Subsystem)  ");
    std::println("=================================================");

    std::println("[1/6] Initializing seat & KMS output...");
    
    std::unique_ptr<platform::Seat> seat;
    core::SeatDevice seat_device;
    std::unique_ptr<platform::drm::DrmOutput> output;

    auto seat_res = platform::Seat::open([](bool active) {
        std::println("  -> [Seat Event] State: active = {}", active);
    });
    if (!seat_res) return 1;
    seat = std::move(*seat_res);

    if (auto act_res = seat->wait_for_activation(2000); !act_res) return 1;

    auto pipeline_res = setup_active_pipeline(*seat);
    if (!pipeline_res) return 1;
    seat_device = std::move(pipeline_res->first);
    output = std::move(pipeline_res->second);

    std::println("  -> KMS Online: {}x{} @ {}Hz (Hardware Double-Buffering Active)", 
                 output->width(), output->height(), output->refresh_rate());

    constexpr uint32_t CORAL_XRGB = 0x00FF7F50;
    if (auto res = output->fill_color(CORAL_XRGB); !res) {
        auto restore_res = output->restore_crtc(seat->is_active());
        if (!restore_res) exit_code = 1;
        output->free_resources();
        return exit_code != 0 ? exit_code : 1;
    }

    // Состояние инфраструктуры сервера, объявляемое до точки защищенного рантайма
    std::unique_ptr<yarilo::ipc::ServerSocket> server_socket;
    std::unique_ptr<platform::input::InputSubsystem> input_subsystem;
    core::SurfaceManager surface_manager;
    std::unordered_map<int, ClientSession> sessions;
    std::unordered_map<uint64_t, uint64_t> surface_last_presented_seq;
    std::optional<uint32_t> active_wm_session_id;
    uint32_t session_id_counter = 0;
#if defined(__linux__) && defined(YARILO_B2_TEST_CONTROL)
    uint64_t recovery_epoch = 0;
    bool b2_test_failed = false;
    bool b2_registered = false;
    bool b2_drain_mode = false;
    bool b2_drain_complete = false;
    bool b2_normal_shutdown_requested = false;
    std::chrono::steady_clock::time_point b2_queue_drain_deadline{};
    constexpr auto B2_DRAIN_TIMEOUT = std::chrono::seconds(3);
    constexpr size_t B2_EXPECTED_SURFACE_COUNT = 2;
    constexpr size_t B2_UNMAPPED_REPORT_CAPACITY = B2_EXPECTED_SURFACE_COUNT;
    static_assert(B2_EXPECTED_SURFACE_COUNT * 2 + 1 <= 8, "B2 event FIFO must hold disconnect and recovery packets");
    std::array<core::UnmappedSurfaceInfo, B2_UNMAPPED_REPORT_CAPACITY> b2_unmapped_report{};
    std::array<bool, B2_EXPECTED_SURFACE_COUNT> b2_recovery_ready_queued{};
    bool b2_recovery_report_active{false};
    core::WmDisconnectReport b2_disconnect_report{};
    auto mark_b2_failed = [&](const char* reason) noexcept {
        b2_test_failed = true;
        try { std::println(stderr, "B2 FAIL: {}", reason); } catch (...) {}
    };
#endif

    auto advance_disconnect_epoch = [&]() noexcept {
#if defined(__linux__) && defined(YARILO_B2_TEST_CONTROL)
        uint64_t next_epoch{};
        if (!yarilo::server::try_advance_recovery_epoch(recovery_epoch, next_epoch)) {
            mark_b2_failed("recovery_epoch overflow");
            return true;
        }
        recovery_epoch = next_epoch;
#endif
        return false;
    };

#if defined(__linux__) && defined(YARILO_B2_TEST_CONTROL)
    constexpr uint64_t B2_REGISTER_MESSAGE_SEQ = 1;
    std::array<b2::Packet, 8> b2_out_queue{};
    size_t b2_out_queue_head{0};
    size_t b2_out_queue_count{0};
    std::optional<size_t> b2_poll_index;
    int b2_coordinator_fd{-1};
    uint64_t b2_message_seq{B2_REGISTER_MESSAGE_SEQ};
    bool b2_shutdown_requested{false};

    auto b2_flush_queue = [&]() -> bool {
        if (b2_coordinator_fd < 0) {
            mark_b2_failed("B2 queue flush without Coordinator connection");
            return false;
        }
        while (b2_out_queue_count > 0) {
            const b2::Packet& packet = b2_out_queue[b2_out_queue_head];
            if (!b2::send(b2_coordinator_fd, packet)) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return true;
                mark_b2_failed("B2 send failed");
                return false;
            }
            b2_out_queue_head = (b2_out_queue_head + 1) % b2_out_queue.size();
            --b2_out_queue_count;
        }
        return true;
    };

    auto b2_can_enqueue = [&](size_t count) noexcept {
        return count <= b2_out_queue.size() - b2_out_queue_count;
    };

    auto b2_enqueue_batch = [&](std::span<const b2::Packet> packets) -> bool {
        if (packets.size() > b2_out_queue.size() - b2_out_queue_count) {
            mark_b2_failed("B2 out queue cannot atomically fit packet batch");
            return false;
        }
        for (const auto& packet : packets) {
            const size_t tail = (b2_out_queue_head + b2_out_queue_count) % b2_out_queue.size();
            b2_out_queue[tail] = packet;
            ++b2_out_queue_count;
        }
        return true;
    };

    auto b2_queue_packet = [&](const b2::Packet& packet) -> bool {
        if (b2_drain_mode || b2_shutdown_requested) {
            mark_b2_failed("attempted to enqueue B2 packet after drain began");
            return false;
        }
        return b2_enqueue_batch(std::span<const b2::Packet>(&packet, 1));
    };

#endif

    std::optional<PreparedFlip> prepared_flip;
    std::vector<PresentationTarget> active_flip_targets;
    int flip_retry_count = 0;
    int blit_retry_count = 0;
    constexpr int MAX_FLIP_RETRIES = 5;
    constexpr int MAX_BLIT_RETRIES = 3;
    constexpr size_t MAX_OUT_QUEUE_DEPTH = 256;

    bool needs_repaint = false;
    auto trigger_repaint = [&]() {
        needs_repaint = true;
        prepared_flip.reset();
    };

    uint64_t frames_rendered        = 0;
    uint64_t flips_completed        = 0;
    uint64_t presentations_sent     = 0;
    uint64_t commits_superseded     = 0;

    drmEventContext drm_ev{};
    drm_ev.version = DRM_EVENT_CONTEXT_VERSION;
    drm_ev.page_flip_handler = page_flip_handler;

    // ГЛОБАЛЬНАЯ ГРАНИЦА ИСКЛЮЧЕНИЙ: покрывает всю работу с момента активации видеорежима KMS
    try {
        std::println("[2/6] Initializing IPC Socket...");
        auto socket_res = yarilo::ipc::ServerSocket::create();
        if (!socket_res) {
            std::println(stderr, "Fatal: ServerSocket creation failed: {}", socket_res.error().message());
            exit_code = 1;
            goto server_teardown;
        }
        server_socket = std::move(*socket_res);
        std::println("  -> Server listening on $XDG_RUNTIME_DIR/yarilo-0");

#if defined(__linux__) && defined(YARILO_B2_TEST_CONTROL)
        const std::string b2_path = b2::socket_path();
        if (b2_path.empty() || b2_path.size() >= sizeof(sockaddr_un::sun_path)) {
            mark_b2_failed("Invalid Coordinator socket path");
            exit_code = 1;
            goto server_teardown;
        }
        const auto connect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        int connect_error = ENOENT;
        bool connected = false;
        while (!connected && std::chrono::steady_clock::now() < connect_deadline) {
            b2_coordinator_fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
            if (b2_coordinator_fd < 0) {
                connect_error = errno;
            } else {
                sockaddr_un b2_addr{};
                b2_addr.sun_family = AF_UNIX;
                std::memcpy(b2_addr.sun_path, b2_path.c_str(), b2_path.size() + 1);
                const int rc = ::connect(b2_coordinator_fd, reinterpret_cast<sockaddr*>(&b2_addr), sizeof(b2_addr));
                if (rc == 0) {
                    connected = true;
                } else if (errno == EINPROGRESS || errno == EAGAIN || errno == EWOULDBLOCK) {
                    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                        connect_deadline - std::chrono::steady_clock::now()).count();
                    pollfd connect_pfd{.fd = b2_coordinator_fd, .events = POLLOUT, .revents = 0};
                    const int ready = ::poll(&connect_pfd, 1, static_cast<int>(std::max<int64_t>(0, left)));
                    if (ready > 0 && (connect_pfd.revents & POLLOUT)) {
                        int socket_error{};
                        socklen_t error_size = sizeof(socket_error);
                        if (::getsockopt(b2_coordinator_fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_size) == 0 &&
                            error_size == sizeof(socket_error) && socket_error == 0) {
                            connected = true;
                        } else {
                            connect_error = socket_error != 0 ? socket_error : errno;
                        }
                    } else if (ready < 0 && errno != EINTR) {
                        connect_error = errno;
                    }
                } else {
                    connect_error = errno;
                }
                if (!connected) {
                    ::close(b2_coordinator_fd);
                    b2_coordinator_fd = -1;
                }
            }
            if (!connected && std::chrono::steady_clock::now() < connect_deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!connected) {
            mark_b2_failed("Coordinator nonblocking connect deadline expired");
            std::println(stderr, "B2 Coordinator connect failed: {}", std::strerror(connect_error));
            exit_code = 1;
            goto server_teardown;
        }

        pid_t coordinator_pid{};
        uid_t coordinator_uid{};
        gid_t coordinator_gid{};
        if (!b2::get_peer_credentials(b2_coordinator_fd, coordinator_pid, coordinator_uid, coordinator_gid) ||
            coordinator_uid != ::getuid() || coordinator_gid != ::getgid()) {
            mark_b2_failed("Coordinator SO_PEERCRED mismatch");
            exit_code = 1;
            goto server_teardown;
        }
        b2::Packet register_request{};
        register_request.type = b2::Type::Register;
        register_request.role = b2::Role::Server;
        register_request.status = static_cast<uint32_t>(b2::Status::Ok);
        register_request.message_seq = B2_REGISTER_MESSAGE_SEQ;
        register_request.pid = static_cast<uint64_t>(::getpid());
        register_request.uid = static_cast<uint64_t>(::getuid());
        register_request.gid = static_cast<uint64_t>(::getgid());
        register_request.server_pid = static_cast<uint64_t>(::getpid());
        const auto register_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        bool register_sent = false;
        while (std::chrono::steady_clock::now() < register_deadline) {
            if (b2::send(b2_coordinator_fd, register_request)) { register_sent = true; break; }
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) break;
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                register_deadline - std::chrono::steady_clock::now()).count();
            pollfd retry_fd{.fd = b2_coordinator_fd, .events = POLLOUT, .revents = 0};
            const int ready = ::poll(&retry_fd, 1, static_cast<int>(std::max<int64_t>(0, std::min<int64_t>(50, left))));
            if (ready < 0 && errno != EINTR) break;
            if (ready > 0 && (retry_fd.revents & (POLLERR | POLLHUP | POLLNVAL))) break;
        }
        b2::Packet register_ack{};
        bool ack_received = false;
        while (register_sent && std::chrono::steady_clock::now() < register_deadline) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                register_deadline - std::chrono::steady_clock::now()).count();
            pollfd retry_fd{.fd = b2_coordinator_fd, .events = POLLIN, .revents = 0};
            const int ready = ::poll(&retry_fd, 1, static_cast<int>(std::max<int64_t>(0, std::min<int64_t>(50, left))));
            if (ready < 0 && errno == EINTR) continue;
            if (ready <= 0) continue;
            if (retry_fd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
            if (retry_fd.revents & POLLIN) {
                if (b2::receive(b2_coordinator_fd, register_ack)) { ack_received = true; break; }
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                break;
            }
        }
        if (!ack_received || register_ack.type != b2::Type::Register ||
            register_ack.role != b2::Role::Coordinator ||
            register_ack.status != static_cast<uint32_t>(b2::Status::Ok) ||
            register_ack.message_seq != register_request.message_seq ||
            register_ack.pid != static_cast<uint64_t>(coordinator_pid) ||
            register_ack.uid != static_cast<uint64_t>(coordinator_uid) ||
            register_ack.gid != static_cast<uint64_t>(coordinator_gid)) {
            mark_b2_failed("Register ACK invalid or timeout");
            exit_code = 1;
            goto server_teardown;
        }
        b2_registered = true;
#endif

        std::println("[3/6] Initializing Input Subsystem (libinput)...");
        auto input_res = platform::input::InputSubsystem::create(*seat, output->width(), output->height());
        if (!input_res) {
            std::println(stderr, "Fatal: InputSubsystem creation failed: {}", input_res.error().message());
            exit_code = 1;
            goto server_teardown;
        }
        input_subsystem = std::move(*input_res);
        std::println("  -> Input Subsystem initialized: Seat admission & Multi-mouse/keyboard active");

        std::println("[4/6] Ready for Hardware Page-Flipping & Input Events. Awaiting test completion or shutdown request...");
#if defined(__linux__) && defined(YARILO_B2_TEST_CONTROL)
        if (!b2_registered || b2_coordinator_fd < 0) {
            mark_b2_failed("Reactor startup without registered Coordinator control socket");
            exit_code = 1;
            goto server_teardown;
        }
#endif

        auto get_session_by_id = [&](uint32_t s_id) -> ClientSession* {
            for (auto& [fd, sess] : sessions) {
                if (sess.session_id == s_id) return &sess;
            }
            return nullptr;
        };

        auto queue_packet = [&](ClientSession& sess, yarilo::ipc::MessageType type, uint64_t seq,
                                std::span<const std::byte> payload, uint64_t map_gid = 0, uint64_t map_uid = 0) -> bool {
            if (sess.is_closing) return true;
            
            if (sess.out_queue.size() >= MAX_OUT_QUEUE_DEPTH) {
                try { std::println(stderr, "  -> [IPC] Disconnecting session {}: out_queue overflow", sess.session_id); } catch (...) {}
                sess.is_closing = true;
                sess.out_queue.clear();
                return false;
            }
            
            try {
                sess.out_queue.push_back(PendingOutPacket{
                    .type = type,
                    .sequence = seq,
                    .payload = std::vector<std::byte>(payload.begin(), payload.end()),
                    .map_req_global_id = map_gid,
                    .map_req_server_uid = map_uid
                });
                return true;
            } catch (...) {
                sess.is_closing = true;
                sess.out_queue.clear();
                return false;
            }
        };

        auto pump_wm_queue = [&](ClientSession& sess) {
            if (sess.role != ClientRole::WindowManager) return;
            
            while (!sess.pending_wm_maps.empty() && sess.out_queue.size() < MAX_OUT_QUEUE_DEPTH) {
                const auto& req = sess.pending_wm_maps.front();
                
                std::byte m_buf[yarilo::ipc::WmMapRequestPayload::ENCODED_SIZE];
                yarilo::ipc::WmMapRequestPayload req_p{
                    .global_surface_id = req.global_id,
                    .server_uid = req.server_uid,
                    .width = req.width,
                    .height = req.height
                };
                
                if (!req_p.encode(m_buf)) {
                    sess.is_closing = true;
                    sess.out_queue.clear();
                    break;
                }

                if (!queue_packet(sess, yarilo::ipc::MessageType::WmMapRequest, 0, m_buf,
                                  req.global_id, req.server_uid)) {
                    break;
                }

                sess.pending_wm_maps.pop_front();
            }
        };

        auto queue_buffer_released = [&](const core::ReleaseInfo& rel) {
            if (auto* sess = get_session_by_id(rel.session_id)) {
                std::byte buf[yarilo::ipc::BufferReleasedPayload::ENCODED_SIZE];
                yarilo::ipc::BufferReleasedPayload p{
                    .surface_id = rel.local_surface_id,
                    .buffer_id = rel.buffer_id,
                    .commit_seq = rel.commit_seq
                };
                if (p.encode(buf)) {
                    (void)queue_packet(*sess, yarilo::ipc::MessageType::BufferReleased, rel.commit_seq, buf);
                }
            }
        };

        auto notify_wm_surface_destroyed = [&](uint64_t global_id) {
            if (!active_wm_session_id.has_value()) return;
            if (auto* wsess = get_session_by_id(*active_wm_session_id)) {
                std::byte buf[yarilo::ipc::WmSurfaceDestroyedPayload::ENCODED_SIZE];
                yarilo::ipc::WmSurfaceDestroyedPayload p{ .global_surface_id = global_id };
                if (p.encode(buf)) {
                    (void)queue_packet(*wsess, yarilo::ipc::MessageType::WmSurfaceDestroyed, 0, buf);
                }
            }
        };

        auto cancel_pending_wm_map_requests = [&](uint64_t gid, uint64_t suid) {
            if (!active_wm_session_id.has_value()) return;
            if (auto* wsess = get_session_by_id(*active_wm_session_id)) {
                std::erase_if(wsess->out_queue, [gid, suid](const PendingOutPacket& pkt) {
                    return pkt.type == yarilo::ipc::MessageType::WmMapRequest &&
                           pkt.map_req_global_id == gid &&
                           pkt.map_req_server_uid == suid;
                });
                std::erase_if(wsess->pending_wm_maps, [gid, suid](const core::MapRequestInfo& req) {
                    return req.global_id == gid && req.server_uid == suid;
                });
            }
        };

        auto flush_out_queue = [&](ClientSession& sess) {
            if (sess.is_closing) {
                sess.out_queue.clear();
                return;
            }

            while (!sess.out_queue.empty()) {
                auto& pkt = sess.out_queue.front();
                auto res = yarilo::ipc::ServerSocket::send_packet(
                    sess.socket_fd.get(), pkt.type, pkt.sequence,
                    pkt.payload.data(), pkt.payload.size());
                    
                if (!res) {
                    if (res.error() == core::make_error(core::ServerError::WouldBlock)) {
                        break;
                    }
                    sess.is_closing = true;
                    sess.out_queue.clear();
                    break;
                }
                
                if (pkt.map_req_server_uid != 0) {
                    surface_manager.mark_map_request_sent(pkt.map_req_global_id, pkt.map_req_server_uid);
                }
                if (pkt.type == yarilo::ipc::MessageType::FramePresented) {
                    presentations_sent++;
                }
                sess.out_queue.pop_front();
            }

            if (sess.out_queue.empty() && sess.close_after_flush) {
                sess.is_closing = true;
            }
        };

        auto send_protocol_error = [&](ClientSession& sess, uint64_t seq, uint32_t error_code) {
            std::byte err_payload[yarilo::ipc::ProtocolErrorPayload::ENCODED_SIZE];
            yarilo::ipc::write_le32(err_payload, error_code);
            (void)queue_packet(sess, yarilo::ipc::MessageType::ProtocolError, seq, err_payload);
            sess.close_after_flush = true;
            flush_out_queue(sess);
        };

        output->set_flip_callback([&](uint64_t, uint32_t, uint64_t, uint64_t vblank_ns) {
            flips_completed++;
            for (const auto& target : active_flip_targets) {
                if (auto* sess = get_session_by_id(target.session_id)) {
                    std::byte pres_payload[yarilo::ipc::FramePresentedPayload::ENCODED_SIZE];
                    yarilo::ipc::FramePresentedPayload p{
                        .surface_id = target.surface_id,
                        .commit_seq = target.commit_seq,
                        .presentation_time_ns = vblank_ns,
                    };
                    if (p.encode(pres_payload)) {
                        (void)queue_packet(*sess, yarilo::ipc::MessageType::FramePresented, target.commit_seq, pres_payload);
                    }
                    try {
                        surface_last_presented_seq[target.global_surface_id] = target.commit_seq;
                    } catch (...) {
                        g_drm_handler_error = 1;
                    }
                }
            }
            active_flip_targets.clear();
        });

        platform::input::InputEventCallbacks input_callbacks{
            .hit_test = [&](double cx, double cy) -> std::optional<platform::input::HitTestResult> {
                return surface_manager.hit_test(cx, cy);
            },
            
            .get_surface_origin = [&](uint64_t gid) -> std::optional<std::pair<int32_t, int32_t>> {
                return surface_manager.get_surface_origin(gid);
            },

            .send_pointer_motion = [&](uint32_t s_id, uint32_t surf_id, int32_t sx, int32_t sy, uint64_t time_us) -> bool {
                auto* sess = get_session_by_id(s_id);
                if (!sess) return false;
                std::byte buf[yarilo::ipc::PointerMotionPayload::ENCODED_SIZE];
                yarilo::ipc::PointerMotionPayload p{
                    .surface_id = surf_id, .surface_x = sx, .surface_y = sy, .pad = 0, .timestamp_us = time_us
                };
                if (!p.encode(buf)) return false;
                return queue_packet(*sess, yarilo::ipc::MessageType::PointerMotion, 0, buf);
            },

            .send_pointer_button = [&](uint32_t s_id, uint32_t surf_id, uint32_t btn, uint32_t state, uint64_t time_us) -> bool {
                auto* sess = get_session_by_id(s_id);
                if (!sess) return false;
                std::byte buf[yarilo::ipc::PointerButtonPayload::ENCODED_SIZE];
                yarilo::ipc::PointerButtonPayload p{
                    .surface_id = surf_id, .button = btn, .state = state, .pad = 0, .timestamp_us = time_us
                };
                if (!p.encode(buf)) return false;
                return queue_packet(*sess, yarilo::ipc::MessageType::PointerButton, 0, buf);
            },

            .send_keyboard_key = [&](uint32_t s_id, uint32_t surf_id, uint32_t key, uint32_t state, uint64_t time_us) -> bool {
                auto* sess = get_session_by_id(s_id);
                if (!sess) return false;
                std::byte buf[yarilo::ipc::KeyboardKeyPayload::ENCODED_SIZE];
                yarilo::ipc::KeyboardKeyPayload p{
                    .surface_id = surf_id, .key = key, .state = state, .pad = 0, .timestamp_us = time_us
                };
                if (!p.encode(buf)) return false;
                return queue_packet(*sess, yarilo::ipc::MessageType::KeyboardKey, 0, buf);
            },

            .dispatch_synthetic_pointer_releases = [&](const platform::input::SyntheticPointerReleaseBatch& batch) -> bool {
                auto* sess = get_session_by_id(batch.session_id);
                if (!sess) return false;
                for (size_t i = 0; i < batch.count; ++i) {
                    std::byte buf[yarilo::ipc::PointerButtonPayload::ENCODED_SIZE];
                    yarilo::ipc::PointerButtonPayload p{
                        .surface_id = batch.surface_local_id, .button = batch.buttons[i], .state = 0, .pad = 0, .timestamp_us = batch.timestamp_us
                    };
                    if (!p.encode(buf) || !queue_packet(*sess, yarilo::ipc::MessageType::PointerButton, 0, buf)) {
                        return false;
                    }
                }
                return true;
            },

            .dispatch_synthetic_key_releases = [&](const platform::input::SyntheticKeyReleaseBatch& batch) -> bool {
                auto* sess = get_session_by_id(batch.session_id);
                if (!sess) return false;
                for (size_t i = 0; i < batch.count; ++i) {
                    std::byte buf[yarilo::ipc::KeyboardKeyPayload::ENCODED_SIZE];
                    yarilo::ipc::KeyboardKeyPayload p{
                        .surface_id = batch.surface_local_id, .key = batch.keys[i], .state = 0, .pad = 0, .timestamp_us = batch.timestamp_us
                    };
                    if (!p.encode(buf) || !queue_packet(*sess, yarilo::ipc::MessageType::KeyboardKey, 0, buf)) {
                        return false;
                    }
                }
                return true;
            },

            .kill_client_session = [&](uint32_t s_id) -> bool {
                auto* sess = get_session_by_id(s_id);
                if (!sess) return false;
                sess->is_closing = true;
                sess->out_queue.clear();
                return true;
            }
        };

        std::vector<pollfd> pfds;
        std::vector<int> active_session_fds;

        while (true) {
#if defined(__linux__) && defined(YARILO_B2_TEST_CONTROL)
            if (g_shutdown_requested && !b2_drain_mode) {
                b2_drain_mode = true;
                g_running = 1; // keep service loop alive in the B2_DRAIN state
                b2_shutdown_requested = true;
                b2_queue_drain_deadline = std::chrono::steady_clock::now() + B2_DRAIN_TIMEOUT;
                server_socket.reset(); // stop accepting IPC clients during B2 drain
                if (b2_out_queue_count == 0) {
                    b2_drain_complete = true;
                    break;
                }
            }
            if (b2_drain_mode && std::chrono::steady_clock::now() >= b2_queue_drain_deadline) {
                mark_b2_failed("B2 queue drain deadline expired");
                exit_code = 1;
                goto server_teardown;
            }
#else
            if (!g_running) break;
#endif
            pfds.clear();
            active_session_fds.clear();

            pfds.push_back(pollfd{ .fd = seat->poll_fd(), .events = POLLIN, .revents = 0 });
            pfds.push_back(pollfd{ .fd = seat_device.fd(), .events = POLLIN, .revents = 0 });
            if (server_socket) {
                pfds.push_back(pollfd{ .fd = server_socket->fd(), .events = POLLIN, .revents = 0 });
            } else {
                pfds.push_back(pollfd{ .fd = -1, .events = 0, .revents = 0 });
            }
            pfds.push_back(pollfd{ .fd = input_subsystem->poll_fd(), .events = POLLIN, .revents = 0 });

            for (auto& [fd, sess] : sessions) {
                if (sess.is_closing) continue;
                short ev = POLLIN;
                if (!sess.out_queue.empty() || !sess.pending_wm_maps.empty()) ev |= POLLOUT;
                pfds.push_back(pollfd{ .fd = fd, .events = ev, .revents = 0 });
                active_session_fds.push_back(fd);
            }

#if defined(__linux__) && defined(YARILO_B2_TEST_CONTROL)
            if (b2_coordinator_fd >= 0) {
                short b2_ev = POLLIN;
                if (b2_out_queue_count > 0) b2_ev |= POLLOUT;
                b2_poll_index = pfds.size();
                pfds.push_back(pollfd{ .fd = b2_coordinator_fd, .events = b2_ev, .revents = 0 });
            } else {
                b2_poll_index.reset();
            }
#endif

            int poll_timeout_ms = 20;
#if defined(__linux__) && defined(YARILO_B2_TEST_CONTROL)
            if (b2_drain_mode && b2_out_queue_count > 0) {
                const auto drain_left = std::chrono::duration_cast<std::chrono::milliseconds>(
                    b2_queue_drain_deadline - std::chrono::steady_clock::now()).count();
                poll_timeout_ms = static_cast<int>(std::min<int64_t>(poll_timeout_ms, std::max<int64_t>(1, drain_left)));
            }
#endif
            int ret = ::poll(pfds.data(), pfds.size(), poll_timeout_ms);
            if (ret < 0 && errno == EINTR) continue;
            if (ret < 0) {
                std::println(stderr, "Yarilo Fatal: poll() failed: {}", std::strerror(errno));
                exit_code = 1;
                goto server_teardown;
            }

            if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                std::println(stderr, "Yarilo Fatal: Seat poll descriptor error.");
                exit_code = 1;
                goto server_teardown;
            }
            if (pfds[0].revents & POLLIN) {
                bool seat_was_active = seat->is_active();
                if (seat->dispatch(0) < 0) {
                    std::println(stderr, "Yarilo Fatal: seat->dispatch() failed: {}", std::strerror(errno));
                    exit_code = 1;
                    goto server_teardown;
                }
                bool seat_is_active = seat->is_active();

                if (seat_was_active && !seat_is_active) {
                    auto susp_res = input_subsystem->suspend(input_callbacks);
                    if (!susp_res) {
                        std::println(stderr, "Yarilo Fatal: Input suspend failed: {}", susp_res.error().message());
                        exit_code = 1;
                        goto server_teardown;
                    }
                } else if (!seat_was_active && seat_is_active) {
                    auto res_res = input_subsystem->resume();
                    if (!res_res) {
                        std::println(stderr, "Yarilo Fatal: Input resume failed: {}", res_res.error().message());
                        exit_code = 1;
                        goto server_teardown;
                    }
                }
            }

            if (pfds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                std::println(stderr, "Yarilo Fatal: DRM device revoked or hung in main loop.");
                drm_fault = true;
                exit_code = 1;
                goto server_teardown;
            }
            if (pfds[1].revents & POLLIN) {
                if (drmHandleEvent(seat_device.fd(), &drm_ev) != 0 || g_drm_handler_error) {
                    std::println(stderr, "Yarilo Fatal: drmHandleEvent() failed in main loop: {}", std::strerror(errno));
                    drm_fault = true;
                    exit_code = 1;
                    goto server_teardown;
                }
            }

            if (pfds[3].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                std::println(stderr, "Yarilo Fatal: Input subsystem poll error.");
                exit_code = 1;
                goto server_teardown;
            }
            if (pfds[3].revents & POLLIN) {
                auto in_res = input_subsystem->process_events(input_callbacks);
                if (!in_res) {
                    std::println(stderr, "Yarilo Fatal: Input processing error: {}. Aborting.", in_res.error().message());
                    exit_code = 1;
                    goto server_teardown;
                }
            }

#if defined(__linux__) && defined(YARILO_B2_TEST_CONTROL)
            if (b2_coordinator_fd >= 0) {
                const short b2_revents = pfds[*b2_poll_index].revents;
                if ((b2_revents & POLLIN) || (b2_revents & POLLHUP)) {
                    for (;;) {
                        b2::Packet unexpected{};
                        if (b2::receive(b2_coordinator_fd, unexpected)) {
                            mark_b2_failed("Unexpected B2 packet from Coordinator");
                            exit_code = 1;
                            goto server_teardown;
                        }
                        if (errno == EINTR) continue;
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        mark_b2_failed(b2_drain_mode ? "Coordinator EOF observed before server closed B2 fd"
                                                     : "Coordinator closed B2 channel before server drain");
                        exit_code = 1;
                        goto server_teardown;
                    }
                }
                if (b2_revents & (POLLERR | POLLNVAL)) {
                    mark_b2_failed("B2 Coordinator socket error");
                    exit_code = 1;
                    goto server_teardown;
                }
                if ((b2_revents & POLLOUT) && b2_out_queue_count > 0) {
                    if (!b2_flush_queue()) {
                        exit_code = 1;
                        goto server_teardown;
                    }
                    if (b2_drain_mode && b2_out_queue_count == 0) {
                        b2_drain_complete = true;
                        break;
                    }
                }
            }
#endif

            if (server_socket && (pfds[2].revents & (POLLERR | POLLHUP | POLLNVAL))) {
                std::println(stderr, "Yarilo Fatal: Server listening socket error.");
                exit_code = 1;
                goto server_teardown;
            }
            if (server_socket && (pfds[2].revents & POLLIN)) {
                auto client_res = server_socket->accept_client();
                if (client_res) {
                    const int raw_fd = client_res->get();
                    uint32_t new_id{};
#if defined(__linux__) && defined(YARILO_B2_TEST_CONTROL)
                    PeerCredentials peer_credentials{};
                    const auto session_status = yarilo::server::prepare_session_id(
                        session_id_counter, new_id, [&]() noexcept {
                            return yarilo::test::b2::get_peer_credentials(
                                raw_fd, peer_credentials.pid, peer_credentials.uid, peer_credentials.gid);
                        });
                    if (session_status == yarilo::server::SessionPreparationStatus::CredentialsRejected) {
                        mark_b2_failed("SO_PEERCRED failed for accepted IPC peer");
                        exit_code = 1;
                        goto server_teardown; // no ID/session allocated; UniqueFd closes the peer
                    }
                    if (session_status == yarilo::server::SessionPreparationStatus::SessionIdExhausted) {
                        std::println(stderr, "IPC: session_id space exhausted; rejecting new connection");
                        continue; // reject only this peer; the server keeps existing sessions
                    }
#else
                    if (!yarilo::server::try_advance_session_id(session_id_counter, new_id)) {
                        std::println(stderr, "IPC: session_id space exhausted; rejecting new connection");
                        continue; // reject only this new connection; RAII closes its fd
                    }
#endif
                    session_id_counter = new_id;
                    std::println("  -> [IPC] Client connected! (Session ID = {}, fd = {})", new_id, raw_fd);
                    sessions.emplace(raw_fd, ClientSession{
                        .session_id = new_id,
#if defined(__linux__) && defined(YARILO_B2_TEST_CONTROL)
                        .peer_credentials = peer_credentials,
#endif
                        .socket_fd = std::move(*client_res),
                        .role = ClientRole::Unauthenticated,
                        .out_queue = {},
                        .pending_wm_maps = {},
                        .is_closing = false,
                        .close_after_flush = false
                    });
                }
            }

            for (size_t i = 0; i < active_session_fds.size(); ++i) {
                int cfd = active_session_fds[i];
                const auto& pfd = pfds[4 + i];
                auto sess_it = sessions.find(cfd);
                if (sess_it == sessions.end() || sess_it->second.is_closing) continue;

                auto& sess = sess_it->second;

                if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
                    sess.is_closing = true;
                    continue;
                }

                if (pfd.revents & POLLOUT) {
                    pump_wm_queue(sess);
                    flush_out_queue(sess);
                }

                if ((pfd.revents & POLLIN) && !sess.is_closing) {
                    auto packet_res = yarilo::ipc::ServerSocket::receive_packet(sess.socket_fd.get());
                    if (!packet_res) {
                        if (packet_res.error() != core::make_error(core::ServerError::WouldBlock)) {
                            sess.is_closing = true;
                        }
                        continue;
                    }

                    auto& packet = *packet_res;

                    if (sess.role == ClientRole::Unauthenticated) {
                        if (packet.header.type == yarilo::ipc::MessageType::WmHandshake) {
                            if (active_wm_session_id.has_value()) {
                                send_protocol_error(sess, packet.header.sequence, 10);
                                continue;
                            }
                            sess.role = ClientRole::WindowManager;
                            active_wm_session_id = sess.session_id;

                            std::byte ack_buf[yarilo::ipc::WmHandshakeAckPayload::ENCODED_SIZE];
                            yarilo::ipc::WmHandshakeAckPayload ack_payload{
                                .screen_width = output->width(),
                                .screen_height = output->height(),
                                .refresh_rate = output->refresh_rate()
                            };
                            
                            if (ack_payload.encode(ack_buf)) {
                                (void)queue_packet(sess, yarilo::ipc::MessageType::WmHandshakeAck, packet.header.sequence, ack_buf);
                            }

                            try {
                                auto pending_maps = surface_manager.get_pending_map_requests();
                                sess.pending_wm_maps.assign(pending_maps.begin(), pending_maps.end());
                                pump_wm_queue(sess);

                                if (sess.is_closing) {
                                    active_wm_session_id.reset();
                                    sess.role = ClientRole::Unauthenticated;
                                    continue;
                                }

                                flush_out_queue(sess);
                                std::println("  -> [IPC] Session {} promoted to Window Manager ({} pending maps queued)", 
                                             sess.session_id, pending_maps.size());
                            } catch (...) {
                                active_wm_session_id.reset();
                                sess.role = ClientRole::Unauthenticated;
                                sess.is_closing = true;
                                sess.out_queue.clear();
                                continue;
                            }
                            continue;
                        } else if (packet.header.type == yarilo::ipc::MessageType::CreateSurface) {
                            sess.role = ClientRole::Application;
                        } else {
                            send_protocol_error(sess, packet.header.sequence, 11);
                            continue;
                        }
                    }

                    if (sess.role == ClientRole::Application) {
                        if (packet.header.type == yarilo::ipc::MessageType::CreateSurface) {
                            auto p = yarilo::ipc::CreateSurfacePayload::decode(
                                std::span<const std::byte>(packet.payload.data(), packet.payload_len));
                            if (p && surface_manager.create_surface(sess.session_id, p->surface_id)) {
                                std::byte ack[4];
                                yarilo::ipc::write_le32(ack, p->surface_id);
                                (void)queue_packet(sess, yarilo::ipc::MessageType::SurfaceCreated, packet.header.sequence, ack);
                            } else {
                                send_protocol_error(sess, packet.header.sequence, 1);
                            }
                        } else if (packet.header.type == yarilo::ipc::MessageType::AttachBuffer) {
                            auto p = yarilo::ipc::AttachBufferPayload::decode(
                                std::span<const std::byte>(packet.payload.data(), packet.payload_len));
                            if (p && packet.passed_fd.is_valid()) {
                                auto map_res = yarilo::shm::ShmValidator::validate_and_map(
                                    packet.passed_fd.get(), p->width, p->height, p->stride, p->format, p->offset);
                                if (map_res) {
                                    auto shared_buf = std::make_shared<yarilo::shm::MappedBuffer>(std::move(*map_res));
                                    auto att_res = surface_manager.attach_buffer(
                                        sess.session_id, p->surface_id, p->buffer_id, std::move(shared_buf));
                                    if (!att_res) {
                                        send_protocol_error(sess, packet.header.sequence, 3);
                                    }
                                } else {
                                    send_protocol_error(sess, packet.header.sequence, 3);
                                }
                            } else {
                                send_protocol_error(sess, packet.header.sequence, 4);
                            }
                        } else if (packet.header.type == yarilo::ipc::MessageType::CommitSurface) {
                            auto p = yarilo::ipc::CommitSurfacePayload::decode(
                                std::span<const std::byte>(packet.payload.data(), packet.payload_len));
                            if (p && packet.header.sequence == p->commit_seq) {
                                auto com_res = surface_manager.commit_surface(
                                    sess.session_id, p->surface_id, p->buffer_id, p->commit_seq);
                                if (com_res) {
                                    for (const auto& rel : com_res->superseded_releases) {
                                        queue_buffer_released(rel);
                                        commits_superseded++;
                                    }

                                    if (com_res->map_request && active_wm_session_id.has_value()) {
                                        if (auto* wsess = get_session_by_id(*active_wm_session_id)) {
                                            try {
                                                wsess->pending_wm_maps.push_back(*com_res->map_request);
                                                pump_wm_queue(*wsess);
                                                flush_out_queue(*wsess);
                                            } catch (...) {
                                                wsess->is_closing = true;
                                                wsess->out_queue.clear();
                                            }
                                        }
                                    }
                                    trigger_repaint();
                                } else {
                                    send_protocol_error(sess, packet.header.sequence, 5);
                                }
                            } else {
                                send_protocol_error(sess, packet.header.sequence, 5);
                            }
                        } else if (packet.header.type == yarilo::ipc::MessageType::DestroySurface) {
                            auto p = yarilo::ipc::DestroySurfacePayload::decode(
                                std::span<const std::byte>(packet.payload.data(), packet.payload_len));
                            if (p) {
                                uint64_t gid = (static_cast<uint64_t>(sess.session_id) << 32) | p->surface_id;
                                
                                auto clr_res = input_subsystem->clear_keyboard_focus_for_surface(gid, input_callbacks);
                                if (!clr_res) {
                                    std::println(stderr, "Yarilo Fatal: clear_keyboard_focus_for_surface failed: {}", clr_res.error().message());
                                    exit_code = 1;
                                    goto server_teardown;
                                }
                                input_subsystem->cancel_grab_for_surface(gid, input_callbacks);

                                auto dest_res = surface_manager.destroy_surface(sess.session_id, p->surface_id);
                                if (dest_res) {
                                    cancel_pending_wm_map_requests(gid, dest_res->server_uid);
                                    surface_last_presented_seq.erase(gid);
                                    
                                    for (const auto& rel : dest_res->releases) {
                                        queue_buffer_released(rel);
                                    }
                                    if (dest_res->wm_notified) {
                                        notify_wm_surface_destroyed(gid);
                                    }
                                    trigger_repaint();
                                } else {
                                    send_protocol_error(sess, packet.header.sequence, 6);
                                }
                            }
                        } else {
                            send_protocol_error(sess, packet.header.sequence, 8);
                        }
                    } else if (sess.role == ClientRole::WindowManager) {
                        if (packet.header.type == yarilo::ipc::MessageType::WmConfigureSurface) {
                            auto p = yarilo::ipc::WmConfigureSurfacePayload::decode(
                                std::span<const std::byte>(packet.payload.data(), packet.payload_len));
                            if (p) {
                                bool was_mapped = surface_manager.surface_is_mapped(p->global_surface_id);
                                auto conf_res = surface_manager.apply_wm_configuration(
                                    p->global_surface_id, p->server_uid, p->x, p->y, p->z_index, p->mapped != 0);
                                if (conf_res) {
                                    if (p->mapped == 0) {
                                        auto clr_res = input_subsystem->clear_keyboard_focus_for_surface(p->global_surface_id, input_callbacks);
                                        if (!clr_res) {
                                            std::println(stderr, "Yarilo Fatal: Failed to clear focus on unmap");
                                            exit_code = 1;
                                            goto server_teardown;
                                        }
                                        input_subsystem->cancel_grab_for_surface(p->global_surface_id, input_callbacks);
                                    }
                                    trigger_repaint();
#if defined(__linux__) && defined(YARILO_B2_TEST_CONTROL)
                                    if (!was_mapped && p->mapped != 0) {
                                        for (size_t rec_index = 0; rec_index < b2_unmapped_report.size(); ++rec_index) {
                                            const auto& rec = b2_unmapped_report[rec_index];
                                            if (!b2_drain_mode && !b2_shutdown_requested &&
                                                b2_recovery_report_active && !b2_recovery_ready_queued[rec_index] &&
                                                active_wm_session_id.has_value() && *active_wm_session_id == sess.session_id &&
                                                surface_manager.surface_is_mapped(p->global_surface_id) &&
                                                rec.session_id == static_cast<uint32_t>(p->global_surface_id >> 32) &&
                                                rec.surface_id == static_cast<uint32_t>(p->global_surface_id & 0xFFFFFFFFULL) &&
                                                rec.server_uid == p->server_uid) {
                                                b2::Packet recovery_pkt{};
                                                recovery_pkt.type = b2::Type::SurfaceRecoveryReady;
                                                recovery_pkt.role = b2::Role::Server;
                                                recovery_pkt.status = static_cast<uint32_t>(b2::Status::Ok);
                                                if (b2_message_seq == std::numeric_limits<uint64_t>::max()) {
                                                    mark_b2_failed("B2 message_seq overflow for SurfaceRecoveryReady");
                                                    exit_code = 1;
                                                    goto server_teardown;
                                                }
                                                recovery_pkt.message_seq = b2_message_seq + 1;
                                                recovery_pkt.pid = static_cast<uint64_t>(::getpid());
                                                recovery_pkt.uid = static_cast<uint64_t>(::getuid());
                                                recovery_pkt.gid = static_cast<uint64_t>(::getgid());
                                                recovery_pkt.server_pid = static_cast<uint64_t>(::getpid());
                                                recovery_pkt.epoch = recovery_epoch;
                                                recovery_pkt.session_id = static_cast<uint32_t>(p->global_surface_id >> 32);
                                                recovery_pkt.surface_id = static_cast<uint32_t>(p->global_surface_id & 0xFFFFFFFFULL);
                                                recovery_pkt.server_uid = p->server_uid;
                                                if (!b2_queue_packet(recovery_pkt)) {
                                                    exit_code = 1;
                                                    goto server_teardown;
                                                }
                                                b2_message_seq = recovery_pkt.message_seq;
                                                b2_recovery_ready_queued[rec_index] = true;
                                                break;
                                            }
                                        }
                                    }
#endif
                                } else {
                                    send_protocol_error(sess, packet.header.sequence, 12);
                                }
                            }
                        } else if (packet.header.type == yarilo::ipc::MessageType::WmSetKeyboardFocus) {
                            auto p = yarilo::ipc::WmSetKeyboardFocusPayload::decode(
                                std::span<const std::byte>(packet.payload.data(), packet.payload_len));
                            if (p) {
                                uint64_t gid = p->global_surface_id;
                                if (gid == 0) {
                                    auto k_res = input_subsystem->set_keyboard_focus(0, 0, 0, input_callbacks);
                                    if (!k_res) {
                                        std::println(stderr, "Yarilo Fatal: Failed to clear keyboard focus");
                                        exit_code = 1;
                                        goto server_teardown;
                                    }
                                } else if (surface_manager.surface_is_mapped(gid)) {
                                    uint32_t target_sess = static_cast<uint32_t>(gid >> 32);
                                    uint32_t target_local = static_cast<uint32_t>(gid & 0xFFFFFFFFULL);
                                    auto k_res = input_subsystem->set_keyboard_focus(gid, target_local, target_sess, input_callbacks);
                                    if (!k_res) {
                                        std::println(stderr, "Yarilo Fatal: Failed to set keyboard focus");
                                        exit_code = 1;
                                        goto server_teardown;
                                    }
                                } else {
                                    send_protocol_error(sess, packet.header.sequence, 13);
                                }
                            }
                        } else {
                            send_protocol_error(sess, packet.header.sequence, 8);
                        }
                    }
                }
            }

            // 5. ДВУХФАЗНЫЙ КОНВЕЙЕР РЕНДЕРИНГА
            if (needs_repaint && !prepared_flip.has_value() && !output->is_page_flip_pending() && seat->is_active()) {
                auto snapshot_res = surface_manager.take_render_snapshot();
                if (!snapshot_res) {
                    std::println(stderr, "Yarilo Fatal: take_render_snapshot failed: {}. Aborting.", snapshot_res.error().message());
                    exit_code = 1;
                    goto server_teardown;
                }

                for (const auto& rel : snapshot_res->newly_released) {
                    queue_buffer_released(rel);
                }

                SnapshotLeaseDrainer drainer(surface_manager, snapshot_res->nodes);
                output->clear_back_buffer(CORAL_XRGB);

                bool blit_ok = true;
                std::vector<PresentationTarget> flip_targets;

                for (const auto& node : snapshot_res->nodes) {
                    auto clipped = core::compute_clipped_blit(
                        node.dst_x, node.dst_y, node.src_width, node.src_height,
                        output->width(), output->height());

                    if (clipped) {
                        bool res = output->blit_subsurface(
                            node.mapping_ref->pixels(), node.mapping_ref->map_size(),
                            node.mapping_ref->width(), node.mapping_ref->height(), node.src_stride,
                            clipped->src_x, clipped->src_y,
                            clipped->dst_x, clipped->dst_y,
                            clipped->width, clipped->height);
                        if (!res) blit_ok = false;
                    }

                    auto rel = drainer.release_current();
                    if (rel) {
                        queue_buffer_released(*rel);
                    }

                    uint64_t gid = (static_cast<uint64_t>(node.session_id) << 32) | node.local_surface_id;
                    auto p_it = surface_last_presented_seq.find(gid);
                    uint64_t last_seq = (p_it != surface_last_presented_seq.end()) ? p_it->second : 0;

                    if (node.commit_seq > last_seq) {
                        flip_targets.push_back(PresentationTarget{
                            .session_id = node.session_id,
                            .surface_id = node.local_surface_id,
                            .commit_seq = node.commit_seq,
                            .global_surface_id = gid
                        });
                    }
                }

                if (blit_ok) {
                    frames_rendered++;
                    prepared_flip = PreparedFlip{ .targets = std::move(flip_targets) };
                    flip_retry_count = 0;
                    blit_retry_count = 0;
                    needs_repaint = false;
                } else {
                    blit_retry_count++;
                    std::println(stderr, "Yarilo Warning: Some nodes failed blit_subsurface validation (attempt {}/{})", 
                                 blit_retry_count, MAX_BLIT_RETRIES);
                    if (blit_retry_count >= MAX_BLIT_RETRIES) {
                        std::println(stderr, "Yarilo Fatal: Max blit retries exceeded. Unrecoverable surface corruption.");
                        exit_code = 1;
                        goto server_teardown;
                    }
                }
            }

            if (prepared_flip.has_value() && !output->is_page_flip_pending() && seat->is_active()) {
                auto flip_res = output->schedule_page_flip(0, 0, 0);
                
                if (flip_res) {
                    active_flip_targets = std::move(prepared_flip->targets);
                    prepared_flip.reset();
                    flip_retry_count = 0;
                } else if (flip_res.error() == core::make_error(core::ServerError::WouldBlock)) {
                    flip_retry_count++;
                    if (flip_retry_count >= MAX_FLIP_RETRIES) {
                        std::println(stderr, "KMS Fatal: EBUSY retry limit ({}) exceeded", MAX_FLIP_RETRIES);
                        prepared_flip.reset();
                        exit_code = 1;
                        goto server_teardown;
                    }
                } else {
                    std::println(stderr, "KMS Fatal: Page flip schedule error ({})", flip_res.error().message());
                    prepared_flip.reset();
                    exit_code = 1;
                    goto server_teardown;
                }
            }

            for (auto it = sessions.begin(); it != sessions.end(); ) {
                if (it->second.is_closing) {
                    uint32_t s_id = it->second.session_id;
                    
                    input_subsystem->cancel_grab_for_closing_session(s_id);

                    if (it->second.role == ClientRole::WindowManager) {
                        std::println("  -> [IPC] Window Manager disconnected (Session {})", s_id);
                        const auto disconnect_outcome = yarilo::server::process_wm_disconnect(
                            active_wm_session_id,
                            s_id,
                            advance_disconnect_epoch,
                            [&]() noexcept {
#if defined(__linux__) && defined(YARILO_B2_TEST_CONTROL)
                                b2_disconnect_report = surface_manager.handle_wm_disconnected(b2_unmapped_report);
                                if (b2_disconnect_report.changed_count_overflow ||
                                    b2_disconnect_report.changed_count != B2_EXPECTED_SURFACE_COUNT ||
                                    b2_disconnect_report.reported_count != B2_EXPECTED_SURFACE_COUNT) {
                                    mark_b2_failed("disconnect report count mismatch");
                                } else {
                                    constexpr size_t DISCONNECT_BATCH_SIZE = 1 + B2_EXPECTED_SURFACE_COUNT;
                                    if (b2_drain_mode || b2_shutdown_requested ||
                                        !b2_can_enqueue(DISCONNECT_BATCH_SIZE) ||
                                        DISCONNECT_BATCH_SIZE > std::numeric_limits<uint64_t>::max() - b2_message_seq) {
                                        mark_b2_failed("cannot reserve complete disconnect report FIFO/sequence range");
                                    } else {
                                        std::array<b2::Packet, DISCONNECT_BATCH_SIZE> batch{};
                                        batch[0].type = b2::Type::WmDisconnected;
                                        batch[0].role = b2::Role::Server;
                                        batch[0].status = static_cast<uint32_t>(b2::Status::Ok);
                                        batch[0].pid = static_cast<uint64_t>(::getpid());
                                        batch[0].uid = static_cast<uint64_t>(::getuid());
                                        batch[0].gid = static_cast<uint64_t>(::getgid());
                                        batch[0].server_pid = static_cast<uint64_t>(::getpid());
                                        batch[0].epoch = recovery_epoch;
                                        batch[0].changed_count = b2_disconnect_report.changed_count;
                                        batch[0].reported_count = b2_disconnect_report.reported_count;
                                        for (size_t i = 0; i < B2_EXPECTED_SURFACE_COUNT; ++i) {
                                            auto& packet = batch[i + 1];
                                            const auto& record = b2_unmapped_report[i];
                                            packet.type = b2::Type::SurfaceUnmapped;
                                            packet.role = b2::Role::Server;
                                            packet.status = static_cast<uint32_t>(b2::Status::Ok);
                                            packet.pid = static_cast<uint64_t>(::getpid());
                                            packet.uid = static_cast<uint64_t>(::getuid());
                                            packet.gid = static_cast<uint64_t>(::getgid());
                                            packet.server_pid = static_cast<uint64_t>(::getpid());
                                            packet.epoch = recovery_epoch;
                                            packet.session_id = record.session_id;
                                            packet.surface_id = record.surface_id;
                                            packet.server_uid = record.server_uid;
                                        }
                                        const uint64_t first_sequence = b2_message_seq + 1;
                                        for (size_t i = 0; i < batch.size(); ++i)
                                            batch[i].message_seq = first_sequence + static_cast<uint64_t>(i);
                                        if (!b2_enqueue_batch(batch)) {
                                            mark_b2_failed("atomic disconnect report enqueue failed");
                                        } else {
                                            b2_message_seq += static_cast<uint64_t>(DISCONNECT_BATCH_SIZE);
                                            b2_recovery_report_active = true;
                                            b2_recovery_ready_queued.fill(false);
                                        }
                                    }
                                }
#endif
                            },
                            [&]() noexcept -> std::error_code {
                                auto result = input_subsystem->cancel_all_input(input_callbacks);
                                return result ? std::error_code{} : result.error();
                            },
                            [&]() noexcept { trigger_repaint(); });
                        if (disconnect_outcome.was_active_owner && disconnect_outcome.input_error) {
                            std::println(stderr, "Yarilo Fatal: cancel_all_input failed during WM disconnect: {}",
                                         disconnect_outcome.input_error.message());
                            exit_code = 1;
                            goto server_teardown;
                        }
#if defined(__linux__) && defined(YARILO_B2_TEST_CONTROL)
                        if (b2_test_failed) {
                            exit_code = 1;
                            goto server_teardown;
                        }
#endif
                    } else if (it->second.role == ClientRole::Application) {
                        std::println("  -> [IPC] Application disconnected (Session {})", s_id);
                        
                        if (active_wm_session_id.has_value()) {
                            if (auto* wsess = get_session_by_id(*active_wm_session_id)) {
                                std::erase_if(wsess->out_queue, [s_id](const PendingOutPacket& pkt) {
                                    return pkt.type == yarilo::ipc::MessageType::WmMapRequest &&
                                           static_cast<uint32_t>(pkt.map_req_global_id >> 32) == s_id;
                                });
                                std::erase_if(wsess->pending_wm_maps, [s_id](const core::MapRequestInfo& req) {
                                    return static_cast<uint32_t>(req.global_id >> 32) == s_id;
                                });
                            }
                        }

                        for (auto s_it = surface_last_presented_seq.begin(); s_it != surface_last_presented_seq.end(); ) {
                            if (static_cast<uint32_t>(s_it->first >> 32) == s_id) {
                                s_it = surface_last_presented_seq.erase(s_it);
                            } else {
                                ++s_it;
                            }
                        }

                        auto dest_res = surface_manager.destroy_client_surfaces(s_id);
                        if (dest_res) {
                            for (uint64_t gid : dest_res->wm_surfaces_to_destroy) {
                                auto clr_res = input_subsystem->clear_keyboard_focus_for_surface(gid, input_callbacks);
                                if (!clr_res) {
                                    exit_code = 1;
                                    goto server_teardown;
                                }
                                notify_wm_surface_destroyed(gid);
                            }
                            trigger_repaint();
                        } else {
                            std::println(stderr, "Yarilo Fatal: Failed to cleanup client surfaces. OOM? Aborting.");
                            exit_code = 1;
                            goto server_teardown;
                        }
                    }
                    
                    it = sessions.erase(it);
                } else {
                    ++it;
                }
            }
        }
    } catch (const std::bad_alloc&) {
        try { std::println(stderr, "Yarilo Fatal: OOM in runtime. Aborting."); } catch (...) {}
        exit_code = 1;
        goto server_teardown;
    } catch (const std::exception& e) {
        try { std::println(stderr, "Yarilo Fatal: Unexpected exception in runtime: {}. Aborting.", e.what()); } catch (...) {}
        exit_code = 1;
        goto server_teardown;
    } catch (...) {
        try { std::println(stderr, "Yarilo Fatal: Unknown exception in runtime. Aborting."); } catch (...) {}
        exit_code = 1;
        goto server_teardown;
    }

server_teardown:
    // Безопасный безаллокационный вывод метрик в фазе аварийного завершения
    try {
        std::println("\n=================================================");
        std::println("  Yarilo v0.6 Hardware Metrics:                  ");
        std::println("    Frames Rendered (CPU Blit): {}", frames_rendered);
        std::println("    Flips Completed (vblank):   {}", flips_completed);
        std::println("    Presentations Sent (IPC):   {}", presentations_sent);
        std::println("    Commits Superseded:         {}", commits_superseded);
        std::println("=================================================");
    } catch (...) {}

#if defined(__linux__) && defined(YARILO_B2_TEST_CONTROL)
    if (b2_registered && b2_coordinator_fd >= 0) {
        if (!b2_drain_mode || !b2_drain_complete || b2_out_queue_count != 0 || b2_test_failed) {
            mark_b2_failed("B2 teardown without successful graceful drain");
            exit_code = 1;
        }
        ::close(b2_coordinator_fd);
        b2_coordinator_fd = -1;
        if (b2_drain_complete && !b2_test_failed && exit_code == 0)
            b2_normal_shutdown_requested = true;
        if (b2_normal_shutdown_requested) {
            std::println("B2_SERVER_DRAINED: bounded outgoing queue delivered; closing Coordinator control channel");
        }
        else
            exit_code = 1;
    }
#endif

    bool safe_to_restore = !drm_fault;
    bool drain_failed = false;

    if (output->is_page_flip_pending()) {
        if (drm_fault) {
            drain_failed = true;
        } else {
            try {
                std::println("Yarilo Shutdown: Draining in-flight page flip before CRTC restore...");
            } catch (...) {}
            
            auto drain_start = std::chrono::steady_clock::now();
            
            while (output->is_page_flip_pending()) {
                auto elapsed = std::chrono::steady_clock::now() - drain_start;
                if (elapsed >= std::chrono::milliseconds(500)) {
                    drain_failed = true;
                    drm_fault = true;
                    break;
                }
                
                int timeout_ms = 500 - static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
                pollfd pfd{ .fd = seat_device.fd(), .events = POLLIN, .revents = 0 };
                
                int p_ret = ::poll(&pfd, 1, timeout_ms);
                if (p_ret < 0 && errno == EINTR) continue;
                if (p_ret < 0) {
                    try { std::println(stderr, "Yarilo Error: poll() failed during drain: {}", std::strerror(errno)); } catch (...) {}
                    drain_failed = true;
                    drm_fault = true;
                    break;
                }
                if (p_ret > 0) {
                    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                        try { std::println(stderr, "Yarilo Error: DRM device revoked or hung during drain."); } catch (...) {}
                        drain_failed = true;
                        drm_fault = true;
                        break;
                    }
                    if (pfd.revents & POLLIN) {
                        if (drmHandleEvent(seat_device.fd(), &drm_ev) != 0 || g_drm_handler_error) {
                            try { std::println(stderr, "Yarilo Fatal: drmHandleEvent() failed in drain loop: {}", std::strerror(errno)); } catch (...) {}
                            drain_failed = true;
                            drm_fault = true;
                            break;
                        }
                    }
                }
            }
        }
    }

    if (drm_fault || drain_failed || output->is_page_flip_pending()) {
        try { std::println(stderr, "Yarilo Fatal: Page flip drain aborted or timed out. Skipping unsafe KMS restore."); } catch (...) {}
        safe_to_restore = false;
        exit_code = 1;
    }

    if (safe_to_restore) {
        try { std::println("Shutting down Yarilo Server cleanly..."); } catch (...) {}
        auto restore_res = output->restore_crtc(seat->is_active());
        if (!restore_res) {
            try { std::println(stderr, "Yarilo Fatal: Failed to restore original CRTC: {}", restore_res.error().message()); } catch (...) {}
            exit_code = 1;
        }
    } else {
        try {
            std::println(stderr, "Yarilo Emergency Teardown: Scanout hardware unresolved.\n"
                                 "Skipping CRTC restore to prevent GPU hang. Display may remain frozen.\n"
                                 "Process termination will release file descriptors and trigger kernel resource reclaim.");
        } catch (...) {}
    }

#if defined(__linux__) && defined(YARILO_B2_TEST_CONTROL)
    if (b2_test_failed && exit_code == 0) exit_code = 1;
#endif
    return exit_code;
}