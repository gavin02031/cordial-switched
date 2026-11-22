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
