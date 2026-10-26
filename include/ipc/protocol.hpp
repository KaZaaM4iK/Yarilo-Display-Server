#pragma once
#include <cstdint>
#include <cstddef>
#include <optional>
#include <span>

namespace yarilo::ipc {

constexpr uint32_t PROTOCOL_MAGIC   = 0x49524159; // ASCII "YARI"
constexpr uint32_t PROTOCOL_VERSION = 1;
constexpr size_t   MAX_MESSAGE_SIZE = 4096;
constexpr size_t   HEADER_SIZE      = 24;
constexpr size_t   MAX_FDS_PER_MSG  = 1;

enum class MessageType : uint32_t {
    // Client -> Server
    CreateSurface   = 0x01,
    AttachBuffer    = 0x02,
    CommitSurface   = 0x03,
    DestroySurface  = 0x04,

    // WM -> Server
    WmHandshake        = 0x10,
    WmConfigureSurface = 0x11,
    WmSetKeyboardFocus = 0x12,

    // Server -> Client
    SurfaceCreated  = 0x81,
    BufferReleased  = 0x82,
    FramePresented  = 0x83,
    PointerMotion   = 0x84,
    PointerButton   = 0x85,
    KeyboardKey     = 0x86,

    // Server -> WM
    WmMapRequest       = 0x90,
    WmSurfaceDestroyed = 0x91,
    WmHandshakeAck     = 0x92,
    
    ProtocolError   = 0xFF,
};

inline uint32_t read_le32(const std::byte* ptr) noexcept {
    return static_cast<uint32_t>(ptr[0]) |
          (static_cast<uint32_t>(ptr[1]) << 8) |
          (static_cast<uint32_t>(ptr[2]) << 16) |
          (static_cast<uint32_t>(ptr[3]) << 24);
}

inline uint64_t read_le64(const std::byte* ptr) noexcept {
    return static_cast<uint64_t>(read_le32(ptr)) |
          (static_cast<uint64_t>(read_le32(ptr + 4)) << 32);
}

inline void write_le32(std::byte* ptr, uint32_t val) noexcept {
    ptr[0] = static_cast<std::byte>(val & 0xFF);
    ptr[1] = static_cast<std::byte>((val >> 8) & 0xFF);
    ptr[2] = static_cast<std::byte>((val >> 16) & 0xFF);
    ptr[3] = static_cast<std::byte>((val >> 24) & 0xFF);
}

inline void write_le64(std::byte* ptr, uint64_t val) noexcept {
    write_le32(ptr, static_cast<uint32_t>(val & 0xFFFFFFFF));
    write_le32(ptr + 4, static_cast<uint32_t>(val >> 32));
}

struct MessageHeader {
    uint32_t magic;
    uint32_t version;
    MessageType type;
    uint32_t length;
    uint64_t sequence;

    static std::optional<MessageHeader> decode(std::span<const std::byte> buf) noexcept {
        if (buf.size() < HEADER_SIZE) return std::nullopt;
        MessageHeader h;
        h.magic    = read_le32(buf.data() + 0);
        h.version  = read_le32(buf.data() + 4);
        h.type     = static_cast<MessageType>(read_le32(buf.data() + 8));
        h.length   = read_le32(buf.data() + 12);
        h.sequence = read_le64(buf.data() + 16);
        return h;
    }

    [[nodiscard]] bool encode(std::span<std::byte> buf) const noexcept {
        if (buf.size() < HEADER_SIZE) return false;
        write_le32(buf.data() + 0, magic);
        write_le32(buf.data() + 4, version);
        write_le32(buf.data() + 8, static_cast<uint32_t>(type));
        write_le32(buf.data() + 12, length);
        write_le64(buf.data() + 16, sequence);
        return true;
    }
};

struct CreateSurfacePayload {
    uint32_t surface_id;
    static constexpr size_t ENCODED_SIZE = 4;
    
    static std::optional<CreateSurfacePayload> decode(std::span<const std::byte> buf) noexcept {
        if (buf.size() != ENCODED_SIZE) return std::nullopt;
        return CreateSurfacePayload{ .surface_id = read_le32(buf.data()) };
    }
};

struct DestroySurfacePayload {
    uint32_t surface_id;
    static constexpr size_t ENCODED_SIZE = 4;
    
    static std::optional<DestroySurfacePayload> decode(std::span<const std::byte> buf) noexcept {
        if (buf.size() != ENCODED_SIZE) return std::nullopt;
        return DestroySurfacePayload{ .surface_id = read_le32(buf.data()) };
    }
};

struct AttachBufferPayload {
    uint32_t surface_id;
    uint32_t buffer_id;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint64_t offset;
    static constexpr size_t ENCODED_SIZE = 32;

    static std::optional<AttachBufferPayload> decode(std::span<const std::byte> buf) noexcept {
        if (buf.size() != ENCODED_SIZE) return std::nullopt;
        AttachBufferPayload p;
        p.surface_id = read_le32(buf.data() + 0);
        p.buffer_id  = read_le32(buf.data() + 4);
        p.width      = read_le32(buf.data() + 8);
        p.height     = read_le32(buf.data() + 12);
        p.stride     = read_le32(buf.data() + 16);
        p.format     = read_le32(buf.data() + 20);
        p.offset     = read_le64(buf.data() + 24);
        return p;
    }
};

struct CommitSurfacePayload {
    uint32_t surface_id;
    uint32_t buffer_id;
    uint64_t commit_seq;
    static constexpr size_t ENCODED_SIZE = 16;

    static std::optional<CommitSurfacePayload> decode(std::span<const std::byte> buf) noexcept {
        if (buf.size() != ENCODED_SIZE) return std::nullopt;
        CommitSurfacePayload p;
        p.surface_id  = read_le32(buf.data() + 0);
        p.buffer_id   = read_le32(buf.data() + 4);
        p.commit_seq  = read_le64(buf.data() + 8);
        return p;
    }
};

// --- WM -> Server Payloads ---

struct WmConfigureSurfacePayload {
    uint64_t global_surface_id;
    uint64_t server_uid;
    int32_t  x;
    int32_t  y;
    uint32_t z_index;
    uint32_t mapped; // 0 or 1
    static constexpr size_t ENCODED_SIZE = 32;

    [[nodiscard]] bool encode(std::span<std::byte> buf) const noexcept {
        if (buf.size() < ENCODED_SIZE || (mapped != 0 && mapped != 1)) {
            return false;
        }
        write_le64(buf.data() + 0, global_surface_id);
        write_le64(buf.data() + 8, server_uid);
        write_le32(buf.data() + 16, static_cast<uint32_t>(x));
        write_le32(buf.data() + 20, static_cast<uint32_t>(y));
        write_le32(buf.data() + 24, z_index);
        write_le32(buf.data() + 28, mapped);
        return true;
    }

    static std::optional<WmConfigureSurfacePayload> decode(std::span<const std::byte> buf) noexcept {
        if (buf.size() != ENCODED_SIZE) return std::nullopt;
        WmConfigureSurfacePayload p;
        p.global_surface_id = read_le64(buf.data() + 0);
        p.server_uid        = read_le64(buf.data() + 8);
        p.x                 = static_cast<int32_t>(read_le32(buf.data() + 16));
        p.y                 = static_cast<int32_t>(read_le32(buf.data() + 20));
        p.z_index           = read_le32(buf.data() + 24);
        p.mapped            = read_le32(buf.data() + 28);
        
        if (p.mapped != 0 && p.mapped != 1) return std::nullopt;
        return p;
    }
};

struct WmSetKeyboardFocusPayload {
    uint64_t global_surface_id;
    static constexpr size_t ENCODED_SIZE = 8;
    
    static std::optional<WmSetKeyboardFocusPayload> decode(std::span<const std::byte> buf) noexcept {
        if (buf.size() != ENCODED_SIZE) return std::nullopt;
        return WmSetKeyboardFocusPayload{ .global_surface_id = read_le64(buf.data()) };
    }
};

// --- Server -> Client & WM Payloads ---

struct WmHandshakeAckPayload {
    uint32_t screen_width;
    uint32_t screen_height;
    uint32_t refresh_rate;
    static constexpr size_t ENCODED_SIZE = 12;

    [[nodiscard]] bool encode(std::span<std::byte> buf) const noexcept {
        if (buf.size() < ENCODED_SIZE) return false;
        write_le32(buf.data() + 0, screen_width);
        write_le32(buf.data() + 4, screen_height);
        write_le32(buf.data() + 8, refresh_rate);
        return true;
    }

    static std::optional<WmHandshakeAckPayload> decode(std::span<const std::byte> buf) noexcept {
        if (buf.size() != ENCODED_SIZE) return std::nullopt;
        WmHandshakeAckPayload p;
        p.screen_width  = read_le32(buf.data() + 0);
        p.screen_height = read_le32(buf.data() + 4);
        p.refresh_rate  = read_le32(buf.data() + 8);
        return p;
    }
};

struct WmMapRequestPayload {
    uint64_t global_surface_id;
    uint64_t server_uid;
    uint32_t width;
    uint32_t height;
    static constexpr size_t ENCODED_SIZE = 24;

    [[nodiscard]] bool encode(std::span<std::byte> buf) const noexcept {
        if (buf.size() < ENCODED_SIZE) return false;
        write_le64(buf.data() + 0, global_surface_id);
        write_le64(buf.data() + 8, server_uid);
        write_le32(buf.data() + 16, width);
        write_le32(buf.data() + 20, height);
        return true;
    }

    static std::optional<WmMapRequestPayload> decode(std::span<const std::byte> buf) noexcept {
        if (buf.size() != ENCODED_SIZE) return std::nullopt;
        WmMapRequestPayload p;
        p.global_surface_id = read_le64(buf.data() + 0);
        p.server_uid        = read_le64(buf.data() + 8);
        p.width             = read_le32(buf.data() + 16);
        p.height            = read_le32(buf.data() + 20);
        return p;
    }
};

struct WmSurfaceDestroyedPayload {
    uint64_t global_surface_id;
    static constexpr size_t ENCODED_SIZE = 8;

    [[nodiscard]] bool encode(std::span<std::byte> buf) const noexcept {
        if (buf.size() < ENCODED_SIZE) return false;
        write_le64(buf.data() + 0, global_surface_id);
        return true;
    }

    static std::optional<WmSurfaceDestroyedPayload> decode(std::span<const std::byte> buf) noexcept {
        if (buf.size() != ENCODED_SIZE) return std::nullopt;
        return WmSurfaceDestroyedPayload{ .global_surface_id = read_le64(buf.data()) };
    }
};

struct SurfaceCreatedPayload {
    uint32_t surface_id;
    static constexpr size_t ENCODED_SIZE = 4;
    
    [[nodiscard]] bool encode(std::span<std::byte> buf) const noexcept {
        if (buf.size() < ENCODED_SIZE) return false;
        write_le32(buf.data() + 0, surface_id);
        return true;
    }
};

struct BufferReleasedPayload {
    uint32_t surface_id;
    uint32_t buffer_id;
    uint64_t commit_seq;
    static constexpr size_t ENCODED_SIZE = 16;
    
    [[nodiscard]] bool encode(std::span<std::byte> buf) const noexcept {
        if (buf.size() < ENCODED_SIZE) return false;
        write_le32(buf.data() + 0, surface_id);
        write_le32(buf.data() + 4, buffer_id);
        write_le64(buf.data() + 8, commit_seq);
        return true;
    }
};

struct FramePresentedPayload {
    uint32_t surface_id;
    uint64_t commit_seq;
    uint64_t presentation_time_ns;
    static constexpr size_t ENCODED_SIZE = 20;
    
    [[nodiscard]] bool encode(std::span<std::byte> buf) const noexcept {
        if (buf.size() < ENCODED_SIZE) return false;
        write_le32(buf.data() + 0, surface_id);
        write_le64(buf.data() + 4, commit_seq);
        write_le64(buf.data() + 12, presentation_time_ns);
        return true;
    }
};

struct ProtocolErrorPayload {
    uint32_t error_code;
    static constexpr size_t ENCODED_SIZE = 4;
    
    [[nodiscard]] bool encode(std::span<std::byte> buf) const noexcept {
        if (buf.size() < ENCODED_SIZE) return false;
        write_le32(buf.data() + 0, error_code);
        return true;
    }
};

} // namespace yarilo::ipc