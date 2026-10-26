#include "ipc/protocol.hpp"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>
#include <cstring>
#include <cerrno>
#include <print>
#include <cstdlib>
#include <string>
#include <vector>
#include <array>
#include <csignal>
#include <optional>

static volatile std::sig_atomic_t g_running = 1;

static void sig_handler(int) {
    g_running = 0;
}

struct WindowSlot {
    uint64_t global_surface_id{0};
    uint64_t server_uid{0};
    uint32_t width{0};
    uint32_t height{0};
    bool occupied{false};
};

int main() {
    std::signal(SIGINT, sig_handler);
    std::signal(SIGTERM, sig_handler);

    std::println("Starting Yarilo External Tiling Window Manager (wm_test)...");

    const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");
    if (!runtime_dir) {
        std::println(stderr, "WM Fatal: XDG_RUNTIME_DIR not set.");
        return 1;
    }
    std::string path = std::string(runtime_dir) + "/yarilo-0";

    int sock_fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (sock_fd < 0) {
        std::println(stderr, "WM Fatal: socket() failed: {}", std::strerror(errno));
        return 1;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(sock_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::println(stderr, "WM Fatal: connect() failed: {}", std::strerror(errno));
        ::close(sock_fd);
        return 1;
    }

    // 1. Handshake
    std::byte hs_packet[yarilo::ipc::HEADER_SIZE];
    yarilo::ipc::MessageHeader hs_hdr{
        .magic    = yarilo::ipc::PROTOCOL_MAGIC,
        .version  = yarilo::ipc::PROTOCOL_VERSION,
        .type     = yarilo::ipc::MessageType::WmHandshake,
        .length   = 0,
        .sequence = 1,
    };
    if (!hs_hdr.encode(hs_packet)) return 1;

    if (::send(sock_fd, hs_packet, sizeof(hs_packet), MSG_NOSIGNAL) != sizeof(hs_packet)) {
        std::println(stderr, "WM Fatal: Failed to send WmHandshake");
        ::close(sock_fd);
        return 1;
    }

    // 2. Ожидание WmHandshakeAck
    std::byte reply_buf[128];
    ssize_t n = ::recv(sock_fd, reply_buf, sizeof(reply_buf), 0);
    if (n < static_cast<ssize_t>(yarilo::ipc::HEADER_SIZE + yarilo::ipc::WmHandshakeAckPayload::ENCODED_SIZE)) {
        std::println(stderr, "WM Fatal: Failed to receive WmHandshakeAck");
        ::close(sock_fd);
        return 1;
    }

    auto hdr = yarilo::ipc::MessageHeader::decode(std::span<const std::byte>(reply_buf, yarilo::ipc::HEADER_SIZE));
    if (!hdr || hdr->type != yarilo::ipc::MessageType::WmHandshakeAck || 
        hdr->length != yarilo::ipc::WmHandshakeAckPayload::ENCODED_SIZE) {
        std::println(stderr, "WM Fatal: Invalid WmHandshakeAck received");
        ::close(sock_fd);
        return 1;
    }

    auto ack = yarilo::ipc::WmHandshakeAckPayload::decode(
        std::span<const std::byte>(reply_buf + yarilo::ipc::HEADER_SIZE, hdr->length));
    if (!ack) return 1;

    const uint32_t screen_w = ack->screen_width;
    const uint32_t screen_h = ack->screen_height;
    std::println("[1/2] Handshake complete: Screen is {}x{} @ {}Hz", screen_w, screen_h, ack->refresh_rate);

    // 3. Слотовый менеджер тайлинга на 2 окна
    std::array<WindowSlot, 2> slots{};
    pollfd pfd{ .fd = sock_fd, .events = POLLIN, .revents = 0 };

    std::println("[2/2] Running Window Manager event loop...");

    auto configure_surface = [&](uint64_t gid, uint64_t uid, int32_t x, int32_t y, uint32_t z, uint32_t mapped) -> bool {
        std::byte conf_packet[yarilo::ipc::HEADER_SIZE + yarilo::ipc::WmConfigureSurfacePayload::ENCODED_SIZE];
        yarilo::ipc::MessageHeader c_hdr{
            .magic    = yarilo::ipc::PROTOCOL_MAGIC,
            .version  = yarilo::ipc::PROTOCOL_VERSION,
            .type     = yarilo::ipc::MessageType::WmConfigureSurface,
            .length   = yarilo::ipc::WmConfigureSurfacePayload::ENCODED_SIZE,
            .sequence = 0,
        };
        if (!c_hdr.encode(conf_packet)) return false;

        yarilo::ipc::WmConfigureSurfacePayload payload{
            .global_surface_id = gid,
            .server_uid = uid,
            .x = x,
            .y = y,
            .z_index = z,
            .mapped = mapped,
        };
        if (!payload.encode(std::span<std::byte>(conf_packet + yarilo::ipc::HEADER_SIZE, yarilo::ipc::WmConfigureSurfacePayload::ENCODED_SIZE))) {
            return false;
        }

        return ::send(sock_fd, conf_packet, sizeof(conf_packet), MSG_NOSIGNAL) == sizeof(conf_packet);
    };

    while (g_running) {
        int ret = ::poll(&pfd, 1, 100);
        if (ret < 0 && errno == EINTR) continue;
        if (ret < 0) break;
        if (ret == 0) continue;

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            std::println("WM: Server connection closed.");
            break;
        }

        if (pfd.revents & POLLIN) {
            std::byte msg_buf[yarilo::ipc::MAX_MESSAGE_SIZE];
            ssize_t bytes = ::recv(sock_fd, msg_buf, sizeof(msg_buf), 0);
            if (bytes < static_cast<ssize_t>(yarilo::ipc::HEADER_SIZE)) break;

            auto in_hdr = yarilo::ipc::MessageHeader::decode(
                std::span<const std::byte>(msg_buf, yarilo::ipc::HEADER_SIZE));
            if (!in_hdr) break;

            if (in_hdr->length > yarilo::ipc::MAX_MESSAGE_SIZE - yarilo::ipc::HEADER_SIZE ||
                bytes != static_cast<ssize_t>(yarilo::ipc::HEADER_SIZE + in_hdr->length)) {
                std::println(stderr, "WM Fatal: Invalid IPC packet length.");
                break;
            }

            if (bytes != static_cast<ssize_t>(yarilo::ipc::HEADER_SIZE + in_hdr->length)) {
                std::println(stderr, "WM Warning: packet length mismatch, dropping.");
                continue;
            }

            if (in_hdr->type == yarilo::ipc::MessageType::WmMapRequest) {
                auto req = yarilo::ipc::WmMapRequestPayload::decode(
                    std::span<const std::byte>(msg_buf + yarilo::ipc::HEADER_SIZE, in_hdr->length));
                if (req) {
                    int target_slot = -1;
                    for (int i = 0; i < 2; ++i) {
                        if (!slots[i].occupied) {
                            target_slot = i;
                            break;
                        }
                    }

                    if (target_slot >= 0) {
                        slots[target_slot] = WindowSlot{
                            .global_surface_id = req->global_surface_id,
                            .server_uid = req->server_uid,
                            .width = req->width,
                            .height = req->height,
                            .occupied = true
                        };

                        int32_t pos_x = 0;
                        int32_t pos_y = 0;

                        if (target_slot == 0) {
                            pos_x = (static_cast<int32_t>(screen_w / 2) - static_cast<int32_t>(req->width)) / 2;
                            pos_y = (static_cast<int32_t>(screen_h) - static_cast<int32_t>(req->height)) / 2;
                            if (pos_x < 0) pos_x = 0;
                        } else {
                            int32_t right_half_start = static_cast<int32_t>(screen_w / 2);
                            int32_t right_half_w = static_cast<int32_t>(screen_w - (screen_w / 2));
                            pos_x = right_half_start + (right_half_w - static_cast<int32_t>(req->width)) / 2;
                            pos_y = (static_cast<int32_t>(screen_h) - static_cast<int32_t>(req->height)) / 2;
                        }
                        if (pos_y < 0) pos_y = 0;

                        std::println("WM: Mapping Window 0x{:016X} to Slot {} at ({}, {})", 
                                     req->global_surface_id, target_slot, pos_x, pos_y);

                        configure_surface(req->global_surface_id, req->server_uid, pos_x, pos_y, 
                                          static_cast<uint32_t>(target_slot + 1), 1);
                    } else {
                        std::println(stderr, "WM Warning: No slots available for surface 0x{:016X}", req->global_surface_id);
                    }
                }
            } else if (in_hdr->type == yarilo::ipc::MessageType::WmSurfaceDestroyed) {
                auto dest = yarilo::ipc::WmSurfaceDestroyedPayload::decode(
                    std::span<const std::byte>(msg_buf + yarilo::ipc::HEADER_SIZE, in_hdr->length));
                if (dest) {
                    for (int i = 0; i < 2; ++i) {
                        if (slots[i].occupied && slots[i].global_surface_id == dest->global_surface_id) {
                            slots[i].occupied = false;
                            std::println("WM: Freed Slot {} after destruction of Window 0x{:016X}", 
                                         i, dest->global_surface_id);
                            break;
                        }
                    }
                }
            }
        }
    }

    std::println("Shutting down Window Manager...");
    ::close(sock_fd);
    return 0;
}