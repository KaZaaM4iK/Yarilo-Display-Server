#include "platform/seat.hpp"
#include "platform/drm/output.hpp"
#include "platform/input/input_subsystem.hpp"
#include "ipc/server_socket.hpp"
#include "core/surface.hpp"
#include "core/geometry.hpp"
#include "shm/validator.hpp"
#include <xf86drm.h>
#include <print>
#include <csignal>
#include <poll.h>
#include <chrono>
#include <vector>
#include <deque>
#include <unordered_map>
#include <string>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <optional>
#include <span>
#include <algorithm>
#include <sys/stat.h>
#include <utility>
#include <unistd.h>
#include <new>
#include <exception>

static volatile std::sig_atomic_t g_running = 1;
static volatile std::sig_atomic_t g_drm_handler_error = 0;

static void sig_handler(int) {
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

struct PendingOutPacket {
    yarilo::ipc::MessageType type;
    uint64_t sequence{0};
    std::vector<std::byte> payload;
    uint64_t map_req_global_id{0};
    uint64_t map_req_server_uid{0};
};

struct ClientSession {
    uint32_t session_id{0};
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

        std::println("[3/6] Initializing Input Subsystem (libinput)...");
        auto input_res = platform::input::InputSubsystem::create(*seat, output->width(), output->height());
        if (!input_res) {
            std::println(stderr, "Fatal: InputSubsystem creation failed: {}", input_res.error().message());
            exit_code = 1;
            goto server_teardown;
        }
        input_subsystem = std::move(*input_res);
        std::println("  -> Input Subsystem initialized: Seat admission & Multi-mouse/keyboard active");

        std::println("[4/6] Ready for Hardware Page-Flipping & Input Events. Running for 15s...");

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

        const auto server_start_time = std::chrono::steady_clock::now();
        constexpr auto total_run_duration = std::chrono::seconds(15);

        std::vector<pollfd> pfds;
        std::vector<int> active_session_fds;

        while (g_running) {
            auto now = std::chrono::steady_clock::now();
            if (now - server_start_time >= total_run_duration) break;

            pfds.clear();
            active_session_fds.clear();

            pfds.push_back(pollfd{ .fd = seat->poll_fd(), .events = POLLIN, .revents = 0 });
            pfds.push_back(pollfd{ .fd = seat_device.fd(), .events = POLLIN, .revents = 0 });
            pfds.push_back(pollfd{ .fd = server_socket->fd(), .events = POLLIN, .revents = 0 });
            pfds.push_back(pollfd{ .fd = input_subsystem->poll_fd(), .events = POLLIN, .revents = 0 });

            for (auto& [fd, sess] : sessions) {
                if (sess.is_closing) continue;
                short ev = POLLIN;
                if (!sess.out_queue.empty() || !sess.pending_wm_maps.empty()) ev |= POLLOUT;
                pfds.push_back(pollfd{ .fd = fd, .events = ev, .revents = 0 });
                active_session_fds.push_back(fd);
            }

            int ret = ::poll(pfds.data(), pfds.size(), 20);
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

            if (pfds[2].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                std::println(stderr, "Yarilo Fatal: Server listening socket error.");
                exit_code = 1;
                goto server_teardown;
            }
            if (pfds[2].revents & POLLIN) {
                auto client_res = server_socket->accept_client();
                if (client_res) {
                    uint32_t new_id = ++session_id_counter;
                    int raw_fd = client_res->get();
                    std::println("  -> [IPC] Client connected! (Session ID = {}, fd = {})", new_id, raw_fd);
                    sessions.emplace(raw_fd, ClientSession{
                        .session_id = new_id,
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
                        if (active_wm_session_id.has_value() && *active_wm_session_id == s_id) {
                            active_wm_session_id.reset();
                            surface_manager.handle_wm_disconnected();
                            auto cancel_all_res = input_subsystem->cancel_all_input(input_callbacks);
                            if (!cancel_all_res) {
                                exit_code = 1;
                                goto server_teardown;
                            }
                            trigger_repaint();
                        }
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

    return exit_code;
}