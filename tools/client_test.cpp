#include "ipc/protocol.hpp"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <cstring>
#include <cerrno>
#include <print>
#include <cstdlib>
#include <string>
#include <vector>
#include <array>
#include <chrono>
#include <thread>
#include <algorithm>
#include <utility>

class ClientBuffer {
public:
    ClientBuffer() = default;
    ~ClientBuffer() { reset(); }

    ClientBuffer(const ClientBuffer&) = delete;
    ClientBuffer& operator=(const ClientBuffer&) = delete;

    ClientBuffer(ClientBuffer&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)),
          ptr_(std::exchange(other.ptr_, nullptr)),
          size_(std::exchange(other.size_, 0)),
          is_available_(other.is_available_),
          active_commit_seq_(other.active_commit_seq_) {}

    ClientBuffer& operator=(ClientBuffer&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
            ptr_ = std::exchange(other.ptr_, nullptr);
            size_ = std::exchange(other.size_, 0);
            is_available_ = other.is_available_;
            active_commit_seq_ = other.active_commit_seq_;
        }
        return *this;
    }

    bool allocate(size_t size) {
        reset();
        fd_ = ::memfd_create("yarilo_client_shm", MFD_ALLOW_SEALING | MFD_CLOEXEC);
        if (fd_ < 0) return false;

        if (::ftruncate(fd_, size) != 0) {
            reset();
            return false;
        }

        void* addr = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (addr == MAP_FAILED) {
            reset();
            return false;
        }

        if (::fcntl(fd_, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW) != 0) {
            reset();
            return false;
        }

        ptr_ = addr;
        size_ = size;
        is_available_ = true;
        active_commit_seq_ = 0;
        return true;
    }

    void reset() noexcept {
        if (ptr_ && ptr_ != MAP_FAILED && size_ > 0) {
            ::munmap(ptr_, size_);
        }
        if (fd_ >= 0) {
            ::close(fd_);
        }
        ptr_ = nullptr;
        fd_ = -1;
        size_ = 0;
        is_available_ = false;
        active_commit_seq_ = 0;
    }

    [[nodiscard]] int fd() const noexcept { return fd_; }
    [[nodiscard]] uint32_t* pixels() const noexcept { return static_cast<uint32_t*>(ptr_); }
    [[nodiscard]] bool is_available() const noexcept { return is_available_; }
    void set_available(bool avail) noexcept { is_available_ = avail; }
    
    [[nodiscard]] uint64_t active_seq() const noexcept { return active_commit_seq_; }
    void set_active_seq(uint64_t seq) noexcept { active_commit_seq_ = seq; }

private:
    int fd_{-1};
    void* ptr_{nullptr};
    size_t size_{0};
    bool is_available_{false};
    uint64_t active_commit_seq_{0};
};

static bool receive_expected_packet(int sock_fd, yarilo::ipc::MessageType expected_type, 
                                    uint64_t expected_seq, size_t expected_payload_len,
                                    std::vector<std::byte>& out_payload) 
{
    pollfd pfd{.fd = sock_fd, .events = POLLIN, .revents = 0};
    while (true) {
        int ret = ::poll(&pfd, 1, 2000);
        if (ret < 0 && errno == EINTR) continue;
        if (ret <= 0) return false;

        if (pfd.revents & POLLIN) break;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return false;
    }

    std::vector<std::byte> buf(yarilo::ipc::HEADER_SIZE + expected_payload_len + 16);
    iovec iov{};
    iov.iov_base = buf.data();
    iov.iov_len  = buf.size();

    msghdr msg{};
    std::memset(&msg, 0, sizeof(msg));
    msg.msg_iov    = &iov;
    msg.msg_iovlen = 1;

    ssize_t n = -1;
    while (true) {
        n = ::recvmsg(sock_fd, &msg, 0);
        if (n < 0 && errno == EINTR) continue;
        break;
    }

    if (n < 0 || (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) return false;

    if (static_cast<size_t>(n) != yarilo::ipc::HEADER_SIZE + expected_payload_len) {
        return false;
    }

    auto hdr = yarilo::ipc::MessageHeader::decode(std::span<const std::byte>(buf.data(), yarilo::ipc::HEADER_SIZE));
    if (!hdr) return false;

    if (hdr->magic != yarilo::ipc::PROTOCOL_MAGIC ||
        hdr->version != yarilo::ipc::PROTOCOL_VERSION ||
        hdr->type != expected_type ||
        hdr->sequence != expected_seq ||
        hdr->length != expected_payload_len) {
        return false;
    }

    out_payload.resize(expected_payload_len);
    if (expected_payload_len > 0) {
        std::memcpy(out_payload.data(), buf.data() + yarilo::ipc::HEADER_SIZE, expected_payload_len);
    }
    return true;
}

int main() {
    std::println("Starting Yarilo v0.4 Presentation Client (Strict Validation)...");

    const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");
    if (!runtime_dir) return 1;
    std::string path = std::string(runtime_dir) + "/yarilo-0";

    int sock_fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (sock_fd < 0) return 1;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(sock_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::println(stderr, "Connect failed: {}", std::strerror(errno));
        ::close(sock_fd);
        return 1;
    }

    // 1. Handshake
    std::byte create_packet[yarilo::ipc::HEADER_SIZE + yarilo::ipc::CreateSurfacePayload::ENCODED_SIZE];
    yarilo::ipc::MessageHeader create_hdr{
        .magic    = yarilo::ipc::PROTOCOL_MAGIC,
        .version  = yarilo::ipc::PROTOCOL_VERSION,
        .type     = yarilo::ipc::MessageType::CreateSurface,
        .length   = yarilo::ipc::CreateSurfacePayload::ENCODED_SIZE,
        .sequence = 1,
    };
    if (!create_hdr.encode(create_packet)) return 1;
    yarilo::ipc::write_le32(create_packet + yarilo::ipc::HEADER_SIZE, 1);
    
    if (::send(sock_fd, create_packet, sizeof(create_packet), MSG_NOSIGNAL) != sizeof(create_packet)) return 1;

    std::vector<std::byte> ack_payload;
    if (!receive_expected_packet(sock_fd, yarilo::ipc::MessageType::SurfaceCreated, 1, 4, ack_payload)) {
        std::println(stderr, "Client Fatal: Failed to receive valid SurfaceCreated handshake");
        ::close(sock_fd);
        return 1;
    }
    uint32_t ack_surface_id = yarilo::ipc::read_le32(ack_payload.data());
    if (ack_surface_id != 1) return 1;
    std::println("[1/3] Surface registered.");

    // 2. Двойная буферизация
    constexpr uint32_t W = 600;
    constexpr uint32_t H = 400;
    constexpr uint32_t STRIDE = W * 4;
    constexpr size_t BUF_SIZE = STRIDE * H;

    std::array<ClientBuffer, 2> pool;
    if (!pool[0].allocate(BUF_SIZE) || !pool[1].allocate(BUF_SIZE)) return 1;

    auto attach_buffer = [&](uint32_t buffer_id, int fd, uint64_t seq) -> bool {
        std::byte attach_packet[yarilo::ipc::HEADER_SIZE + yarilo::ipc::AttachBufferPayload::ENCODED_SIZE];
        yarilo::ipc::MessageHeader attach_hdr{
            .magic    = yarilo::ipc::PROTOCOL_MAGIC,
            .version  = yarilo::ipc::PROTOCOL_VERSION,
            .type     = yarilo::ipc::MessageType::AttachBuffer,
            .length   = yarilo::ipc::AttachBufferPayload::ENCODED_SIZE,
            .sequence = seq,
        };
        if (!attach_hdr.encode(attach_packet)) return false;

        std::byte* p = attach_packet + yarilo::ipc::HEADER_SIZE;
        yarilo::ipc::write_le32(p + 0, 1);
        yarilo::ipc::write_le32(p + 4, buffer_id);
        yarilo::ipc::write_le32(p + 8, W);
        yarilo::ipc::write_le32(p + 12, H);
        yarilo::ipc::write_le32(p + 16, STRIDE);
        yarilo::ipc::write_le32(p + 20, 0x34325258);
        yarilo::ipc::write_le64(p + 24, 0);

        char cmsg_buf[CMSG_SPACE(sizeof(int))]{};
        std::memset(cmsg_buf, 0, sizeof(cmsg_buf));
        iovec iov{ .iov_base = attach_packet, .iov_len = sizeof(attach_packet) };
        msghdr msg{};
        std::memset(&msg, 0, sizeof(msg));
        msg.msg_iov        = &iov;
        msg.msg_iovlen     = 1;
        msg.msg_control    = cmsg_buf;
        msg.msg_controllen = sizeof(cmsg_buf);

        cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type  = SCM_RIGHTS;
        cmsg->cmsg_len   = CMSG_LEN(sizeof(int));
        int raw_fd = fd;
        std::memcpy(CMSG_DATA(cmsg), &raw_fd, sizeof(int));

        return ::sendmsg(sock_fd, &msg, MSG_NOSIGNAL) == sizeof(attach_packet);
    };

    if (!attach_buffer(1, pool[0].fd(), 2) || !attach_buffer(2, pool[1].fd(), 3)) return 1;
    std::println("[2/3] Buffer pool attached.");

    // 3. Анимация со строгой валидацией FramePresented
    std::println("[3/3] Streaming 300 frames with hardware page-flip tracking...");

    int box_x = 50, box_y = 50;
    int box_vx = 6, box_vy = 4;
    constexpr int BOX_SIZE = 120;

    uint64_t current_seq = 10;
    uint64_t last_presented_seq = 0;
    uint64_t confirmed_presented_count = 0;
    pollfd pfd{.fd = sock_fd, .events = POLLIN, .revents = 0};

    auto render_frame = [&](ClientBuffer& buf) {
        uint32_t* p = buf.pixels();
        std::fill_n(p, W * H, 0x00101828);

        for (uint32_t y = 0; y < H; ++y) {
            for (uint32_t x = 0; x < W; ++x) {
                if (x < 4 || x >= W - 4 || y < 4 || y >= H - 4) {
                    p[y * W + x] = 0x00FFFFFF;
                }
            }
        }

        for (int y = box_y; y < box_y + BOX_SIZE; ++y) {
            if (y >= 0 && y < static_cast<int>(H)) {
                for (int x = box_x; x < box_x + BOX_SIZE; ++x) {
                    if (x >= 0 && x < static_cast<int>(W)) {
                        p[y * W + x] = 0x0000F5D4;
                    }
                }
            }
        }
    };

    auto handle_incoming_events = [&](int timeout_ms) -> bool {
        while (true) {
            int ret = ::poll(&pfd, 1, timeout_ms);
            if (ret < 0 && errno == EINTR) continue;
            if (ret < 0) return false;
            if (ret == 0) return true;

            if (pfd.revents & POLLIN) {
                std::byte reply[128];
                ssize_t bytes_recv = ::recv(sock_fd, reply, sizeof(reply), 0);
                if (bytes_recv < static_cast<ssize_t>(yarilo::ipc::HEADER_SIZE)) return false;

                auto hdr = yarilo::ipc::MessageHeader::decode(
                    std::span<const std::byte>(reply, yarilo::ipc::HEADER_SIZE));
                if (!hdr || hdr->magic != yarilo::ipc::PROTOCOL_MAGIC || hdr->version != yarilo::ipc::PROTOCOL_VERSION) {
                    return false;
                }

                if (bytes_recv != static_cast<ssize_t>(yarilo::ipc::HEADER_SIZE + hdr->length)) {
                    return false;
                }

                if (hdr->type == yarilo::ipc::MessageType::BufferReleased) {
                    if (hdr->length != yarilo::ipc::BufferReleasedPayload::ENCODED_SIZE) {
                        return false;
                    }

                    const std::byte* payload = reply + yarilo::ipc::HEADER_SIZE;

                    const uint32_t surface_id = yarilo::ipc::read_le32(payload);
                    const uint32_t buffer_id = yarilo::ipc::read_le32(payload + 4);
                    const uint64_t commit_seq = yarilo::ipc::read_le64(payload + 8);

                    if (surface_id == 1 && buffer_id >= 1 && buffer_id <= 2) {
                        auto& b = pool[buffer_id - 1];
                        if (b.active_seq() == commit_seq) {
                            b.set_available(true);
                        }
                    }
                } else if (hdr->type == yarilo::ipc::MessageType::FramePresented) {
                    if (hdr->length != yarilo::ipc::FramePresentedPayload::ENCODED_SIZE) {
                        return false;
                    }

                    const std::byte* payload = reply + yarilo::ipc::HEADER_SIZE;

                    const uint32_t surface_id = yarilo::ipc::read_le32(payload);
                    const uint64_t commit_seq = yarilo::ipc::read_le64(payload + 4);
                    // Поле presentation_time_ns занимает следующие 8 байт.
                    const uint64_t presentation_time_ns = yarilo::ipc::read_le64(payload + 12);
                    (void)presentation_time_ns;

                    if (surface_id == 1 &&
                        commit_seq > last_presented_seq &&
                        commit_seq <= current_seq) {
                        last_presented_seq = commit_seq;
                        confirmed_presented_count++;
                    } else {
                        std::println(stderr,
                            "Client Warning: Non-monotonic or invalid FramePresented sequence");
                    }
                } else if (hdr->type == yarilo::ipc::MessageType::ProtocolError) {
                    return false;
                }
            } else if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                return false;
            }
            timeout_ms = 0;
        }
    };

    const auto client_start_time = std::chrono::steady_clock::now();
    auto get_client_deadline = [&](uint64_t idx) {
        auto ns = std::chrono::nanoseconds((idx * 1'000'000'000ULL) / 60ULL);
        return client_start_time + ns;
    };

    for (int frame = 0; frame < 300; ++frame) {
        auto deadline = get_client_deadline(frame + 1);

        box_x += box_vx;
        box_y += box_vy;
        if (box_x <= 4 || box_x + BOX_SIZE >= static_cast<int>(W) - 4) box_vx = -box_vx;
        if (box_y <= 4 || box_y + BOX_SIZE >= static_cast<int>(H) - 4) box_vy = -box_vy;

        if (!handle_incoming_events(0)) return 1;

        auto wait_start = std::chrono::steady_clock::now();
        while (!pool[0].is_available() && !pool[1].is_available()) {
            if (!handle_incoming_events(10)) return 1;
            if (std::chrono::steady_clock::now() - wait_start > std::chrono::milliseconds(3000)) return 1;
        }

        int target_idx = pool[0].is_available() ? 0 : 1;
        auto& buf = pool[target_idx];
        
        uint64_t seq = ++current_seq;
        buf.set_available(false);
        buf.set_active_seq(seq);
        render_frame(buf);

        std::byte commit_packet[yarilo::ipc::HEADER_SIZE + yarilo::ipc::CommitSurfacePayload::ENCODED_SIZE];
        yarilo::ipc::MessageHeader commit_hdr{
            .magic    = yarilo::ipc::PROTOCOL_MAGIC,
            .version  = yarilo::ipc::PROTOCOL_VERSION,
            .type     = yarilo::ipc::MessageType::CommitSurface,
            .length   = yarilo::ipc::CommitSurfacePayload::ENCODED_SIZE,
            .sequence = seq,
        };
        if (!commit_hdr.encode(commit_packet)) return 1;

        std::byte* p = commit_packet + yarilo::ipc::HEADER_SIZE;
        yarilo::ipc::write_le32(p + 0, 1);
        yarilo::ipc::write_le32(p + 4, static_cast<uint32_t>(target_idx + 1));
        yarilo::ipc::write_le64(p + 8, seq);

        if (::send(sock_fd, commit_packet, sizeof(commit_packet), MSG_NOSIGNAL) != sizeof(commit_packet)) {
            buf.set_available(true);
            return 1;
        }

        auto now = std::chrono::steady_clock::now();
        if (deadline > now) {
            std::this_thread::sleep_for(deadline - now);
        }
    }

    auto drain_start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - drain_start < std::chrono::milliseconds(500)) {
        handle_incoming_events(20);
    }

    std::println("\nClient Summary: 300 committed, {} verified presented by hardware vblank.", confirmed_presented_count);
    ::close(sock_fd);
    return 0;
}