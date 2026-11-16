# Playability milestones

Versions describe verified milestones, not the number of development builds.
The link-31 report from v0.99.1 records 547 supported provider lookups, 82
missing lookups including two optional weak references, and 80 required missing
relocation sites. These are not necessarily unique symbol counts. No Roblox
engine code or graphics calls executed. The first required missing name is fchown.

| Milestone | Required evidence | Current state |
| --- | --- | --- |
| A | Complete required-import resolution against the actual Horizon providers, including dependency/version identity | Blocked: 80 required sites in link-31 |
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
aligned SVC #0 candidates in executable PT_LOAD segments. This reads bytes;
it never executes or patches guest code. Malformed input or unavailable dynamic
symbol metadata fails rather than claiming zero imports.

Optionally pass `--providers /path/to/providers.json`. This file is a JSON array
of `{ "name": "pread", "version": "LIBC", "library": "libc.so" }` records
obtained from a verified target provider inventory. Unversioned references use
JSON null for version and library. Comparison emits every `missing_required`
entry and exits 1 if any are absent. Do not infer this inventory merely by
grepping MAP macros: target/compiler gates and runtime providers matter. A
complete Horizon provider-manifest exporter is not implemented yet, so this
comparison alone is not an established milestone-A check. CI currently tests
the audit tool with synthetic fixtures; it does not have the proprietary ELF.

SVC matches can be data or unreachable instructions. A nonzero count needs
inspection, not automatic patching; zero does not exclude instructions in
dependencies or generated code. The repository's no-hooking/no-memory-patching
rules still apply. Record evidence before selecting any compatibility strategy.

## Runtime work still required

Classify imported operations by implemented semantics, not by whether a stub
allows linking. FAT/exFAT ownership policy needs explicit descriptor and request
validation. Unsupported process creation must report failure; memory mapping,
threading, readiness and clocks need functional implementations. Neither a
successful import audit nor a fail-returning function proves an engine can boot.

The Horizon target has no constructor runner today. Before adding one, implement
and test mapping lifetime, dependency ordering, once-only initialization,
thread-state requirements, and durable per-call breadcrumbs. Retain diagnostic
mode and require an explicit execution action. Log virtual and runtime addresses
with the actual ELF load bias (runtime base minus the lowest mapped virtual page),
not just an assumed zero-based image, for crash-report correlation.

Port startup signatures/order from existing Cordial code and Android traces;
do not guess Java field values or native argument layouts. Completing the
constructor runner alone does not supply the missing Android application.
