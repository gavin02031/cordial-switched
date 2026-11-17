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
