#pragma once

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <poll.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace yarilo::test::b2 {

constexpr uint32_t MAGIC = 0x32425259; // YRB2
constexpr const char* SOCKET_NAME = "yarilo-b2-control";
constexpr uint32_t VERSION = 1;
constexpr size_t PACKET_SIZE = 100;

enum class Type : uint32_t {
    Register = 1,
    SurfaceBound = 2,
    ClientPreCrashReady = 3,
    WmDisconnected = 4,
    SurfaceUnmapped = 5,
    SurfaceMapped = 6,
    SurfaceRecoveryReady = 7,
    RecoveryBarrier = 8,
    ClientRecoveryPresented = 9,
    ClientPaused = 10,
    ClientResume = 11,
    Error = 12,
};

enum class Role : uint32_t { Server = 1, Client = 2, Coordinator = 3 };
enum class Status : uint32_t { Ok = 0, Failed = 1 };

struct Packet {
    Type type{Type::Error};
    Role role{Role::Client};
    uint32_t status{0};
    uint64_t epoch{0};
    uint64_t message_seq{0};
    uint64_t pid{0};
    uint64_t uid{0};
    uint64_t gid{0};
    uint64_t server_pid{0};
    uint64_t session_id{0};
    uint64_t surface_id{0};
    uint64_t server_uid{0};
    uint64_t commit_seq{0};
};

inline void put32(std::byte* p, uint32_t v) noexcept {
    p[0] = std::byte(v); p[1] = std::byte(v >> 8);
    p[2] = std::byte(v >> 16); p[3] = std::byte(v >> 24);
}
inline void put64(std::byte* p, uint64_t v) noexcept {
    put32(p, static_cast<uint32_t>(v));
    put32(p + 4, static_cast<uint32_t>(v >> 32));
}
inline uint32_t get32(const std::byte* p) noexcept {
    return std::to_integer<uint32_t>(p[0]) |
           (std::to_integer<uint32_t>(p[1]) << 8) |
           (std::to_integer<uint32_t>(p[2]) << 16) |
           (std::to_integer<uint32_t>(p[3]) << 24);
}
inline uint64_t get64(const std::byte* p) noexcept {
    return static_cast<uint64_t>(get32(p)) |
           (static_cast<uint64_t>(get32(p + 4)) << 32);
}
inline bool send(int fd, const Packet& p) noexcept {
    std::array<std::byte, PACKET_SIZE> b{};

    put32(b.data(), MAGIC);
    put32(b.data() + 4, VERSION);
    put32(b.data() + 8, static_cast<uint32_t>(p.type));
    put32(b.data() + 12, static_cast<uint32_t>(p.role));
    put32(b.data() + 16, p.status);

    put64(b.data() + 20, p.epoch);
    put64(b.data() + 28, p.message_seq);
    put64(b.data() + 36, p.pid);
    put64(b.data() + 44, p.uid);
    put64(b.data() + 52, p.gid);
    put64(b.data() + 60, p.server_pid);
    put64(b.data() + 68, p.session_id);
    put64(b.data() + 76, p.surface_id);
    put64(b.data() + 84, p.server_uid);
    put64(b.data() + 92, p.commit_seq);

    ssize_t n = ::send(fd, b.data(), b.size(), MSG_NOSIGNAL | MSG_DONTWAIT);

    if (n == static_cast<ssize_t>(b.size())) {
        return true;
    }

    if (n >= 0) {
        errno = EIO;
    }

    return false;
}
inline bool receive(int fd, Packet& p) noexcept {
    std::array<std::byte, PACKET_SIZE> b{};

    struct iovec iov {};
    iov.iov_base = b.data();
    iov.iov_len = b.size();

    struct msghdr msg {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    ssize_t n;

    do {
        n = ::recvmsg(fd, &msg, 0);
    } while (n < 0 && errno == EINTR);

    if (n < 0) {
        return false;
    }

    if (n == 0) {
        errno = ECONNRESET;
        return false;
    }

    if (n != static_cast<ssize_t>(PACKET_SIZE) ||
        (msg.msg_flags & MSG_TRUNC) != 0) {
        errno = EPROTO;
        return false;
    }

    if (get32(b.data()) != MAGIC ||
        get32(b.data() + 4) != VERSION) {
        errno = EPROTO;
        return false;
    }

    uint32_t raw_type = get32(b.data() + 8);
    uint32_t raw_role = get32(b.data() + 12);
    uint32_t raw_status = get32(b.data() + 16);

    if (raw_type < static_cast<uint32_t>(Type::Register) ||
        raw_type > static_cast<uint32_t>(Type::Error) ||
        raw_role < static_cast<uint32_t>(Role::Server) ||
        raw_role > static_cast<uint32_t>(Role::Coordinator) ||
        raw_status > static_cast<uint32_t>(Status::Failed)) {
        errno = EPROTO;
        return false;
    }

    p.type = static_cast<Type>(raw_type);
    p.role = static_cast<Role>(raw_role);
    p.status = raw_status;

    p.epoch = get64(b.data() + 20);
    p.message_seq = get64(b.data() + 28);
    p.pid = get64(b.data() + 36);
    p.uid = get64(b.data() + 44);
    p.gid = get64(b.data() + 52);
    p.server_pid = get64(b.data() + 60);
    p.session_id = get64(b.data() + 68);
    p.surface_id = get64(b.data() + 76);
    p.server_uid = get64(b.data() + 84);
    p.commit_seq = get64(b.data() + 92);

    return true;
}
inline std::string socket_path() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");

    if (runtime == nullptr || runtime[0] == '\0') {
        return {};
    }

    return std::string(runtime) + "/" + SOCKET_NAME;
}
inline bool get_peer_credentials(int fd, pid_t& pid, uid_t& uid, gid_t& gid) noexcept {
#ifdef SO_PEERCRED
    struct ucred cred{};
    socklen_t len = sizeof(cred);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 || len != sizeof(cred)) return false;
    pid = cred.pid;
    uid = cred.uid;
    gid = cred.gid;
    return true;
#else
    (void)fd; (void)pid; (void)uid; (void)gid;
    return false;
#endif
}
inline bool connect_socket(const std::string& path, int& fd) noexcept {
    fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) { ::close(fd); fd = -1; return false; }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd); fd = -1; return false;
    }
    return true;
}
inline bool connect_retry(
    int& fd,
    int attempts = 500,
    const std::string& suffix = ""
) {
    const std::string base_path = socket_path();

    if (base_path.empty()) {
        return false;
    }

    const std::string path = base_path + suffix;

    for (int i = 0; i < attempts; ++i) {
        if (connect_socket(path, fd)) {
            return true;
        }

        usleep(10000);
    }

    return false;
}
inline bool listen_socket(const std::string& path, int& fd) noexcept {
    fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);

    if (fd < 0) {
        return false;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;

    if (path.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        fd = -1;
        errno = ENAMETOOLONG;
        return false;
    }

    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        int saved_errno = errno;

        ::close(fd);
        fd = -1;

        errno = saved_errno;
        return false;
    }

    if (::listen(fd, 8) != 0) {
        int saved_errno = errno;

        ::close(fd);
        fd = -1;

        ::unlink(path.c_str());

        errno = saved_errno;
        return false;
    }

    return true;
}
inline bool wait_readable(int fd, int timeout_ms) noexcept {
    pollfd pfd{
        .fd = fd,
        .events = POLLIN,
        .revents = 0
    };

    int rc;

    do {
        rc = ::poll(&pfd, 1, timeout_ms);
    } while (rc < 0 && errno == EINTR);

    if (rc <= 0) {
        return false;
    }

    return (pfd.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0;
}
inline bool send_retry(int fd, const Packet& packet, int attempts = 100) noexcept {
    for (int i = 0; i < attempts; ++i) {
        if (send(fd, packet)) return true;
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return false;
        usleep(10000);
    }
    return false;
}
inline bool receive_retry(
    int fd,
    Packet& packet,
    int attempts = 100,
    int timeout_ms = 100
) noexcept {
    for (int i = 0; i < attempts; ++i) {
        if (!wait_readable(fd, timeout_ms)) {
            continue;
        }

        if (receive(fd, packet)) {
            return true;
        }

        if (errno == EAGAIN ||
            errno == EWOULDBLOCK ||
            errno == EINTR) {
            continue;
        }

        return false;
    }

    return false;
}

} // namespace yarilo::test::b2
