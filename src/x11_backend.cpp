#include "x11_backend.h"
#include "render.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/shape.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace codex_salute {

extern volatile sig_atomic_t g_interrupted;

struct X11Output {
    int x, y, width, height;
    std::string name;
};

static std::vector<X11Output> get_outputs(Display* dpy, Window root) {
    std::vector<X11Output> outputs;
    int event_base, error_base;
    if (!XRRQueryExtension(dpy, &event_base, &error_base)) return outputs;

    XRRScreenResources* res = XRRGetScreenResources(dpy, root);
    if (!res) return outputs;

    for (int i = 0; i < res->noutput; ++i) {
        XRROutputInfo* info = XRRGetOutputInfo(dpy, res, res->outputs[i]);
        if (!info || !info->crtc) { XRRFreeOutputInfo(info); continue; }

        XRRCrtcInfo* crtc = XRRGetCrtcInfo(dpy, res, info->crtc);
        if (!crtc || crtc->width == 0) { XRRFreeCrtcInfo(crtc); XRRFreeOutputInfo(info); continue; }

        X11Output out;
        out.x = crtc->x;
        out.y = crtc->y;
        out.width = static_cast<int>(crtc->width);
        out.height = static_cast<int>(crtc->height);
        if (info->name) out.name = info->name;
        outputs.push_back(out);

        XRRFreeCrtcInfo(crtc);
        XRRFreeOutputInfo(info);
    }
    XRRFreeScreenResources(res);
    return outputs;
}

static Visual* find_argb_visual(Display* dpy, int& depth) {
    XVisualInfo tpl;
    tpl.screen = DefaultScreen(dpy);
    tpl.depth = 32;
    tpl.c_class = TrueColor;
    int n;
    XVisualInfo* infos = XGetVisualInfo(dpy, VisualScreenMask | VisualDepthMask | VisualClassMask, &tpl, &n);
    if (infos) {
        for (int i = 0; i < n; ++i) {
            auto& v = infos[i];
            if (v.red_mask == 0xff0000 && v.green_mask == 0xff00 && v.blue_mask == 0xff) {
                depth = v.depth;
                Visual* vis = v.visual;
                XFree(infos);
                return vis;
            }
        }
        XFree(infos);
    }
    // Fallback to default visual
    depth = DefaultDepth(dpy, DefaultScreen(dpy));
    return DefaultVisual(dpy, DefaultScreen(dpy));
}

class X11Backend {
public:
    int run(const AppOptions& opts) {
        animation_info_ = load_animation(opts.asset_path.c_str());
        if (!animation_info_.animation) {
            fprintf(stderr, "[salute] failed to load: %s\n", opts.asset_path.c_str());
            return 1;
        }
        printf("[salute] loaded: %dx%d frames=%d fps=%.1f duration=%dms\n",
               animation_info_.width, animation_info_.height,
               animation_info_.total_frames, animation_info_.frame_rate,
               animation_info_.duration_ms);

        dpy_ = XOpenDisplay(nullptr);
        if (!dpy_) {
            fprintf(stderr, "[salute] failed to open X11 display\n");
            return 1;
        }

        auto outputs = get_outputs(dpy_, DefaultRootWindow(dpy_));
        if (outputs.empty()) {
            X11Output fallback;
            fallback.x = 0; fallback.y = 0;
            fallback.width = DisplayWidth(dpy_, DefaultScreen(dpy_));
            fallback.height = DisplayHeight(dpy_, DefaultScreen(dpy_));
            outputs.push_back(fallback);
        }

        if (!opts.output_name.empty()) {
            outputs.erase(
                std::remove_if(outputs.begin(), outputs.end(),
                    [&](const X11Output& o) { return o.name != opts.output_name; }),
                outputs.end());
        }

        int depth;
        Visual* visual = find_argb_visual(dpy_, depth);
        Colormap cmap = XCreateColormap(dpy_, DefaultRootWindow(dpy_), visual, AllocNone);

        Atom wm_type = XInternAtom(dpy_, "_NET_WM_WINDOW_TYPE", False);
        Atom type_notification = XInternAtom(dpy_, "_NET_WM_WINDOW_TYPE_NOTIFICATION", False);
        Atom type_dock = XInternAtom(dpy_, "_NET_WM_WINDOW_TYPE_DOCK", False);
        Atom wm_state = XInternAtom(dpy_, "_NET_WM_STATE", False);
    Atom state_above = XInternAtom(dpy_, "_NET_WM_STATE_ABOVE", False);
    Atom state_skip_taskbar = XInternAtom(dpy_, "_NET_WM_STATE_SKIP_TASKBAR", False);
    Atom state_skip_pager = XInternAtom(dpy_, "_NET_WM_STATE_SKIP_PAGER", False);

        struct WinState {
            Window win;
            XImage* img;
            GC gc;
            std::vector<uint32_t> buf;
            std::vector<uint32_t> tmp;
            int w, h;
        };
        std::vector<WinState> wins;

        for (auto& out : outputs) {
            XSetWindowAttributes swa;
            swa.colormap = cmap;
            swa.background_pixel = 0;
            swa.border_pixel = 0;
            swa.event_mask = ExposureMask;

            Window win = XCreateWindow(dpy_, DefaultRootWindow(dpy_),
                out.x, out.y, out.width, out.height, 0,
                depth, InputOutput, visual,
                CWColormap | CWBackPixel | CWBorderPixel | CWEventMask, &swa);

            // Window type: notification, fallback dock
            Atom type = type_notification ? type_notification : type_dock;
            XChangeProperty(dpy_, win, wm_type, XA_ATOM, 32,
                            PropModeReplace, reinterpret_cast<unsigned char*>(&type), 1);

            // State hints
            Atom states[] = { state_above, state_skip_taskbar, state_skip_pager };
            XChangeProperty(dpy_, win, wm_state, XA_ATOM, 32,
                            PropModeReplace, reinterpret_cast<unsigned char*>(states), 3);

            // Input pass-through via shape
            if (XShapeQueryExtension(dpy_, &depth, &depth)) {
                XShapeCombineRectangles(dpy_, win, ShapeInput, 0, 0, nullptr, 0, ShapeSet, Unsorted);
            }

            XMapWindow(dpy_, win);

            WinState ws;
            ws.win = win;
            ws.w = out.width;
            ws.h = out.height;
            ws.buf.resize(static_cast<size_t>(out.width * out.height), 0);
            ws.img = XCreateImage(dpy_, visual, depth, ZPixmap, 0,
                                  reinterpret_cast<char*>(ws.buf.data()),
                                  out.width, out.height, 32, out.width * 4);
            ws.gc = XCreateGC(dpy_, win, 0, nullptr);
            wins.push_back(std::move(ws));
        }

        XFreeColormap(dpy_, cmap);

        printf("[salute] x11 outputs=%zu\n", wins.size());

        // Render loop
        auto start = std::chrono::steady_clock::now();
        const double speed = opts.speed > 0.0 ? opts.speed : 1.0;
        const int target_fps = opts.fps > 0 ? opts.fps : static_cast<int>(std::round(animation_info_.frame_rate));
        const int64_t frame_dur_us = target_fps > 0 ? static_cast<int64_t>(1000000.0 / target_fps) : 33333;
        const int max_duration_ms = opts.duration_ms > 0
            ? opts.duration_ms
            : (!opts.loop ? static_cast<int>(animation_info_.duration_ms / speed) : 0);
        auto next_frame = start;
        int64_t first_render_elapsed_ms = -1;

        while (!g_interrupted) {
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();

            if (max_duration_ms > 0 && elapsed_ms >= max_duration_ms) break;

            const int anim_time_ms = static_cast<int>(elapsed_ms * speed);
            if (!opts.loop && anim_time_ms >= animation_info_.duration_ms) break;

            int frame = static_cast<int>(
                ((anim_time_ms % animation_info_.duration_ms) / 1000.0) * animation_info_.frame_rate);
            if (frame >= animation_info_.total_frames) break;
            if (first_render_elapsed_ms < 0) first_render_elapsed_ms = elapsed_ms;
            const int64_t visible_elapsed_ms = elapsed_ms - first_render_elapsed_ms;
            const int visible_total_ms = max_duration_ms > 0
                ? std::max<int64_t>(1, static_cast<int64_t>(max_duration_ms) - first_render_elapsed_ms)
                : 0;

            double fade_factor = 1.0;
            if (opts.fade_in) {
                const int fade_in_ms = std::max(1, opts.fade_in_ms);
                fade_factor = std::min(
                    fade_factor,
                    std::clamp(static_cast<double>(visible_elapsed_ms) / fade_in_ms, 0.0, 1.0));
            }
            if (opts.fade_out && max_duration_ms > 0) {
                const int fade_out_ms = std::max(1, opts.fade_out_ms);
                fade_factor = std::min(
                    fade_factor,
                    std::clamp(static_cast<double>(visible_total_ms - visible_elapsed_ms) / fade_out_ms, 0.0, 1.0));
            }

            RenderConfig cfg;
            cfg.size_ratio = opts.size_ratio;
            cfg.pos_x = opts.pos_x;
            cfg.pos_y = opts.pos_y;
            cfg.offset_x = opts.offset_x;
            cfg.offset_y = opts.offset_y;
            cfg.opacity = opts.opacity * fade_factor;
            cfg.rotate_deg = opts.rotate_deg;
            cfg.flip = opts.flip;

            for (auto& ws : wins) {
                int rx, ry, rw, rh;
                compute_rect(ws.w, ws.h, animation_info_.width, animation_info_.height,
                            cfg, rx, ry, rw, rh);
                if (rw <= 0 || rh <= 0) continue;

                // Render at target resolution for crisp edges
                const size_t tmp_size = static_cast<size_t>(rw) * static_cast<size_t>(rh);
                if (ws.tmp.size() != tmp_size) ws.tmp.resize(tmp_size);
                render_lottie_frame(*animation_info_.animation, frame, ws.tmp, rw, rh);

                int x_start = std::max(0, -rx);
                int x_end = std::min(rw, ws.w - rx);
                int y_start = std::max(0, -ry);
                int y_end = std::min(rh, ws.h - ry);
                if (x_end <= x_start || y_end <= y_start) continue;

                for (int y = y_start; y < y_end; ++y) {
                    uint32_t* row = ws.buf.data() + (ry + y) * ws.w + rx;
                    memset(row, 0, static_cast<size_t>((x_end - x_start) * 4));
                }

                blit_to_dst(ws.buf.data(), ws.w, ws.w, ws.h,
                           ws.tmp.data(), rw, rh, rx, ry, cfg.flip, cfg.opacity, cfg.rotate_deg);

                XPutImage(dpy_, ws.win, ws.gc, ws.img,
                          rx + x_start, ry + y_start,
                          rx + x_start, ry + y_start,
                          static_cast<unsigned int>(x_end - x_start),
                          static_cast<unsigned int>(y_end - y_start));
            }

            XFlush(dpy_);

            next_frame += std::chrono::microseconds(frame_dur_us);
            auto now = std::chrono::steady_clock::now();
            if (next_frame > now) {
                std::this_thread::sleep_until(next_frame);
            }
        }

        for (auto& ws : wins) {
            XFreeGC(dpy_, ws.gc);
            ws.img->data = nullptr; // we own the buffer
            XDestroyImage(ws.img);
            XDestroyWindow(dpy_, ws.win);
        }
        XCloseDisplay(dpy_);
        return 0;
    }

private:
    AnimationInfo animation_info_;
    Display* dpy_ = nullptr;
};

int run_x11(const AppOptions& opts) {
    X11Backend backend;
    return backend.run(opts);
}

} // namespace codex_salute
