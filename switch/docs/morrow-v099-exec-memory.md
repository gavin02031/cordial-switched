# Guest execute memory (v0.99 line)

How `android_mapping_compat` serves guest `mmap`/`mprotect` calls that ask
for execute permission, and why it works this way. Guest JIT engines (Luau
codegen in libroblox) need this; before this change every `PROT_EXEC`
request failed with `ENOTSUP`, which routes the guest into its error
logger — the exact pattern that previously entered the allocator-init
guard spin.

## Why not svcSetMemoryPermission

`svcSetMemoryPermission` only accepts None/R/RW for the caller's own
address space (documented SVC behaviour; the kernel-side page-table
implementation in Atmosphere `KPageTableBase::SetMemoryPermission` adds no
execute restriction of its own, so the rejection lives in SVC validation —
INFERRED from the two implementations compared here). Execute transitions
go through the process code-memory syscalls instead:

- `svcMapProcessCodeMemory(dst, src, size)`: dst must be free space in the
  code region (`CanContain(AliasCode)`), src must be Normal heap pages in
  user RW. The kernel moves src to NotMapped+Locked for the lifetime of
  the alias — nothing may touch the backing pages until the matching
  unmap. (Verified in `KPageTableBase::MapCodeMemory`.)
- `svcSetProcessMemoryPermission(addr, size, perm)`: requires every block
  in range to carry `FlagCode` — heap pages cannot be made executable in
  place; only code memory (or the process's own code) can. Accepts
  None/R/RW/RX/X. The kernel performs dcache store + full icache
  invalidation itself when setting execute permission.
- `svcUnmapProcessCodeMemory(dst, src, size)`: any permission on the
  alias; restores src to Normal heap RW unlocked. No pre-flip needed.

All three are granted to NRO processes by hbloader's kernel capabilities
(`svcSetProcessMemoryPermission` 0x73, `svcMapProcessMemory` 0x74,
`svcUnmapProcessMemory` 0x75, `svcQueryProcessMemory` 0x76,
`svcMapProcessCodeMemory` 0x77, `svcUnmapProcessCodeMemory` 0x78 in the
stock `hbl.json`). The shim gates on `envIsSyscallHinted` for 0x73/0x77/0x78
plus a valid own-process handle and falls back to `ENOTSUP` if absent —
the same probe `horizon_code_memory` uses for the engine image.

## Design

An exec region is the shim's normal heap backing plus a code-region alias:

1. Backing pages come from the same `memalign` heap allocation as data
   regions (Normal state, so the pages qualify as code-memory sources).
   File-backed contents are copied in before the alias is created.
2. A code-region VA is reserved with `virtmemFindCodeMemory` +
   `virtmemAddReservation`, the backing is dcache-flushed, then
   `svcMapProcessCodeMemory` aliases it and
   `svcSetProcessMemoryPermission` applies the requested RX or X.
3. The guest receives the code-region address and never sees the backing
   address. `find_range` bookkeeping keys on the code address.

Permission flips (`mprotect`) on an exec region, whole-region only:

| Transition | Mechanism |
| --- | --- |
| anything -> RX/X | direct `svcSetProcessMemoryPermission` (kernel-validated `is_x` path, cache maintenance included) |
| -> RW, alias never writable yet | direct: AliasCode -> AliasCodeData is the kernel's supported `is_w` case |
| -> RW, alias already writable once | unmap + re-map the alias, then set RW (see below) |
| -> R/None | direct (no write transition involved) |

The re-alias dance exists because `SetProcessMemoryPermission`'s `is_w`
switch only names `Code`/`AliasCode` source states; calling it with a
write permission while already in `AliasCodeData` reaches the kernel's
`MESOSPHERE_UNREACHABLE_DEFAULT_CASE` — tolerated as a no-op-ish fall
through on release builds (what retail consoles run) but an abort on
debug kernels. The shim avoids the path entirely. Cost is three syscalls
on a rare transition.

Teardown (`munmap` of the whole region): `svcUnmapProcessCodeMemory`
restores the backing to Normal RW, the reservation is removed, and the
backing returns to the heap through the normal `drop_piece` path.

## Refusals (honest, errno = ENOTSUP)

- Any protection with W and X simultaneously (`PROT_WRITE|PROT_EXEC`,
  `PROT_READ|PROT_WRITE|PROT_EXEC`): the kernel asserts `!(is_w && is_x)`.
- `mprotect` adding execute to an ordinary heap-born region. This is the
  important one: execute permission requires `FlagCode` state, heap pages
  can never carry it, and a code alias cannot occupy a heap address, so
  the same-address RW -> RX conversion every textbook JIT uses is
  impossible on this kernel. The only serveable pattern is execute
  requested at `mmap` time, where the shim chooses the address.
- Partial-range `mprotect`/`munmap` on an exec region: one kernel code
  mapping covers the whole region; splitting it is not approximated.
- `mremap` (grow or shrink) and `madvise` on an exec region.
- `PROT_WRITE` alone (pre-existing refusal, kept).

## What the guest actually does (verified call-site census)

Every `bl` to the `mmap`/`mprotect` PLT stubs in libroblox.so was
disassembled and its constant arguments decoded (six `mmap`, four
`mprotect` sites; no `mmap64` import exists):

- The guest JIT code allocator (function containing 0x2176120) does
  `mmap(NULL, aligned, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS)`
  and then `mprotect` parts of the SAME mapping to `PROT_READ` (0x2175fd0)
  and `PROT_READ|PROT_EXEC` (0x2175ff0) after copying code in. It never
  probes RWX first. This pattern is kernel-impossible to serve at the
  same address (see Refusals), so the shim's `ENOTSUP` stands — and the
  guest handles it cleanly: on a nonzero `mprotect` result the allocator
  zeroes its result struct and returns failure to its caller (verified at
  0x2175ff4-0x2176030), with no error-logger call on this path. Whether
  the caller then disables codegen and runs interpreted is beyond static
  reach. The earlier claim in this document that the RW + mprotect
  fallback "is served" was wrong and has been corrected.
- The guest data allocator (0x23ffb54 cluster) mmaps RW anonymous and
  fences guard pages with `PROT_NONE` at both ends — fully served by the
  ordinary path.
- Two shared file mappings (`flags & MAP_SHARED`, one RW at 0x1eb1be4,
  one R|MAP_NORESERVE at 0x20194e8) are refused by the pre-existing
  private-only rule, unchanged.
- One read-only `MAP_PRIVATE` file mapping (0x2059ec4) is served.
- A code alias is also created for private file mappings requested with
  execute permission (e.g. guest `dlopen` of further libraries), with the
  contents staged into the backing before the alias is mapped.

### JIT allocation failure propagation (verified three levels)

What happens when the shim refuses the JIT allocator's RX transition:

1. The block allocator (entry 0x2175e38) zeroes its result struct and
   returns; mmap and mprotect failures both land there.
2. Its single caller (0x21750c4, in the function at 0x2174d68) skips the
   virtual calls that consume the block and returns `block != NULL` as a
   plain boolean.
3. The next caller up (0x21748b8) treats false as "no allocator": runs a
   cleanup/destructor call, stores a null out-pointer, returns.

No error logger, abort, or allocator-guard interaction appears in these
three levels. The string table confirms the engine is Luau with codegen
("LuauCodeGenBlockSize", "LuauCodegenStatsEventHundredthsPercent",
"LuauJitInlineSmallFunSize"). INFERRED, beyond static reach: codegen
disablement propagates to per-function interpreted execution, the stock
Luau behaviour on allocation failure. NOT verified: whether Roblox's
product layer tolerates long-term interpreted mode (performance,
feature gates).

### madvise call paths (verified) and shim response

Six madvise sites. The guest allocator's decommit wrapper (0x1eba2xx)
loads its advice from a global it initializes to MADV_FREE (8), retries
on EAGAIN, and on EINVAL stores 4 into the global and retries with
MADV_DONTNEED (verified 0x1eba338-0x1eba3c8). Persistent failure after
that reaches a printf-style logger with "cannot reset OS memory"
(0x1b32ccc), severity beyond the format call not followed. Other sites:
MADV_HUGEPAGE (14, result ignored), MADV_DONTDUMP (16, result recorded
not gated), one direct MADV_DONTNEED (0x1eba598, errno checked).

Shim response, changed in the same commit as this note: hint advices
(NORMAL/RANDOM/SEQUENTIAL/WILLNEED, HUGEPAGE/NOHUGEPAGE,
DONTDUMP/DODUMP, COLD/PAGEOUT, POPULATE_READ/WRITE) succeed as
validated no-ops; MADV_DONTNEED zeroes as before; everything else,
including MADV_FREE, now fails EINVAL instead of ENOTSUP so the guest's
own verified fallback selects MADV_DONTNEED, which the shim serves.

## Verification status

- Host-exercised (x86_64 CI test, native mmap backend): allocation,
  rejection of W&X combos, refusal of partial/resize/discard operations,
  refusal of execute transitions on ordinary regions (regression test for
  a bug where such an mprotect silently fenced the pages PROT_NONE and
  reported success), the full flip sequence RW -> RX -> R -> None -> RX
  -> RW -> RX, real execution of generated code at the returned address,
  and teardown. The same test compiles and runs under qemu-aarch64 in the
  preview workflow.
- Source-grounded (not hardware-observed): every kernel transition used
  is validated against Atmosphere page-table sources. The code-memory
  syscall sequence itself mirrors `horizon_code_memory`, which the
  delivered build already uses for the engine image.
- Not yet observed: a guest actually requesting execute memory on the
  console, code-region capacity alongside the engine-image reservation at
  runtime, and any guest JIT behaviour on hardware.
