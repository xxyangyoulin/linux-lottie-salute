#include "wayland_backend.h"
#include "render.h"

#include <wayland-client.h>
#include <wlr-layer-shell-client-protocol.h>
#include <xdg-output-unstable-v1-client-protocol.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <fcntl.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

#ifdef __linux__
#include <linux/memfd.h>
#include <sys/syscall.h>
#endif

// Stub for cross-protocol reference (wlr-layer-shell references xdg_popup_interface)
extern "C" const struct wl_interface xdg_popup_interface = {"xdg_popup", 0, 0, nullptr};

namespace codex_salute {

extern volatile sig_atomic_t g_interrupted;

// ── SHM helpers ─────────────────────────────────────────────────────────────

static int create_shm_fd(size_t size) {
    int fd = -1;
#ifdef __linux__
    fd = static_cast<int>(syscall(SYS_memfd_create, "lottie_salute_shm",
                                  MFD_CLOEXEC));
    if (fd >= 0) {
        if (ftruncate(fd, static_cast<off_t>(size)) < 0) { close(fd); return -1; }
        return fd;
    }
    fd = -1;
#endif
    // Fallback: mkstemp
    char tmpl[] = "/tmp/lottie_salute_XXXXXX";
    fd = mkstemp(tmpl);
    if (fd < 0) return -1;
    unlink(tmpl);
    if (ftruncate(fd, static_cast<off_t>(size)) < 0) { close(fd); return -1; }
    return fd;
}

struct ShmBuffer {
    wl_shm_pool* pool = nullptr;
    wl_buffer* buffer = nullptr;
    uint32_t* data = nullptr;
    int width = 0, height = 0, stride = 0;

    ~ShmBuffer() {
        if (buffer) wl_buffer_destroy(buffer);
        if (pool) wl_shm_pool_destroy(pool);
        if (data) munmap(data, static_cast<size_t>(stride * height));
    }

    bool create(wl_shm* shm, int w, int h) {
        width = w; height = h; stride = w * 4;
        int fd = create_shm_fd(static_cast<size_t>(stride * h));
        if (fd < 0) return false;

        data = static_cast<uint32_t*>(
            mmap(nullptr, static_cast<size_t>(stride * h),
                 PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
        if (data == MAP_FAILED) { data = nullptr; close(fd); return false; }

        pool = wl_shm_create_pool(shm, fd, stride * h);
        close(fd);
        if (!pool) return false;

        buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
        return buffer != nullptr;
    }
};

// ── Per-output state ────────────────────────────────────────────────────────

struct OutputState {
    wl_output* output = nullptr;
    wl_surface* surface = nullptr;
    wl_region* input_region = nullptr;
    zwlr_layer_surface_v1* layer_surface = nullptr;
    zxdg_output_v1* xdg_output = nullptr;
    wl_shm* shm = nullptr;
    std::string name;
    int width = 0, height = 0;
    int mode_w = 0, mode_h = 0;
    bool configured = false;
    bool has_geometry = false;
    std::unique_ptr<ShmBuffer> buf;
};

// ── WaylandBackend ──────────────────────────────────────────────────────────

class WaylandBackend {
public:
    int run(const AppOptions& opts) {
        opts_ = &opts;

        animation_info_ = load_animation(opts.asset_path.c_str());
        if (!animation_info_.animation) {
            fprintf(stderr, "[salute] failed to load: %s\n", opts.asset_path.c_str());
            return 1;
        }
        printf("[salute] loaded: %dx%d frames=%d fps=%.1f duration=%dms\n",
               animation_info_.width, animation_info_.height,
               animation_info_.total_frames, animation_info_.frame_rate,
               animation_info_.duration_ms);

        display_ = wl_display_connect(nullptr);
        if (!display_) {
            fprintf(stderr, "[salute] failed to connect to Wayland display\n");
            return 1;
        }

        registry_ = wl_display_get_registry(display_);
        wl_registry_add_listener(registry_, &kRegistryListener, this);
        wl_display_roundtrip(display_);
        wl_display_roundtrip(display_);

        if (!shm_ || !compositor_ || !layer_shell_) {
            fprintf(stderr, "[salute] required Wayland protocols missing\n");
            return 1;
        }

        if (!has_argb8888_) {
            fprintf(stderr, "[salute] ARGB8888 SHM format not supported\n");
            return 1;
        }

        if (xdg_output_manager_) {
            for (auto& os : outputs_) {
                os.xdg_output = zxdg_output_manager_v1_get_xdg_output(
                    xdg_output_manager_, os.output);
                zxdg_output_v1_add_listener(os.xdg_output, &kXdgOutputListener, &os);
            }
            wl_display_roundtrip(display_);
        }

        for (auto& os : outputs_) {
            if (!os.has_geometry && os.mode_w > 0) {
                os.width = os.mode_w;
                os.height = os.mode_h;
            }
            if (os.width == 0) { os.width = 1920; os.height = 1080; }
        }

        // Filter by output name if specified
        if (!opts_->output_name.empty()) {
            outputs_.erase(
                std::remove_if(outputs_.begin(), outputs_.end(),
                    [&](const OutputState& os) {
                        return os.name != opts_->output_name;
                    }),
                outputs_.end());
        }

        if (outputs_.empty()) {
            OutputState os;
            os.width = 1920; os.height = 1080;
            os.shm = shm_;
            outputs_.push_back(std::move(os));
        }

        for (auto& os : outputs_) {
            os.shm = shm_;
            create_layer_surface(os);
        }
        wl_display_roundtrip(display_);

        printf("[salute] wayland outputs=%zu\n", outputs_.size());

        // Render loop
        auto start = std::chrono::steady_clock::now();
        const double speed = opts_->speed > 0.0 ? opts_->speed : 1.0;
        const int target_fps = opts_->fps > 0 ? opts_->fps : static_cast<int>(std::round(animation_info_.frame_rate));
        const int64_t frame_dur_us = target_fps > 0 ? static_cast<int64_t>(1000000.0 / target_fps) : 33333;
        const int max_duration_ms = opts_->duration_ms > 0
            ? opts_->duration_ms
            : (!opts_->loop ? static_cast<int>(animation_info_.duration_ms / speed) : 0);
        auto next_frame = start;
        int64_t first_render_elapsed_ms = -1;

        while (!g_interrupted) {
            wl_display_dispatch_pending(display_);

            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();

            if (max_duration_ms > 0 && elapsed_ms >= max_duration_ms) break;

            const int anim_time_ms = static_cast<int>(elapsed_ms * speed);
            if (!opts_->loop && anim_time_ms >= animation_info_.duration_ms) break;

            int frame = static_cast<int>(
                ((anim_time_ms % animation_info_.duration_ms) / 1000.0) * animation_info_.frame_rate);
            if (frame >= animation_info_.total_frames) break;

            bool has_render_target = false;
            for (const auto& os : outputs_) {
                if (os.configured && os.buf) {
                    has_render_target = true;
                    break;
                }
            }
            if (!has_render_target) {
                next_frame += std::chrono::microseconds(frame_dur_us);
                auto now = std::chrono::steady_clock::now();
                if (next_frame > now) {
                    std::this_thread::sleep_until(next_frame);
                }
                continue;
            }
            if (first_render_elapsed_ms < 0) first_render_elapsed_ms = elapsed_ms;
            const int64_t visible_elapsed_ms = elapsed_ms - first_render_elapsed_ms;
            const int visible_total_ms = max_duration_ms > 0
                ? std::max<int64_t>(1, static_cast<int64_t>(max_duration_ms) - first_render_elapsed_ms)
                : 0;

            double fade_factor = 1.0;
            if (opts_->fade_in) {
                const int fade_in_ms = std::max(1, opts_->fade_in_ms);
                fade_factor = std::min(
                    fade_factor,
                    std::clamp(static_cast<double>(visible_elapsed_ms) / fade_in_ms, 0.0, 1.0));
            }
            if (opts_->fade_out && max_duration_ms > 0) {
                const int fade_out_ms = std::max(1, opts_->fade_out_ms);
                fade_factor = std::min(
                    fade_factor,
                    std::clamp(static_cast<double>(visible_total_ms - visible_elapsed_ms) / fade_out_ms, 0.0, 1.0));
            }

            for (auto& os : outputs_) {
                if (os.configured && os.buf) {
                    render_frame(os, frame, fade_factor);
                }
            }

            wl_display_flush(display_);

            next_frame += std::chrono::microseconds(frame_dur_us);
            auto now = std::chrono::steady_clock::now();
            if (next_frame > now) {
                std::this_thread::sleep_until(next_frame);
            }
        }

        // Cleanup
        for (auto& os : outputs_) {
            os.buf.reset();
            if (os.input_region) wl_region_destroy(os.input_region);
            if (os.surface) wl_surface_attach(os.surface, nullptr, 0, 0);
            if (os.layer_surface) zwlr_layer_surface_v1_destroy(os.layer_surface);
            if (os.xdg_output) zxdg_output_v1_destroy(os.xdg_output);
            if (os.surface) wl_surface_destroy(os.surface);
        }
        if (xdg_output_manager_) zxdg_output_manager_v1_destroy(xdg_output_manager_);
        if (layer_shell_) zwlr_layer_shell_v1_destroy(layer_shell_);
        wl_registry_destroy(registry_);
        wl_display_disconnect(display_);
        return 0;
    }

private:
    void render_frame(OutputState& os, int frame, double fade_factor) {
        if (!os.buf || !os.buf->data) return;

        RenderConfig cfg;
        cfg.size_ratio = opts_->size_ratio;
        cfg.pos_x = opts_->pos_x;
        cfg.pos_y = opts_->pos_y;
        cfg.offset_x = opts_->offset_x;
        cfg.offset_y = opts_->offset_y;
        cfg.opacity = opts_->opacity * fade_factor;
        cfg.flip = opts_->flip;

        int rx, ry, rw, rh;
        compute_rect(os.width, os.height, animation_info_.width, animation_info_.height,
                     cfg, rx, ry, rw, rh);

        // Render at target resolution for crisp edges
        std::vector<uint32_t> tmp(static_cast<size_t>(rw * rh));
        render_lottie_frame(*animation_info_.animation, frame, tmp, rw, rh);

        // Partial clear
        int x_start = std::max(0, -rx);
        int x_end = std::min(rw, os.width - rx);
        int y_start = std::max(0, -ry);
        int y_end = std::min(rh, os.height - ry);

        for (int y = y_start; y < y_end; ++y) {
            uint32_t* row = os.buf->data + (ry + y) * os.width + rx;
            memset(row, 0, static_cast<size_t>((x_end - x_start) * 4));
        }

        blit_to_dst(os.buf->data, os.width, os.width, os.height,
                    tmp.data(), rw, rh, rx, ry, cfg.flip, cfg.opacity);

        wl_surface_attach(os.surface, os.buf->buffer, 0, 0);
        wl_surface_damage_buffer(os.surface, 0, 0, os.width, os.height);
        wl_surface_commit(os.surface);
    }

    void create_layer_surface(OutputState& os) {
        os.surface = wl_compositor_create_surface(compositor_);
        os.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
            layer_shell_, os.surface, os.output,
            ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "lottie-salute");

        zwlr_layer_surface_v1_set_anchor(os.layer_surface,
            ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
        zwlr_layer_surface_v1_set_exclusive_zone(os.layer_surface, -1);
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            os.layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);

        os.input_region = wl_compositor_create_region(compositor_);
        wl_surface_set_input_region(os.surface, os.input_region);

        zwlr_layer_surface_v1_add_listener(os.layer_surface, &kLayerSurfaceListener, &os);
        wl_surface_commit(os.surface);
    }

    // ── Listeners ──

    static void layer_surface_configure(void* data, zwlr_layer_surface_v1* surf,
                                        uint32_t serial, uint32_t w, uint32_t h) {
        auto* os = static_cast<OutputState*>(data);
        os->width = std::max(1, static_cast<int>(w));
        os->height = std::max(1, static_cast<int>(h));
        zwlr_layer_surface_v1_ack_configure(surf, serial);

        if (!os->buf) {
            os->buf = std::make_unique<ShmBuffer>();
            if (!os->buf->create(os->shm, os->width, os->height)) {
                fprintf(stderr, "[salute] failed to create SHM buffer %dx%d\n", os->width, os->height);
            } else {
                os->configured = true;
                printf("[salute] output configured size=%dx%d\n", os->width, os->height);
            }
        }
    }

    static void layer_surface_closed(void* data, zwlr_layer_surface_v1*) {
        static_cast<OutputState*>(data)->configured = false;
    }

    static constexpr zwlr_layer_surface_v1_listener kLayerSurfaceListener = {
        layer_surface_configure, layer_surface_closed};

    static void shm_format(void* data, wl_shm*, uint32_t fmt) {
        auto* self = static_cast<WaylandBackend*>(data);
        if (fmt == WL_SHM_FORMAT_ARGB8888) self->has_argb8888_ = true;
    }
    static constexpr wl_shm_listener kShmListener = {shm_format};

    static void on_global(void* data, wl_registry* reg, uint32_t name,
                          const char* iface, uint32_t version) {
        auto* self = static_cast<WaylandBackend*>(data);
        if (strcmp(iface, wl_compositor_interface.name) == 0) {
            self->compositor_ = static_cast<wl_compositor*>(
                wl_registry_bind(reg, name, &wl_compositor_interface, std::min(version, 4u)));
        } else if (strcmp(iface, wl_shm_interface.name) == 0) {
            self->shm_ = static_cast<wl_shm*>(
                wl_registry_bind(reg, name, &wl_shm_interface, 1));
            wl_shm_add_listener(self->shm_, &kShmListener, self);
        } else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
            self->layer_shell_ = static_cast<zwlr_layer_shell_v1*>(
                wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, std::min(version, 3u)));
        } else if (strcmp(iface, zxdg_output_manager_v1_interface.name) == 0) {
            self->xdg_output_manager_ = static_cast<zxdg_output_manager_v1*>(
                wl_registry_bind(reg, name, &zxdg_output_manager_v1_interface, std::min(version, 3u)));
        } else if (strcmp(iface, wl_output_interface.name) == 0) {
            auto* output = static_cast<wl_output*>(
                wl_registry_bind(reg, name, &wl_output_interface, std::min(version, 4u)));
            OutputState os;
            os.output = output;
            auto& ref = self->outputs_.emplace_back(std::move(os));
            wl_output_add_listener(output, &kOutputListener, &ref);
        }
    }

    static void on_global_remove(void*, wl_registry*, uint32_t) {}
    static constexpr wl_registry_listener kRegistryListener = {on_global, on_global_remove};

    // wl_output
    static void output_geometry(void*, wl_output*, int32_t, int32_t, int32_t, int32_t,
                                int32_t, const char*, const char*, int32_t) {}
    static void output_mode(void* data, wl_output*, uint32_t flags, int32_t w, int32_t h, int32_t) {
        if (flags & WL_OUTPUT_MODE_CURRENT) {
            auto* os = static_cast<OutputState*>(data);
            os->mode_w = w;
            os->mode_h = h;
        }
    }
    static void output_done(void*, wl_output*) {}
    static void output_scale(void*, wl_output*, int32_t) {}
    static void output_name(void* data, wl_output*, const char* name) {
        static_cast<OutputState*>(data)->name = name;
    }
    static void output_description(void*, wl_output*, const char*) {}
    static constexpr wl_output_listener kOutputListener = {
        output_geometry, output_mode, output_done, output_scale, output_name, output_description};

    // xdg-output
    static void xdg_output_logical_position(void*, zxdg_output_v1*, int32_t, int32_t) {}
    static void xdg_output_logical_size(void* data, zxdg_output_v1*, int32_t w, int32_t h) {
        auto* os = static_cast<OutputState*>(data);
        os->width = std::max(1, w);
        os->height = std::max(1, h);
        os->has_geometry = true;
    }
    static void xdg_output_done(void*, zxdg_output_v1*) {}
    static void xdg_output_name(void* data, zxdg_output_v1*, const char* name) {
        auto* os = static_cast<OutputState*>(data);
        if (os->name.empty()) os->name = name;
    }
    static void xdg_output_description(void*, zxdg_output_v1*, const char*) {}
    static constexpr zxdg_output_v1_listener kXdgOutputListener = {
        xdg_output_logical_position, xdg_output_logical_size,
        xdg_output_done, xdg_output_name, xdg_output_description};

    // State
    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;
    wl_compositor* compositor_ = nullptr;
    wl_shm* shm_ = nullptr;
    zwlr_layer_shell_v1* layer_shell_ = nullptr;
    zxdg_output_manager_v1* xdg_output_manager_ = nullptr;
    std::vector<OutputState> outputs_;
    bool has_argb8888_ = false;

    const AppOptions* opts_ = nullptr;
    AnimationInfo animation_info_;
};

int run_wayland(const AppOptions& opts) {
    WaylandBackend backend;
    return backend.run(opts);
}

} // namespace codex_salute
