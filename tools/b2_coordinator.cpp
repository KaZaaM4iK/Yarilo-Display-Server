#include "test/b2_control.hpp"

#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <climits>
#include <span>
#include <sys/types.h>
#include <sys/un.h>
#include <poll.h>
#include <signal.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace b2 = yarilo::test::b2;

namespace {

constexpr uint32_t EVENT_MAGIC = 0x32455642; // BVE2
constexpr uint16_t EVENT_VERSION = 1;
constexpr uint16_t EVENT_READY = 1;
constexpr uint16_t EVENT_REGISTERED = 2;
constexpr uint16_t EVENT_BARRIER = 3;
constexpr uint16_t EVENT_PASS = 4;
constexpr uint16_t EVENT_FAIL = 5;
constexpr size_t EVENT_READY_SIZE = 8;
constexpr size_t EVENT_REGISTERED_SIZE = 8;
constexpr size_t EVENT_BARRIER_SIZE = 40;
constexpr size_t EVENT_RESULT_SIZE = 8;
constexpr size_t EVENT_MAX_SIZE = EVENT_BARRIER_SIZE;
static_assert(EVENT_MAX_SIZE <= PIPE_BUF, "B2 event records must be atomic pipe writes");

void event_put16(std::byte* out, uint16_t value) noexcept {
    out[0] = static_cast<std::byte>(value & 0xff);
    out[1] = static_cast<std::byte>((value >> 8) & 0xff);
}
void event_put32(std::byte* out, uint32_t value) noexcept {
    for (unsigned i = 0; i < 4; ++i) out[i] = static_cast<std::byte>((value >> (i * 8)) & 0xff);
}
void event_put64(std::byte* out, uint64_t value) noexcept {
    for (unsigned i = 0; i < 8; ++i) out[i] = static_cast<std::byte>((value >> (i * 8)) & 0xff);
}
bool emit_event_record(int fd, uint16_t type, std::span<const std::byte> body) noexcept {
    if (body.size() + 8 > EVENT_MAX_SIZE || body.size() + 8 > PIPE_BUF) return false;
    std::array<std::byte, EVENT_MAX_SIZE> record{};
    event_put32(record.data(), EVENT_MAGIC);
    event_put16(record.data() + 4, EVENT_VERSION);
    event_put16(record.data() + 6, type);
    std::memcpy(record.data() + 8, body.data(), body.size());
    const size_t size = body.size() + 8;
    for (;;) {
        const ssize_t written = ::write(fd, record.data(), size);
        if (written == static_cast<ssize_t>(size)) return true;
        if (written < 0 && errno == EINTR) continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd pfd{.fd = fd, .events = POLLOUT, .revents = 0};
            int rc;
            do { rc = ::poll(&pfd, 1, 1000); } while (rc < 0 && errno == EINTR);
            if (rc > 0 && (pfd.revents & POLLOUT)) continue;
        }
        return false;
    }
}

bool emit_simple_event(int fd, uint16_t type) noexcept {
    return emit_event_record(fd, type, {});
}

bool parse_positive(std::string_view text, long& value) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size() && value > 0;
}

bool emit_event(int fd, std::string_view event) {
    uint16_t type{};
    if (event == "B2_COORDINATOR_READY") type = EVENT_READY;
    else if (event == "B2_SERVER_REGISTERED") type = EVENT_REGISTERED;
    else if (event == "B2_RESULT_PASS") type = EVENT_PASS;
    else if (event == "B2_RESULT_FAIL") type = EVENT_FAIL;
    else return false;
    return emit_simple_event(fd, type);
}

struct Fd {
    int value{-1};
    ~Fd() { if (value >= 0) ::close(value); }
    Fd() = default;
    explicit Fd(int fd) : value(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};

} // namespace

int main(int argc, char** argv) {
#ifndef __linux__
    std::fprintf(stderr, "B2 Coordinator requires Linux SO_PEERCRED\n");
    return 2;
#else
    struct sigaction ignore_pipe{};
    ignore_pipe.sa_handler = SIG_IGN;
    ::sigemptyset(&ignore_pipe.sa_mask);
    if (::sigaction(SIGPIPE, &ignore_pipe, nullptr) != 0) {
        std::fprintf(stderr, "B2 Coordinator: cannot ignore SIGPIPE: %s\n", std::strerror(errno));
        return 1;
    }
    if (argc != 2) {
        std::fprintf(stderr, "usage: yarilo_b2_coordinator EXPECTED_SERVER_PID\n");
        return 2;
    }
    long expected_server_pid = 0;
    const char* event_fd_text = std::getenv("YARILO_B2_EVENT_FD");
    long event_fd_value = 0;
    if (!parse_positive(argv[1], expected_server_pid) || event_fd_text == nullptr ||
        !parse_positive(event_fd_text, event_fd_value) || event_fd_value > INT32_MAX) {
        std::fprintf(stderr, "B2 Coordinator: invalid server PID or YARILO_B2_EVENT_FD\n");
        return 2;
    }

    const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");
    struct stat runtime_stat{};
    if (runtime_dir == nullptr || ::stat(runtime_dir, &runtime_stat) != 0 ||
        !S_ISDIR(runtime_stat.st_mode) || runtime_stat.st_uid != ::getuid() ||
        (runtime_stat.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        std::fprintf(stderr, "B2 Coordinator: XDG_RUNTIME_DIR must be an owned private directory\n");
        return 2;
    }
    const std::string path = b2::socket_path();
    if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path)) {
        std::fprintf(stderr, "B2 Coordinator: invalid XDG_RUNTIME_DIR/socket path\n");
        return 2;
    }
    const int event_fd = static_cast<int>(event_fd_value);
    Fd listener;
    listener.value = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (listener.value < 0) {
        std::fprintf(stderr, "B2 Coordinator: socket: %s\n", std::strerror(errno));
        return 1;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (::bind(listener.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        std::fprintf(stderr, "B2 Coordinator: bind %s: %s\n", path.c_str(), std::strerror(errno));
        return 1;
    }
    struct stat owned_stat{};
    if (::lstat(path.c_str(), &owned_stat) != 0) {
        const int saved = errno;
        std::fprintf(stderr, "B2 Coordinator: cannot stat bound socket path: %s; ownership cannot be verified\n", std::strerror(saved));
        (void)::close(listener.value);
        listener.value = -1;
        return 1;
    }
    if (!S_ISSOCK(owned_stat.st_mode)) {
        std::fprintf(stderr, "B2 Coordinator: bound path is not a socket\n");
        (void)::close(listener.value);
        listener.value = -1;
        return 1;
    }
    const auto socket_dev = owned_stat.st_dev;
    const auto socket_ino = owned_stat.st_ino;
    auto unlink_owned_socket = [&]() noexcept {
        struct stat current{};
        if (::lstat(path.c_str(), &current) != 0) {
            if (errno == ENOENT) return true;
            std::fprintf(stderr, "B2 Coordinator: cannot verify socket path during cleanup: %s\n", std::strerror(errno));
            return false;
        }
        if (current.st_dev != socket_dev || current.st_ino != socket_ino) {
            std::fprintf(stderr, "B2 Coordinator: socket path was replaced; refusing to unlink it\n");
            return false;
        }
        if (::unlink(path.c_str()) != 0) {
            std::fprintf(stderr, "B2 Coordinator: cannot unlink owned socket: %s\n", std::strerror(errno));
            return false;
        }
        return true;
    };
    if (::listen(listener.value, 8) != 0) {
        const int saved = errno;
        (void)unlink_owned_socket();
        std::fprintf(stderr, "B2 Coordinator: listen: %s\n", std::strerror(saved));
        return 1;
    }
    if (!emit_event(event_fd, "B2_COORDINATOR_READY")) {
        std::fprintf(stderr, "B2 Coordinator: runner event pipe unavailable\n");
        (void)unlink_owned_socket();
        return 1;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    Fd peer;
    bool registered = false;
    bool fatal_registration = false;
    b2::Packet accepted_ack{};
    while (!registered && !fatal_registration) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        const auto remaining_duration = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const int remaining = static_cast<int>(remaining_duration.count());
        if (remaining <= 0) break;
        pollfd pfd{.fd = listener.value, .events = POLLIN, .revents = 0};
        const int ready = ::poll(&pfd, 1, remaining);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) break;
        int accepted = ::accept4(listener.value, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (accepted < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
        if (accepted < 0) break;
        Fd candidate(accepted);
        pid_t pid{}; uid_t uid{}; gid_t gid{};
        if (!b2::get_peer_credentials(accepted, pid, uid, gid) || pid != expected_server_pid) continue;
        pollfd client_poll{.fd = accepted, .events = POLLIN, .revents = 0};
        if (::poll(&client_poll, 1, 2000) <= 0 || !(client_poll.revents & POLLIN)) continue;
        b2::Packet packet{};
        if (!b2::receive(accepted, packet)) {
            std::fprintf(stderr, "B2 Coordinator: invalid or incomplete registration packet\n");
            fatal_registration = true;
            break;
        }
        if (packet.type != b2::Type::Register || packet.role != b2::Role::Server ||
            packet.status != static_cast<uint32_t>(b2::Status::Ok) || packet.message_seq == 0 ||
            packet.pid != static_cast<uint64_t>(pid) || packet.uid != static_cast<uint64_t>(uid) ||
            packet.gid != static_cast<uint64_t>(gid) ||
            packet.server_pid != static_cast<uint64_t>(expected_server_pid)) {
            std::fprintf(stderr, "B2 Coordinator: rejected malformed or unauthenticated server registration\n");
            fatal_registration = true;
            break;
        }
        b2::Packet ack{};
        ack.type = b2::Type::Register;
        ack.role = b2::Role::Coordinator;
        ack.status = static_cast<uint32_t>(b2::Status::Ok);
        ack.message_seq = packet.message_seq;
        ack.pid = static_cast<uint64_t>(::getpid());
        ack.uid = static_cast<uint64_t>(::getuid());
        ack.gid = static_cast<uint64_t>(::getgid());
        accepted_ack = ack;
        peer.value = candidate.value;
        candidate.value = -1;
        registered = true;
    }

    const bool socket_clean = unlink_owned_socket();
    if (!registered || !socket_clean || !b2::send(peer.value, accepted_ack) ||
        !emit_event(event_fd, "B2_SERVER_REGISTERED")) {
        (void)emit_event(event_fd, "B2_RESULT_FAIL");
        std::fprintf(stderr, "B2 Coordinator: registration or ACK publication failed\n");
        return 1;
    }

    enum class Phase { WaitDisconnect, WaitUnmapped, WaitRecoveryReady, WaitEof, Failed };
    Phase phase = Phase::WaitDisconnect;
    std::array<b2::Packet, 2> unmapped{};
    std::array<bool, 2> recovery_ready{};
    size_t unmapped_count = 0;
    size_t recovery_count = 0;
    uint64_t expected_seq = accepted_ack.message_seq + 1;
    const uint64_t expected_server_uid = accepted_ack.uid;
    const uint64_t expected_server_gid = accepted_ack.gid;
    uint64_t epoch = 0;
    auto packet_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
    bool barrier_emitted = false;

    auto valid_server_packet = [&](const b2::Packet& packet) noexcept {
        if (packet.message_seq != expected_seq || expected_seq == std::numeric_limits<uint64_t>::max()) return false;
        ++expected_seq;
        return packet.role == b2::Role::Server &&
               packet.status == static_cast<uint32_t>(b2::Status::Ok) &&
               packet.pid == static_cast<uint64_t>(expected_server_pid) &&
               packet.server_pid == static_cast<uint64_t>(expected_server_pid) &&
               packet.uid == expected_server_uid &&
               packet.gid == expected_server_gid;
    };

    auto emit_barrier = [&]() noexcept {
        std::array<std::byte, EVENT_BARRIER_SIZE - 8> body{};
        event_put64(body.data(), epoch);
        for (size_t i = 0; i < 2; ++i) {
            event_put32(body.data() + 8 + i * 8, static_cast<uint32_t>(unmapped[i].session_id));
            event_put32(body.data() + 12 + i * 8, static_cast<uint32_t>(unmapped[i].surface_id));
        }
        return !barrier_emitted && emit_event_record(event_fd, EVENT_BARRIER, body);
    };

    while (phase != Phase::Failed && phase != Phase::WaitEof) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= packet_deadline) { phase = Phase::Failed; break; }
        const int timeout = static_cast<int>(std::max<int64_t>(1,
            std::chrono::duration_cast<std::chrono::milliseconds>(packet_deadline - now).count()));
        pollfd pfd{.fd = peer.value, .events = POLLIN, .revents = 0};
        const int ready = ::poll(&pfd, 1, timeout);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0 || (pfd.revents & (POLLERR | POLLNVAL))) { phase = Phase::Failed; break; }

        bool saw_eof = false;
        if (pfd.revents & (POLLIN | POLLHUP)) {
            for (;;) {
                b2::Packet packet{};
                if (!b2::receive(peer.value, packet)) {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    if (errno == ECONNRESET) { saw_eof = true; break; }
                    phase = Phase::Failed;
                    break;
                }
                packet_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
                if (!valid_server_packet(packet)) { phase = Phase::Failed; break; }
                if (phase == Phase::WaitDisconnect) {
                    if (packet.type != b2::Type::WmDisconnected || packet.epoch != 1 ||
                        packet.changed_count != 2 || packet.reported_count != 2) { phase = Phase::Failed; break; }
                    epoch = packet.epoch;
                    phase = Phase::WaitUnmapped;
                } else if (phase == Phase::WaitUnmapped) {
                    if (packet.type != b2::Type::SurfaceUnmapped || packet.epoch != epoch ||
                        packet.session_id == 0 || packet.surface_id == 0 || packet.server_uid == 0 ||
                        unmapped_count >= unmapped.size()) { phase = Phase::Failed; break; }
                    bool duplicate = false;
                    for (size_t i = 0; i < unmapped_count; ++i)
                        if (unmapped[i].session_id == packet.session_id || unmapped[i].surface_id == packet.surface_id)
                            duplicate = true;
                    if (duplicate) { phase = Phase::Failed; break; }
                    unmapped[unmapped_count++] = packet;
                    if (unmapped_count == unmapped.size()) phase = Phase::WaitRecoveryReady;
                } else if (phase == Phase::WaitRecoveryReady) {
                    if (packet.type != b2::Type::SurfaceRecoveryReady || packet.epoch != epoch ||
                        recovery_count >= recovery_ready.size()) { phase = Phase::Failed; break; }
                    bool matched = false;
                    for (size_t i = 0; i < unmapped.size(); ++i) {
                        if (unmapped[i].session_id == packet.session_id && unmapped[i].surface_id == packet.surface_id &&
                            unmapped[i].server_uid == packet.server_uid && !recovery_ready[i]) {
                            recovery_ready[i] = true;
                            ++recovery_count;
                            matched = true;
                            break;
                        }
                    }
                    if (!matched) { phase = Phase::Failed; break; }
                    if (recovery_count == recovery_ready.size()) {
                        if (!emit_barrier()) { phase = Phase::Failed; break; }
                        barrier_emitted = true;
                        phase = Phase::WaitEof;
                    }
                } else {
                    phase = Phase::Failed;
                    break;
                }
            }
        }
        if (phase == Phase::Failed) break;
        if (saw_eof) {
            if (phase == Phase::WaitEof && barrier_emitted && emit_event(event_fd, "B2_RESULT_PASS")) return 0;
            phase = Phase::Failed;
            break;
        }
        if ((pfd.revents & POLLHUP) && !saw_eof) { phase = Phase::Failed; break; }
    }

    (void)emit_event(event_fd, "B2_RESULT_FAIL");
    std::fprintf(stderr, "B2 Coordinator: protocol failure, timeout, or premature EOF\n");
    return 1;
#endif
}
