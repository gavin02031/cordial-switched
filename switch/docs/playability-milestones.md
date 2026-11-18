# Playability milestones

Versions describe verified milestones, not the number of development builds.
The link-31 report from v0.99.1 records 547 supported provider lookups, 82
missing lookups including two optional weak references, and 80 required missing
relocation sites. These are not necessarily unique symbol counts. No Roblox
engine code or graphics calls executed. The first required missing name is fchown.

| Milestone | Required evidence | Current state |
| --- | --- | --- |
| A | Complete required-import resolution against the actual Horizon providers, including dependency/version identity | Blocked: complete target-observed provider manifest unavailable; the 80-site link-31 count is historical, not a verified current symbol count |
| B | Dependency initialization, DT_INIT, then DT_INIT_ARRAY complete in order | Blocked: 2,505 guest array entries; current code validates targets only |
| C | JNI_OnLoad and observed Android startup finish | Not reached; Java application parameters/lifecycle incomplete |
| D | A visible frame produced by guest engine calls through GLES | Not reached; launcher/triangle frames do not qualify |
| E | Working engine authentication, with a supported Quick Login flow if available | Not implemented |
| F / v1.0 | Join a game and demonstrate playable input, rendering and continued execution | Not reached |

The 0.99.4 console preflight reported **629 total import lookups**, **562
resolved** and **67 missing** (two weak references and 65 required relocation
sites). Its strict pass stopped at `geteuid@LIBC`, with two weak `dyninit`
boundary symbols left unresolved. This is a historical **target-observed**
result, not a prediction of the current build. The same report indicated that
the `dyninit` section had not been verified, no initializer ran and no guest
engine or graphics code executed. Treat a later diagnostic as a new measurement;
do not convert the 65 relocation sites into a distinct-symbol count.

## Offline audit

Keep the user-supplied ARM64 library outside Git and public Actions artifacts.
Run locally (GNU readelf also works via `--readelf readelf`):

```sh
python3 switch/tools/audit_android_elf.py /path/to/libroblox.so \
  --readelf llvm-readelf --output /path/to/audit.json
```

The JSON contains DT_NEEDED entries, all visible undefined dynamic symbols,
weak/required binding, version/provider identity, the binary SHA-256, and
aligned SVC #0 candidates in executable PT_LOAD segments **and separately**
within a named executable `.text` section, when section headers exist. This reads bytes;
it never executes or patches guest code. Malformed input or unavailable dynamic
symbol metadata fails rather than claiming zero imports.

### Verified private ARM64 library audit (2026-11-16)

The user-supplied Android AArch64 `libroblox.so` was independently read with
GNU `readelf` and the offline auditor, without executing it or checking it in:

| Evidence | Observed result |
| --- | --- |
| SHA-256 | `2ce7db6dc4bb3d907b0318fc79617fbf63c66298657286b4f6b5821985e40c43` |
| Required undefined dynamic imports | 576 |
| Optional weak dynamic imports | 6 |
| DT_NEEDED dependencies | 10 |
| Raw SVC #0 matches in executable PT_LOAD segments | 130 |
| Raw SVC #0 matches specifically in named `.text` | **16** |
| `.text` matches with directly preceding `MOVZ x8/w8` Linux syscall number | **7**: `futex` (2), `getpid` (3), `gettid` (2) |

The distinction between **130 segment-wide** and **16 `.text`** matches is
intentional. Segments may contain data and other executable sections; a
segment-wide scan does not establish 130 syscall instructions. Conversely, a
`.text` match is still only a static candidate, not evidence of a reached
instruction. If section headers are missing, `svc_text_candidates` is `null`
and the absence cannot be misreported as zero.

An earlier offline comparison reported **77 distinct missing symbols**, but its
exact 77-name provider comparison was not retained, so we cannot independently
reproduce that figure yet. It is **not** the total number of imports in this
ELF. The private binary supplies the import side, but a **complete, verified
Horizon provider manifest** is still needed to reproduce the exact outstanding
Milestone A list. Do not ask for a separate Switch run per missing symbol or
substitute provider names inferred from source. The Switch remains necessary
for checks of guest runtime behaviour and constructor execution.

On 64-bit Linux, add `--host-providers` to compile the checked-out adapters
with `cc` and query every exact import key. The resulting
`host_provider_coverage` includes weak imports and marks each entry separately.
For the x86-64 host build and image above, 426 resolved, 154 required and two
optional imports were unavailable. A second scan of the staged relocation
tables produced the same lookup results for every key. Switch-specific
graphics are disabled in the host build.

Optionally pass `--providers /path/to/providers.json`. This file is a JSON array
of `{ "name": "pread", "version": "LIBC", "library": "libc.so" }` records
obtained from a verified target provider inventory. Unversioned references use
JSON null for version and library. Comparison emits every `missing_required`
entry and exits 1 if any are absent. Do not infer this inventory merely by
grepping MAP macros: target/compiler gates and runtime providers matter. A
target-observed Horizon provider-manifest exporter is available from a **new**
final-base diagnostic report, but a complete report has not yet been collected.
It does not guess providers from the source tree:

```sh
python3 switch/tools/export_horizon_providers.py /path/to/link-XX.txt \
  --output /path/to/horizon-providers.json \
  --evidence-output /path/to/horizon-provider-evidence.json
python3 switch/tools/audit_android_elf.py /path/to/libroblox.so \
  --readelf readelf --providers /path/to/horizon-providers.json \
  --output /path/to/audit-with-providers.json
```

The exporter accepts only a final-base report with a completed relocation scan,
complete numbered success **and** missing-import inventories, consistent lookup
counts and a recorded build identity. Every accepted key was presented to the
actual Horizon resolver. Distinct matches do not prove that a provider's runtime
behaviour is sufficient for Roblox. The evidence JSON contains the link report's
SHA-256 and lookup totals; keep it with the offline audit. Older link reports
(including link-31) lack the resolved-import inventory and cannot be upgraded
into this form by inference. The private ELF and target report still need to be
made available to the same trusted runtime before milestone A can be verified.

With a verified manifest, the audit writes both `missing_required` (import
entries) and `missing_required_distinct` (unique name/version/provider keys),
prints the complete distinct missing list, and exits with status 1 if any
required keys remain. Do not confuse either count with relocation sites.
CI exercises the parsers with synthetic fixtures, without committing or
uploading the proprietary ELF.

SVC matches can be data or unreachable instructions. A nonzero count needs
inspection, not automatic patching; zero does not exclude instructions in
dependencies or generated code. The offline audit also labels a `syscall_hint`
when the **immediately preceding** aligned instruction is `MOVZ x8/w8, #N`
with no shift. It recognizes Linux AArch64 numbers 98 (`futex`), 172
(`getpid`) and 178 (`gettid`), but also retains other immediate values as
numbers without names. This narrow sequence match is **not** control-flow
analysis or proof that execution reaches the instruction; it does not infer
values assembled across multiple instructions. A missing hint is not proof
that a candidate is harmless. CI exercises the parser with synthetic ARM64
instructions; the proprietary ELF remains outside the repository.
The repository's no-hooking/no-memory-patching rules still apply. Record
evidence before selecting any compatibility strategy.

## Best-effort Switch exception diagnostics

The Horizon NRO now supplies a libnx `__libnx_exception_handler` that attempts
to record exception type, PC, x8, ESR and FAR in
`sdmc:/switch/cordial-switched/exception-last.txt`, and sends the same record
through `svcOutputDebugString` when a debugger is present. Host tests check
record formatting, and the libnx target is cross-built in CI. The error path
must not be treated as a reliable trap router: libnx's [user exception entry](https://github.com/switchbrew/libnx/blob/master/nx/source/runtime/exception.s)
may never be called for a fault in dynamically mapped or JIT-like code;
[Switchbrew's exception documentation](https://switchbrew.org/wiki/SVC#Exception_handling)
notes kernel debug/crash dispatch in those cases. Failure to create the SD
file does **not** establish that no `svc #0` was attempted. The handler does
not resume execution, rewrite any instruction, or translate Linux syscalls.

## Runtime work still required

Classify imported operations by implemented semantics, not by whether a stub
allows linking. FAT/exFAT ownership policy needs explicit descriptor and request
validation. Unsupported process creation must report failure; memory mapping,
threading, readiness and clocks need functional implementations. Neither a
successful import audit nor a fail-returning function proves an engine can boot.

### Explicit process-model decisions

The process compatibility provider resolves the versioned `LIBC` imports below
without claiming Horizon implements Linux process features. These are failure
policies, **not functional replacements**; guest code requesting them may still
refuse to initialise. The host-side staging build uses actual host UID/parent
IDs; Horizon returns `ENOSYS` for those queries instead of inventing a sandbox
identity that disagrees with file ownership and permissions.

| Bionic imports | Horizon policy | Reason |
| --- | --- | --- |
| `getuid`, `geteuid`, `getppid` | Error sentinel with `ENOSYS` | No verified Android guest user or parent process |
| `fork` | `-1`, `ENOSYS` | No guest child-process model |
| `execv`, `execve` | `-1`, `ENOSYS` | No guest process-image replacement |
| `waitpid` | `-1`, `ENOSYS` | No guest process to reap |
| `system` | `0` only for `NULL`; otherwise `-1`, `ENOSYS` | No command interpreter |
| `ptrace` | `-1`, `ENOSYS` | No guest tracing permission or target process |
| `__register_atfork` | Return error number `ENOSYS` | Cannot promise DSO-scoped fork callbacks |

The separate `getpid` and `gettid` providers are unchanged. Unit tests ensure
no actual child is created or command executed, reject wrong symbol versions,
and verify desktop identity forwarding. An actual Horizon return has not been
measured. The relocator can now resolve these names, but resolving a name is
not evidence that its caller can continue after a genuine `ENOSYS` result.

The Horizon target has a **synthetic-only constructor run controller**, not
a live guest constructor runner. Its callback-gated ordering requires durable
begin/completed journal writes around each call and refuses replay after any
attempt, including a failed marker. Host tests verify those mechanics without
executing guest code. Future live wiring must first prove mapping lifetime,
dependency ordering, guest TLS/thread state and durable crash recovery. Retain diagnostic
mode and require an explicit execution action. Log virtual and runtime addresses
with the actual ELF load bias (runtime base minus the lowest mapped virtual page),
not just an assumed zero-based image, for crash-report correlation.

Port startup signatures/order from existing Cordial code and Android traces;
do not guess Java field values or native argument layouts. Completing the
constructor runner alone does not supply the missing Android application.

### Private anonymous data mappings

The runtime now exposes the engine's LIBC imports for mmap, mprotect and
munmap, mremap, madvise and msync. Supported mappings include private anonymous data with offset zero and
private snapshots of regular files at page-aligned offsets, with
no access, read access or read/write access. On Horizon it obtains a
page-aligned backing allocation, maps a kernel alias and changes permissions
on the alias. A non-fixed address hint may be ignored. Shared mappings, fixed
addresses and executable permissions remain unavailable. Whole mappings can
shrink in place. A uniformly writable mapping can grow with MREMAP_MAYMOVE,
preserving the old bytes and zeroing new pages. Failed growth leaves the
original intact. Private file snapshots use positional reads, retain zeroes
after EOF in the final page, and stay usable after closing the descriptor.
Requests spanning wholly nonexistent file pages fail. MS_SYNC and MS_ASYNC
on live private ranges require no writeback. Invalidation is unsupported.
MADV_DONTNEED on writable anonymous pages restores their
zero contents. On Horizon that operation retains backing memory.
Protection and release accept a subrange of one live region. Removing a
middle range retains the surviving pieces and their shared backing allocation.
Backing memory can therefore stay committed until the final piece is removed.

Loader L3 probes a private page and records zero initial contents, a byte
surviving protection changes, kernel-reported permissions and cleanup in the
numbered link report. The probe uses Morrow-owned memory only. An allocation
or permission failure is evidence of a target limitation. Host CI tests use
Linux mappings and observe actual protection faults in child processes.
Neither those results nor a successful cross-build demonstrates a Roblox frame.

The guest sysconf adapter translates Android's page-size aliases and CPU and
memory queries. On Horizon it uses the core mask and memory budget available
to the current process. Unknown queries return EINVAL. A host staging scan of
the private image identified above resolved 426 distinct imports and missed
156, including two weak imports. The host disables target-only graphics
providers. Use the console inventory for milestone A.

Legacy IPv4 DNS lookups now keep the returned address list alive until the
thread replaces it or exits. The host test checks two threads resolving
different IPv4 addresses. Live DNS and Roblox login on Horizon remain unverified.

Calendar conversion functions now use the full 56-byte Bionic ARM64 struct tm.
UTC and local results include the offset and timezone name, and reentrant
calls write into caller-owned storage. The calendar tests cover the New Year
boundary west of UTC, the valid timestamp -1, and formatting a guest-supplied
timezone containing a percent sign. Extended nonstandard format modifiers
still depend on the native formatter.

The preview's pinned libnx stack allocator reserves an unmapped prefix for
Android joinable workers. With default attributes this is 16 KiB, below a
992 KiB requested stack. Guard reservations share libnx's virtual-memory
lock with mapping and survive until successful stack unmapping. In the L3
report, `guest_stack_guard_join=0 unmapped_16k=1 released=1` requires a
worker to inspect its stack and the kernel memory map and then join. That
result has not yet been observed on a console.

### System provider batch and measured import coverage (2026-11-18)

A new `android_system_compat` provider resolves 44 more of the engine's
versioned `LIBC` imports. Two groups, same rule as the process adapter:
resolving a name is not evidence its caller survives the answer.

The genuinely functional entries are `android_set_abort_message`, which
captures the engine libc's final message into a bounded buffer and the
diagnostic log, and `openlog`/`syslog`/`closelog`, which format the guest's
syslog traffic and forward it to the same sink as its liblog output. Bionic
routes syslog to logd; Morrow has no logd, and nothing is acknowledged that
was not delivered to the sink.

The rest fail closed with `ENOSYS` (or the interface's own error shape:
`signal` returns `SIG_ERR`, `if_nametoindex` returns 0, `posix_fallocate` and
the `pthread_*` policy calls return the error number). Covered: the signal
installation family (`sigaction`, `signal`, `raise`, `pthread_sigmask`,
`sigaltstack`), scheduling policy (`sched_*`, `getpriority`, `setpriority`,
the `pthread_*sched*` calls and `pthread_setname_np`), descriptor facilities
with no Horizon backing yet (`epoll_*`, `eventfd`, `timerfd_*`, `pipe`,
`socketpair`, `accept4`, `ioctl`, `syscall`, `prctl`), and miscellaneous
POSIX surface (`mlock`, `statvfs`, `utime`, `utimes`, `gethostname`,
`if_nametoindex`, `if_indextoname`, `tcgetattr`, `tcsetattr`,
`posix_fallocate`). Fail-closed providers never dereference their arguments,
so no untranslated guest structure is read. Several of these will need real
implementations before the engine's event loop can run - `epoll_*`,
`eventfd` and `timerfd_*` back Android's Looper - but a caller that installs
a crash handler or queries scheduler policy now fails at the call instead of
blocking the relocation audit.

Re-running the offline audit on the verified private image (SHA-256
`2ce7db6d…e40c43`) against host-built providers measures the batch directly:
582 visible imports, 481 supported, 99 required missing, 2 optional missing.
Of the 99, 91 are the EGL/GLES2 names the host build disables (target
providers link Switch Mesa) and two are `setjmp`/`longjmp`, which the ARM64
jump bridge provides and the host build deliberately refuses. That leaves
six genuinely outstanding required imports: `__ctype_get_mb_cur_max`,
`__system_property_get`, `dl_iterate_phdr`, `mallinfo`, `sysinfo` and
`uname`. Each is deferred rather than fail-closed for a stated reason:
`mallinfo` returns a struct by value and cannot report failure, and the
guest statically links its own allocator (`malloc`/`calloc`/`free` are not
imported), so any host answer describes the wrong allocator; `uname`,
`sysinfo`, `__ctype_get_mb_cur_max` and `__system_property_get` need a
documented presented-environment decision, not invented values;
`dl_iterate_phdr` has no error return and wants a real walk over the
loader's own module records.

The L3 mapping probe now logs each protection transition's raw
`svcSetMemoryPermission` result and the queried `MemoryInfo` (perm, type,
attr) alongside the existing summary keys, so the next console run can
distinguish a refused permission change from a successful one the query
disagrees with - the ambiguity in the v0.99.4 `read_only=0 none=0 rw=0`
report. These are host-measured results; none of it has run on a console.
