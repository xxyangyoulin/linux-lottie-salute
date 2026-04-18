#pragma once

#include <csignal>
#include <string>

namespace codex_salute {

struct AppOptions {
    std::string asset_path;
    double size_ratio = 0.333;
    double pos_x = 1.0;
    double pos_y = 0.0;
    int offset_x = 0;
    int offset_y = 0;
    double speed = 1.0;
    double opacity = 1.0;
    int duration_ms = 0;
    int fps = 0;
    std::string backend;
    std::string output_name;
    bool loop = false;
    bool flip = false;
    bool fade_in = false;
    bool fade_out = false;
    int fade_in_ms = 800;
    int fade_out_ms = 800;
};

extern volatile sig_atomic_t g_interrupted;

int run_wayland(const AppOptions& opts);

} // namespace codex_salute
