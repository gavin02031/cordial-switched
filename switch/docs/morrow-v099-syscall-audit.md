# Direct-syscall audit of libroblox.so (2026-11-19)

Scope: static analysis of the guest ELF (`libroblox.so`, 108,360,288 bytes,
sha256 2ce7db6d...) for raw Linux syscall sites that would bypass the import
resolver. Host-side analysis only; no guest code has executed on hardware.
This matters before any constructor/JNI execution path is unlocked.

## Method

- Scanned the executable PT_LOAD segment (0x61649d0 bytes) for the AArch64
  `svc` encoding (base 0xD4000001).
- 199 raw pattern hits: 130 `svc #0`, 69 with non-zero immediates.
- Confirmed real syscall stubs by requiring the wrapper shape
  `mov x8, #nr; svc #0` (literal-pool constants interleaved in `.text` alias
  the encoding, so the pattern alone is not proof of an instruction).

## Confirmed findings

- Exactly 7 confirmed syscall wrapper stubs, clustered at 0x267c9b8-0x267d0f0
  (~1.8 KB), using 3 Linux syscall numbers: futex (98) x2, getpid (172) x3,
  gettid (178) x2.
- Six end with `cmn w0, #4095`, the classic Linux errno-range check,
  confirming they are genuine syscall wrapper functions.
- Direct-call scan: zero BL/B instructions in the executable segment target
  any stub.
- Static-pointer scan: zero 8-byte words anywhere in the file equal any stub
  address. Because the ELF's bulk relocations are Android packed (APS2)
  RELATIVE relocs whose addend is the slot content itself, this scan also
  covers all relocation-derived absolute pointers. Result: no static table
  references the stubs.

## Horizon dispatch mechanics (source-grounded)

Sources examined (Atmosphere master @ 3e8915011e1de89f595462d929c1cc3294470896,
libnx master @ b156b4774fb2df7b93402169509266da7547fc4c, both 2026-11-19):

- https://github.com/switchbrew/libnx/blob/b156b4774fb2df7b93402169509266da7547fc4c/nx/source/kernel/svc.s
- https://github.com/Atmosphere-NX/Atmosphere/blob/3e8915011e1de89f595462d929c1cc3294470896/libraries/libmesosphere/source/arch/arm64/svc/kern_svc_handlers_asm.s
- https://github.com/Atmosphere-NX/Atmosphere/blob/3e8915011e1de89f595462d929c1cc3294470896/libraries/libmesosphere/source/arch/arm64/kern_exception_handlers.cpp
- https://switchbrew.org/wiki/SVC (unused-ID list)

These sources establish the behavior of the EXAMINED Atmosphere
implementation. They do not identify which kernel version runs on the
test console, and nothing here is hardware-observed behavior.

- The kernel dispatches on the svc INSTRUCTION IMMEDIATE, not x8
  (libnx svc.s emits `svc 0x1`, `svc 0x2`, ...; kern_svc_handlers_asm.s
  reads the id from `esr_el1 & 0xFF`). The guest's `svc #0` therefore
  reaches the kernel as SVC id 0 regardless of the Linux number in x8.
- Invalid-SVC behavior: an out-of-range or NULL-table SVC id is NOT an
  error return in the examined implementation. kern_svc_handlers_asm.s
  label 3 routes to `HandleException`; kern_exception_handlers.cpp maps
  EsrEc_Svc64 to `ExceptionType_InvalidSystemCall` and re-enters the
  process at its entrypoint for usermode exception delivery; with libnx's
  default exception handling this fatals the process ("software closed").
  With a debugger attached the flow differs. So under the examined
  implementation: if any of the 7 stubs executes, the process dies - it
  does not get an error value to survive.

## Implications

- Bearing on the current L3 diagnostic crash: the INTENDED L3-only path
  runs no guest code and all runtime SVCs in it are valid libnx calls, so
  this audit gives no candidate cause for that path. The build and input
  path Gavin actually executed are not yet evidenced, so a connection to
  the observed crash is not categorically excluded.
- For the execution phase: any reached raw-syscall stub is a fatal, not a
  degraded call. Reachability of the 7 sites is therefore load-bearing
  before constructor execution is unlocked. An
  ExceptionType_InvalidSystemCall crash during a constructor attempt would
  point directly at these stubs.
- Zero direct/static references is bounded negative evidence, not proof of
  unreachability: 32-bit relative jump tables or runtime-computed dispatch
  can still reach them. futex/getpid/gettid-only is also consistent with
  dead code from a statically linked archive (e.g. a bundled crash-handler
  or allocator library kept for its other symbols).

## Limits of this audit

- Stub confirmation recognizes only `mov x8, #nr; svc #0`. Wrappers loading
  the syscall number into x8 from a register or memory, or using other
  forms, are not detected.
- The 123 remaining `svc #0` pattern hits are presumed literal-pool data but
  are not individually disproven; the true raw-syscall count is >= 7.
- 3 hits carry small non-zero immediates (12, 13, 20) that would be valid
  Horizon ids. Near-certain data constants: an Android ELF contains no
  Horizon-targeted code. Flagged, not resolved.
- `brk`-encoding hits (161) not analyzed; Android uses `svc #0` for traps,
  so these are presumed data.
