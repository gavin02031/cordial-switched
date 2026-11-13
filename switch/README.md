# Cordial Switched: native Horizon experiment

This directory builds a **Nintendo Switch `.nro` preview**, separate from
Cordial's existing Linux binaries. Version 0.9 includes a 720p GPU-first
dashboard built with SDL2/OpenGL ES, Nintendo's system font via `pl` and
SDL2_ttf, an Overview page, a live UI frame-rate monitor and a development
roadmap, an ARM64 ELF inspection screen, and a read-only Loader readiness page.
It reads handheld controls and
touchscreen input. **It does not
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

The [Cordial Switched Horizon preview](../.github/workflows/switch-homebrew.yml)
GitHub Actions workflow uses the `devkitpro/devkita64` toolchain container.
Download the resulting `cordial-switched-switch-preview` artifact from the
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
2. **A/B**, **left/right** or **L/R** switch Overview, Performance, Roadmap,
   Engine, and Loader.
3. Tapping navigation items switches pages; on Performance, **X** or the toggle
   turns the ambient movement on and off.
4. Check the Performance page after a few seconds. Report the renderer type,
   average UI frame rate and any stutters; do not treat it as Roblox FPS.
5. On the Engine tab, press **X** (or tap **X: TEST JIT**) to try the optional
   isolated executable-memory check described below. It does **not** require
   a Roblox library, and it does **not** launch Roblox.
6. Open the **Loader** page after adding your library. It reports segment
   permissions, memory span, relative/PLT relocation counts, Android packed
   relocation bytes and symbol version tags. Press **X** to cycle between
   the profile, the dependency list and imported PLT symbol names. **Y**
   rescans on either Engine or Loader; it is metadata inspection only.
   Press **ZL** on Loader to save a numbered `imports-XX.txt` report in
   `sdmc:/switch/cordial-switched/`, with names and symbol version indices
   but no APK bytes or credentials. Old reports are never overwritten.
7. **Plus** returns to the Homebrew Menu without a crash.

### Engine inspection (optional)

The Engine tab looks for the library at:

```text
sdmc:/switch/cordial-switched/libroblox.so
```

If you have a **legitimate official Android Roblox APK** available, you may
extract your own `lib/arm64-v8a/libroblox.so` from it and place that file in
the location above on the SD card. Press **Y** on the Engine page to rescan.
Do **not** upload the APK, extracted library or Roblox account credentials to
GitHub or send them to us.

The scanner recognises ELF64/AArch64 shared objects, checks the bounds and
alignment of loadable segments, and lists a few `DT_NEEDED` Android libraries.
It reads only a small amount of metadata and does not map executable pages,
relocate code, call into the engine or verify the APK's signing certificate.
**A structurally valid ELF is not necessarily a genuine or safe Roblox build.**
We will need signed-package verification before any future execution feature.

### Actual Switch Lite observations (November 13, 2026)

In the user's photograph of a v0.6 run on their Switch Lite, the on-screen
results showed **JIT VERIFIED / ARM64 test returned 42**, and an
**ELF STRUCTURE OK** result for a user-supplied 103.3 MiB file, with
3 `PT_LOAD` segments, maximum alignment 16,384 bytes and 10 `DT_NEEDED`
libraries (including `libandroid.so`, `libmediandk.so`, `liblog.so`,
`libGLESv2.so` and `libOpenMAXAL.so`). These are **user-reported hardware
observations**; they establish neither APK authenticity nor engine startup.

In subsequent v0.7 Switch Lite photographs, the read-only Loader reported a
119.1 MiB virtual image span, 1 executable and 2 writable segments, 549 PLT
relocations, 2,173,222 bytes of Android packed relocations, and versioned
symbol metadata. The ten imported libraries were `libandroid.so`,
`liblog.so`, `libOpenMAXAL.so`, `libmediandk.so`, `libGLESv2.so`, `libEGL.so`,
`libOpenSLES.so`, `libdl.so`, `libm.so`, and `libc.so`. These are readouts of a
user-supplied file; its authenticity has not been verified.

The v0.7 **Loader** tab reads further dynamic metadata with bounded small
reads; it does not load the file into RAM, resolve symbols, map executable
pages or call any library function. This distinguishes the actual dependency
and relocation requirements from what our current synthetic tests cover.

Version 0.9 can also inspect undefined PLT symbol names and version indices,
then export a text manifest on an explicit **ZL** press. This is a linker
**preflight**, not an engine launch. Android library providers, bionic symbol
resolution and JNI/GameActivity are still not connected to the Switch `.nro`.

The inspector is independently tested on the development host using synthetic
ELF images with no proprietary content:

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror -Iswitch/source \
  switch/tests/elf_probe_test.c switch/source/elf_probe.c -o /tmp/cordial-elf-probe-test
/tmp/cordial-elf-probe-test
```

### Segment staging experiments (host tests only)

Version 0.4 and later also compile `engine_stage.c` into the homebrew build. It can
construct a **bounded, non-executable scratch image** from an inspected ARM64
ELF: copy file-backed `PT_LOAD` bytes, zero-fill BSS, and apply the
`R_AARCH64_RELATIVE` RELA and ELF/Android RELR relative relocations. It checks
each relocation destination against a real loaded segment and fails closed on
unknown relocations. Version 0.8 adds synthetic APS2 packed-RELA support. This prepares data
only; it does not map executable memory, perform bionic symbol resolution,
verify an APK signature or start Roblox. **The Switch UI does not invoke the
stager yet**; the validation is against synthetic ELF fixtures, not an actual
Roblox engine or a Switch Lite hardware run.

Version 0.6 extends that scratch-only layer with an optional, strict
**symbol-resolver callback**. Synthetic tests demonstrate applying
`R_AARCH64_ABS64`, `R_AARCH64_GLOB_DAT`, and `R_AARCH64_JUMP_SLOT` from ordinary
RELA and PLT (`DT_JMPREL`) tables. It validates dynamic symbol/string names,
never fabricates a success address, and rejects unresolved, malformed,
versioned, or unsupported relocation formats. The original no-resolver API
still fails closed on external imports. **This is a linker unit-test capability,
not a working Android bionic linker**: it has no actual Android symbol
providers, dependency loader, symbol version lookup, TLS or constructor support.
No caller on Switch currently supplies a resolver or executes staged ELF code.

Version 0.8 adds a bounded parser for Android's APS2 signed-LEB128 grouped
RELA format, with tests for grouped and ungrouped addends and malformed inputs.
It applies supported relocations only to non-executable scratch RAM and
explicitly rejects unresolved imports, malformed data, unsupported REL
records and symbol version lookups. **This does not mean the user-supplied
2.17 MB relocation section has been successfully decoded or linked:** testing
uses synthetic ELF images, and the Switch UI does not invoke the stager.

The native CI runs these tests, which you can also execute locally:

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror -Iswitch/source \
  switch/tests/engine_stage_test.c switch/source/engine_stage.c switch/source/elf_probe.c \
  -o /tmp/cordial-engine-stage-test
/tmp/cordial-engine-stage-test
```

### Executable-memory capability check (v0.5)

Version 0.5 adds an **opt-in hardware JIT check** on the Engine page, using
libnx's `jitCreate`, `jitTransitionToExecutable`, and `jitClose` API. After
pressing **X**, it allocates a single 4 KiB JIT page, copies **only two
hard-coded instructions** (`movz w0, #42; ret`) into the writable alias,
flushes the data cache, transitions to RX, invalidates the instruction cache,
calls this small function, checks that it returns 42, and releases the page.
The result is shown on screen, including the libnx error code on failure.
The check is never triggered automatically at launch.

The memory capability can legitimately be unavailable depending on Horizon
homebrew permissions, launch mode, firmware or system configuration. **A
successful compilation on GitHub Actions is not a successful hardware test.**
The user has observed `JIT execution: VERIFIED` and `ARM64 test returned 42`
on their own Lite, but that proves only the tiny native test, not the
Android/Roblox engine.

This probe **never loads or executes `libroblox.so` or any external code**.
The staged ELF image remains non-executable and disconnected from the probe.
Running the full engine will additionally require executable pages with the
correct memory permissions, relocations against the final code addresses,
Android-compatible dynamic linking, JNI, graphics, services and more.

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
   library with bionic-compatible symbols. `elf_probe.c` inspects its ELF
   metadata; `engine_stage.c` prepares a limited set of relocations and
   callback-resolved symbols in scratch memory under synthetic tests. `native/` and
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

**Morrow** is the proposed v1.0 rebrand, reserved for the point where real
account login and joining playable games have been demonstrated. Current
development builds remain **Cordial Switched**; 60 FPS gameplay and the v1.0
timeline are not guaranteed.
