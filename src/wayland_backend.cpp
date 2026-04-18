#include "wayland_backend.h"
#include "render.h"

#include <wayland-client.h>
#include <wlr-layer-shell-client-protocol.h>
#include <xdg-output-unstable-v1-client-protocol.h>
#ifdef ENABLE_WAYLAND_GPU
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#endif

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

class WaylandBackend;

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
    WaylandBackend* owner = nullptr;
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
    bool gpu_ready = false;
    std::unique_ptr<ShmBuffer> buf;
    std::vector<uint32_t> tmp;
#ifdef ENABLE_WAYLAND_GPU
    wl_egl_window* egl_window = nullptr;
    EGLSurface egl_surface = EGL_NO_SURFACE;
    GLuint texture = 0;
    int tex_w = 0;
    int tex_h = 0;
#endif
};

// ── WaylandBackend ──────────────────────────────────────────────────────────

class WaylandBackend {
public:
    int run(const AppOptions& opts) {
        opts_ = &opts;
        auto finish = [&](int code) {
            cleanup();
            return code;
        };

        animation_info_ = load_animation(opts.asset_path.c_str());
        if (!animation_info_.animation) {
            fprintf(stderr, "[salute] failed to load: %s\n", opts.asset_path.c_str());
            return finish(1);
        }
        printf("[salute] loaded: %dx%d frames=%d fps=%.1f duration=%dms\n",
               animation_info_.width, animation_info_.height,
               animation_info_.total_frames, animation_info_.frame_rate,
               animation_info_.duration_ms);

        display_ = wl_display_connect(nullptr);
        if (!display_) {
            fprintf(stderr, "[salute] failed to connect to Wayland display\n");
            return finish(1);
        }

        registry_ = wl_display_get_registry(display_);
        wl_registry_add_listener(registry_, &kRegistryListener, this);
        wl_display_roundtrip(display_);
        wl_display_roundtrip(display_);

        if (!compositor_ || !layer_shell_) {
            fprintf(stderr, "[salute] required Wayland protocols missing\n");
            return finish(1);
        }

        bool want_gpu = opts_->gpu != "off";
#ifndef ENABLE_WAYLAND_GPU
        if (opts_->gpu == "on") {
            fprintf(stderr, "[salute] GPU path was requested but this build has no Wayland GPU support\n");
            return finish(1);
        }
#else
        if (want_gpu) {
            gpu_enabled_ = init_gpu();
            if (!gpu_enabled_ && opts_->gpu == "on") {
                fprintf(stderr, "[salute] failed to initialize Wayland GPU path\n");
                return finish(1);
            }
            if (!gpu_enabled_ && opts_->gpu == "auto") {
                printf("[salute] GPU path unavailable, falling back to SHM CPU path\n");
            }
        }
#endif
        if (gpu_enabled_) {
            printf("[salute] wayland renderer=gpu\n");
        } else {
            if (!shm_) {
                fprintf(stderr, "[salute] wl_shm is required for CPU path but is unavailable\n");
                return finish(1);
            }
            if (!has_argb8888_) {
                fprintf(stderr, "[salute] ARGB8888 SHM format not supported\n");
                return finish(1);
            }
            printf("[salute] wayland renderer=cpu\n");
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
            os.owner = this;
            os.width = 1920; os.height = 1080;
            os.shm = shm_;
            outputs_.push_back(std::move(os));
        }

        for (auto& os : outputs_) {
            os.owner = this;
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
        int no_target_ticks = 0;
        bool runtime_gpu_fallback_attempted = false;
        bool final_fade_committed = false;
        int max_no_target_ticks = std::max(60, target_fps * 2);

        while (!g_interrupted) {
            wl_display_dispatch_pending(display_);

            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();

            const int anim_time_ms = static_cast<int>(elapsed_ms * speed);
            int frame_time_ms = anim_time_ms;
            if (opts_->loop) {
                frame_time_ms = anim_time_ms % animation_info_.duration_ms;
            } else {
                frame_time_ms = std::min(anim_time_ms, animation_info_.duration_ms - 1);
            }

            int frame = static_cast<int>(
                (frame_time_ms / 1000.0) * animation_info_.frame_rate);
            if (frame >= animation_info_.total_frames) break;

            bool has_render_target = false;
            for (const auto& os : outputs_) {
                if (os.configured && (gpu_enabled_ ? os.gpu_ready : static_cast<bool>(os.buf))) {
                    has_render_target = true;
                    break;
                }
            }
            if (!has_render_target) {
                ++no_target_ticks;
#ifdef ENABLE_WAYLAND_GPU
                if (gpu_enabled_ && opts_->gpu == "auto" && !runtime_gpu_fallback_attempted) {
                    runtime_gpu_fallback_attempted = true;
                    if (fallback_to_cpu_from_gpu("runtime GPU output setup failed")) {
                        no_target_ticks = 0;
                        continue;
                    }
                }
#endif
                if (no_target_ticks >= max_no_target_ticks) {
                    fprintf(stderr, "[salute] no render target became available\n");
                    break;
                }
                next_frame += std::chrono::microseconds(frame_dur_us);
                auto now = std::chrono::steady_clock::now();
                if (next_frame > now) {
                    std::this_thread::sleep_until(next_frame);
                }
                continue;
            }
            no_target_ticks = 0;
            if (first_render_elapsed_ms < 0) first_render_elapsed_ms = elapsed_ms;
            const int64_t raw_visible_elapsed_ms = elapsed_ms - first_render_elapsed_ms;
            const int visible_total_ms = max_duration_ms > 0
                ? std::max<int64_t>(1, static_cast<int64_t>(max_duration_ms) - first_render_elapsed_ms)
                : 0;
            const int64_t visible_elapsed_ms = visible_total_ms > 0
                ? std::clamp<int64_t>(raw_visible_elapsed_ms, 0, visible_total_ms)
                : std::max<int64_t>(0, raw_visible_elapsed_ms);

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
                if (!os.configured) continue;
                if (gpu_enabled_) {
#ifdef ENABLE_WAYLAND_GPU
                    if (os.gpu_ready) render_frame_gpu(os, frame, fade_factor);
#endif
                } else {
                    if (os.buf) render_frame(os, frame, fade_factor);
                }
            }

            wl_display_flush(display_);

            const bool reached_max_duration = (max_duration_ms > 0 && elapsed_ms >= max_duration_ms);
            const bool reached_anim_end = (!opts_->loop && anim_time_ms >= animation_info_.duration_ms);
            if (reached_max_duration || (reached_anim_end && max_duration_ms == 0)) {
                if (!final_fade_committed && opts_->fade_out && max_duration_ms > 0) {
                    for (auto& os : outputs_) {
                        if (!os.configured) continue;
                        if (gpu_enabled_) {
#ifdef ENABLE_WAYLAND_GPU
                            if (os.gpu_ready) render_frame_gpu(os, frame, 0.0);
#endif
                        } else {
                            if (os.buf) render_frame(os, frame, 0.0);
                        }
                    }
                    wl_display_flush(display_);
                    std::this_thread::sleep_for(std::chrono::microseconds(frame_dur_us));
                    final_fade_committed = true;
                }
                break;
            }

            next_frame += std::chrono::microseconds(frame_dur_us);
            auto now = std::chrono::steady_clock::now();
            if (next_frame > now) {
                std::this_thread::sleep_until(next_frame);
            }
        }

        return finish(0);
    }

private:
    void configure_output(OutputState& os) {
        if (gpu_enabled_) {
#ifdef ENABLE_WAYLAND_GPU
            if (!ensure_gpu_output(os)) {
                fprintf(stderr, "[salute] failed to initialize GPU output %dx%d\n", os.width, os.height);
                os.configured = false;
                os.gpu_ready = false;
                return;
            }
            os.configured = true;
            printf("[salute] output configured size=%dx%d (gpu)\n", os.width, os.height);
#endif
            return;
        }

        if (!os.buf || os.buf->width != os.width || os.buf->height != os.height) {
            os.buf = std::make_unique<ShmBuffer>();
            if (!os.buf->create(os.shm, os.width, os.height)) {
                fprintf(stderr, "[salute] failed to create SHM buffer %dx%d\n", os.width, os.height);
                os.configured = false;
                return;
            }
        }
        os.configured = true;
        printf("[salute] output configured size=%dx%d (cpu)\n", os.width, os.height);
    }

    void render_frame(OutputState& os, int frame, double fade_factor) {
        if (!os.buf || !os.buf->data) return;

        RenderConfig cfg;
        cfg.size_ratio = opts_->size_ratio;
        cfg.pos_x = opts_->pos_x;
        cfg.pos_y = opts_->pos_y;
        cfg.offset_x = opts_->offset_x;
        cfg.offset_y = opts_->offset_y;
        cfg.opacity = opts_->opacity * fade_factor;
        cfg.rotate_deg = opts_->rotate_deg;
        cfg.flip = opts_->flip;

        int rx, ry, rw, rh;
        compute_rect(os.width, os.height, animation_info_.width, animation_info_.height,
                     cfg, rx, ry, rw, rh);
        if (rw <= 0 || rh <= 0) return;

        // Render at target resolution for crisp edges
        const size_t tmp_size = static_cast<size_t>(rw) * static_cast<size_t>(rh);
        if (os.tmp.size() != tmp_size) os.tmp.resize(tmp_size);
        render_lottie_frame(*animation_info_.animation, frame, os.tmp, rw, rh);

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
                    os.tmp.data(), rw, rh, rx, ry, cfg.flip, cfg.opacity, cfg.rotate_deg);

        wl_surface_attach(os.surface, os.buf->buffer, 0, 0);
        if (x_end > x_start && y_end > y_start) {
            wl_surface_damage_buffer(
                os.surface,
                rx + x_start,
                ry + y_start,
                x_end - x_start,
                y_end - y_start);
        }
        wl_surface_commit(os.surface);
    }

#ifdef ENABLE_WAYLAND_GPU
    bool fallback_to_cpu_from_gpu(const char* reason) {
        if (opts_->gpu != "auto") return false;
        printf("[salute] %s, falling back to SHM CPU path\n", reason);

        for (auto& os : outputs_) {
            destroy_gpu_output(os);
            os.configured = false;
        }
        shutdown_gpu();
        gpu_enabled_ = false;

        if (!shm_ || !has_argb8888_) return false;

        bool has_cpu_target = false;
        for (auto& os : outputs_) {
            if (os.width <= 0 || os.height <= 0) continue;
            configure_output(os);
            if (os.configured && os.buf) has_cpu_target = true;
        }
        return has_cpu_target;
    }

    static GLuint compile_shader(GLenum type, const char* src) {
        GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &src, nullptr);
        glCompileShader(shader);
        GLint ok = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024];
            GLsizei len = 0;
            glGetShaderInfoLog(shader, sizeof(log), &len, log);
            fprintf(stderr, "[salute] shader compile failed: %.*s\n", static_cast<int>(len), log);
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    }

    bool init_gpu() {
        egl_display_ = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(display_));
        if (egl_display_ == EGL_NO_DISPLAY) return false;
        if (!eglInitialize(egl_display_, nullptr, nullptr)) return false;

        static const EGLint cfg_attrs[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_NONE
        };
        EGLint num = 0;
        if (!eglChooseConfig(egl_display_, cfg_attrs, &egl_config_, 1, &num) || num <= 0) return false;
        if (!eglBindAPI(EGL_OPENGL_ES_API)) return false;

        static const EGLint ctx_attrs[] = {
            EGL_CONTEXT_CLIENT_VERSION, 2,
            EGL_NONE
        };
        egl_context_ = eglCreateContext(egl_display_, egl_config_, EGL_NO_CONTEXT, ctx_attrs);
        if (egl_context_ == EGL_NO_CONTEXT) return false;
        return true;
    }

    void shutdown_gpu() {
        if (gl_vbo_) glDeleteBuffers(1, &gl_vbo_);
        gl_vbo_ = 0;
        if (gl_program_) glDeleteProgram(gl_program_);
        gl_program_ = 0;
        if (egl_display_ != EGL_NO_DISPLAY) {
            eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (egl_context_ != EGL_NO_CONTEXT) eglDestroyContext(egl_display_, egl_context_);
            eglTerminate(egl_display_);
        }
        egl_context_ = EGL_NO_CONTEXT;
        egl_display_ = EGL_NO_DISPLAY;
        egl_config_ = nullptr;
    }

    void destroy_gpu_output(OutputState& os) {
        if (os.texture) {
            if (egl_display_ != EGL_NO_DISPLAY && os.egl_surface != EGL_NO_SURFACE && egl_context_ != EGL_NO_CONTEXT) {
                eglMakeCurrent(egl_display_, os.egl_surface, os.egl_surface, egl_context_);
                glDeleteTextures(1, &os.texture);
            }
            os.texture = 0;
        }
        if (os.egl_surface != EGL_NO_SURFACE && egl_display_ != EGL_NO_DISPLAY) {
            eglDestroySurface(egl_display_, os.egl_surface);
            os.egl_surface = EGL_NO_SURFACE;
        }
        if (os.egl_window) {
            wl_egl_window_destroy(os.egl_window);
            os.egl_window = nullptr;
        }
        os.gpu_ready = false;
        os.tex_w = 0;
        os.tex_h = 0;
    }

    bool ensure_gpu_output(OutputState& os) {
        if (egl_display_ == EGL_NO_DISPLAY || egl_context_ == EGL_NO_CONTEXT) return false;

        if (!os.egl_window) {
            os.egl_window = wl_egl_window_create(os.surface, os.width, os.height);
            if (!os.egl_window) return false;
        } else {
            wl_egl_window_resize(os.egl_window, os.width, os.height, 0, 0);
        }

        if (os.egl_surface == EGL_NO_SURFACE) {
            os.egl_surface = eglCreateWindowSurface(
                egl_display_, egl_config_,
                reinterpret_cast<EGLNativeWindowType>(os.egl_window), nullptr);
            if (os.egl_surface == EGL_NO_SURFACE) return false;
        }

        if (!eglMakeCurrent(egl_display_, os.egl_surface, os.egl_surface, egl_context_)) return false;

        if (gl_program_ == 0) {
            const char* vs_src =
                "attribute vec2 a_pos;\n"
                "attribute vec2 a_uv;\n"
                "varying vec2 v_uv;\n"
                "void main(){ v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
            const char* fs_src =
                "precision mediump float;\n"
                "varying vec2 v_uv;\n"
                "uniform sampler2D u_tex;\n"
                "uniform float u_opacity;\n"
                "void main(){ vec4 c = texture2D(u_tex, v_uv); gl_FragColor = vec4(c.bgr * u_opacity, c.a * u_opacity); }\n";

            GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
            GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
            if (!vs || !fs) return false;

            gl_program_ = glCreateProgram();
            glAttachShader(gl_program_, vs);
            glAttachShader(gl_program_, fs);
            glBindAttribLocation(gl_program_, 0, "a_pos");
            glBindAttribLocation(gl_program_, 1, "a_uv");
            glLinkProgram(gl_program_);
            glDeleteShader(vs);
            glDeleteShader(fs);

            GLint linked = 0;
            glGetProgramiv(gl_program_, GL_LINK_STATUS, &linked);
            if (!linked) {
                char log[1024];
                GLsizei len = 0;
                glGetProgramInfoLog(gl_program_, sizeof(log), &len, log);
                fprintf(stderr, "[salute] shader link failed: %.*s\n", static_cast<int>(len), log);
                glDeleteProgram(gl_program_);
                gl_program_ = 0;
                return false;
            }
            u_opacity_ = glGetUniformLocation(gl_program_, "u_opacity");
        }
        if (gl_vbo_ == 0) glGenBuffers(1, &gl_vbo_);

        if (!os.texture) {
            glGenTextures(1, &os.texture);
            glBindTexture(GL_TEXTURE_2D, os.texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        os.gpu_ready = true;
        return true;
    }

    void render_frame_gpu(OutputState& os, int frame, double fade_factor) {
        if (!os.gpu_ready) return;
        if (!eglMakeCurrent(egl_display_, os.egl_surface, os.egl_surface, egl_context_)) return;

        RenderConfig cfg;
        cfg.size_ratio = opts_->size_ratio;
        cfg.pos_x = opts_->pos_x;
        cfg.pos_y = opts_->pos_y;
        cfg.offset_x = opts_->offset_x;
        cfg.offset_y = opts_->offset_y;
        cfg.opacity = opts_->opacity * fade_factor;
        cfg.rotate_deg = opts_->rotate_deg;
        cfg.flip = opts_->flip;

        int rx, ry, rw, rh;
        compute_rect(os.width, os.height, animation_info_.width, animation_info_.height, cfg, rx, ry, rw, rh);
        if (rw <= 0 || rh <= 0) return;

        int x_start = std::max(0, -rx);
        int x_end = std::min(rw, os.width - rx);
        int y_start = std::max(0, -ry);
        int y_end = std::min(rh, os.height - ry);
        if (x_end <= x_start || y_end <= y_start) return;

        const size_t tmp_size = static_cast<size_t>(rw) * static_cast<size_t>(rh);
        if (os.tmp.size() != tmp_size) os.tmp.resize(tmp_size);
        render_lottie_frame(*animation_info_.animation, frame, os.tmp, rw, rh);

        glViewport(0, 0, os.width, os.height);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(gl_program_);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        glBindTexture(GL_TEXTURE_2D, os.texture);
        if (os.tex_w != rw || os.tex_h != rh) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rw, rh, 0, GL_RGBA, GL_UNSIGNED_BYTE, os.tmp.data());
            os.tex_w = rw;
            os.tex_h = rh;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, rw, rh, GL_RGBA, GL_UNSIGNED_BYTE, os.tmp.data());
        }

        const float px_left = static_cast<float>(rx + x_start);
        const float px_right = static_cast<float>(rx + x_end);
        const float px_top = static_cast<float>(ry + y_start);
        const float px_bottom = static_cast<float>(ry + y_end);

        const float u0 = static_cast<float>(x_start) / static_cast<float>(rw);
        const float u1 = static_cast<float>(x_end) / static_cast<float>(rw);
        const float v0 = static_cast<float>(y_start) / static_cast<float>(rh);
        const float v1 = static_cast<float>(y_end) / static_cast<float>(rh);
        const float ul = cfg.flip ? (1.0f - u0) : u0;
        const float ur = cfg.flip ? (1.0f - u1) : u1;

        float px0 = px_left,  py0 = px_top;
        float px1 = px_right, py1 = px_top;
        float px2 = px_left,  py2 = px_bottom;
        float px3 = px_right, py3 = px_bottom;
        if (std::abs(cfg.rotate_deg) > 1e-6) {
            const float rad = static_cast<float>(cfg.rotate_deg * 3.14159265358979323846 / 180.0);
            const float cr = std::cos(rad);
            const float sr = std::sin(rad);
            const float cx = 0.5f * (px_left + px_right);
            const float cy = 0.5f * (px_top + px_bottom);
            auto rot = [&](float& x, float& y) {
                const float dx = x - cx;
                const float dy = y - cy;
                x = cx + cr * dx - sr * dy;
                y = cy + sr * dx + cr * dy;
            };
            rot(px0, py0);
            rot(px1, py1);
            rot(px2, py2);
            rot(px3, py3);
        }

        auto to_ndc_x = [&](float px) {
            return (2.0f * px / static_cast<float>(os.width)) - 1.0f;
        };
        auto to_ndc_y = [&](float py) {
            return 1.0f - (2.0f * py / static_cast<float>(os.height));
        };

        const float vx0 = to_ndc_x(px0), vy0 = to_ndc_y(py0);
        const float vx1 = to_ndc_x(px1), vy1 = to_ndc_y(py1);
        const float vx2 = to_ndc_x(px2), vy2 = to_ndc_y(py2);
        const float vx3 = to_ndc_x(px3), vy3 = to_ndc_y(py3);

        const float vertices[] = {
            vx0, vy0, ul, v0,
            vx1, vy1, ur, v0,
            vx2, vy2, ul, v1,
            vx3, vy3, ur, v1,
        };

        glBindBuffer(GL_ARRAY_BUFFER, gl_vbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STREAM_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(0));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(2 * sizeof(float)));
        glUniform1f(u_opacity_, std::clamp(static_cast<float>(cfg.opacity), 0.0f, 1.0f));
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        eglSwapBuffers(egl_display_, os.egl_surface);
    }
#endif

    void cleanup() {
        for (auto& os : outputs_) {
#ifdef ENABLE_WAYLAND_GPU
            destroy_gpu_output(os);
#endif
            os.buf.reset();
            if (os.input_region) wl_region_destroy(os.input_region);
            if (os.surface) wl_surface_attach(os.surface, nullptr, 0, 0);
            if (os.layer_surface) zwlr_layer_surface_v1_destroy(os.layer_surface);
            if (os.xdg_output) zxdg_output_v1_destroy(os.xdg_output);
            if (os.surface) wl_surface_destroy(os.surface);
            if (os.output) wl_output_destroy(os.output);
            os.input_region = nullptr;
            os.layer_surface = nullptr;
            os.xdg_output = nullptr;
            os.surface = nullptr;
            os.output = nullptr;
        }
        outputs_.clear();
#ifdef ENABLE_WAYLAND_GPU
        shutdown_gpu();
#endif
        if (xdg_output_manager_) zxdg_output_manager_v1_destroy(xdg_output_manager_);
        xdg_output_manager_ = nullptr;
        if (layer_shell_) zwlr_layer_shell_v1_destroy(layer_shell_);
        layer_shell_ = nullptr;
        if (shm_) wl_shm_destroy(shm_);
        shm_ = nullptr;
        if (compositor_) wl_compositor_destroy(compositor_);
        compositor_ = nullptr;
        if (registry_) wl_registry_destroy(registry_);
        registry_ = nullptr;
        if (display_) wl_display_disconnect(display_);
        display_ = nullptr;
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
        if (!os->owner) return;
        os->width = std::max(1, static_cast<int>(w));
        os->height = std::max(1, static_cast<int>(h));
        zwlr_layer_surface_v1_ack_configure(surf, serial);
        os->owner->configure_output(*os);
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
    bool gpu_enabled_ = false;

#ifdef ENABLE_WAYLAND_GPU
    EGLDisplay egl_display_ = EGL_NO_DISPLAY;
    EGLContext egl_context_ = EGL_NO_CONTEXT;
    EGLConfig egl_config_ = nullptr;
    GLuint gl_program_ = 0;
    GLuint gl_vbo_ = 0;
    GLint u_opacity_ = -1;
#endif

    const AppOptions* opts_ = nullptr;
    AnimationInfo animation_info_;
};

int run_wayland(const AppOptions& opts) {
    WaylandBackend backend;
    return backend.run(opts);
}

} // namespace codex_salute
