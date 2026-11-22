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
  Guests that probe RWX first (Luau does) fall back to RW + mprotect,
  which the shim does serve.
- Partial-range `mprotect`/`munmap` on an exec region: one kernel code
  mapping covers the whole region; splitting it is not approximated.
- `mremap` (grow or shrink) and `madvise` on an exec region.
- `PROT_WRITE` alone (pre-existing refusal, kept).

## Verification status

- Host-exercised (x86_64 CI test, native mmap backend): allocation,
  rejection of W&X combos, refusal of partial/resize/discard operations,
  the full flip sequence RW -> RX -> R -> None -> RX -> RW -> RX, real
  execution of generated code at the returned address, and teardown. The
  same test compiles and runs under qemu-aarch64 in the preview workflow.
- Source-grounded (not hardware-observed): every kernel transition used
  is validated against Atmosphere page-table sources. The code-memory
  syscall sequence itself mirrors `horizon_code_memory`, which the
  delivered build already uses for the engine image.
- Not yet observed: a guest actually requesting execute memory on the
  console, code-region capacity alongside the engine-image reservation at
  runtime, and any guest JIT behaviour on hardware.
