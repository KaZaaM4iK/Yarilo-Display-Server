// include/ipc/server_socket.hpp
#pragma once
#include "core/types.hpp"
#include "ipc/protocol.hpp"
#include <string>
#include <memory>
#include <array>

namespace yarilo::ipc {

struct ReceivedPacket {
    MessageHeader header{};
    std::array<std::byte, MAX_MESSAGE_SIZE - HEADER_SIZE> payload{};
    size_t payload_len{0};
    core::UniqueFd passed_fd;
};

class ServerSocket {
public:
    static core::Result<std::unique_ptr<ServerSocket>> create();
    ~ServerSocket();

    [[nodiscard]] int fd() const noexcept { return server_fd_.get(); }
    
    core::Result<core::UniqueFd> accept_client();
    static core::Result<ReceivedPacket> receive_packet(int client_fd);
    static core::Result<void> send_packet(int client_fd, MessageType type, uint64_t seq, const void* data, size_t size);

private:
    explicit ServerSocket(core::UniqueFd fd, std::string path)
        : server_fd_(std::move(fd)), socket_path_(std::move(path)) {}

    static void cleanup_socket_file(const std::string& path, dev_t dev, ino_t ino) noexcept;

    core::UniqueFd server_fd_;
    std::string socket_path_;
    dev_t bound_dev_{0};
    ino_t bound_ino_{0};
};

} // namespace yarilo::ipc