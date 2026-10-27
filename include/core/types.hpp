#pragma once
#include <unistd.h>
#include <cstddef>
#include <type_traits>
#include <xf86drm.h>
#include <xf86drmMode.h>
extern "C" {
#include <libseat.h>
}
#include <utility>
#include <memory>
#include <expected>
#include <system_error>

namespace yarilo::core {

template <typename T>
using Result = std::expected<T, std::error_code>;

enum class ServerError {
    DeviceNotFound = 1,
    PermissionDenied,
    SeatActivationTimeout,
    NoConnectedOutput,
    NoCompatibleCrtc,
    CrtcInspectionFailed,
    DumbBufferFailed,
    ModesetFailed,
    MmapFailed,
    SeatDisabled,
    NotImplemented,
    WouldBlock,
    AddressInUse,
    ProtocolViolation,
    InternalStateError,
    OutOfMemory,
};

const std::error_category& server_category() noexcept;
inline std::error_code make_error(ServerError e) noexcept {
    return {static_cast<int>(e), server_category()};
}

// RAII-обертка над файловым дескриптором
class UniqueFd {
public:
    constexpr UniqueFd() noexcept : fd_(-1) {}
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    ~UniqueFd() { reset(); }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool is_valid() const noexcept { return fd_ >= 0; }

    int release() noexcept { return std::exchange(fd_, -1); }
    void reset(int new_fd = -1) noexcept {
        if (fd_ >= 0) ::close(fd_);
        fd_ = new_fd;
    }

private:
    int fd_;
};

// RAII-делетеры для libdrm
struct DrmResDeleter { void operator()(drmModeRes* p) const noexcept { if (p) drmModeFreeResources(p); } };
struct DrmConnectorDeleter { void operator()(drmModeConnector* p) const noexcept { if (p) drmModeFreeConnector(p); } };
struct DrmEncoderDeleter { void operator()(drmModeEncoder* p) const noexcept { if (p) drmModeFreeEncoder(p); } };
struct DrmCrtcDeleter { void operator()(drmModeCrtc* p) const noexcept { if (p) drmModeFreeCrtc(p); } };

template <typename T, typename Deleter>
using DrmPtr = std::unique_ptr<T, Deleter>;

// RAII для устройства libseat
class SeatDevice {
public:
    SeatDevice() noexcept = default;
    SeatDevice(libseat* seat, int device_id, int fd) noexcept
        : seat_(seat), device_id_(device_id), fd_(fd) {}

    ~SeatDevice() { reset(); }

    SeatDevice(const SeatDevice&) = delete;
    SeatDevice& operator=(const SeatDevice&) = delete;

    SeatDevice(SeatDevice&& other) noexcept
        : seat_(std::exchange(other.seat_, nullptr)),
          device_id_(std::exchange(other.device_id_, -1)),
          fd_(std::exchange(other.fd_, -1)) {}

    SeatDevice& operator=(SeatDevice&& other) noexcept {
        if (this != &other) {
            reset();
            seat_ = std::exchange(other.seat_, nullptr);
            device_id_ = std::exchange(other.device_id_, -1);
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    [[nodiscard]] int fd() const noexcept { return fd_; }
    [[nodiscard]] int device_id() const noexcept { return device_id_; }
    [[nodiscard]] bool is_valid() const noexcept { return fd_ >= 0; }

    void reset() noexcept {
        if (seat_ && device_id_ >= 0) {
            libseat_close_device(seat_, device_id_);
        }
        if (fd_ >= 0) {
            ::close(fd_);
        }
        seat_ = nullptr;
        device_id_ = -1;
        fd_ = -1;
    }

private:
    libseat* seat_{nullptr};
    int device_id_{-1};
    int fd_{-1};
};

} // namespace yarilo::core

// Алиас для обратной совместимости во всех файлах
namespace core = yarilo::core;

namespace std {
    template <> struct is_error_code_enum<yarilo::core::ServerError> : true_type {};
}