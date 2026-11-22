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
