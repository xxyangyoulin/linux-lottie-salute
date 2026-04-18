#pragma once

#include <rlottie.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace codex_salute {

struct AnimationInfo {
    std::unique_ptr<rlottie::Animation> animation;
    int width = 0;
    int height = 0;
    int total_frames = 0;
    double frame_rate = 30.0;
    int duration_ms = 0;
};

inline AnimationInfo load_animation(const char* path) {
    AnimationInfo info;
    info.animation = rlottie::Animation::loadFromFile(path);
    if (!info.animation) return info;

    info.total_frames = static_cast<int>(info.animation->totalFrame());
    info.frame_rate = static_cast<double>(info.animation->frameRate());
    if (info.frame_rate <= 0.0) info.frame_rate = 30.0;
    info.duration_ms = static_cast<int>(info.total_frames / info.frame_rate * 1000.0);

    size_t w = 0, h = 0;
    info.animation->size(w, h);
    info.width = static_cast<int>(std::max<size_t>(1, w));
    info.height = static_cast<int>(std::max<size_t>(1, h));
    return info;
}

// Render a single frame at a specific resolution
inline void render_lottie_frame(rlottie::Animation& anim, int frame,
                                 std::vector<uint32_t>& buf, int w, int h) {
    rlottie::Surface surface(buf.data(),
                              static_cast<size_t>(w),
                              static_cast<size_t>(h),
                              static_cast<size_t>(w * 4));
    anim.renderSync(static_cast<size_t>(frame), surface);
}

// rlottie outputs RGBA byte order: R in byte 0, B in byte 2
// uint32_t on LE: A<<24 | B<<16 | G<<8 | R
// Wayland ARGB8888 / X11 TrueColor: A<<24 | R<<16 | G<<8 | B
// So we swap R and B channels.
inline uint32_t lottie_to_argb(uint32_t pixel) {
    uint32_t r = pixel & 0xFF;
    uint32_t g = (pixel >> 8) & 0xFF;
    uint32_t b = (pixel >> 16) & 0xFF;
    uint32_t a = (pixel >> 24) & 0xFF;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

struct RenderConfig {
    double size_ratio = 0.333;
    double pos_x = 1.0;
    double pos_y = 0.0;
    int offset_x = 0;
    int offset_y = 0;
    double opacity = 1.0;
    double rotate_deg = 0.0;
    bool flip = false;
};

// Compute destination rectangle for animation on a given output
inline void compute_rect(int screen_w, int screen_h, int anim_w, int anim_h,
                          const RenderConfig& cfg, int& out_x, int& out_y,
                          int& out_w, int& out_h) {
    out_h = static_cast<int>(screen_h * cfg.size_ratio);
    double scale = static_cast<double>(out_h) / anim_h;
    out_w = static_cast<int>(anim_w * scale);

    out_x = static_cast<int>((screen_w - out_w) * cfg.pos_x);
    out_y = static_cast<int>((screen_h - out_h) * cfg.pos_y);
    out_x += cfg.offset_x;
    out_y += cfg.offset_y;
}

// Copy rendered frame to destination with format conversion and optional flip
// src: rendered frame at (rw x rh) in rlottie RGBA byte order
// dst: destination buffer in ARGB format
inline void blit_to_dst(uint32_t* dst, int dst_stride, int dst_w, int dst_h,
                        const uint32_t* src, int rw, int rh,
                        int rx, int ry, bool flip, double opacity, double rotate_deg = 0.0) {
    int x_start = std::max(0, -rx);
    int x_end = std::min(rw, dst_w - rx);
    int y_start = std::max(0, -ry);
    int y_end = std::min(rh, dst_h - ry);
    const double clamped_opacity = std::clamp(opacity, 0.0, 1.0);
    const uint32_t opacity_scale = static_cast<uint32_t>(clamped_opacity * 255.0 + 0.5);
    if (opacity_scale == 0) return;
    const double norm_deg = std::fmod(rotate_deg, 360.0);
    const bool has_rotation = std::abs(norm_deg) > 1e-6;

    if (!has_rotation && opacity_scale == 255) {
        for (int y = y_start; y < y_end; ++y) {
            const uint32_t* src_row = src + y * rw;
            uint32_t* dst_row = dst + (ry + y) * dst_stride + rx;
            if (!flip) {
                for (int x = x_start; x < x_end; ++x) {
                    dst_row[x] = lottie_to_argb(src_row[x]);
                }
            } else {
                for (int x = x_start; x < x_end; ++x) {
                    dst_row[x] = lottie_to_argb(src_row[rw - 1 - x]);
                }
            }
        }
        return;
    }

    uint8_t scale_lut[256];
    for (int i = 0; i < 256; ++i) {
        scale_lut[i] = static_cast<uint8_t>((i * opacity_scale + 127) / 255);
    }

    if (!has_rotation) {
        for (int y = y_start; y < y_end; ++y) {
            const uint32_t* src_row = src + y * rw;
            uint32_t* dst_row = dst + (ry + y) * dst_stride + rx;
            for (int x = x_start; x < x_end; ++x) {
                int sx = flip ? (rw - 1 - x) : x;
                uint32_t argb = lottie_to_argb(src_row[sx]);
                uint32_t r = scale_lut[(argb >> 16) & 0xFF];
                uint32_t g = scale_lut[(argb >> 8) & 0xFF];
                uint32_t b = scale_lut[argb & 0xFF];
                uint32_t a = scale_lut[(argb >> 24) & 0xFF];
                dst_row[x] = (a << 24) | (r << 16) | (g << 8) | b;
            }
        }
        return;
    }

    const double rad = norm_deg * 3.14159265358979323846 / 180.0;
    const double c = std::cos(rad);
    const double s = std::sin(rad);
    const double cx = (rw - 1) * 0.5;
    const double cy = (rh - 1) * 0.5;

    for (int y = y_start; y < y_end; ++y) {
        uint32_t* dst_row = dst + (ry + y) * dst_stride + rx;
        const double dy = static_cast<double>(y) - cy;
        for (int x = x_start; x < x_end; ++x) {
            const double dx = static_cast<double>(x) - cx;
            double src_xf = c * dx + s * dy + cx;
            double src_yf = -s * dx + c * dy + cy;
            int sy = static_cast<int>(std::lround(src_yf));
            int sx = static_cast<int>(std::lround(src_xf));
            if (sy < 0 || sy >= rh || sx < 0 || sx >= rw) {
                continue;
            }
            if (flip) sx = rw - 1 - sx;
            uint32_t argb = lottie_to_argb(src[sy * rw + sx]);
            uint32_t r = scale_lut[(argb >> 16) & 0xFF];
            uint32_t g = scale_lut[(argb >> 8) & 0xFF];
            uint32_t b = scale_lut[argb & 0xFF];
            uint32_t a = scale_lut[(argb >> 24) & 0xFF];
            dst_row[x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

} // namespace codex_salute
