#include "ipc/server_socket.hpp"
#include "core/error.hpp"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <print>

namespace yarilo::ipc {

void ServerSocket::cleanup_socket_file(const std::string& path, dev_t dev, ino_t ino) noexcept {
    if (path.empty() || dev == 0 || ino == 0) return;
    struct stat st{};
    if (::lstat(path.c_str(), &st) == 0) {
        if (st.st_dev == dev && st.st_ino == ino) {
            ::unlink(path.c_str());
        }
    }
}

core::Result<std::unique_ptr<ServerSocket>> ServerSocket::create() {
    const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");
    if (!runtime_dir) {
        std::println(stderr, "Yarilo Error: XDG_RUNTIME_DIR not set.");
        return std::unexpected(core::make_error(core::ServerError::PermissionDenied));
    }

    struct stat dir_st{};
    if (::stat(runtime_dir, &dir_st) != 0 || !S_ISDIR(dir_st.st_mode) || 
        dir_st.st_uid != ::getuid() || (dir_st.st_mode & 0777) != 0700) {
        std::println(stderr, "Yarilo Error: XDG_RUNTIME_DIR must be directory with exact 0700 permissions.");
        return std::unexpected(core::make_error(core::ServerError::PermissionDenied));
    }

    std::string path = std::string(runtime_dir) + "/yarilo-0";

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.length() >= sizeof(addr.sun_path)) {
        return std::unexpected(core::make_error(core::ServerError::PermissionDenied));
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    struct stat existing_st{};
    if (::lstat(path.c_str(), &existing_st) == 0) {
        std::println(stderr, "Yarilo Error: Endpoint already exists at {}. Remove it manually if stale.", path);
        return std::unexpected(core::make_error(core::ServerError::AddressInUse));
    } else if (errno != ENOENT) {
        return std::unexpected(core::make_error(core::ServerError::PermissionDenied));
    }

    int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        return std::unexpected(core::make_error(core::ServerError::PermissionDenied));
    }
    core::UniqueFd sfd(fd);

    if (::bind(sfd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        return std::unexpected(core::make_error(core::ServerError::PermissionDenied));
    }

    struct stat created_st{};
    if (::lstat(path.c_str(), &created_st) != 0) {
        return std::unexpected(core::make_error(core::ServerError::PermissionDenied));
    }

    if (::listen(sfd.get(), 16) != 0) {
        cleanup_socket_file(path, created_st.st_dev, created_st.st_ino);
        return std::unexpected(core::make_error(core::ServerError::PermissionDenied));
    }

    auto server = std::unique_ptr<ServerSocket>(new ServerSocket(std::move(sfd), path));
    server->bound_dev_ = created_st.st_dev;
    server->bound_ino_ = created_st.st_ino;
    return server;
}

ServerSocket::~ServerSocket() {
    cleanup_socket_file(socket_path_, bound_dev_, bound_ino_);
}

core::Result<core::UniqueFd> ServerSocket::accept_client() {
    while (true) {
        int cfd = ::accept4(server_fd_.get(), nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return std::unexpected(core::make_error(core::ServerError::WouldBlock));
            }
            return std::unexpected(core::make_error(core::ServerError::PermissionDenied));
        }
        return core::UniqueFd(cfd);
    }
}

core::Result<ReceivedPacket> ServerSocket::receive_packet(int client_fd) {
    char control_buf[CMSG_SPACE(sizeof(int))]{};
    std::array<std::byte, MAX_MESSAGE_SIZE> raw_buffer{};

    iovec iov{
        .iov_base = raw_buffer.data(),
        .iov_len  = raw_buffer.size(),
    };

    msghdr msg{
        .msg_name       = nullptr,
        .msg_namelen    = 0,
        .msg_iov        = &iov,
        .msg_iovlen     = 1,
        .msg_control    = control_buf,
        .msg_controllen = sizeof(control_buf),
        .msg_flags      = 0,
    };

    ssize_t bytes_read = -1;
    while (true) {
        bytes_read = ::recvmsg(client_fd, &msg, MSG_CMSG_CLOEXEC);
        if (bytes_read < 0 && errno == EINTR) continue;
        break;
    }

    std::array<core::UniqueFd, 1> static_fds;
    size_t fd_count = 0;

    if (bytes_read >= 0) {
        for (cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
                if (cmsg->cmsg_len < CMSG_LEN(sizeof(int))) continue;
                size_t n_fds = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                const int* raw_fds = reinterpret_cast<const int*>(CMSG_DATA(cmsg));
                for (size_t i = 0; i < n_fds; ++i) {
                    if (fd_count < 1) {
                        static_fds[fd_count++].reset(raw_fds[i]);
                    } else {
                        ::close(raw_fds[i]);
                    }
                }
            }
        }
    }

    if (bytes_read < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return std::unexpected(core::make_error(core::ServerError::WouldBlock));
        }
        return std::unexpected(core::make_error(core::ServerError::ProtocolViolation));
    }

    if (bytes_read == 0) {
        // Чистый EOF
        return std::unexpected(core::make_error(core::ServerError::ProtocolViolation));
    }

    if (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) {
        return std::unexpected(core::make_error(core::ServerError::ProtocolViolation));
    }

    if (bytes_read < static_cast<ssize_t>(HEADER_SIZE)) {
        return std::unexpected(core::make_error(core::ServerError::ProtocolViolation));
    }

    auto header_opt = MessageHeader::decode(std::span<const std::byte>(raw_buffer.data(), HEADER_SIZE));
    if (!header_opt) {
        return std::unexpected(core::make_error(core::ServerError::ProtocolViolation));
    }
    const auto& header = *header_opt;

    if (header.magic != PROTOCOL_MAGIC || header.version != PROTOCOL_VERSION) {
        return std::unexpected(core::make_error(core::ServerError::ProtocolViolation));
    }

    if (header.length > (MAX_MESSAGE_SIZE - HEADER_SIZE)) {
        return std::unexpected(core::make_error(core::ServerError::ProtocolViolation));
    }

    if (bytes_read != static_cast<ssize_t>(HEADER_SIZE + header.length)) {
        return std::unexpected(core::make_error(core::ServerError::ProtocolViolation));
    }

    size_t expected_payload_len = 0;
    size_t expected_fds = 0;

    switch (header.type) {
        case MessageType::CreateSurface:
            expected_payload_len = CreateSurfacePayload::ENCODED_SIZE;
            expected_fds = 0;
            break;
        case MessageType::AttachBuffer:
            expected_payload_len = AttachBufferPayload::ENCODED_SIZE;
            expected_fds = 1;
            break;
        case MessageType::CommitSurface:
            expected_payload_len = CommitSurfacePayload::ENCODED_SIZE;
            expected_fds = 0;
            break;
        case MessageType::DestroySurface:
            expected_payload_len = DestroySurfacePayload::ENCODED_SIZE;
            expected_fds = 0;
            break;
        case MessageType::WmHandshake:
            expected_payload_len = 0;
            expected_fds = 0;
            break;
        case MessageType::WmConfigureSurface:
            expected_payload_len = WmConfigureSurfacePayload::ENCODED_SIZE;
            expected_fds = 0;
            break;
        case MessageType::WmSetKeyboardFocus:
            expected_payload_len = WmSetKeyboardFocusPayload::ENCODED_SIZE;
            expected_fds = 0;
            break;
        default:
            return std::unexpected(core::make_error(core::ServerError::ProtocolViolation));
    }

    if (header.length != expected_payload_len || fd_count != expected_fds) {
        return std::unexpected(core::make_error(core::ServerError::ProtocolViolation));
    }

    ReceivedPacket packet;
    packet.header = header;
    packet.payload_len = header.length;
    if (header.length > 0) {
        std::memcpy(packet.payload.data(), raw_buffer.data() + HEADER_SIZE, header.length);
    }
    if (expected_fds == 1) {
        packet.passed_fd = std::move(static_fds[0]);
    }

    return packet;
}

core::Result<void> ServerSocket::send_packet(int client_fd, MessageType type, uint64_t seq, const void* data, size_t size) {
    if (size > (MAX_MESSAGE_SIZE - HEADER_SIZE)) {
        return std::unexpected(core::make_error(core::ServerError::ProtocolViolation));
    }
    if (size > 0 && data == nullptr) {
        return std::unexpected(core::make_error(core::ServerError::ProtocolViolation));
    }

    MessageHeader header{
        .magic    = PROTOCOL_MAGIC,
        .version  = PROTOCOL_VERSION,
        .type     = type,
        .length   = static_cast<uint32_t>(size),
        .sequence = seq,
    };

    std::byte encoded_header[HEADER_SIZE];
    if (!header.encode(encoded_header)) {
        return std::unexpected(core::make_error(core::ServerError::ProtocolViolation));
    }

    iovec iov[2];
    iov[0].iov_base = encoded_header;
    iov[0].iov_len  = HEADER_SIZE;
    iov[1].iov_base = const_cast<void*>(data);
    iov[1].iov_len  = size;

    msghdr msg{};
    msg.msg_iov    = iov;
    msg.msg_iovlen = (size > 0) ? 2 : 1;

    const size_t total_expected = HEADER_SIZE + size;
    ssize_t sent = -1;
    while (true) {
        sent = ::sendmsg(client_fd, &msg, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) continue;
        break;
    }

    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return std::unexpected(core::make_error(core::ServerError::WouldBlock));
        }
        return std::unexpected(core::make_error(core::ServerError::ProtocolViolation));
    }

    if (static_cast<size_t>(sent) != total_expected) {
        return std::unexpected(core::make_error(core::ServerError::ProtocolViolation));
    }
    return {};
}

} // namespace yarilo::ipc