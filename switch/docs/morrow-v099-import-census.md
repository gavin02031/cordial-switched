# Guest import call census (v0.99 line)

Where libroblox.so actually spends its imported calls, so compat audit
effort goes to what the guest really executes. Method: disassemble every
executable section, collect `bl` targets into PLT stubs, map stubs to
`.rela.plt` JUMP_SLOT symbol names. Scope: ordinary decodable-stream
execution. Calls made indirectly through GOT-loaded function pointers or
in corrupted control flow are not counted; the mmap/mprotect/mprotect
family census is in morrow-v099-exec-memory.md.

Totals: 16,749 resolved call sites over 212 distinct imported symbols
(out of 549 JUMP_SLOTs). The top of the list is stack-check and string
plumbing; the compat-relevant findings are below.

## dlopen / dlsym (6 dlopen sites, 48 dlsym sites)

| Site | Library | Flags | Guest handling of NULL | Shim today |
| --- | --- | --- | --- | --- |
| 0x2031590 | libcamera2ndk.so | RTLD_LAZY | (probe context) | honest NULL + dlerror |
| 0x20315a8 | libmediandk.so | RTLD_LAZY | (probe context) | honest NULL + dlerror |
| 0x2171628 | libvulkan.so.1 | RTLD_NOW | falls through to next | honest NULL |
| 0x217163c | libvulkan.so | RTLD_NOW | branch to 0x21716dc fallback | honest NULL |
| 0x1dd11d0 | libc.so, then dlsym("getauxval") | RTLD_LAZY | cbz fallback at 0x1dd1214 | honest NULL: libc.so is not a registered module |
| 0x219a90c | libandroid.so, then dlsym("AThermal_acquireManager") | (checked, fallback at 0x219a9d4) | cbz fallback | module registered; AThermal_* not in the libandroid resolver set (looper/window/asset/configuration only), so the dlsym fails honestly |

The Vulkan probe pair with its verified fallback means the guest's
graphics path settles on EGL/GLES — the registered, backend-gated
modules — rather than dying on Vulkan's absence. The libc.so probe is a
candidate improvement: getauxval is already supported as a linked import
(9 call sites), but runtime `dlopen("libc.so")` is refused because libc
is not in the dl module table; serving it through the existing import
resolver would let such probes succeed. Not done: changes which library
identity the guest sees, needs its own care.

## Raw syscall() (7 call sites, dispatcher is a blanket ENOSYS)

Two sites statically decodable:

- 0x23ffbdc: `syscall(284)` = mlock2(addr, len, MLOCK_ONFAULT) in the
  data allocator. The guest compares errno against ENOSYS (0x26)
  explicitly and degrades gracefully; the shim's blanket ENOSYS is
  exactly the answer this probe is built for.
- 0x24481b4: `syscall(278)` = getrandom(buf, len, 0), reached only when
  the preceding getentropy call fails with ENOSYS. getentropy is
  genuinely implemented (libnx OS-seeded ChaCha,
  android_libc_compat.c), so this fallback does not run.

Five sites pass the number in registers from their callers and are not
statically decodable without dataflow work: 0x1e173cc, 0x1e553c0,
0x1e553f8, 0x1f4f758, 0x23ffbdc (see also mmap/madvise clusters). Any
raw syscall gets ENOSYS; a guest path that requires one to succeed would
show up as hardware evidence, not something to pre-guess.

## Already covered

- mmap/mprotect/munmap/mremap/msync/madvise call-site census and the
  exec-memory design: switch/docs/morrow-v099-exec-memory.md.
- Raw-svc (in-guest SVC instruction) census: morrow-v099-syscall-audit.md.

## GLES import catalogue coverage (addendum, this change)

The guest binary contains 191 distinct egl*/gl* strings. The import
catalogue in android_egl_gles_compat.c previously covered 91 (EGL core +
ES 2.0 core). This change adds 40: 39 ES 3.0 core entry points plus
glCopyImageSubData (ES 3.2 core), bringing the catalogue to 131.

Verification basis for every added name, no guessing:

- Export: src/mapi/es2api/gles2-symbols.txt in the Mesa source tree is
  the ABI-check list libGLESv2 is tested against at build time
  (src/mapi/es2api/meson.build, 'es2-ABI-check' target). The file is
  byte-identical between mesa-20.1.0-rc3 (the exact base devkitPro's
  switch-mesa 20.1.0-5 PKGBUILD builds) and the mesa-20.1.0 release;
  both tarballs fetched from archive.mesa3d.org and diffed.
- Declaration: each added name is declared in Mesa 20.1's own installed
  headers (GLES3/gl3.h for the 39, GLES3/gl32.h for glCopyImageSubData).
  The Switch-only resolution branch of android_egl_gles_compat.c was
  type-checked on host against those headers; address-of references for
  all 40 resolve. Header presence is re-gated at compile time with
  __has_include so a toolchain without them degrades to an honest NULL
  instead of a guessed prototype.

Remaining 60 uncovered strings, deliberately NOT catalogued:

- EXT/OES/KHR-suffixed variants (glBlitFramebufferEXT,
  glBindVertexArrayOES, glObjectLabelKHR, ...). Mesa's eglGetProcAddress
  may still answer some of these at runtime from the shared glapi
  dispatch; the guest's standard extension probing tolerates NULL.
  INFERRED: most are probe strings for optional features, not hard
  requirements. Not verified against the guest's call paths one by one.
- Core names Mesa 20.1's libGLESv2 does not export: glMapBuffer,
  glGetQueryObjectiv, glGetQueryObjectui64v, glBufferStorage. A future
  compat shim could serve glMapBuffer via glMapBufferRange (guest
  tolerance unverified), the query-object getters via the uiv variant,
  and glBufferStorage via glBufferData (different semantics, needs
  care). None of that is done here; the guest probe sees an honest NULL.

UNVERIFIED: whether devkitPro's nouveau-backed Mesa exposes an ES 3.x
context at runtime on hardware at all. If it exposes ES 2.0 only, the
added symbols resolve but a 3.x context creation fails honestly and the
guest falls back per its own renderer probing. Hardware evidence needed
either way; no playability claim follows from this change.

## Guest EGL usage, statically decoded (addendum, this change)

The guest's EGL entry path was traced directly (single call site each):

- eglInitialize: one site, 0x2e2600c.
- eglChooseConfig: one site, 0x2e274a8. The attrib list is stack-built in
  two variants selected by a flag: {runtime pair, MIN_SWAP_INTERVAL,
  RED_SIZE, GREEN_SIZE, BLUE_SIZE, EGL_NONE} and the same plus
  {STENCIL_SIZE=8} before the terminator. Red/green/blue depths and the
  swap interval are runtime arguments; no SAMPLES/MSAA request appears in
  the statically visible portion. Modest config; nothing nouveau cannot
  advertise. INFERRED: RGB depths are 8/8/8 from the engine's defaults;
  the first runtime pair could not be decoded statically.
- eglCreateContext: one site, 0x2e2614c. Attrib list is rodata at
  0x7ed1bc and reads exactly {EGL_CONTEXT_CLIENT_VERSION=2, EGL_NONE}.
  VERIFIED: the guest requests an ES 2.0 context. This scopes the whole
  renderer question: the ES2 core catalogue is the live path; the ES3
  core additions from the previous change answer probes but an ES2
  context cannot use ES3-only calls, and the EXT/OES variant strings
  (glMapBufferOES, glBindVertexArrayOES, EXT occlusion query, KHR_debug
  markers) are the real fidelity surface on the ES2 path.
- eglGetProcAddress: 72 call sites. Probing is the guest's dominant GL
  resolution mechanism, so probe answers matter more than the static
  catalogue size.

Policy change in this commit: our eglGetProcAddress shim previously
returned NULL for every name outside the explicit catalogue. It now
forwards uncatalogued gl* names to the real driver eglGetProcAddress and
returns the driver's answer (real entry point or NULL), which is the
same answer a real Android EGL gives. EGL names stay catalogue-only so
the checked window-surface and error wrappers cannot be probed around.
GL entry points carry no EGL handles; ABI is the driver's own dispatch
type. CAVEAT (INFERRED): Mesa can return non-NULL for entry points whose
extension the context does not advertise; Android engines are built for
exactly that semantic (they gate on the GL_EXTENSIONS string), so this
matches the platform the guest was written for. Any residual misuse is
hardware-evidence territory, not something to pre-guess.

## libc.so registered for dlopen/dlsym (addendum, this change)

The dlopen table now registers libc.so alongside the existing native
adapters. dlsym against its handle routes to android_libc_compat_resolve
with the same ("LIBC", "libc.so") identity the static import resolver
already uses, so runtime lookups and link-time imports serve identical
symbols; unknown names still get an honest NULL plus dlerror, and
unregistered libraries (libssl.so et al.) stay refused. Verified guest
path motivating this: dlopen("libc.so", RTLD_LAZY) then
dlsym("getauxval") at 0x1dd11d0 with a cbz fallback at 0x1dd1214 that
honestly tolerates either answer; getauxval itself is the existing
implementation (AT_PAGESZ, AT_RANDOM, else ENOENT). Host test covers
open/lookup/miss/close and CI compiles the added translation units.

## Raw syscall dispatcher: gettid + getrandom routed (addendum, this change)

The four raw-syscall sites the census left undecoded all pass the number
as an immediate in w0 right before the call:

- 0x1e173cc and 0x1f4f758: syscall(178) = gettid; result stored into a
  per-structure slot without an error check.
- 0x1e553c0 and 0x1e553f8: syscall(278) = getrandom(buf, len<=0x100, 0)
  in a chunked fill loop (function at 0x1e55388) with EINTR/EAGAIN retry;
  any other failure returns -1 to its caller. Call graph, verified: the
  init function probes the loop once; on failure it opens "/dev/random"
  (string at 0x5829ad, adjacent "ossl_rand_pool" — this is OpenSSL's
  rand_unix entropy path) and falls back to fd reads; the fill entry
  selects getrandom vs the fd by a flag the probe sets. Our open shim has
  no /dev/random, so without routing, entropy init fails outright, and
  OpenSSL treats seed failure as fatal for TLS use.

guest_syscall now routes 178 to pthread_self (unique per live thread on
newlib and glibc) and 278 to android_getrandom, a new exported helper
over the same real OS-seeded source getentropy already uses, with
GRND_NONBLOCK/GRND_RANDOM validated and unknown flags EINVAL. All other
numbers keep the blanket ENOSYS. Host tests cover routing, EFAULT/EINVAL,
per-thread tid stability, and the unchanged ENOSYS default; CI compile
lines gained android_libc_compat.c where android_system_compat.c is
linked without it.

## Load-time dependencies and the audio path (addendum, this change)

DT_NEEDED, read from the guest's dynamic section: libandroid.so,
liblog.so, libOpenMAXAL.so, libmediandk.so, libGLESv2.so, libEGL.so,
libOpenSLES.so, libdl.so, libm.so, libc.so. Of the 582 undefined dynamic
symbols, zero belong to libOpenSLES or libOpenMAXAL: those NEEDED entries
carry no directly referenced imports, so they need no symbol answers at
load time. libGLESv2 (74) and libEGL (17) imports are catalogue-served,
libandroid (39) and libmediandk (31) have their own resolvers.

libmediandk.so is now registered in the dlopen table (this change):
Android semantics for a NEEDED library make dlopen succeed for the
already-loaded module, and dlsym routes to the same media resolvers the
static imports use. The verified guest probe at 0x20315a8 now gets the
semantically correct answer instead of a refusal.

Audio, traced: FMOD is the output stack (fmod_output_opensl.cpp strings,
SL_IID_* and AAudioStreamBuilder_* symbol strings). It dlopens
libaaudio.so (site 0x28bf2f4) and libOpenSLES.so through a helper
(0x28be3a0: dlopen(name, RTLD_NOW), error 0x12 on NULL) and resolves
through another (0x28be3e8: dlsym, error 0xd on NULL). Both propagate
honest errors with no abort; FMOD's driver enumeration then tries the
next output. With libaaudio/libOpenSLES unregistered, audio outputs fail
cleanly and the guest continues without sound. INFERRED: engine-level
tolerance of FMOD output-init failure; not crash-relevant at the FMOD
layer. A real audren-backed output is a separate feature, not a fix.

## JNI class surface census (2026-11-22)

141 JNI class-path strings were recovered from the guest binary; 139 have
code cross-references (batched adrp+add xref scan). The JNI runtime models
10 classes. The unmodeled remainder is mostly app-shell surface (ads,
purchase, bugreporter, age-signals, calling, connectivity proxies,
androidx); engine-core entries (GameActivity, NativeGLJavaInterface,
MotionEvent, KeyEvent) are already modeled.

Unmodeled classes do not crash the pinned libjnivm VM. Source-grounded in
the pinned commit (third_party/libjnivm): FindClass auto-creates an empty
Class and returns non-null; GetMethodID (the runtime constructs the VM
with the default non-null mode) auto-creates a placeholder Method and
returns non-null; calling a placeholder returns a zero/false/null default
for the return type. What the guest does with those default returns is
unknown until observed on hardware; this census is not evidence the guest
tolerates them.

### Unknown-surface observer (compiled diagnostic, not yet observed on console)

morrow_jni_unknown_observer.cpp wraps FindClass / GetMethodID /
GetStaticMethodID through the VM's AddHook mechanism and appends the first
resolution of each unmodeled class or unhooked method to
jni-unknown-surface.txt in the report directory. Bounded by design: each
distinct key is logged once, capped at 192 entries (overflow counted, not
written); entries name classes and JNI signatures only, never call
arguments. The file is expected to appear on the next console run and is
the intended input for deciding which classes deserve real models.

Host-verified in the provider test (morrow_jni_runtime_test.cpp): the
observer logs each distinct unmodeled class/method once, stops writing at
the 192-entry cap, and a placeholder static call returns the zero default
without crashing. Host result, not a console observation.

## Input entry points (2026-11-22)

The guest exports 34 Java_* input natives as dynamic symbols; binding is
Android's automatic native resolution, so the runtime can resolve them
through the loader exactly like JNI_OnLoad. Verified shapes:
nativePassInput(env, cls, int, int, float, float) @0x275949c;
nativePassKeyEvent(env, cls, int, int, int, int) @0x2755560 on
NativeGLInterface; nativeGamepadButtonEvent(env, cls, int, int, int)
@0x2759c70; nativeGamepadAxisEvent(env, cls, int, int, float, float,
float) @0x2759cfc; nativeGamepadConnectEventWithGamepadType @0x27599f0;
nativeSetGamepadSupportedKeyWithGamepadType @0x2759b30. None of them read
the JNIEnv or jclass argument. Each posts to a lazily-initialized,
acquire-guarded event-stream singleton in guest .bss; cross-thread posting
is a source-supported interpretation, not yet a proven synchronization
contract.

The guest translates the public Android code domain itself: its key mapper
(0x2759248) has one branch per honored keycode (4, 19-23, 96, 97, 99,
100, 102-109 honored; 98 and 101 return unmapped) and its axis mapper
(0x27593f0) honors axes 0, 1, 11, 14 plus hat/trigger axes 15-18, 22, 23
(the hat axes are split into dpad events by the caller at 0x2759d2c).
morrow_input_map.c maps the abstract pad to exactly this honored set and
its host test asserts no emittable code falls outside it. Digital triggers
use the L2/R2 keys, which reach the same guest codes as the trigger axes.
Unresolved: the float payload layout of axis events, per-argument
semantics of the int fields, and the post path's synchronization; those
stay inactive until pinned from the consumer side.

### Input post-path lifetime contract (2026-11-22, second pass)

Verified against the axis post (0x56036e8) and button post (0x5603288),
which share one structure: stream delegate at [stream+0x1228/+0x1220],
null-context and null-delegate fallbacks (0x560e57c / 0x560dac0), global
context accessor 0x1f53938, atomic refcount increment 0x2747d40
(ldaddal / ldaxr-stlxr) on the delegate while the event is built and
appended. nativeReleaseAllRbxEventStreams (@0x275b318 -> 0x27a0364) is
refcount-symmetric: atomic decrement, virtual destroy only at zero, so a
post racing teardown holds its reference and cannot use-after-free.

The ring-buffer append store-advances the write pointer without a lock
(verified in both post families): the streams are single-producer by
design, matching Android, where one UI thread calls these natives. No
post-path function reads thread-local storage (scanned entries, posts,
record constructor/copy, context once-init, fallback prologue), so the
producer thread needs no guest TLS setup.

Consequences for wiring: post all guest input events from one dedicated
Morrow input thread; start posting only after the readiness signal (the
GameActivity handshake flag is the best anchored signal we have, but it
proves that handshake, not necessarily consumer readiness); stop posting
and join the input thread when the engine stops.

Explicit limits, kept unproven by design (2026-11-22, second-pass
correction): the atomic load-then-incref in the post is the guest's own
synchronization, but the guest side of that acquisition window is
UNPROVEN. Not calling the exported release function does not establish
that the guest never replaces or releases delegates internally; the
delegate-writer scan produced no verified non-Morrow writer. The
stop-and-join ordering above therefore protects Morrow-initiated
teardown only; it does not exclude every post-vs-teardown race
structurally. Refcount symmetry in nativeReleaseAllRbxEventStreams makes
OUR teardown path safe; it is not evidence about internal guest timing.
Wiring that treats these limits as proven is a bug.

### Wiring module: gated and inactive (2026-11-22)

`switch/source/morrow_input_pump.{h,c}` owns gating and argument
validation for the pinned connect/disconnect/supported-key/button posts.
It resolves no guest symbols; entries arrive as function pointers at
activation time. Every emit refuses unless the pump is LIVE (entries
bound AND the readiness signal observed), the entry is bound, the
control maps to a guest-honored code, and the gamepad type enum is
inside the guest's 1..3 clamp. Stop is terminal. The header carries the
lifetime limits verbatim (unproven acquisition window, handshake is not
consumer readiness, single producer) so they cannot silently drift from
the census. Axis events stay INACTIVE: the float payload layout of
nativeGamepadAxisEvent is unresolved. Button action encoding follows
Android (0 = down, 1 = up), INFERRED until consumer-verified, and the
pump treats it as non-executable: button emits additionally require the
explicit encoding-resolved note, so LIVE + bound entries alone never
deliver inferred semantics. Activation of any path requires resolved
semantics plus the readiness/lifetime conditions; readiness and bind
flags alone are not those proofs.
`switch/tests/morrow_input_pump_test.c` host-tests the gates, including
the button refusal before encoding resolution.
