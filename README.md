# lottie-salute

A lightweight Lottie animation overlay for Wayland and X11. Plays a Lottie animation on top of all windows, then exits.

> [!TIP]
> Find animations on [LottieFiles](https://lottiefiles.com/) and download the JSON format for `--asset`.

## Demo

<video src="./demo.mp4" controls width="900"></video>

[Download demo video](./demo.mp4)

## Fun Use with Codex

You can use `lottie-salute` as a fun completion effect for each Codex turn.

Add this to `~/.codex/config.toml`:

```toml
notify = ["sh", "-lc", "lottie-salute --asset /home/yyl/Documents/Confetti.json --flip --pos-x 1.07 --pos-y -0.03 --fade-in --fade-out --fade-in-ms 300 --fade-out-ms 100 >/dev/null 2>&1 &"]
```

Then restart `codex` interactive mode.

## Quick Start

```bash
cmake -B build
cmake --build build
./build/lottie-salute --asset ./salute.json
```

If you only want to verify CLI parameters:

```bash
./build/lottie-salute --help
```

## Dependencies

- CMake >= 3.16
- C++17 compiler
- [rlottie](https://github.com/Samsung/rlottie)
- Wayland backend (optional): `wayland-client`, `wayland-scanner`, `wlr-layer-shell` protocol
- X11 backend (optional): `libx11`, `libxrender`, `libxcb`, `xcb-randr`

At least one display backend (Wayland or X11) is required.

## Build

```bash
cmake -B build
cmake --build build
```

## Install

```bash
sudo cmake --install build
# Or to a custom prefix:
cmake --install build --prefix /usr/local
```

## Usage

```bash
lottie-salute --asset animation.json [OPTIONS]
```

## Playback Rules

- `--duration-ms` limits total playback wall time.
- Without `--loop`, animation stops when content ends (or `--duration-ms` is reached, whichever comes first).
- With `--loop`, playback continues until interrupted, unless `--duration-ms` is set.
- `--fade-in-ms` and `--fade-out-ms` also enable fade-in / fade-out automatically.
- `--fade-out-ms` takes effect near the end of visible playback time, so set `--duration-ms` when you need deterministic fade-out timing.

### Options

| Option | Description | Default |
|--------|-------------|---------|
| `--asset PATH` | Path to Lottie JSON file | (required) |
| `--size FLOAT` | Animation height as fraction of screen height | `0.333` |
| `--pos-x FLOAT` | Horizontal position (0=left, 1=right) | `1.0` |
| `--pos-y FLOAT` | Vertical position (0=top, 1=bottom) | `0.0` |
| `--offset-x INT` | Horizontal pixel offset | `0` |
| `--offset-y INT` | Vertical pixel offset | `0` |
| `--speed FLOAT` | Playback speed multiplier | `1.0` |
| `--opacity FLOAT` | Opacity (`0..1`) | `1.0` |
| `--rotate FLOAT` | Rotation in degrees | `0` |
| `--duration-ms INT` | Max playback duration in milliseconds (`0` means auto) | `0` |
| `--fps INT` | Render FPS cap (`0` means animation FPS) | `0` |
| `--gpu MODE` | Wayland GPU mode: `auto`, `on`, `off` | `auto` |
| `--backend NAME` | Force backend: `wayland` or `x11` | auto |
| `--output NAME` | Target output name (e.g. `eDP-1`) | all outputs |
| `--loop` | Loop animation | off |
| `--flip` | Horizontally flip animation | off |
| `--fade-in` | Fade in at playback start | off |
| `--fade-out` | Fade out at playback end | off |
| `--fade-in-ms INT` | Fade-in duration in milliseconds (also enables fade-in) | `800` |
| `--fade-out-ms INT` | Fade-out duration in milliseconds (also enables fade-out) | `800` |
| `-h, --help` | Show help | |

### GPU Modes (Wayland)

```bash
# Auto: try GPU first, fallback to CPU if unavailable
lottie-salute --asset salute.json --backend wayland --gpu auto

# Force GPU: fail if GPU path is not available
lottie-salute --asset salute.json --backend wayland --gpu on

# Force CPU path on Wayland
lottie-salute --asset salute.json --backend wayland --gpu off
```

### Examples

```bash
# Play animation at default position (top-right, 1/3 screen height)
lottie-salute --asset salute.json

# Bottom-left, half screen height, looping
lottie-salute --asset salute.json --size 0.5 --pos-x 0.0 --pos-y 1.0 --loop

# On a specific output, flipped
lottie-salute --asset salute.json --output HDMI-A-1 --flip

# Play at 1.5x speed for 3 seconds on Wayland, with 80% opacity
lottie-salute --asset salute.json --speed 1.5 --duration-ms 3000 --opacity 0.8 --backend wayland

# Enable both fade-in and fade-out
lottie-salute --asset salute.json --fade-in --fade-out

# Customize fade durations
lottie-salute --asset salute.json --fade-in --fade-in-ms 1500 --fade-out --fade-out-ms 1200 --duration-ms 5000

# Rotate 20 degrees
lottie-salute --asset salute.json --rotate 20
```

## Backend Selection

The backend is auto-detected at runtime:
1. If `WAYLAND_DISPLAY` is set → Wayland backend (layer-shell overlay)
2. Else if `DISPLAY` is set → X11 backend (notification-type window)

You can also force a backend with:

```bash
lottie-salute --asset animation.json --backend wayland
lottie-salute --asset animation.json --backend x11
```

## Compatibility Notes

- Wayland mode requires a compositor with `wlr-layer-shell` support.
- GPU acceleration mode (`--gpu`) is available on Wayland backend only.
- GNOME (Mutter) typically does not expose `wlr-layer-shell`, so Wayland backend may not work there.
- X11 backend requires a working X11 session and the listed X11 development/runtime libraries.
- At least one backend must be available at build time.

## Test Assets

- Input must be a Lottie/Bodymovin JSON file.
- If you do not have one, download any small Lottie JSON sample and point `--asset` to that file.

## Troubleshooting

1. No animation appears:
   Check backend detection with `--backend wayland` or `--backend x11` explicitly.
2. Fade-out does not look obvious:
   Set a fixed `--duration-ms` and a longer `--fade-out-ms` (for example `2000`).
3. Wrong monitor / output:
   Verify exact output name from your compositor/X11 tools, then pass it via `--output`.
4. Animation exits too early:
   Check if `--duration-ms` is too small or if you expected looping without `--loop`.

## License

MIT
