#include "wayland_backend.h"
#include "x11_backend.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>

namespace codex_salute {

volatile sig_atomic_t g_interrupted = 0;

} // namespace codex_salute

static void on_signal(int) {
    codex_salute::g_interrupted = 1;
}

static void print_usage(const char* prog) {
    printf("lottie-salute v1.0.0 — Lottie animation overlay for Wayland/X11\n\n");
    printf("Usage: %s --asset PATH [OPTIONS]\n\n", prog);
    printf("Options:\n");
    printf("  --asset PATH     Lottie JSON file path (required)\n");
    printf("  --size FLOAT     Animation height as fraction of screen (default: 0.333)\n");
    printf("  --pos-x FLOAT    Horizontal position 0=left 1=right (default: 1.0)\n");
    printf("  --pos-y FLOAT    Vertical position 0=top 1=bottom (default: 0.0)\n");
    printf("  --offset-x INT   Horizontal pixel offset (default: 0)\n");
    printf("  --offset-y INT   Vertical pixel offset (default: 0)\n");
    printf("  --speed FLOAT    Playback speed multiplier (default: 1.0)\n");
    printf("  --opacity FLOAT  Opacity 0..1 (default: 1.0)\n");
    printf("  --duration-ms N  Max playback time in ms (default: 0=auto)\n");
    printf("  --fps INT        Render FPS cap (default: animation FPS)\n");
    printf("  --backend NAME   Backend: wayland|x11 (default: auto)\n");
    printf("  --output NAME    Target output name (default: all)\n");
    printf("  --loop           Loop animation\n");
    printf("  --flip           Horizontally flip animation\n");
    printf("  --fade-in        Enable fade-in at start\n");
    printf("  --fade-out       Enable fade-out at end\n");
    printf("  --fade-in-ms N   Fade-in duration in ms (default: 800)\n");
    printf("  --fade-out-ms N  Fade-out duration in ms (default: 800)\n");
    printf("  --gpu MODE       Wayland GPU mode: auto|on|off (default: auto)\n");
    printf("  -h, --help       Show this help\n");
}

int main(int argc, char** argv) {
    codex_salute::AppOptions opts;

    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--asset") {
            if (i + 1 >= argc) { fprintf(stderr, "Missing value for --asset\n"); return 1; }
            opts.asset_path = argv[++i];
        } else if (key == "--size") {
            if (i + 1 >= argc) { fprintf(stderr, "Missing value for --size\n"); return 1; }
            opts.size_ratio = std::atof(argv[++i]);
        } else if (key == "--pos-x") {
            if (i + 1 >= argc) { fprintf(stderr, "Missing value for --pos-x\n"); return 1; }
            opts.pos_x = std::atof(argv[++i]);
        } else if (key == "--pos-y") {
            if (i + 1 >= argc) { fprintf(stderr, "Missing value for --pos-y\n"); return 1; }
            opts.pos_y = std::atof(argv[++i]);
        } else if (key == "--offset-x") {
            if (i + 1 >= argc) { fprintf(stderr, "Missing value for --offset-x\n"); return 1; }
            opts.offset_x = std::atoi(argv[++i]);
        } else if (key == "--offset-y") {
            if (i + 1 >= argc) { fprintf(stderr, "Missing value for --offset-y\n"); return 1; }
            opts.offset_y = std::atoi(argv[++i]);
        } else if (key == "--speed") {
            if (i + 1 >= argc) { fprintf(stderr, "Missing value for --speed\n"); return 1; }
            opts.speed = std::atof(argv[++i]);
        } else if (key == "--opacity") {
            if (i + 1 >= argc) { fprintf(stderr, "Missing value for --opacity\n"); return 1; }
            opts.opacity = std::atof(argv[++i]);
        } else if (key == "--duration-ms") {
            if (i + 1 >= argc) { fprintf(stderr, "Missing value for --duration-ms\n"); return 1; }
            opts.duration_ms = std::atoi(argv[++i]);
        } else if (key == "--fps") {
            if (i + 1 >= argc) { fprintf(stderr, "Missing value for --fps\n"); return 1; }
            opts.fps = std::atoi(argv[++i]);
        } else if (key == "--backend") {
            if (i + 1 >= argc) { fprintf(stderr, "Missing value for --backend\n"); return 1; }
            opts.backend = argv[++i];
        } else if (key == "--output") {
            if (i + 1 >= argc) { fprintf(stderr, "Missing value for --output\n"); return 1; }
            opts.output_name = argv[++i];
        } else if (key == "--loop") {
            opts.loop = true;
        } else if (key == "--flip") {
            opts.flip = true;
        } else if (key == "--fade-in") {
            opts.fade_in = true;
        } else if (key == "--fade-out") {
            opts.fade_out = true;
        } else if (key == "--fade-in-ms") {
            if (i + 1 >= argc) { fprintf(stderr, "Missing value for --fade-in-ms\n"); return 1; }
            opts.fade_in_ms = std::atoi(argv[++i]);
            opts.fade_in = true;
        } else if (key == "--fade-out-ms") {
            if (i + 1 >= argc) { fprintf(stderr, "Missing value for --fade-out-ms\n"); return 1; }
            opts.fade_out_ms = std::atoi(argv[++i]);
            opts.fade_out = true;
        } else if (key == "--gpu") {
            if (i + 1 >= argc) { fprintf(stderr, "Missing value for --gpu\n"); return 1; }
            opts.gpu = argv[++i];
        } else if (key == "-h" || key == "--help") {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", key.c_str());
            return 1;
        }
    }

    if (opts.asset_path.empty()) {
        fprintf(stderr, "Usage: %s --asset PATH [OPTIONS]\nTry '%s --help' for more information.\n", argv[0], argv[0]);
        return 1;
    }
    if (opts.gpu != "auto" && opts.gpu != "on" && opts.gpu != "off") {
        fprintf(stderr, "Invalid --gpu: %s (expected auto, on, or off)\n", opts.gpu.c_str());
        return 1;
    }

    // Signal handling
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // Auto-detect backend: Wayland > X11
    const char* wayland_display = std::getenv("WAYLAND_DISPLAY");
    const char* x11_display = std::getenv("DISPLAY");

    if (!opts.backend.empty() && opts.backend != "wayland" && opts.backend != "x11") {
        fprintf(stderr, "Invalid --backend: %s (expected wayland or x11)\n", opts.backend.c_str());
        return 1;
    }

    if (opts.backend == "wayland") {
#ifdef ENABLE_WAYLAND
        return codex_salute::run_wayland(opts);
#else
        fprintf(stderr, "[salute] wayland backend is not enabled in this build\n");
        return 1;
#endif
    }

    if (opts.backend == "x11") {
#ifdef ENABLE_X11
        return codex_salute::run_x11(opts);
#else
        fprintf(stderr, "[salute] x11 backend is not enabled in this build\n");
        return 1;
#endif
    }

#ifdef ENABLE_WAYLAND
    if (wayland_display && wayland_display[0] != '\0') {
        printf("[salute] using Wayland backend\n");
        return codex_salute::run_wayland(opts);
    }
#endif

#ifdef ENABLE_X11
    if (x11_display && x11_display[0] != '\0') {
        printf("[salute] using X11 backend\n");
        return codex_salute::run_x11(opts);
    }
#endif

    fprintf(stderr, "[salute] no supported display found (need WAYLAND_DISPLAY or DISPLAY)\n");
    return 1;
}
