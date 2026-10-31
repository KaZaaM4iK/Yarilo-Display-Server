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
#include <poll.h>
#include <signal.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace b2 = yarilo::test::b2;

namespace {

bool parse_positive(std::string_view text, long& value) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size() && value > 0;
}

bool emit_event(int fd, std::string_view event) {
    std::string line(event);
    line.push_back('\n');
    size_t offset = 0;
    while (offset < line.size()) {
        const ssize_t written = ::write(fd, line.data() + offset, line.size() - offset);
        if (written > 0) {
            offset += static_cast<size_t>(written);
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd pfd{.fd = fd, .events = POLLOUT, .revents = 0};
            int rc;
            do { rc = ::poll(&pfd, 1, 1000); } while (rc < 0 && errno == EINTR);
            if (rc > 0 && (pfd.revents & POLLOUT)) continue;
            return false;
        } else {
            return false;
        }
    }
    return true;
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

    int exit_code = 1;
    const bool socket_clean = unlink_owned_socket();
    if (registered && socket_clean) {
        if (b2::send(peer.value, accepted_ack) && emit_event(event_fd, "B2_SERVER_REGISTERED")) {
            // Registration ACK only; later stages implement the recovery state machine.
            std::fprintf(stderr, "B2 Coordinator: server authenticated; recovery phases not yet implemented\n");
            exit_code = 0;
        } else {
            std::fprintf(stderr, "B2 Coordinator: failed to deliver registration ACK or runner event\n");
            (void)emit_event(event_fd, "B2_RESULT_FAIL");
        }
    } else {
        (void)emit_event(event_fd, "B2_RESULT_FAIL");
        if (!registered)
            std::fprintf(stderr, "B2 Coordinator: server registration failed or timed out\n");
    }
    return exit_code;
#endif
}
