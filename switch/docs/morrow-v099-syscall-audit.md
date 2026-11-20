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
  address. SUPERSEDED - see Revision 1 below: the APS2 addend mechanism
  assumed here was wrong, and the corrected relocation-derived scan finds a
  static table that does reference one stub.

## Revision 1 (2026-11-19): packed-reloc correction and new confirmed sites

The original pointer-coverage claim was based on a wrong assumption about
Android packed relocations (APS2). Verified against the bionic iterator
source (android.googlesource.com/platform/bionic linker_reloc_iterators.h,
oreo-r4) and the ELF itself: APS2 RELATIVE addends are STREAM-ENCODED
accumulated sleb128 deltas (header: relocation count + initial offset, then
groups), not slot contents. Proof in this binary: all 2505 .init_array
slots are zero in the file, yet decoding the APS2 stream (588,786 relocs,
38,390 groups, consumes the section exactly) resolves every slot to a code
address. The whole-file 8-byte word scan therefore never covered
relocation-derived pointers.

Corrected scan over all decoded APS2 RELATIVE addends (init_array resolved
as a byproduct: 2,505 entries, ctor[3] = 0x1b2fe48, which calls directly
into the 0x26dc68c init complex containing the mmap/spin sites). The
decoder is switch/tools/aps2_relocs.py with synthetic-stream unit tests in
switch/tools/test_aps2_relocs.py; against the real binary it decodes
588,786 relocations in 38,390 groups with byte-exact stream consumption
and resolves all 2,505 init_array slots, so the correction is
reproducible:

- slot 0x61dec08 (.data.rel.ro function-pointer table) -> 0x267d0c0, the
  getpid wrapper stub (one of the 7). The "no static table references the
  stubs" result is FALSIFIED for that stub; the other six wrapper entries
  have no relocation-derived pointer (widened range 0x267c800-0x267d100
  checked). Direct-call (BL/B) result is unchanged: zero.
- Nine additional CONFIRMED raw-syscall sites at 0x5a7aa34-0x5a7d060:
  inline `svc #0` with register-computed syscall numbers (sxtw x8, w8) and
  inline error handling, inside ordinary-looking functions - exactly the
  wrapper form the original audit's limits section said it could not
  detect. These are not dead leaf stubs.
- Five more table slots (0x66226b0-0x66227c0) point at function entries
  0x5a7a0e8-0x5a7a744 in that region. ~~Only 0x5a7a744's function is
  confirmed to contain inline svc sites (0x5a7aa34, 0x5a7aaa4)~~ -
  WITHDRAWN in Revision 2: that attribution was wrong. None of the five
  referenced function bodies contains an inline svc site.
- Total confirmed raw-svc sites: 16 (7 wrappers + 9 inline). The true
  total remains open (see Revision 2 for the sharpened census).

## Revision 2 (2026-11-20): svc-site attribution correction and census

Revision 1 attributed the inline svc sites at 0x5a7aa34 and 0x5a7aaa4 to
the table-referenced function at 0x5a7a744. Direct disassembly shows that
function is a 0x3c-byte wrapper (stp prologue, one BL to 0x1db7d3c, ret at
0x5a7a780) containing no svc. The attribution is withdrawn.

Control-flow check of all five table-referenced functions (0x5a7a0e8,
0x5a7a100, 0x5a7a168, 0x5a7a590, 0x5a7a744): recursive descent from each
entry, following direct b/b.cond/cbz/cbnz/tbz/tbnz branches and call
fallthrough, finds NO inline svc site in any of the five bodies. The only
indirect control transfers are three `blr x8` C++ virtual calls in
0x5a7a168 (vtable load: ldr x8,[obj]; ldr x8,[x8,#0x10]; blr x8), whose
callees are runtime data and statically unbounded; no direct branch from
any of the five reaches the svc region.

The nine inline svc sites (0x5a7aa34-0x5a7d060) instead belong to larger
functions starting at or after 0x5a7a7b0 (e.g. the one at 0x5a7a7b0 with a
0x360-byte frame containing 0x5a7aa34/0x5a7aaa4), which are NOT referenced
by the five table slots. What references them remains unresolved.

What still stands from Revision 1: the relocation-derived pointer
reachability result (slot 0x61dec08 -> getpid wrapper stub 0x267d0c0),
the nine inline sites themselves, and all dispatch-mechanics analysis.

Census, sharpened: an aligned word scan of the executable segment
((word & 0xFFE0001F) == 0xD4000001) finds 130 `svc #0` pattern hits and
69 hits with non-zero immediates. Per-1MB regions of the svc #0 hits:
0xc00000: 43, 0xd00000: 41, 0xe00000: 30, 0x2600000: 7, 0x5a00000: 9.
The 0x2600000 and 0x5a00000 regions match the 16 confirmed sites exactly
(7 stub svcs, 9 inline svcs - no extras in those regions). The remaining
114 svc #0 hits cluster in 0xc00000-0xefffff and are UNVERIFIED: word-scan
hits can be embedded data, and these have not been confirmed as decoded
instructions by disassembly. The 69 non-zero-immediate hits are presumed
data constants (their immediate values are irregular) but are likewise
unverified. True reachable-svc total: still open.

Implication changes: "zero references" is dead as a reachability argument.
Any reached svc #0 still fatals under the examined kernel implementation
(see dispatch mechanics below), so the execution-phase risk is UNCHANGED in
kind but WIDER in surface: ordinary functions containing inline syscalls
with runtime-computed numbers must be assumed reachable through the normal
call graph until a per-path analysis says otherwise. (Revision 2 withdrew
the specific five-slot linkage to the inline sites; the conservative
reachability posture stands because the containing functions' callers
remain unresolved.)

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
- Reference reachability: see Revision 1 - a static table pointer to the
  getpid wrapper exists, and 9 inline register-computed raw-syscall sites
  are confirmed in ordinary functions. 32-bit relative jump tables or
  runtime-computed dispatch can reach any site as well. The
  futex/getpid/gettid wrapper shapes remain consistent with dead code from
  a statically linked archive, but that is no longer the only possibility
  for the getpid wrapper.

## Limits of this audit

- Stub confirmation recognizes only `mov x8, #nr; svc #0`. Wrappers loading
  the syscall number into x8 from a register or memory, or using other
  forms, are not detected. (Revision 1: this gap is now confirmed real -
  9 inline register-computed sites found at 0x5a7aa34-0x5a7d060. Callers
  of their containing functions are not yet enumerated.)
- The remaining `svc #0` pattern hits are not individually disproven;
  Revision 2 counts 130 aligned `svc #0` word hits in total, 16 confirmed
  as instructions, leaving 114 unverified (clustered in 0xc00000-0xefffff;
  word-scan hits can be embedded data). The true raw-syscall count is
  >= 16.
- 3 hits carry small non-zero immediates (12, 13, 20) that would be valid
  Horizon ids. Near-certain data constants: an Android ELF contains no
  Horizon-targeted code. Flagged, not resolved.
- `brk`-encoding hits (161) not analyzed; Android uses `svc #0` for traps,
  so these are presumed data.
