# Cordial Switched: native Horizon experiment

This directory builds a **Nintendo Switch `.nro` preview**, separate from
Cordial's existing Linux binaries. Version 0.26 includes a 720p GPU-first
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
   Press **ZR** to run the opt-in **engine bootstrap preflight** against your
   real library, including copying/relocating it into non-executable scratch
   RAM. It audits missing imports without binding them and automatically
   saves `sdmc:/switch/cordial-switched/stage-XX.txt`. The file records
   inspected dependencies, relocation requirements, stage status, missing-import
   count and up to 24 missing-import examples. It never includes engine bytes
   or credentials, and does **not**
   call a Roblox function. Logs are numbered and previous reports are kept.
7. On the **Loader** page, click **R3** (right stick) to run the opt-in
   synthetic ARM64 ELF mapping check. It writes only an original 1 KiB
   fixture, reserves and maps a 4 KiB page, verifies a relative relocation,
   calls its two-instruction `movz w0,#42; ret` function and releases the page.
   It saves a numbered `map-XX.txt` log under `sdmc:/switch/cordial-switched/`
   and removes the temporary fixture. **It never reads or runs `libroblox.so`.**
   This v0.25 test passed synthetic host checks; real Switch Lite execution
   remains unverified until a device log is collected.
8. With your own Android ARM64 `libroblox.so` on the SD card, click **L3**
   (left stick) on Loader for a strict final-base relocation preflight.
   It reserves a real Horizon code address, stages supported relocations
   against that address, reports the first unresolved or unsupported import,
   then discards the NX staging bytes and releases the reservation.
   This new option saves `link-XX.txt` in the same folder. **It does not
   map the Roblox image as executable, call any engine initialiser, sign in,
   or join a game.** A real engine still needs Android TLS, JNI and graphics.
   Starting with v0.27, this same report checks whether a separate, aligned
   copy of the real library's PT_TLS initializer and a single-module DTV
   shadow can be prepared. These are diagnostic per-thread allocations only;
   the CPU thread pointer and Android thread callbacks are **not** installed.
9. **Plus** returns to the Homebrew Menu without a crash.

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

Version 0.10 adds actual implementations of four unversioned Android `liblog`
imports: `__android_log_write`, `__android_log_buf_write`,
`__android_log_print` and the fatal `__android_log_assert`. A strict resolver
maps **only** these names to real functions and passes synthetic linker tests.
The Imports view now counts these implemented candidates. **None have been
bound to or called by the actual Roblox engine**, and Android dependency
loading, versioned bionic imports, JNI and graphics remain unimplemented.
The log compatibility functions can be tested without a Switch or an APK:

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror -Iswitch/source \
  switch/tests/android_log_compat_test.c switch/source/android_log_compat.c \
  -o /tmp/cordial-android-log-test
/tmp/cordial-android-log-test
```

Version 0.11 adds a strict GNU symbol-version reader for Android's
`DT_VERSYM` and `DT_VERNEED` tables. It passes the **name, version and
provider library** to the resolver and rejects unmatched imports; it does
not misbind versioned libc routines to unrelated Switch/newlib functions.
The **ZR** action on the Loader profile now runs the existing scratch linker
against the user's actual `libroblox.so` and the four implemented `liblog`
adapters. This is the first **on-device relocation attempt with real input**,
not an engine boot: unsupported imports, graphics and JNI still stop the
linker, and no library memory becomes executable. In actual Switch Lite tests,
the first attempt failed to allocate scratch memory in Album mode; a later
attempt reached APS2 decoding but reported `Invalid APS2 offset delta`.
The parser now accepts signed APS2 offset deltas with bounds checks, matching
Android's linker format. This change passed synthetic host tests, but **has
not yet been verified against the user's library on Switch hardware**. Host
CI cannot access their SD card or attest their APK.
Version 0.13 adds an **opt-in, automatically saved engine-startup preflight
trace**. It flushes the inspection and dependency results before attempting
the potentially memory-intensive scratch link, so a failed stage leaves a
useful partial report. If staging completes, the test still records
`engine_executed=no`: bionic, JNI/GameActivity, EGL/GLES and APK signature
verification are not connected. This is a readiness test, **not** a playable
Roblox launch or a claim that Roblox's entry point was called. Hardware logs
were collected on a Switch Lite: `stderr@LIBC` was the first unresolved import.

Version 0.14 adds an **NX-only audit mode** that leaves unresolved relocation
slots zero and continues inspecting remaining relocations. Reports now
distinguish total unresolved lookups from the first 24 examples. Any unresolved
symbol still prevents a prepared/usable image; no Android code executes.

Version 0.15 adds real, provider- and version-checked native math bindings for
the plain AArch64 float/double signatures in Android `libm.so` (`LIBC` symbol
version), such as `sinf`, `cosf`, `pow`, and `log`. These are actual compiled
math routines, not address placeholders. Android's JNI, pthread runtime and
long-double routines remain intentionally unresolved, because a wrong ABI
binding could crash the console. The host suite validates a
versioned math binding without ever executing the Roblox library itself.

Version 0.16 adds provider/version-checked Bionic `libc.so` adapters using
the Android-to-Switch wrapper approach demonstrated by Max Payne NX, Adventures
of Mana NX and Angry Birds 2 NX. `stdin`, `stdout` and `stderr` now resolve
as Android-facing `FILE *` data symbols with opaque handles; stdio calls
through those handles are translated by dedicated adapters. The resolver also
exposes ABI-identical plain C routines and translates Android's numeric
`clock_gettime` IDs. Neither native `FILE` layouts nor pthread state are passed
to Android as if they were Bionic objects. The new host suite checks the
`stderr@LIBC` resolution that blocked the v0.13 hardware trace, but a fresh
device stage log is required to establish the next real blocker. This is a
partial compatibility layer: `__errno`, Bionic pthread/fortify functions,
JNI/GameActivity, graphics, assets, dependency loading and executable memory
mapping are **not** implemented. The library is still never executed, and the
Switch Lite's unsuccessful title takeover may also prevent full-memory tests.
See [Angry Birds 2 NX](https://github.com/NaGaa95/angrybirds2_nx) for the
stdio/clock reference and [Adventures of Mana NX](https://github.com/NaGaa95/aom_nx)
for a working native ARM64 executable loader architecture.

Version 0.17 adds Bionic FORTIFY checks for the AArch64 imports visible in
the user's `imports-04.txt`: checked memory and string operations, the
thread-local `__errno` accessor, and fatal `__assert`. Checked `fread` and
`fwrite` use Android's `LIBC_N` export version and still translate the opaque
Android FILE handles. Bad buffer sizes **abort instead of silently writing
past the caller's buffer**. These signatures and symbol versions were checked
against Android's official Bionic sources. The host suite tests ordinary
success cases, intentional fatal bounds failures in isolated child processes,
and wrong-provider rejection; real Switch Lite runtime behaviour
remains unverified. This still does not execute the Roblox engine.

Version 0.18 adds a **partial Bionic pthread object adapter**, following the
opaque-object technique used in native Android-to-Switch ports while using a
thread-safe registry and lifetime tracking. It translates Bionic's inline
mutex and condition storage into distinct native pthread objects rather than
casting between incompatible structs. Static normal, recursive and errorcheck
mutex initializers, default condition variables, `pthread_once`, mutex attrs,
and basic lock/wait/signal/broadcast/destroy calls have host concurrency
tests. Unsupported attributes, nonstandard object patterns and unresolved
pthread services (`pthread_create`, TLS keys, rwlocks, timed waits, etc.) still
**fail closed**. The wrapper does not establish that Roblox's particular
threads run on the Switch Lite. JNI/GameActivity, graphics, assets, Android
dependency loading and executable engine startup are still missing. A fresh
**ZR stage log** is needed to measure the remaining imports on your hardware.

Version 0.19 adds a provider/version-checked **Bionic pthread TLS key bridge**.
Android provides at least 128 user TLS keys while native Switch pthread key
resources may be scarcer. The bridge multiplexes 128 Bionic-compatible
32-bit key IDs onto one native pthread TLS key, isolates values by thread,
invalidates stale values on key deletion/recreation, and invokes destructors
on thread exit with up to four POSIX iterations. Host tests confirm all 128
keys, multiple simultaneous threads, repeated destructor callbacks, invalid
keys and provider-version rejection. This does **not** add the rest of Bionic
thread-local state, a native `pthread_create`/`pthread_self` bridge, or any
ability to execute Roblox: JNI, graphics and live engine loading remain
unimplemented. Real Switch behaviour needs another ZR stage trace.

Version 0.20 adds another provider-checked piece of Bionic pthread support:
the real **56-byte ARM64 `pthread_attr_t` layout**, with stack size, guard size,
detach-state and attribute lifetime handling. The attribute storage is
independent of libnx/newlib types, follows documented Bionic offsets and
minimum stack sizes, and is checked by host tests with canary bytes and
wrong-provider rejection. Unsupported custom stacks, scheduling and actual
thread creation remain deliberately unresolved; this adds ABI compatibility
to the loader preflight, **not Roblox execution**.

Version 0.21 adds an **experimental, bounded pthread lifecycle bridge** for
`pthread_create`, `pthread_self`, `pthread_join`, `pthread_detach` and
`pthread_equal`. Native thread handles are hidden behind stable Bionic-sized
tokens. It translates supported stack, guard and detach attributes, rejects
custom stacks and scheduling requests, and prevents unknown, stale and
double-consumed handles from reaching native pthread functions. Host tests
cover 64 worker joins, detached threads and invalid-handle behaviour. The
bridge **does not create Bionic TCB/TLS, implement the complete Android
runtime, or run Roblox**; its provider mappings are used only in the existing
non-executable loader audit until those blockers are solved. The next on-device
stage trace is needed to check real symbol coverage.

Version 0.22 adds read-only `PT_TLS` inspection (initializer size, per-thread
memory size and alignment) and a tested **ARM64 Bionic TCB shadow** with slots
`-1` through `7`. Each bridged thread now has a distinct diagnostic TCB
layout, separate from the host pthread identity. These bytes are **not**
installed into the CPU's thread-pointer register, and the ELF DTV, Bionic TLS
object, stack guard and real Bionic thread record remain uninitialized. This
is an architectural check and helps diagnose the next loader trace; it does
not let the engine execute or join Roblox games.

Version 0.23 adds a bounded **non-executable PT_TLS scratch-template copier**.
Loader → ZR now reports whether it copied the file-backed initializer and
zero-filled the remaining per-thread memory, respecting the ELF alignment and
rejecting templates over 1 MiB. Two independent copies are verified by host
tests, along with bad alignment, truncation and size limits. The copy is
discarded after the read-only diagnostic. It is **not** registered with a
dynamic thread vector or the CPU thread pointer: actual Bionic TLS, relocations
and Roblox startup remain blocked.

Version 0.24 improves the **non-executable AArch64 relocation audit**: it
recognizes Android's four DTPMOD, DTPREL, TPREL and TLSDESC relocation kinds,
validates their target ranges (including the TLSDESC two-word slot), and
records counts without modifying their target bytes or fabricating a Bionic
thread vector. The read-only scan now continues after unsupported relocations
so a later missing import or TLS blocker is not hidden by the first failure.
Loader → ZR exports `unsupported_relocations_seen`, `tls_relocations_seen`
and `tlsdesc_relocations_seen`; the result remains a failed preflight whenever
unsupported types occur. This **does not execute Roblox**.

The inspector is independently tested on the development host using synthetic
ELF images with no proprietary content:

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror -Iswitch/source \
  switch/tests/elf_probe_test.c switch/source/elf_probe.c switch/source/android_log_compat.c \
  -o /tmp/cordial-elf-probe-test
/tmp/cordial-elf-probe-test
```

### Segment staging experiments

Version 0.4 and later also compile `engine_stage.c` into the homebrew build. It can
construct a **bounded, non-executable scratch image** from an inspected ARM64
ELF: copy file-backed `PT_LOAD` bytes, zero-fill BSS, and apply the
`R_AARCH64_RELATIVE` RELA and ELF/Android RELR relative relocations. It checks
each relocation destination against a real loaded segment and fails closed on
unknown relocations. Version 0.8 adds synthetic APS2 packed-RELA support. This prepares data
only; it does not map executable memory, perform bionic symbol resolution,
verify an APK signature or start Roblox. **Before v0.11 the Switch UI did not
invoke the stager**; the validation used synthetic ELF fixtures, not an actual
Roblox engine or a Switch Lite hardware run.

Version 0.6 extends that scratch-only layer with an optional, strict
**symbol-resolver callback**. Synthetic tests demonstrate applying
`R_AARCH64_ABS64`, `R_AARCH64_GLOB_DAT`, and `R_AARCH64_JUMP_SLOT` from ordinary
RELA and PLT (`DT_JMPREL`) tables. It validates dynamic symbol/string names,
never fabricates a success address, and rejects unresolved, malformed,
or unsupported relocation formats. Versioned symbols require a matching
version and provider via the callback. The original no-resolver API
still fails closed on external imports. **This is a linker unit-test capability,
not a working Android bionic linker**: it lacks most actual Android symbol
providers, dependency loading, TLS and constructors. Version resolution was
added in v0.11; the Switch now passes its four liblog adapters to the bounded
scratch linker on demand, but never executes staged ELF code.

Version 0.8 adds a bounded parser for Android's APS2 signed-LEB128 grouped
RELA format, with tests for grouped and ungrouped addends and malformed inputs.
It applies supported relocations only to non-executable scratch RAM and
explicitly rejects unresolved imports, malformed data and unsupported REL
records. **This does not mean the user-supplied 2.17 MB relocation section
has been fully linked:** host tests use synthetic ELF images, and the actual
Switch log from v0.13 stopped at an unresolved Bionic `stderr` import.

The native CI runs these tests, which you can also execute locally:

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror -Iswitch/source \
  switch/tests/engine_stage_test.c switch/source/engine_stage.c switch/source/elf_probe.c \
  switch/source/engine_boot_log.c switch/source/android_log_compat.c \
  switch/source/android_math_compat.c -lm \
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

### Native ARM64 wrapper references (in progress; after v0.24)

The MIT-licensed [Adventures of Mana Switch wrapper](https://github.com/NaGaa95/aom_nx),
[Half-Life 2 Switch wrapper](https://github.com/NaGaa95/hl2_nx), and
[Pocket Camp Switch wrapper](https://github.com/NaGaa95/acpc_nx) demonstrate
real ARM64 Android-library execution on Horizon. Their working game-specific
adapters are **not** general-purpose Roblox or Android runtimes.

The next native-loader foundation uses the same separation of concerns as
Adventures of Mana's `so_finalize`: build a per-page RX/R/RW permission plan,
reject W+X page conflicts, map a separately prepared aligned source with
`svcMapProcessCodeMemory`, and set RX runs **before** writable data pages.
`engine_memory_plan.c` now builds and validates that bounded plan on the host;
`horizon_code_memory.c` contains the corresponding Horizon mapping and cleanup
primitive. The latter compiles into the Switch target but is **not invoked**
by the UI or linked to the Roblox ELF at this stage.

The loader now separates **address reservation** from mapping:
`horizon_code_memory_reserve` holds the final Horizon code address before
relocations, and `horizon_code_memory_map_reserved` can map the prepared
page-aligned heap image there later. `engine_stage_stream_resolved_at`
computes ordinary RELA, Android APS2 and RELR relative relocations for this
explicit final address, while its working buffer remains non-executable.
The inspector also retains validated PT_LOAD page flags, allowing
`engine_memory_plan_from_probe` to construct a permission plan for the actual
ELF. Host fixtures cover the final-base arithmetic. The new
`engine_runtime_image_prepare` path connects inspector → permission plan →
address reservation → final-base relocation → aligned source → code-memory
mapping. Its mapping backend is implemented for Horizon, but **the UI does not
invoke this path, and it has not been run against Roblox code or on Switch
hardware**. Host tests use a synthetic ELF and a fake code-memory backend;
they verify actual relocation bytes, reservation failures, map failures and
retryable cleanup. Successful mapping is explicitly not treated as execution.

The mapper now observes the ELF's `p_align` requirement (16 KiB in the
user's reported library) instead of assuming every page-aligned address can
be a valid load bias. An unsupported nonzero alignment phase fails closed.
The scratch stage uses page-aligned NX memory that the runtime mapping path
can take ownership of without copying the entire roughly 119 MiB image. This
reduces peak host allocation demand but has **not** established a successful
full-memory load on the user's Switch Lite.

The final-address relocation API is only one part of a live linker. All imports,
PT_TLS/DTV thread state, JNI/GameActivity, callbacks and ownership checks must
be established before any Roblox initializer can be called. Half-Life 2's
`dl_emu.c` illustrates module-scoped `dlopen`/`dlsym` and Mesa EGL/GLES symbol
lookup; Pocket Camp's `android_native_unity.c` demonstrates an `NWindow` and
per-thread `ALooper` adapter. These will require Roblox-specific ABI
validation rather than copying arbitrary stubs or hooks. This is runtime
groundwork, **not a runnable Roblox release**.
The first testable deliverable is deliberately labelled a smoke test so it
cannot be mistaken for a playable Roblox client.

**Morrow** is the proposed v1.0 rebrand, reserved for the point where real
account login and joining playable games have been demonstrated. Current
development builds remain **Cordial Switched**; 60 FPS gameplay and the v1.0
timeline are not guaranteed.
