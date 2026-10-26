#pragma once
#include "core/types.hpp"
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdint>
#include <cstddef>
#include <utility>
#include <expected>

#ifdef __linux__
#include <fcntl.h>
#include <drm_fourcc.h>
#endif

namespace yarilo::shm {

constexpr uint64_t MAX_SHM_SIZE = 256 * 1024 * 1024; // 256 MiB

class MappedBuffer {
public:
    MappedBuffer() noexcept = default;
    
    MappedBuffer(void* base, size_t size, const uint32_t* pixels, uint32_t w, uint32_t h, uint32_t s) noexcept
        : map_base_(base), map_size_(size), pixel_data_(pixels), width_(w), height_(h), stride_(s) {}

    ~MappedBuffer() noexcept {
        reset();
    }

    MappedBuffer(const MappedBuffer&) = delete;
    MappedBuffer& operator=(const MappedBuffer&) = delete;

    MappedBuffer(MappedBuffer&& other) noexcept
        : map_base_(std::exchange(other.map_base_, nullptr)),
          map_size_(std::exchange(other.map_size_, 0)),
          pixel_data_(std::exchange(other.pixel_data_, nullptr)),
          width_(std::exchange(other.width_, 0)),
          height_(std::exchange(other.height_, 0)),
          stride_(std::exchange(other.stride_, 0)) {}

    MappedBuffer& operator=(MappedBuffer&& other) noexcept {
        if (this != &other) {
            reset();
            map_base_   = std::exchange(other.map_base_, nullptr);
            map_size_   = std::exchange(other.map_size_, 0);
            pixel_data_ = std::exchange(other.pixel_data_, nullptr);
            width_      = std::exchange(other.width_, 0);
            height_     = std::exchange(other.height_, 0);
            stride_     = std::exchange(other.stride_, 0);
        }
        return *this;
    }

    void reset() noexcept {
        if (map_base_ && map_base_ != MAP_FAILED && map_size_ > 0) {
            ::munmap(map_base_, map_size_);
        }
        map_base_   = nullptr;
        map_size_   = 0;
        pixel_data_ = nullptr;
        width_      = 0;
        height_     = 0;
        stride_     = 0;
    }

    [[nodiscard]] const uint32_t* pixels() const noexcept { return pixel_data_; }
    [[nodiscard]] uint32_t width() const noexcept { return width_; }
    [[nodiscard]] uint32_t height() const noexcept { return height_; }
    [[nodiscard]] uint32_t stride() const noexcept { return stride_; }
    [[nodiscard]] size_t map_size() const noexcept { return map_size_; }
    [[nodiscard]] bool is_valid() const noexcept { return pixel_data_ != nullptr; }

private:
    void* map_base_{nullptr};
    size_t map_size_{0};
    const uint32_t* pixel_data_{nullptr};
    uint32_t width_{0};
    uint32_t height_{0};
    uint32_t stride_{0};
};

class ShmValidator {
public:
    static core::Result<MappedBuffer> validate_and_map(
        int fd, uint32_t width, uint32_t height, uint32_t stride, 
        uint32_t format, uint64_t offset) 
    {
#ifndef __linux__
        return std::unexpected(core::make_error(core::ServerError::NotImplemented));
#else
        if (format != DRM_FORMAT_XRGB8888) {
            return std::unexpected(core::make_error(core::ServerError::DumbBufferFailed));
        }

        if (width == 0 || width > 16384 || height == 0 || height > 16384) {
            return std::unexpected(core::make_error(core::ServerError::DumbBufferFailed));
        }

        if (offset % 4 != 0) {
            return std::unexpected(core::make_error(core::ServerError::DumbBufferFailed));
        }

        uint64_t min_stride = static_cast<uint64_t>(width) * 4;
        if (stride < min_stride || (stride % 4 != 0)) {
            return std::unexpected(core::make_error(core::ServerError::DumbBufferFailed));
        }

        uint64_t total_span = static_cast<uint64_t>(height - 1) * stride + min_stride;
        if (total_span > MAX_SHM_SIZE || offset > (MAX_SHM_SIZE - total_span)) {
            return std::unexpected(core::make_error(core::ServerError::DumbBufferFailed));
        }
        uint64_t total_required = offset + total_span;

        struct stat st{};
        if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
            return std::unexpected(core::make_error(core::ServerError::MmapFailed));
        }

        if (static_cast<uint64_t>(st.st_size) < total_required) {
            return std::unexpected(core::make_error(core::ServerError::MmapFailed));
        }

        int seals = ::fcntl(fd, F_GET_SEALS);
        if (seals < 0) {
            return std::unexpected(core::make_error(core::ServerError::PermissionDenied));
        }
        constexpr int REQUIRED_SEALS = F_SEAL_SHRINK | F_SEAL_GROW;
        if ((seals & REQUIRED_SEALS) != REQUIRED_SEALS) {
            return std::unexpected(core::make_error(core::ServerError::PermissionDenied));
        }

        long page_size = ::sysconf(_SC_PAGE_SIZE);
        if (page_size <= 0) page_size = 4096;

        uint64_t aligned_offset = (offset / page_size) * page_size;
        uint64_t delta = offset - aligned_offset;
        size_t map_length = static_cast<size_t>(total_span + delta);

        void* addr = ::mmap(nullptr, map_length, PROT_READ, MAP_SHARED, fd, static_cast<off_t>(aligned_offset));
        if (addr == MAP_FAILED) {
            return std::unexpected(core::make_error(core::ServerError::MmapFailed));
        }

        const auto* pixel_ptr = reinterpret_cast<const uint32_t*>(static_cast<const std::byte*>(addr) + delta);
        return MappedBuffer(addr, map_length, pixel_ptr, width, height, stride);
#endif
    }
};

} // namespace yarilo::shm