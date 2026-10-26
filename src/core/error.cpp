#include "core/error.hpp"
#include <string>

namespace yarilo::core {

class ServerCategory : public std::error_category {
public:
    [[nodiscard]] const char* name() const noexcept override { return "Yarilo"; }
    [[nodiscard]] std::string message(int c) const override {
        switch (static_cast<ServerError>(c)) {
            case ServerError::DeviceNotFound: return "GPU device not found";
            case ServerError::PermissionDenied: return "Permission denied accessing seat/device";
            case ServerError::SeatActivationTimeout: return "Timed out waiting for seat activation";
            case ServerError::NoConnectedOutput: return "No physically connected displays found";
            case ServerError::NoCompatibleCrtc: return "No free compatible CRTC found for output";
            case ServerError::CrtcInspectionFailed: return "Could not safely inspect active CRTC state";
            case ServerError::DumbBufferFailed: return "Dumb buffer creation or FB allocation failed";
            case ServerError::ModesetFailed: return "drmModeSetCrtc execution failed";
            case ServerError::MmapFailed: return "mmap on dumb buffer failed";
            case ServerError::SeatDisabled: return "Session is currently disabled";
            case ServerError::NotImplemented: return "Feature not implemented for current platform";
            case ServerError::WouldBlock: return "Operation would block";
            case ServerError::AddressInUse: return "Address or endpoint already in use";
            case ServerError::ProtocolViolation: return "IPC protocol violation";
            case ServerError::InternalStateError: return "Internal server state consistency error";
            case ServerError::OutOfMemory: return "Out of memory or resource limit exceeded";
            default: return "Unknown Yarilo server error";
        }
    }
};

const std::error_category& server_category() noexcept {
    static ServerCategory instance;
    return instance;
}

} // namespace core