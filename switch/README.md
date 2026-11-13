# Cordial Switched: native Horizon experiment

This directory builds a **Nintendo Switch `.nro` smoke test**, separate from
Cordial's existing Linux binaries. The first milestone is a real executable
which opens through the Homebrew Menu, renders text and reads handheld controls
and touchscreen input. **It does not currently run Roblox, connect to Roblox
servers or sign in to any Roblox account.**

## Build

The [Switch homebrew smoke build](../.github/workflows/switch-homebrew.yml)
GitHub Actions workflow uses the `devkitpro/devkita64` toolchain container.
Download the resulting `cordial-switched-nro-smoke-test` artifact from the
workflow run; its contents are `cordial-switched.nro`.

For a local build, install devkitPro with `devkitA64` and `libnx`, set
`DEVKITPRO`, and run:

```sh
make -C switch
```

Copy `switch/cordial-switched.nro` to `sdmc:/switch/cordial-switched.nro`,
launch it from the Homebrew Menu on a Switch with working homebrew support,
then check:

1. The first page is legible on the Switch Lite screen.
2. **A** and **B** change pages; directional left and right also work.
3. On page 3, tapping the screen changes the displayed coordinates.
4. **Plus** returns to the Homebrew Menu without a crash.

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
