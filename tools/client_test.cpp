#include "ipc/protocol.hpp"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <print>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <chrono>
#include <unordered_set>
#include <thread>
#include <algorithm>
#include <utility>
#include <stdexcept>

// Локальный RAII-guard для сокета, чтобы не зависеть от серверного core::UniqueFd
struct ScopedFd {
    int fd{-1};
    explicit ScopedFd(int f) noexcept : fd(f) {}
    ~ScopedFd() noexcept { if (fd >= 0) ::close(fd); }
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    int get() const noexcept { return fd; }
};

struct TestConfig {
    std::string client_name{"Client"};
    bool expect_motion{false};
    bool expect_no_motion{false};
    bool expect_out_of_bounds{false};
    int input_timeout_ms{500};
    int crash_after_commits{0};
    bool allow_dropped_frames{false};
};


static constexpr uint64_t FIRST_COMMIT_SEQ = 11;
struct TestMetrics {
    uint64_t current_seq{FIRST_COMMIT_SEQ - 1};
    uint64_t last_presented_seq{0};
    uint64_t confirmed_presented_count{0};
    uint32_t commits_sent_count{0};

    uint64_t motion_events_count{0};
    uint64_t inside_bounds_motion_count{0};
    uint64_t outside_bounds_motion_count{0};
    uint64_t button_events_count{0};
    uint64_t key_events_count{0};
    uint64_t protocol_errors_count{0};

    // Drag state machine для --expect-out-of-bounds
    enum class DragState {
        WAIT_PRESS,           // Ожидается нажатие BTN_LEFT, курсор внутри
        PRESSED,              // Кнопка нажата, ожидается движение за границы
        DRAGGED_OUTSIDE,      // Движение за границы при удерживаемой кнопке подтверждено
        RELEASED,             // Отпускание после подтверждённого drag (терминальное)
        FAILED                // Нарушена последовательность (терминальное)
    };
    DragState drag_state{DragState::WAIT_PRESS};
    bool inside_motion_received_before_outside{false};
    bool cursor_position_known{false};
    bool cursor_inside_window{false};
    int32_t cursor_x{0};
    int32_t cursor_y{0};

    // Диагностика FramePresented
    uint64_t duplicate_presented_count{0};
    uint64_t out_of_order_presented_count{0};
    std::unordered_set<uint64_t> confirmed_presented_seqs;

    std::vector<std::string> fail_reasons;

    void add_fail(const std::string& reason) {
        fail_reasons.push_back(reason);
    }
};

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

        ptr_ = addr;
        size_ = size;

        if (::fcntl(fd_, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW) != 0) {
            reset();
            return false;
        }

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
                                    std::vector<std::byte>& out_payload, TestMetrics& metrics) 
{
    pollfd pfd{.fd = sock_fd, .events = POLLIN, .revents = 0};
    while (true) {
        int ret = ::poll(&pfd, 1, 2000);
        if (ret < 0 && errno == EINTR) continue;
        if (ret <= 0) {
            metrics.add_fail("Poll timeout or error during handshake");
            return false;
        }
        if (pfd.revents & POLLIN) break;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            metrics.add_fail("Socket error/EOF during handshake");
            return false;
        }
    }

    std::vector<std::byte> buf(yarilo::ipc::HEADER_SIZE + expected_payload_len + 16);
    iovec iov{ .iov_base = buf.data(), .iov_len = buf.size() };
    msghdr msg{};
    msg.msg_iov    = &iov;
    msg.msg_iovlen = 1;

    ssize_t n = -1;
    while (true) {
        n = ::recvmsg(sock_fd, &msg, 0);
        if (n < 0 && errno == EINTR) continue;
        break;
    }

    if (n <= 0) {
        metrics.add_fail("Socket EOF or read error");
        return false;
    }
    if (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) {
        metrics.add_fail("Truncated packet received");
        return false;
    }
    if (static_cast<size_t>(n) != yarilo::ipc::HEADER_SIZE + expected_payload_len) {
        metrics.add_fail("Packet length mismatch");
        return false;
    }

    auto hdr = yarilo::ipc::MessageHeader::decode(std::span<const std::byte>(buf.data(), yarilo::ipc::HEADER_SIZE));
    if (!hdr || hdr->magic != yarilo::ipc::PROTOCOL_MAGIC || hdr->version != yarilo::ipc::PROTOCOL_VERSION) {
        metrics.add_fail("Invalid protocol header");
        return false;
    }

    if (hdr->type != expected_type || hdr->sequence != expected_seq || hdr->length != expected_payload_len) {
        metrics.add_fail("Unexpected packet type, seq or length");
        return false;
    }

    out_payload.resize(expected_payload_len);
    if (expected_payload_len > 0) {
        std::memcpy(out_payload.data(), buf.data() + yarilo::ipc::HEADER_SIZE, expected_payload_len);
    }
    return true;
}

void run_test(const TestConfig& config, TestMetrics& metrics) {
    const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");
    if (!runtime_dir) {
        metrics.add_fail("XDG_RUNTIME_DIR not set");
        return;
    }
    std::string path = std::string(runtime_dir) + "/yarilo-0";

    int sock_fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (sock_fd < 0) {
        metrics.add_fail("socket() failed");
        return;
    }

    ScopedFd socket_guard(sock_fd);

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(sock_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        metrics.add_fail(std::string("Connect failed: ") + std::strerror(errno));
        return;
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
    if (!create_hdr.encode(create_packet)) { 
        metrics.add_fail("Encode CreateSurface failed"); 
        return; 
    }
    yarilo::ipc::write_le32(create_packet + yarilo::ipc::HEADER_SIZE, 1);
    
    if (::send(sock_fd, create_packet, sizeof(create_packet), MSG_NOSIGNAL) != sizeof(create_packet)) {
        metrics.add_fail("Send CreateSurface failed");
        return;
    }

    std::vector<std::byte> ack_payload;
    if (!receive_expected_packet(sock_fd, yarilo::ipc::MessageType::SurfaceCreated, 1, 4, ack_payload, metrics)) {
        return;
    }
    
    uint32_t ack_surface_id = yarilo::ipc::read_le32(ack_payload.data());
    if (ack_surface_id != 1) {
        metrics.add_fail("Invalid surface_id returned from server");
        return;
    }
    std::println("[1/3] Surface registered.");

    // 2. Буферизация
    constexpr uint32_t W = 600;
    constexpr uint32_t H = 400;
    constexpr uint32_t STRIDE = W * 4;
    constexpr size_t BUF_SIZE = STRIDE * H;
    std::array<ClientBuffer, 2> pool;

    if (!pool[0].allocate(BUF_SIZE) || !pool[1].allocate(BUF_SIZE)) {
        metrics.add_fail("SHM allocation failed");
        return;
    }

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
        iovec iov{ .iov_base = attach_packet, .iov_len = sizeof(attach_packet) };
        msghdr msg{};
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

    if (!attach_buffer(1, pool[0].fd(), 2) || !attach_buffer(2, pool[1].fd(), 3)) {
        metrics.add_fail("AttachBuffer sendmsg failed");
        return;
    }
    std::println("[2/3] Buffer pool attached.");

    std::println("[3/3] Streaming 300 frames with strict input verification...");

    int box_x = 50, box_y = 50;
    int box_vx = 6, box_vy = 4;
    constexpr int BOX_SIZE = 120;

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
            if (ret < 0) { metrics.add_fail("poll() error in event loop"); return false; }
            if (ret == 0) return true;

            if (pfd.revents & POLLIN) {
                std::byte reply[256];
                iovec iov{ .iov_base = reply, .iov_len = sizeof(reply) };
                msghdr msg{};
                msg.msg_iov = &iov;
                msg.msg_iovlen = 1;

                ssize_t bytes_recv = ::recvmsg(sock_fd, &msg, 0);
                if (bytes_recv < 0) {
                    if (errno == EINTR) continue;
                    metrics.add_fail("recvmsg error"); 
                    return false;
                }
                if (bytes_recv == 0) { 
                    metrics.add_fail("Socket EOF"); 
                    return false; 
                }

                if (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) {
                    metrics.add_fail("Message truncated in recvmsg");
                    metrics.protocol_errors_count++; return false;
                }

                if (bytes_recv < static_cast<ssize_t>(yarilo::ipc::HEADER_SIZE)) {
                    metrics.add_fail("Message shorter than header");
                    metrics.protocol_errors_count++; return false;
                }

                auto hdr = yarilo::ipc::MessageHeader::decode(
                    std::span<const std::byte>(reply, yarilo::ipc::HEADER_SIZE));
                if (!hdr || hdr->magic != yarilo::ipc::PROTOCOL_MAGIC || hdr->version != yarilo::ipc::PROTOCOL_VERSION) {
                    metrics.add_fail("Invalid magic or version");
                    metrics.protocol_errors_count++; return false;
                }

                if (bytes_recv != static_cast<ssize_t>(yarilo::ipc::HEADER_SIZE + hdr->length)) {
                    metrics.add_fail("Payload length mismatch");
                    metrics.protocol_errors_count++; return false;
                }

                const std::byte* payload = reply + yarilo::ipc::HEADER_SIZE;

                if (hdr->type == yarilo::ipc::MessageType::BufferReleased) {
                    if (hdr->length != yarilo::ipc::BufferReleasedPayload::ENCODED_SIZE) {
                        metrics.add_fail("Bad BufferReleased length"); metrics.protocol_errors_count++; return false;
                    }
                    const uint32_t surface_id = yarilo::ipc::read_le32(payload);
                    const uint32_t buffer_id = yarilo::ipc::read_le32(payload + 4);
                    const uint64_t commit_seq = yarilo::ipc::read_le64(payload + 8);

                    if (surface_id != 1 || buffer_id < 1 || buffer_id > 2) {
                        metrics.add_fail("BufferReleased: invalid surface_id or buffer_id");
                        metrics.protocol_errors_count++; return false;
                    }
                    if (commit_seq == 0) {
                        metrics.add_fail("BufferReleased: zero commit_seq");
                        metrics.protocol_errors_count++; return false;
                    }
                    auto& b = pool[buffer_id - 1];
                    if (b.active_seq() != commit_seq) {
                        metrics.add_fail("BufferReleased: commit_seq mismatch");
                        metrics.protocol_errors_count++; return false;
                    }
                    if (b.is_available()) {
                        metrics.add_fail("BufferReleased: duplicate release received");
                        metrics.protocol_errors_count++; return false;
                    }
                    b.set_available(true);
                } else if (hdr->type == yarilo::ipc::MessageType::FramePresented) {
                    if (hdr->length != yarilo::ipc::FramePresentedPayload::ENCODED_SIZE) {
                        metrics.add_fail("Bad FramePresented length"); metrics.protocol_errors_count++; return false;
                    }
                    const uint32_t surface_id = yarilo::ipc::read_le32(payload);
                    const uint64_t commit_seq = yarilo::ipc::read_le64(payload + 4);

                    if (surface_id != 1) {
                        metrics.add_fail("FramePresented: invalid surface_id");
                        metrics.protocol_errors_count++; return false;
                    }

                    if (commit_seq == 0) {
                        metrics.add_fail("FramePresented: zero commit_seq");
                        metrics.protocol_errors_count++;
                    } else if (commit_seq > metrics.current_seq) {
                        metrics.add_fail("FramePresented: commit_seq exceeds last sent commit");
                        metrics.protocol_errors_count++;
                    } else if (commit_seq < FIRST_COMMIT_SEQ) {
                        metrics.add_fail("FramePresented: commit_seq for uncommitted buffer");
                        metrics.protocol_errors_count++;
                    } else if (!metrics.confirmed_presented_seqs.insert(commit_seq).second) {
                        metrics.duplicate_presented_count++;
                        metrics.add_fail("FramePresented: duplicate sequence received");
                        metrics.protocol_errors_count++;
                    } else {
                        metrics.confirmed_presented_count++;
                        if (commit_seq < metrics.last_presented_seq) {
                            metrics.out_of_order_presented_count++;
                            metrics.add_fail("FramePresented: out-of-order sequence received");
                            metrics.protocol_errors_count++;
                        }
                        if (commit_seq > metrics.last_presented_seq) {
                            metrics.last_presented_seq = commit_seq;
                        }
                    }
                } else if (hdr->type == yarilo::ipc::MessageType::PointerMotion) {
                    auto p = yarilo::ipc::PointerMotionPayload::decode(std::span<const std::byte>(payload, hdr->length));
                    if (!p) {
                        metrics.add_fail("Malformed PointerMotionPayload");
                        metrics.protocol_errors_count++;
                        return false;
                    }

                    metrics.motion_events_count++;

                    bool inside = (p->surface_x >= 0 && p->surface_x < static_cast<int32_t>(W) &&
                                   p->surface_y >= 0 && p->surface_y < static_cast<int32_t>(H));

                    // Обновляем текущие координаты курсора (всегда)
                    metrics.cursor_x = p->surface_x;
                    metrics.cursor_y = p->surface_y;
                    metrics.cursor_inside_window = inside;
                    metrics.cursor_position_known = true;

                    if (inside) {
                        metrics.inside_bounds_motion_count++;
                        // Фиксируем внутриоконное движение ТОЛЬКО до первого выхода за границы
                        if (metrics.drag_state == TestMetrics::DragState::PRESSED &&
                            !metrics.inside_motion_received_before_outside) {
                            metrics.inside_motion_received_before_outside = true;
                        }
                    } else {
                        metrics.outside_bounds_motion_count++;
                        if (metrics.drag_state == TestMetrics::DragState::PRESSED) {
                            metrics.drag_state = TestMetrics::DragState::DRAGGED_OUTSIDE;
                        }
                    }

                    if (metrics.motion_events_count % 10 == 1) {
                        std::println("  -> [Input] {} Motion #{} at local ({}, {}) time={}us",
                                     config.client_name, metrics.motion_events_count, p->surface_x, p->surface_y, p->timestamp_us);
                    }
                } else if (hdr->type == yarilo::ipc::MessageType::PointerButton) {
                    auto p = yarilo::ipc::PointerButtonPayload::decode(std::span<const std::byte>(payload, hdr->length));
                    if (!p) {
                        metrics.add_fail("Malformed PointerButtonPayload"); metrics.protocol_errors_count++; return false;
                    }
                    metrics.button_events_count++;
                    std::println("  -> [Input] {} Button btn={} state={} time={}us", 
                                 config.client_name, p->button, p->state, p->timestamp_us);

                    // Drag state machine для --expect-out-of-bounds
                    if (p->button == BTN_LEFT) {
                        // Терминальные состояния — игнорируем последующие события
                        if (metrics.drag_state == TestMetrics::DragState::FAILED ||
                            metrics.drag_state == TestMetrics::DragState::RELEASED) {
                            // Автомат завершён, не меняем состояние
                        } else if (p->state == 1) { // Press
                            if (metrics.drag_state == TestMetrics::DragState::WAIT_PRESS) {
                                if (!metrics.cursor_position_known || !metrics.cursor_inside_window) {
                                    metrics.drag_state = TestMetrics::DragState::FAILED;
                                } else {
                                    metrics.drag_state = TestMetrics::DragState::PRESSED;
                                }
                            } else {
                                // Любое нажатие вне WAIT_PRESS — нарушение
                                metrics.drag_state = TestMetrics::DragState::FAILED;
                            }
                        } else { // Release
                            if (metrics.drag_state == TestMetrics::DragState::WAIT_PRESS) {
                                metrics.drag_state = TestMetrics::DragState::FAILED;
                            } else if (metrics.drag_state == TestMetrics::DragState::PRESSED) {
                                // Отпускание до выхода за границы
                                metrics.drag_state = TestMetrics::DragState::FAILED;
                            } else if (metrics.drag_state == TestMetrics::DragState::DRAGGED_OUTSIDE) {
                                metrics.drag_state = TestMetrics::DragState::RELEASED;
                            }
                        }
                    }
                } else if (hdr->type == yarilo::ipc::MessageType::KeyboardKey) {
                    auto p = yarilo::ipc::KeyboardKeyPayload::decode(std::span<const std::byte>(payload, hdr->length));
                    if (!p) {
                        metrics.add_fail("Malformed KeyboardKeyPayload"); metrics.protocol_errors_count++; return false;
                    }
                    metrics.key_events_count++;
                    std::println("  -> [Input] {} Key scancode={} state={} time={}us", 
                                 config.client_name, p->key, p->state, p->timestamp_us);
                } else if (hdr->type == yarilo::ipc::MessageType::ProtocolError) {
                    metrics.add_fail("Server returned ProtocolError"); metrics.protocol_errors_count++; return false;
                } else {
                    metrics.add_fail("Unexpected message type"); metrics.protocol_errors_count++; return false;
                }
            } else if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                metrics.add_fail("Socket POLLERR/HUP"); 
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

    const int max_frames = (config.crash_after_commits > 0) ? config.crash_after_commits : 300;
    for (int frame = 0; frame < max_frames; ++frame) {
        auto deadline = get_client_deadline(frame + 1);

        box_x += box_vx; box_y += box_vy;
        if (box_x <= 4 || box_x + BOX_SIZE >= static_cast<int>(W) - 4) box_vx = -box_vx;
        if (box_y <= 4 || box_y + BOX_SIZE >= static_cast<int>(H) - 4) box_vy = -box_vy;

        if (!handle_incoming_events(0)) return;

        auto wait_start = std::chrono::steady_clock::now();
        while (!pool[0].is_available() && !pool[1].is_available()) {
            if (!handle_incoming_events(10)) return;
            if (std::chrono::steady_clock::now() - wait_start > std::chrono::milliseconds(3000)) {
                metrics.add_fail("Timeout waiting for free buffer");
                return;
            }
        }

        int target_idx = pool[0].is_available() ? 0 : 1;
        auto& buf = pool[target_idx];
        
        uint64_t seq = ++metrics.current_seq;
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
        if (!commit_hdr.encode(commit_packet)) { 
            metrics.add_fail("Encode CommitSurface failed"); 
            return; 
        }

        std::byte* p = commit_packet + yarilo::ipc::HEADER_SIZE;
        yarilo::ipc::write_le32(p + 0, 1);
        yarilo::ipc::write_le32(p + 4, static_cast<uint32_t>(target_idx + 1));
        yarilo::ipc::write_le64(p + 8, seq);

        if (::send(sock_fd, commit_packet, sizeof(commit_packet), MSG_NOSIGNAL) != sizeof(commit_packet)) {
            metrics.add_fail("Send CommitSurface failed"); 
            return;
        }
        metrics.commits_sent_count++;

        auto now = std::chrono::steady_clock::now();
        if (deadline > now) {
            std::this_thread::sleep_for(deadline - now);
        }
    }

    if (config.crash_after_commits > 0) {
        auto wait_start = std::chrono::steady_clock::now();
        while (metrics.confirmed_presented_count < static_cast<uint64_t>(config.crash_after_commits)) {
            if (!handle_incoming_events(20)) break;
            if (std::chrono::steady_clock::now() - wait_start > std::chrono::milliseconds(3000)) {
                metrics.add_fail("Timeout waiting for FramePresented before crash checkpoint");
                break;
            }
        }

        if (metrics.confirmed_presented_count == static_cast<uint64_t>(config.crash_after_commits) &&
            metrics.fail_reasons.empty() && metrics.protocol_errors_count == 0 &&
            metrics.duplicate_presented_count == 0 && metrics.out_of_order_presented_count == 0) 
        {
            std::println("  -> [CRASH CHECKPOINT] {} verified {}/{} presentations. Terminating via SIGKILL...",
                         config.client_name, metrics.confirmed_presented_count, config.crash_after_commits);
            ::kill(::getpid(), SIGKILL);
        }

        std::println(stderr, "  -> [ERROR] {} failed to reach clean crash checkpoint!", config.client_name);
        return;
    }

    auto drain_start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - drain_start < std::chrono::milliseconds(config.input_timeout_ms)) {
        if (!handle_incoming_events(20)) return;
    }
}

int main(int argc, char** argv) {
    TestConfig config;
    bool client_id_set = false;
    
    // Строгий парсер аргументов
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--expect-motion") {
            config.expect_motion = true;
        } else if (arg == "--expect-no-motion") {
            config.expect_no_motion = true;
        } else if (arg == "--expect-out-of-bounds") {
            config.expect_out_of_bounds = true;
        } else if (arg == "--input-timeout-ms") {
            if (i + 1 >= argc) {
                std::println(stderr, "Client Fatal: Missing value for --input-timeout-ms");
                return 1;
            }
            std::string val = argv[++i];
            size_t pos = 0;
            try {
                config.input_timeout_ms = std::stoi(val, &pos);
                if (pos != val.length() || config.input_timeout_ms <= 0) {
                    throw std::invalid_argument("Invalid or non-positive timeout");
                }
            } catch (...) {
                std::println(stderr, "Client Fatal: Invalid value for --input-timeout-ms");
                return 1;
            }
            } else if (arg == "--allow-dropped-frames") {
                config.allow_dropped_frames = true;
            } else if (arg == "--crash-after-commits") {
            if (i + 1 >= argc) {
                std::println(stderr, "Client Fatal: Missing value for --crash-after-commits");
                return 1;
            }
            std::string val = argv[++i];
            size_t pos = 0;
            try {
                config.crash_after_commits = std::stoi(val, &pos);
                if (pos != val.length() || config.crash_after_commits <= 0) {
                    throw std::invalid_argument("Invalid or non-positive commit count");
                }
            } catch (...) {
                std::println(stderr, "Client Fatal: Invalid value for --crash-after-commits");
                return 1;
            }
        } else if (arg.starts_with("--")) {
            std::println(stderr, "Client Fatal: Unknown argument {}", arg);
            return 1;
        } else {
            if (client_id_set) {
                std::println(stderr, "Client Fatal: Multiple client identifiers provided.");
                return 1;
            }
            config.client_name = "Client " + std::string(arg);
            client_id_set = true;
        }
    }

    if (config.expect_motion && config.expect_no_motion) {
        std::println(stderr, "Client Fatal: --expect-motion and --expect-no-motion are mutually exclusive.");
        return 1;
    }

    if (config.expect_no_motion && config.expect_out_of_bounds) {
        std::println(stderr, "Client Fatal: --expect-no-motion and --expect-out-of-bounds are mutually exclusive.");
        return 1;
    }

    TestMetrics metrics;
    run_test(config, metrics);

    // Валидация метрик после завершения сетевой фазы
    if (!config.allow_dropped_frames) {
        if (metrics.confirmed_presented_count != metrics.commits_sent_count) {
            metrics.add_fail(std::string("Frame presentation count mismatch: sent=") + 
                             std::to_string(metrics.commits_sent_count) + 
                             ", confirmed=" + std::to_string(metrics.confirmed_presented_count));
        }
    } else {
        if (metrics.confirmed_presented_count > metrics.commits_sent_count) {
            metrics.add_fail("Confirmed presentations exceed sent commits");
        }
        if (metrics.confirmed_presented_count == 0 && metrics.commits_sent_count > 0) {
            metrics.add_fail("No frames were presented despite commits sent");
        }
        // Строгое доказательство возобновления презентаций:
        // Хвостовой commit_seq обязан быть показан после восстановления WM
        if (metrics.commits_sent_count >= 30) {
            uint64_t min_tail_seq = metrics.current_seq - 15;
            if (metrics.last_presented_seq < min_tail_seq) {
                metrics.add_fail(std::string("Presentation did not resume after WM recovery: last_presented_seq=") +
                                 std::to_string(metrics.last_presented_seq) +
                                 ", expected >= " + std::to_string(min_tail_seq));
            }
            if (metrics.confirmed_presented_count < (metrics.commits_sent_count / 2)) {
                metrics.add_fail(std::string("Too many dropped frames during WM recovery: confirmed=") +
                                 std::to_string(metrics.confirmed_presented_count) +
                                 ", sent=" + std::to_string(metrics.commits_sent_count));
            }
        }
    }

    if (config.expect_motion) {
        if (metrics.inside_bounds_motion_count == 0) {
            metrics.add_fail("Expected inside-bounds motion, got 0");
        }
        if (!config.expect_out_of_bounds && metrics.outside_bounds_motion_count > 0) {
            metrics.add_fail("Got outside-bounds motion when strictly not expected");
        }
    }

    if (config.expect_out_of_bounds) {
        if (!metrics.inside_motion_received_before_outside) {
            metrics.add_fail("Expected out-of-bounds drag: inside motion before drag not received");
        }
        if (metrics.outside_bounds_motion_count == 0) {
            metrics.add_fail("Expected out-of-bounds drag: no outside-bounds motion");
        }
        if (metrics.drag_state != TestMetrics::DragState::RELEASED) {
            metrics.add_fail("Drag sequence incomplete: press -> inside motion -> outside motion while pressed -> release not confirmed");
        }
    }
    
    if (config.expect_no_motion && metrics.motion_events_count > 0) {
        metrics.add_fail(std::string("Expected NO motion events, but received ") + 
                         std::to_string(metrics.motion_events_count));
    }

    bool success = metrics.fail_reasons.empty() && 
                   (metrics.protocol_errors_count == 0) &&
                   (metrics.duplicate_presented_count == 0) &&
                   (metrics.out_of_order_presented_count == 0);

    std::println("\n=================================================");
    std::println("  {} Verification Summary:                      ", config.client_name);
    if (!metrics.fail_reasons.empty()) {
        for (const auto& reason : metrics.fail_reasons) {
            std::println("    [ERROR]: {}", reason);
        }
    } else {
        std::println("    [OK]: No fatal transport or protocol errors.");
    }
    std::println("    Commits sent:       {}", metrics.commits_sent_count);
    std::println("    VBlank verified:    {}", metrics.confirmed_presented_count);
    std::println("    Motion total:       {}", metrics.motion_events_count);
    std::println("    Motion inside win:  {}", metrics.inside_bounds_motion_count);
    std::println("    Motion out of win:  {}", metrics.outside_bounds_motion_count);
    std::println("    Button events:      {}", metrics.button_events_count);
    std::println("    Key events:         {}", metrics.key_events_count);
    std::println("    Protocol errors:    {}", metrics.protocol_errors_count);
    std::println("    Duplicate presented:{}", metrics.duplicate_presented_count);
    std::println("    Out of order:       {}", metrics.out_of_order_presented_count);
    std::println("    Test Verdict:       {}", success ? "[PASS]" : "[FAIL]");
    std::println("=================================================");

    return success ? 0 : 1;
}