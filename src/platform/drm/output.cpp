#include "platform/drm/output.hpp"
#include "core/error.hpp"
#include <drm_fourcc.h>
#include <sys/mman.h>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <print>
#include <limits>
#include <optional>
#include <cstddef>
#include <utility>

namespace platform::drm {

DrmOutput::DrmOutput(int drm_fd, uint32_t connector_id, uint32_t crtc_id, 
                     drmModeModeInfo mode, core::DrmPtr<drmModeCrtc, core::DrmCrtcDeleter> original_crtc,
                     std::vector<uint32_t> original_connectors)
    : drm_fd_(drm_fd), connector_id_(connector_id), crtc_id_(crtc_id),
      mode_(mode), original_crtc_(std::move(original_crtc)),
      original_connectors_(std::move(original_connectors)) {}

DrmOutput::~DrmOutput() noexcept {
    try {
        if (page_flip_pending_) {
            std::println(stderr, "Yarilo Critical: DrmOutput destroyed while Page Flip was still in flight!");
        } else if (state_ == ModesetState::Applied) {
            std::println(stderr, "Yarilo Warning: DrmOutput destroyed before CRTC was restored.");
        }
    } catch (...) {}

    free_resources();
}

core::Result<void> DrmOutput::restore_crtc(bool can_access_kms) {
    if (state_ == ModesetState::NotApplied || state_ == ModesetState::Restored) {
        return {}; 
    }

    if (page_flip_pending_) {
        std::println(stderr, "Yarilo Error: Cannot restore CRTC: Page flip is currently in-flight.");
        return std::unexpected(core::make_error(core::ServerError::WouldBlock));
    }

    if (!can_access_kms) {
        state_ = ModesetState::RestoreFailed;
        std::println(stderr, "Yarilo Error: Cannot restore CRTC: VT inactive (DRM Master revoked).");
        return std::unexpected(core::make_error(core::ServerError::SeatDisabled));
    }

    if (!original_crtc_) {
        state_ = ModesetState::RestoreFailed;
        return std::unexpected(core::make_error(core::ServerError::CrtcInspectionFailed));
    }

    int ret = -1;
    if (original_crtc_->buffer_id == 0) {
        ret = drmModeSetCrtc(drm_fd_, original_crtc_->crtc_id, 0, 0, 0, nullptr, 0, nullptr);
    } else if (!original_connectors_.empty()) {
        ret = drmModeSetCrtc(drm_fd_, original_crtc_->crtc_id, original_crtc_->buffer_id,
                             original_crtc_->x, original_crtc_->y,
                             original_connectors_.data(), static_cast<int>(original_connectors_.size()),
                             &original_crtc_->mode);
    } else {
        state_ = ModesetState::RestoreFailed;
        std::println(stderr, "Yarilo Error: Active CRTC has no associated connectors to restore.");
        return std::unexpected(core::make_error(core::ServerError::CrtcInspectionFailed));
    }

    if (ret != 0) {
        int err = errno;
        state_ = ModesetState::RestoreFailed;
        std::println(stderr, "Yarilo Error: drmModeSetCrtc restoration failed: {}", std::strerror(err));
        return std::unexpected(core::make_error(core::ServerError::ModesetFailed));
    }

    state_ = ModesetState::Restored;
    return {};
}

bool DrmOutput::free_buffer(DumbBuffer& buf) noexcept {
    bool all_cleared = true;

    if (buf.mmap_addr) {
        if (munmap(buf.mmap_addr, buf.size) == 0) {
            buf.mmap_addr = nullptr;
        } else {
            int err = errno;
            try { std::println(stderr, "Yarilo Error: munmap failed at {}: {}", buf.mmap_addr, std::strerror(err)); } catch (...) {}
            return false; // Каскад прерывается: VMA утечка, нельзя удалять объекты ядра!
        }
    }

    if (buf.fb_id != 0) {
        if (drmModeRmFB(drm_fd_, buf.fb_id) == 0) {
            buf.fb_id = 0;
        } else {
            int err = errno;
            try { std::println(stderr, "Yarilo Error: drmModeRmFB failed for FB {}: {}", buf.fb_id, std::strerror(err)); } catch (...) {}
            return false; // Каскад прерывается: FB жив, нельзя удалять dumb buffer!
        }
    }

    if (buf.fb_id == 0 && buf.handle != 0) {
        drm_mode_destroy_dumb destroy_req{.handle = buf.handle};
        if (drmIoctl(drm_fd_, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req) == 0) {
            buf.handle = 0;
        } else {
            int err = errno;
            try { std::println(stderr, "Yarilo Error: DESTROY_DUMB failed for handle {}: {}", buf.handle, std::strerror(err)); } catch (...) {}
            all_cleared = false;
        }
    }

    return all_cleared;
}

void DrmOutput::free_resources() noexcept {
    if (resources_freed_) return;

    // ИНВАРИАНТ БЕЗОПАСНОСТИ: Учитываем и Applied (не было попытки), и RestoreFailed (попытка провалилась)
    if (page_flip_pending_ || state_ == ModesetState::Applied || state_ == ModesetState::RestoreFailed) {
        try {
            std::println(stderr, "Yarilo Emergency Guard: Scanout hardware unresolved. "
                                 "Preserving DRM framebuffer handles. Process termination will reclaim kernel GEM objects.");
        } catch (...) {}
        
        for (auto& b : buffers_) {
            if (b.mmap_addr) {
                if (munmap(b.mmap_addr, b.size) == 0) {
                    b.mmap_addr = nullptr;
                } else {
                    try { std::println(stderr, "Yarilo Error: Emergency munmap failed. Memory leaked in process VMA."); } catch (...) {}
                }
            }
        }
        return;
    }

    bool b0_cleared = free_buffer(buffers_[0]);
    bool b1_cleared = free_buffer(buffers_[1]);

    if (b0_cleared && b1_cleared) {
        resources_freed_ = true;
    }
}

core::Result<std::unique_ptr<DrmOutput>> DrmOutput::create_and_bind(int drm_fd) {
    core::DrmPtr<drmModeRes, core::DrmResDeleter> res(drmModeGetResources(drm_fd));
    if (!res) return std::unexpected(core::make_error(core::ServerError::DeviceNotFound));

    core::DrmPtr<drmModeConnector, core::DrmConnectorDeleter> target_conn;
    for (int i = 0; i < res->count_connectors; ++i) {
        core::DrmPtr<drmModeConnector, core::DrmConnectorDeleter> conn(
            drmModeGetConnector(drm_fd, res->connectors[i]));
        if (conn && conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
            target_conn = std::move(conn);
            break;
        }
    }

    if (!target_conn) return std::unexpected(core::make_error(core::ServerError::NoConnectedOutput));

    drmModeModeInfo selected_mode = target_conn->modes[0];
    for (int i = 0; i < target_conn->count_modes; ++i) {
        if (target_conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
            selected_mode = target_conn->modes[i];
            break;
        }
    }

    auto count_attached_connectors = [&](uint32_t crtc_id) -> std::pair<size_t, bool> {
        size_t count = 0;
        bool has_target = false;
        for (int i = 0; i < res->count_connectors; ++i) {
            core::DrmPtr<drmModeConnector, core::DrmConnectorDeleter> c(
                drmModeGetConnector(drm_fd, res->connectors[i]));
            if (c && c->encoder_id != 0) {
                core::DrmPtr<drmModeEncoder, core::DrmEncoderDeleter> e(
                    drmModeGetEncoder(drm_fd, c->encoder_id));
                if (e && e->crtc_id == crtc_id) {
                    count++;
                    if (c->connector_id == target_conn->connector_id) has_target = true;
                }
            }
        }
        return {count, has_target};
    };

    uint32_t chosen_crtc_id = 0;

    if (target_conn->encoder_id != 0) {
        core::DrmPtr<drmModeEncoder, core::DrmEncoderDeleter> current_enc(
            drmModeGetEncoder(drm_fd, target_conn->encoder_id));
        if (current_enc && current_enc->crtc_id != 0) {
            auto [attached_count, has_target] = count_attached_connectors(current_enc->crtc_id);
            if (attached_count == 1 && has_target) {
                chosen_crtc_id = current_enc->crtc_id;
            }
        }
    }

    if (chosen_crtc_id == 0) {
        for (int e = 0; e < target_conn->count_encoders; ++e) {
            core::DrmPtr<drmModeEncoder, core::DrmEncoderDeleter> enc(
                drmModeGetEncoder(drm_fd, target_conn->encoders[e]));
            if (!enc) continue;

            for (int c = 0; c < res->count_crtcs; ++c) {
                // Защита от UB при сдвиге и использование беззнакового литерала 1U
                if (c < 32 && (enc->possible_crtcs & (1U << c))) {
                    uint32_t crtc_candidate = res->crtcs[c];
                    core::DrmPtr<drmModeCrtc, core::DrmCrtcDeleter> crtc_info(
                        drmModeGetCrtc(drm_fd, crtc_candidate));
                    if (crtc_info && crtc_info->buffer_id == 0) {
                        auto [attached_count, _] = count_attached_connectors(crtc_candidate);
                        if (attached_count == 0) {
                            chosen_crtc_id = crtc_candidate;
                            break;
                        }
                    }
                }
            }
            if (chosen_crtc_id != 0) break;
        }
    }

    if (chosen_crtc_id == 0) return std::unexpected(core::make_error(core::ServerError::NoCompatibleCrtc));

    core::DrmPtr<drmModeCrtc, core::DrmCrtcDeleter> original_crtc(drmModeGetCrtc(drm_fd, chosen_crtc_id));
    if (!original_crtc) return std::unexpected(core::make_error(core::ServerError::CrtcInspectionFailed));

    std::vector<uint32_t> original_connectors;
    for (int i = 0; i < res->count_connectors; ++i) {
        core::DrmPtr<drmModeConnector, core::DrmConnectorDeleter> conn(
            drmModeGetConnector(drm_fd, res->connectors[i]));
        if (conn && conn->encoder_id != 0) {
            core::DrmPtr<drmModeEncoder, core::DrmEncoderDeleter> enc(
                drmModeGetEncoder(drm_fd, conn->encoder_id));
            if (enc && enc->crtc_id == chosen_crtc_id) {
                original_connectors.push_back(conn->connector_id);
            }
        }
    }

    if (original_crtc->buffer_id != 0 && original_connectors.empty()) {
        return std::unexpected(core::make_error(core::ServerError::CrtcInspectionFailed));
    }

    auto output = std::unique_ptr<DrmOutput>(new DrmOutput(
        drm_fd, target_conn->connector_id, chosen_crtc_id, selected_mode, 
        std::move(original_crtc), std::move(original_connectors)
    ));

    auto alloc_res = output->allocate_double_buffers();
    if (!alloc_res) return std::unexpected(alloc_res.error());

    return output;
}

core::Result<void> DrmOutput::allocate_double_buffers() {
    for (size_t i = 0; i < 2; ++i) {
        drm_mode_create_dumb create_req{};
        create_req.height = mode_.vdisplay;
        create_req.width  = mode_.hdisplay;
        create_req.bpp    = 32;

        if (drmIoctl(drm_fd_, DRM_IOCTL_MODE_CREATE_DUMB, &create_req) < 0) {
            return std::unexpected(core::make_error(core::ServerError::DumbBufferFailed));
        }

        buffers_[i].handle = create_req.handle;
        buffers_[i].pitch  = create_req.pitch;
        buffers_[i].size   = create_req.size;

        uint32_t handles[4] = {buffers_[i].handle, 0, 0, 0};
        uint32_t pitches[4] = {buffers_[i].pitch, 0, 0, 0};
        uint32_t offsets[4] = {0, 0, 0, 0};

        if (drmModeAddFB2(drm_fd_, mode_.hdisplay, mode_.vdisplay, DRM_FORMAT_XRGB8888,
                          handles, pitches, offsets, &buffers_[i].fb_id, 0) != 0) {
            return std::unexpected(core::make_error(core::ServerError::DumbBufferFailed));
        }

        drm_mode_map_dumb map_req{};
        map_req.handle = buffers_[i].handle;
        if (drmIoctl(drm_fd_, DRM_IOCTL_MODE_MAP_DUMB, &map_req) < 0) {
            return std::unexpected(core::make_error(core::ServerError::MmapFailed));
        }

        buffers_[i].mmap_addr = mmap(nullptr, buffers_[i].size, PROT_READ | PROT_WRITE,
                                     MAP_SHARED, drm_fd_, map_req.offset);
        if (buffers_[i].mmap_addr == MAP_FAILED) {
            buffers_[i].mmap_addr = nullptr;
            return std::unexpected(core::make_error(core::ServerError::MmapFailed));
        }
    }
    return {};
}

core::Result<void> DrmOutput::fill_color(uint32_t xrgb_color) {
    for (size_t i = 0; i < 2; ++i) {
        if (!buffers_[i].mmap_addr) return std::unexpected(core::make_error(core::ServerError::MmapFailed));
        auto* base_bytes = static_cast<std::byte*>(buffers_[i].mmap_addr);
        for (uint32_t y = 0; y < mode_.vdisplay; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(base_bytes + (static_cast<uint64_t>(y) * buffers_[i].pitch));
            for (uint32_t x = 0; x < mode_.hdisplay; ++x) {
                row[x] = xrgb_color;
            }
        }
    }

    if (drmModeSetCrtc(drm_fd_, crtc_id_, buffers_[front_idx_].fb_id, 0, 0, &connector_id_, 1, &mode_) != 0) {
        return std::unexpected(core::make_error(core::ServerError::ModesetFailed));
    }

    state_ = ModesetState::Applied;
    return {};
}

void DrmOutput::clear_back_buffer(uint32_t xrgb_color) noexcept {
    auto& back = buffers_[back_idx_];
    if (!back.mmap_addr) return;
    auto* base_bytes = static_cast<std::byte*>(back.mmap_addr);
    for (uint32_t y = 0; y < mode_.vdisplay; ++y) {
        auto* row = reinterpret_cast<uint32_t*>(base_bytes + (static_cast<uint64_t>(y) * back.pitch));
        std::fill_n(row, mode_.hdisplay, xrgb_color);
    }
}

static std::optional<uint64_t> checked_pixel_offset(uint32_t y, uint32_t stride, uint32_t x) noexcept {
    if (y > 0 && static_cast<uint64_t>(stride) > std::numeric_limits<uint64_t>::max() / y) return std::nullopt;
    uint64_t row_offset = static_cast<uint64_t>(y) * stride;
    uint64_t col_offset = static_cast<uint64_t>(x) * sizeof(uint32_t);
    if (col_offset > std::numeric_limits<uint64_t>::max() - row_offset) return std::nullopt;
    return row_offset + col_offset;
}

bool DrmOutput::blit_subsurface(const uint32_t* src_pixels, size_t src_size,
                               uint32_t src_width, uint32_t src_height, uint32_t src_stride,
                               uint32_t src_x, uint32_t src_y,
                               uint32_t dst_x, uint32_t dst_y,
                               uint32_t width, uint32_t height) noexcept {
    auto& back = buffers_[back_idx_];
    if (!back.mmap_addr || !src_pixels || width == 0 || height == 0) return false;
    if (src_width == 0 || src_height == 0) return false;

    if (src_x > src_width || width > src_width - src_x) return false;
    if (src_y > src_height || height > src_height - src_y) return false;
    if (dst_x > mode_.hdisplay || width > mode_.hdisplay - dst_x) return false;
    if (dst_y > mode_.vdisplay || height > mode_.vdisplay - dst_y) return false;

    uint64_t min_src_stride = static_cast<uint64_t>(src_width) * sizeof(uint32_t);
    if (static_cast<uint64_t>(src_stride) < min_src_stride) return false;
    if (static_cast<uint64_t>(back.pitch) < static_cast<uint64_t>(mode_.hdisplay) * sizeof(uint32_t)) return false;

    auto src_start = checked_pixel_offset(src_y, src_stride, src_x);
    if (!src_start || *src_start > src_size) return false;

    auto dst_start = checked_pixel_offset(dst_y, back.pitch, dst_x);
    if (!dst_start || *dst_start > back.size) return false;

    if (height > 1) {
        if (static_cast<uint64_t>(src_stride) > (std::numeric_limits<uint64_t>::max() / (height - 1))) return false;
        if (static_cast<uint64_t>(back.pitch) > (std::numeric_limits<uint64_t>::max() / (height - 1))) return false;
    }

    uint64_t src_rows_span = static_cast<uint64_t>(height - 1) * src_stride;
    uint64_t src_last_row_bytes = static_cast<uint64_t>(width) * sizeof(uint32_t);
    if (std::numeric_limits<uint64_t>::max() - src_rows_span < src_last_row_bytes) return false;
    uint64_t src_span = src_rows_span + src_last_row_bytes;
    if (src_span > src_size - *src_start) return false;

    uint64_t dst_rows_span = static_cast<uint64_t>(height - 1) * back.pitch;
    uint64_t dst_last_row_bytes = static_cast<uint64_t>(width) * sizeof(uint32_t);
    if (std::numeric_limits<uint64_t>::max() - dst_rows_span < dst_last_row_bytes) return false;
    uint64_t dst_span = dst_rows_span + dst_last_row_bytes;
    if (dst_span > back.size - *dst_start) return false;

    const auto* src_row_bytes = static_cast<const std::byte*>(reinterpret_cast<const void*>(src_pixels)) + *src_start;
    auto* dst_row_bytes = static_cast<std::byte*>(back.mmap_addr) + *dst_start;
    const size_t row_bytes_to_copy = static_cast<size_t>(width) * sizeof(uint32_t);

    for (uint32_t row = 0; row < height; ++row) {
        std::memcpy(dst_row_bytes, src_row_bytes, row_bytes_to_copy);
        if (row + 1 < height) {
            src_row_bytes += src_stride;
            dst_row_bytes += back.pitch;
        }
    }
    return true;
}

core::Result<void> DrmOutput::schedule_page_flip(uint64_t session_id, uint32_t surface_id, uint64_t commit_seq) {
    if (page_flip_pending_) return std::unexpected(core::make_error(core::ServerError::WouldBlock));

    pending_session_id_ = session_id;
    pending_surface_id_ = surface_id;
    pending_commit_seq_ = commit_seq;

    int ret = drmModePageFlip(drm_fd_, crtc_id_, buffers_[back_idx_].fb_id, 
                              DRM_MODE_PAGE_FLIP_EVENT, this);
    if (ret != 0) {
        int err = errno;
        if (err == EBUSY) return std::unexpected(core::make_error(core::ServerError::WouldBlock));
        std::println(stderr, "Yarilo Error: drmModePageFlip failed: {}", std::strerror(err));
        return std::unexpected(core::make_error(core::ServerError::ModesetFailed));
    }

    page_flip_pending_ = true;
    return {};
}

void DrmOutput::handle_page_flip_event(unsigned int /*sequence*/, unsigned int tv_sec, unsigned int tv_usec) noexcept {
    if (!page_flip_pending_) {
        try { std::println(stderr, "Yarilo Warning: Spurious page flip event ignored."); } catch (...) {}
        return;
    }
    std::swap(front_idx_, back_idx_);
    page_flip_pending_ = false;

    uint64_t vblank_ns = (static_cast<uint64_t>(tv_sec) * 1'000'000'000ULL) + (static_cast<uint64_t>(tv_usec) * 1'000ULL);
    if (flip_cb_) {
        try { flip_cb_(pending_session_id_, pending_surface_id_, pending_commit_seq_, vblank_ns); } catch (...) {}
    }
}

} // namespace platform::drm