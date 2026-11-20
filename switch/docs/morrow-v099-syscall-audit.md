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
(7 stub svcs, 9 inline svcs - no extras in those regions).

Revision 2 follow-up (2026-11-20): the census is now CLOSED, with its
coverage and assumptions stated precisely in Revision 3. The 114
svc #0 hits in 0xc00000-0xefffff all lie inside .gcc_except_table
(0xc0c97c-0xeea7a8, non-code) and the tail of .rodata; the bytes around
them do not decode as instruction streams (capstone terminates
immediately), while all 16 confirmed sites lie inside .text
(0x1b2fc00-0x61621d4) and the .text hits match the 16 confirmed addresses
exactly. The 69 non-zero-immediate hits likewise all fall in data
sections (.rela.dyn, .rodata, .gcc_except_table).

## Revision 3 (2026-11-20): census scope, reachability, post-mprotect path

Census coverage and assumptions (precision correction to the Revision 2
follow-up wording): the guest's only executable MAPPING is PT_LOAD[0],
RX across 0x0-0x61649d0, which also contains .rodata, .gcc_except_table,
.eh_frame_hdr and .eh_frame. Section flags therefore do NOT make the
data-section hits unexecutable: a computed control transfer landing
exactly on one of those aligned pattern words WOULD execute svc #0. The
scan covers every 4-aligned word of that segment (all svc encodings are
4-aligned, so coverage of encodings is exhaustive): 199 pattern hits
total. SHF_EXECINSTR sections are .text, malloc_hook (0x61621d4-0x6162754)
and .plt (0x6162760-0x61649d0); the latter two contain zero svc
encodings. The 183 data-section hits are classified as data on three
legs: ALLOC-only section flags, failure of surrounding bytes to decode as
instruction streams, and no relocation-derived or direct-branch
references found. Conclusion, scoped: under ordinary control flow through
decodable instruction streams, exactly 16 raw-syscall sites exist.
Out-of-scope residual: corrupted/adversarial control flow into aligned
data bytes could still hit a pattern word; that flow would fatal by many
other mechanisms first and is not the benign-execution question.

Reachability of the inline sites, resolved one link: the nine inline svc
sites are all in ONE large function at 0x5a7a7b0 (0x360-byte frame;
multiple return paths including the ret at 0x5a7b910; out-of-line
continuation blocks follow it, e.g. the b.ne at 0x5a7b8f0 targeting
0x5a7d0bc inside the same frame - Revision 2's two-function split was an
artifact of the post-ret boundary heuristic). That function is called by
a direct BL at 0x5a7a2e8, which sits inside the control flow of
0x5a7a168 - one of the five relocation-derived table entries (slot
0x66227a0). Chain: static .data.rel.ro function-pointer table -> fn
0x5a7a168 -> BL -> fn 0x5a7a7b0 -> 9 inline raw-svc sites. Revision 2's
literal statements stand (the five referenced bodies contain no svc; no
direct BRANCH from them reaches the region - the link is a CALL); its
"what references them remains unresolved" is superseded for the
containing function. The remaining link - who calls through the table
pointer - is the same open question as for the getpid wrapper stub.

Post-protection constructor path (static): the ctor-3 chain function at
0x26e63a8 (arena reserve) mmap's 0x7b2000 bytes PROT_NONE/ANON
(BL 0x61633f0 at 0x26e640c), then mprotects it PROT_READ|PROT_WRITE
(BL 0x6163400 at 0x26e6424). On mprotect failure it munmaps and exits
through the error-string path (blr x8 at 0x26e6464). On success it calls
0x2747d70(size, struct) at 0x26e643c, stores the arena base into the
owner struct, then runs a slot-initialization loop (stride 0x7b180)
whose first writes into the arena memory are the stores at
0x26e649c-0x26e64a0 (str xzr,[x27,#0x118]; stp xzr,x8,[x27]). These
stores are statically the first guest dereferences of the arena after the
permission change. This static picture is consistent with - but does not
confirm - the external link-58 claim pattern (mprotect success, then
silence); the actual first fault site remains hardware-evidence
territory and the link-58 file itself is not in our possession.

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
  9 inline register-computed sites found at 0x5a7aa34-0x5a7d060.
  Revision 3: all nine are in one function at 0x5a7a7b0, reached via
  table slot 0x66227a0 -> fn 0x5a7a168 -> BL. Who calls through the
  table pointer itself is not yet enumerated.)
- `svc #0` word-scan hits outside the 16 confirmed sites are now resolved
  (Revision 2 follow-up): all 114 fall in .gcc_except_table/.rodata and
  all 69 non-zero-immediate hits fall in data sections. The confirmed
  raw-syscall instruction count is exactly 16; what remains open is
  reachability, not enumeration.
- 3 hits carry small non-zero immediates (12, 13, 20) that would be valid
  Horizon ids. Resolved as data (Revision 2 follow-up): they lie in
  non-code sections like all 69 non-zero-immediate hits.
- `brk`-encoding hits (161) not analyzed; Android uses `svc #0` for traps,
  so these are presumed data.
