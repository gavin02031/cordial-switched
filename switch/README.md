# Cordial Switched: native Horizon experiment

This directory builds a **Nintendo Switch `.nro` preview**, separate from
Cordial's existing Linux binaries. Version 0.2 includes a 720p GPU-first
dashboard built with SDL2/OpenGL ES, Nintendo's system font via `pl` and
SDL2_ttf, an Overview page, a live UI frame-rate monitor and a development
roadmap. It reads handheld controls and touchscreen input. **It does not
currently run Roblox, connect to Roblox servers or sign in to any Roblox
account.**

The renderer requests GPU acceleration and VSync, then falls back to an
accelerated renderer without VSync or a software renderer if necessary. Static
backgrounds and text are cached instead of rasterised every frame. The
performance page reports the actual renderer, the measured UI frame rate and
average frame time; 60 FPS is a **target**, not a measured hardware result or
a promise about eventual game performance. The current layout targets the
Switch Lite's 1280x720 handheld display.

## Build

The [Switch homebrew smoke build](../.github/workflows/switch-homebrew.yml)
GitHub Actions workflow uses the `devkitpro/devkita64` toolchain container.
Download the resulting `cordial-switched-nro-smoke-test` artifact from the
workflow run; its contents are `cordial-switched.nro`.

For a local build, install devkitPro with `devkitA64`, `libnx`,
`switch-sdl2`, and `switch-sdl2_ttf`, set `DEVKITPRO`, and run:

```sh
make -C switch
```

Copy `switch/cordial-switched.nro` to `sdmc:/switch/cordial-switched.nro`,
launch it from the Homebrew Menu on a Switch with working homebrew support,
then check:

1. The Overview page renders correctly, including fonts, colours and cards.
2. **A/B**, **left/right** or **L/R** switch Overview, Performance and Roadmap.
3. Tapping navigation items switches pages; on Performance, **X** or the toggle
   turns the ambient movement on and off.
4. Check the Performance page after a few seconds. Report the renderer type,
   average UI frame rate and any stutters; do not treat it as Roblox FPS.
5. **Plus** returns to the Homebrew Menu without a crash.

Do not enter Roblox credentials into this build. It has no login functionality.

## What needs porting next

Cordial's aarch64 support currently targets **Linux**, not Horizon OS. The
Android ARM64 engine matches the CPU family, but the existing runtime relies
on glibc, POSIX memory and thread primitives, Linux file descriptors and
shared-library loading, Wayland/windowing, and desktop EGL/Vulkan interfaces.
Horizon does not provide those Linux services just because it uses ARM64.

The actual engine port must establish, in order:

1. A working Horizon platform abstraction for memory, threads, TLS and files.
2. Loading and relocating an **unmodified, user-provided** Android ARM64 ELF
   library with bionic-compatible symbols. `native/` and
   `third_party/mcpelauncher-linker/` are reference implementations, not yet
   compiled into this `.nro`.
3. Working JNI and GameActivity services without Android itself.
4. A graphics path for the engine's GLES/EGL or Vulkan calls, plus audio,
   inputs and networking.
5. An observed engine startup on hardware, then an official account login and
   live Roblox experience **if** Roblox accepts the runtime. Account integrity
   and access policy remain external constraints, not something this repo can
   guarantee or bypass.

This sequence is **INFERRED**, not a demonstrated Switch engine boot. No APK,
engine binaries, account tokens, or proprietary Roblox files belong in Git.
The first testable deliverable is deliberately labelled a smoke test so it
cannot be mistaken for a playable Roblox client.
