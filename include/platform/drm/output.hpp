#pragma once
#include "core/types.hpp"
#include <xf86drmMode.h>
#include <memory>
#include <vector>
#include <array>
#include <cstdint>
#include <cstddef>
#include <utility>
#include <functional>

namespace platform::drm {

enum class ModesetState {
    NotApplied,
    Applied,
    Restored,
    RestoreFailed
};

struct DumbBuffer {
    uint32_t handle{0};
    uint32_t pitch{0};
    uint64_t size{0};
    uint32_t fb_id{0};
    void* mmap_addr{nullptr};
};

class DrmOutput {
public:
    using FlipCompleteCallback = std::function<void(uint64_t session_id, uint32_t surface_id, uint64_t commit_seq, uint64_t vblank_time_ns)>;

    static core::Result<std::unique_ptr<DrmOutput>> create_and_bind(int drm_fd);
    ~DrmOutput() noexcept;

    DrmOutput(const DrmOutput&) = delete;
    DrmOutput& operator=(const DrmOutput&) = delete;

    core::Result<void> fill_color(uint32_t xrgb_color);
    void clear_back_buffer(uint32_t xrgb_color) noexcept;

    [[nodiscard]] bool blit_subsurface(const uint32_t* src_pixels, size_t src_size,
                                       uint32_t src_width, uint32_t src_height, uint32_t src_stride,
                                       uint32_t src_x, uint32_t src_y,
                                       uint32_t dst_x, uint32_t dst_y,
                                       uint32_t width, uint32_t height) noexcept;

    core::Result<void> schedule_page_flip(uint64_t session_id, uint32_t surface_id, uint64_t commit_seq);

    void handle_page_flip_event(unsigned int sequence, unsigned int tv_sec, unsigned int tv_usec) noexcept;

    void set_flip_callback(FlipCompleteCallback cb) noexcept { flip_cb_ = std::move(cb); }

    core::Result<void> restore_crtc(bool can_access_kms);
    void free_resources() noexcept;

    [[nodiscard]] uint32_t width() const noexcept { return mode_.hdisplay; }
    [[nodiscard]] uint32_t height() const noexcept { return mode_.vdisplay; }
    [[nodiscard]] uint32_t refresh_rate() const noexcept { return mode_.vrefresh; }
    [[nodiscard]] bool is_page_flip_pending() const noexcept { return page_flip_pending_; }
    [[nodiscard]] ModesetState state() const noexcept { return state_; }
    [[nodiscard]] bool is_resources_freed() const noexcept { return resources_freed_; }

private:
    DrmOutput(int drm_fd, uint32_t connector_id, uint32_t crtc_id, 
              drmModeModeInfo mode, core::DrmPtr<drmModeCrtc, core::DrmCrtcDeleter> original_crtc,
              std::vector<uint32_t> original_connectors);

    core::Result<void> allocate_double_buffers();
    bool free_buffer(DumbBuffer& buf) noexcept;

    int drm_fd_{-1};
    uint32_t connector_id_{0};
    uint32_t crtc_id_{0};
    drmModeModeInfo mode_{};
    core::DrmPtr<drmModeCrtc, core::DrmCrtcDeleter> original_crtc_;
    std::vector<uint32_t> original_connectors_;

    std::array<DumbBuffer, 2> buffers_{};
    size_t front_idx_{0};
    size_t back_idx_{1};

    ModesetState state_{ModesetState::NotApplied};
    bool resources_freed_{false};
    bool page_flip_pending_{false};

    uint64_t pending_session_id_{0};
    uint32_t pending_surface_id_{0};
    uint64_t pending_commit_seq_{0};
    FlipCompleteCallback flip_cb_;
};

} // namespace platform::drm