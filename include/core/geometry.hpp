#pragma once
#include <cstdint>
#include <algorithm>
#include <optional>

namespace yarilo::core {

struct BlitSpan {
    uint32_t src_x;
    uint32_t src_y;
    uint32_t dst_x;
    uint32_t dst_y;
    uint32_t width;
    uint32_t height;
};

inline std::optional<BlitSpan> compute_clipped_blit(
    int32_t dst_x, int32_t dst_y,
    uint32_t src_w, uint32_t src_h,
    uint32_t screen_w, uint32_t screen_h) noexcept 
{
    // Защита от вырожденных нулевых размеров
    if (src_w == 0 || src_h == 0 || screen_w == 0 || screen_h == 0) {
        return std::nullopt;
    }

    int64_t x0 = dst_x;
    int64_t y0 = dst_y;
    int64_t x1 = x0 + static_cast<int64_t>(src_w);
    int64_t y1 = y0 + static_cast<int64_t>(src_h);

    int64_t scr_w = screen_w;
    int64_t scr_h = screen_h;

    if (x1 <= 0 || y1 <= 0 || x0 >= scr_w || y0 >= scr_h) {
        return std::nullopt;
    }

    int64_t clip_dst_x0 = std::max<int64_t>(0, x0);
    int64_t clip_dst_y0 = std::max<int64_t>(0, y0);
    int64_t clip_dst_x1 = std::min<int64_t>(scr_w, x1);
    int64_t clip_dst_y1 = std::min<int64_t>(scr_h, y1);

    int64_t clip_src_x0 = clip_dst_x0 - x0;
    int64_t clip_src_y0 = clip_dst_y0 - y0;

    return BlitSpan{
        .src_x  = static_cast<uint32_t>(clip_src_x0),
        .src_y  = static_cast<uint32_t>(clip_src_y0),
        .dst_x  = static_cast<uint32_t>(clip_dst_x0),
        .dst_y  = static_cast<uint32_t>(clip_dst_y0),
        .width  = static_cast<uint32_t>(clip_dst_x1 - clip_dst_x0),
        .height = static_cast<uint32_t>(clip_dst_y1 - clip_dst_y0),
    };
}

} // namespace yarilo::core