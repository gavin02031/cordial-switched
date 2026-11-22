# Console run playbook (draft for the next supervised run)

What the next run on Gavin's Switch Lite answers, where the evidence lands,
and how to read it. Nothing here has executed on hardware yet.

## Pre-console gate (host, already passing)

`switch/tools/host_full_audit.c` runs the real guest ELF through the same
`engine_stage_stream_audit_at` relocation machinery and the same
`android_import_resolve` chain the console's full_audit uses - nothing is
executed; the scratch image stays NX and is discarded. Build and run from
the repo root (the guest ELF is proprietary; pass its path, never commit):

    PROVIDERS=$(grep -v '^#' switch/source/PROVIDER_SOURCES.txt | sed 's|^|switch/source/|')
    cc -std=c11 -O2 -Wall -Wextra -Werror -Iswitch/source \
      switch/tools/host_full_audit.c switch/source/engine_stage.c \
      switch/source/engine_boot_log.c switch/source/elf_probe.c \
      $PROVIDERS -lm -pthread -o /tmp/morrow-host-full-audit
    /tmp/morrow-host-full-audit /path/to/libroblox.so

Expected on a 64-bit Linux host (verified 2026-11-18, host-measured):
relocation scan complete; 588783 packed, 536 symbol and 0 RELR relocations
applied; 0 unsupported or TLS relocations; 94 required missing relocations
over exactly the 93 target-registered names (91 EGL/GLES2 + setjmp/longjmp)
plus the 2 optional dyninit symbols; no other missing name. The 94-vs-93
delta is longjmp, referenced at two sites (one .rela.plt JUMP_SLOT plus one
APS2 packed-table entry) - established by relocation tracing, not inferred
from counts. Any other outcome is a pre-console finding: an unexpected
missing name means a resolver regression, a nonzero unsupported count means
a relocation-type gap, and both appear before the console ever boots.

## What to install

The CI "Horizon preview" workflow's `cordial-switched-switch-preview`
artifact (the .nro) from the latest GREEN run on branch
feat/switch-horizon-smoke-test, plus libroblox.so at the path the engine
probes (ENGINE_LIBRARY_PATH). Copy the .nro onto the SD card and launch
through the Homebrew Menu as before.

Assets are NOT required for the diagnostic run (the Loader page path never
binds an asset directory). They become required when the exclusive engine
path runs JNI startup, which binds `sdmc:/switch/cordial-switched/assets`.
Prepare them ahead of that stage on a computer, never in the repository:

    python3 switch/tools/extract_apk_assets.py /path/to/Roblox.apk \
        /Volumes/SWITCHSD/switch/cordial-switched/assets

The tool extracts only the APK's top-level `assets/` tree, rejects entries
that would escape it, and writes `asset-manifest.json` with per-file SHA-256
and a combined hash (host-side verification) plus an `asset-manifest.txt`
twin. On the next engine-launch attempt the runtime compares the SD tree's
file count and total bytes against that manifest and records
`asset_manifest_*` lines in the onload report: a missing manifest means the
tool never ran, a counts mismatch means a partial or stale copy. On-device
comparison covers counts and bytes only, not content identity. The tool
reports whether classes.dex is present for information only; dex content is
never extracted because the classpath is not installed on Horizon. The APK
stays private.

## Resolved: the two dyninit symbols

`__start_ls_rbx_dyninit1` / `__stop_ls_rbx_dyninit1` are GNU ld boundary
symbols for a section named `ls_rbx_dyninit1`, and this ELF has no such
section (it ships `ls_rbx_dyninit2` through `dyninit6`, whose boundary
symbols are defined). An absent section leaves the pair weak-undefined;
the stage writes S=0 for both, so guest code walking `[__start, __stop)` sees
an empty range. That is the correct, complete handling: no special case is
required, and the `strict_missing_imports=2` line naming exactly this pair
is the expected steady state.

## What the run will do, in order

1. Probe and audit libroblox.so, map it, bind the 576 imports.
2. Run the engine's startup constructors (milestone B), each leaving a
   durable marker before and after it runs.
3. Only if every constructor completes cleanly: enter JNI_OnLoad.
4. Refuse honestly, with a named blocker, at the first thing that fails.

## Where the evidence lands (SD card)

`sdmc:/switch/cordial-switched/`
- `diag-progress.txt` - durable crash breadcrumbs (`l3_press`,
  `jni_preamble_done`, `real_link_returned`, `link_opened`), one per
  line, fsync'd at write. Each line starts with epoch seconds and the
  boot tick so breadcrumbs correlate with the
  `attempt_epoch=`/`attempt_tick=` lines in any `link-NN.txt` from the
  same run. `link_opened link-NN.txt` is written the moment a numbered
  report is created, so a fatal during the run attributes to exactly
  that file without relying on SD mtimes. If a run fatals with no
  `link-NN.txt` at all, the last line here localizes the stage; a line
  whose timestamp predates the run means the press never reached the
  handler.
- `link-NN.txt` - the full link report. The lines that matter:
  - `guest_main_tls_ready=` - this ELF has no TLS segment; expect `no`.
  - `guest_tpidr_bridge=` - expect `not_needed_no_tls_template`.
  - `constructor_begin_NNN=` / `constructor_completed_NNN=` - how far
    startup got, best-effort: the last `begin` without a matching
    `completed` names the routine that was running when things stopped.
    A fault inside the journaling or bridge code itself can leave an
    earlier or no marker, so treat the index as the strongest hint, not
    proof.
  - `constructor_run_completed=`, `constructor_run_next_index=`,
    `constructors_ok_for_jni_onload=`.
  - `experimental_jni_onload_preflight=` - the pre-constructor gate. On any
    ELF with an init array this reads `elf_constructors_not_run` by design;
    that reason alone no longer stops the run (it once did, stranding the
    constructor phase - fixed after the first hardware attempt showed it).
    `experimental_jni_onload_prerequisites=` verifies every non-constructor
    rule (runtime readiness, memory plan, RX target) BEFORE the first guest
    constructor executes; `blocker=jni_entry_prerequisites_not_met` refuses
    there. `experimental_jni_onload_decision_after_constructors=` is the
    full entry re-check; `blocker=post_constructor_entry_refused` means
    that re-check, not the constructors, refused entry.
  - `event=entering_real_engine_jni_onload` - only appears if every
    constructor finished; then `engine_jni_onload_return=0x00010006` is a
    clean return.
  - `blocker=...` - the named refusal if it stops.
  - `jni_surface_bound=`, `jni_configuration_bound=`,
    `jni_game_activity_ready=`, `jni_system_locale_bound=`,
    `jni_asset_configuration_bound=`, `asset_manager_bound=` (exclusive
    engine path only) - the JNI precondition snapshot recorded before any
    gate can stop the run. These are evidence, not gates: which of them
    Roblox startup strictly requires is unobserved guest behavior.
  - `asset_manifest_present/valid/declared/observed/counts_match=` - the
    SD asset-copy check described above.
  - `guest_heap_protect_read_only=0x...` / `guest_heap_protect_restore_rw=`
    - whether a plain heap page accepts svcSetMemoryPermission. Confirmed
    on the Lite (link-36): both return 0x0, the page queries as type 0x5
    (heap/Normal) with the requested perm, and `freed=1`. This matches the
    kernel source: Atmosphere's KPageTableBase::MapMemory hardcodes the
    alias destination to state Stack without FlagCanReprotect (hence the
    mirror alias's 0x0000d401 refusals), while the heap's Normal state
    carries it. The guest mapping provider now backs mappings with heap
    pages directly, so the guest_mapping_protect_* lines should show
    success from this build on; 0x0000d401 there would be a regression.
    `freed=` records whether the page was returned to the allocator (only
    when provably writable again); `page_retained=1` means it was
    deliberately leaked instead.
  - `guest_code_memory_syscalls_hinted=map:.. unmap:.. setperm:..` -
    envIsSyscallHinted for the privileged code-memory syscalls the
    executable (RX) path needs. Confirmed hinted 1/1/1 on the Lite
    (link-36), matching nx-hbloader's NPDM whitelist. A hint means the
    process may attempt the call, not that it is granted or will succeed;
    the path's actual svc results remain the only capability evidence.
  - `guest_code_mapping_map=0x...` / `guest_code_mapping_rx=0x...` /
    `guest_code_mapping_query=.. perm=0x.. type=0x.. content_match=..` /
    `guest_code_mapping_rollback=0x...` - the granted-capability test for
    those hinted syscalls, logged per step so a fatal localizes the
    failing stage: one private test page is mapped into the code region,
    set RX, queried and verified against its known pattern. No code is
    executed. The kernel marks the source page NotMapped+Locked while it
    is mapped (KPageTableBase::MapCodeMemory), so only the destination is
    read; an earlier build crashed by reading the source at this point.
    `guest_code_mapping_probe=reservation_failed` or
    `process_handle_unavailable` mean the test could not start;
    `reservation_retained=1 source_retained=1` means rollback failed and
    both ends of the possibly-live alias are deliberately kept;
    `source_retained=1 query=.. perm=..` alone means the unmap succeeded
    but the source did not query as heap RW afterwards.
    `guest_code_mapping_source_restored=1 content_match=..` means the
    unmap returned the source to heap RW and the bytes were re-read;
    content_match=1 confirms the round trip left them intact.
- `crash-NN.txt` - archived marker from a run that died mid-step, with the
  phase (`constructors` or `jni_onload`) it died in. The app recovers this
  automatically on the next launch; a fresh process re-running constructors
  is correct (their side effects died with the process).
- `constructor-watchdog.txt` - separate, append-only liveness observations
  for the experimental exclusive engine launch. The 2026-11-19 `link-44.txt`
  run executed and returned from constructors 0-2, but stopped recording at
  `constructor_begin_003` with the UI black and PLUS unresponsive. The next
  build creates a `watchdog_started=yes` record before entering guest code;
  if a constructor stays active for approximately 3, 10, 30, or another
  30-second interval, a separate libnx thread records its index, elapsed
  time and `guest_call_active` flag. A value of 1 means the guest call was
  entered and has not yet returned; 0 means the main thread is before the
  call or has returned and may be stalled in report synchronisation. The
  existing `constructor_begin_NNN` marker alone cannot distinguish those
  cases because it is written before a final fsync. A heartbeat proves the
  host process and monitoring thread were still running; it does not by
  itself identify why the call stopped returning. No
  heartbeat can mean a fatal, watcher startup failure, SD write failure or a
  blocked monitor, so compare the numbered report's
  `constructor_watchdog_started=` and `constructor_crash_marker_created=`
  fields before drawing a conclusion. This is diagnostic only: it does not
  skip constructors, terminate the guest, or claim JNI_OnLoad completed.
  The 2026-11-19 `link-46.txt` watchdog independently confirmed constructor
  index 3 stayed inside guest execution for at least 270 seconds; `link-48.txt`
  repeated the same stall for at least 10 seconds. The next build records
  `constructor_register_sampling=` in the link report and, where Horizon
  permits SVC 0x32/0x33, snapshots that thread's PC, LR, FP and SP at each
  heartbeat (`register_sample=ok`). The monitor pauses the thread only long
  enough to read registers, resumes it *before* any SD I/O, and records
  failure codes if the syscall is unavailable. A stable PC across samples
  suggests a tight loop or wait; a changing PC suggests forward execution.
  The `engine_module_registered=bias_0x...` line is the address base to
  subtract from a PC inside the mapped guest ELF. This samples execution;
  it does not change the Roblox library or bypass a constructor.
  The `link-50.txt` hardware sample succeeded twice, with an identical
  PC `0xFB774D14C` (guest virtual `0x26DD14C`), LR `0xFB774D014`
  (guest virtual `0x26DD014`) and unchanged stack frame at 3s/10s.
  That is evidence of a stable wait or spin, not of further constructor
  progress. The next build appends `instruction_status=...`, eight 32-bit
  `insn_-4` through `insn_+3` words around the sampled PC (after verifying
  that the entire range is readable and inside the pinned guest image),
  and x0-x28 register values. It reads only executable memory already
  mapped by Morrow; no guest binary modification or execution occurs.
  Hardware `link-52.txt` confirmed the actual instructions at guest
  `0x26DD14C` form an acquire-load loop: `ldar w8, [x19]`, `cmp w8, #2`,
  `b.ne` back to the load. The observed x8 is 1, so an initializer/state
  owner never publishes the completed state 2. This is a specific
  synchronization wait, not a graphics problem; it is not yet proof of
  recursion, a failed worker start, or which implementation owns the flag.
  The next diagnostic records a bounded, read-only frame-pointer chain
  (`frameN` / `returnN`) from the currently readable stack mapping, plus
  `bridge_creates`, `bridge_success`, `bridge_starts`, `bridge_returns`
  and `bridge_last_error`. The bridge counters are atomic so the watchdog
  can read them without taking a mutex that the stalled guest might own.
  An interrupted/optimized frame chain may be partial; a worker using a
  different creation route will not appear in these bridge counters.
  The first run after accepting an ignored anonymous mmap file descriptor
  (link-56) started the watchdog but died before its first three-second
  sample. The next diagnostic also appends short, durable
  trace_event=ctor_enter / ctor_return records for constructors 0-7 and
  later powers of two, and arena_mmap_*, arena_protect_* or arena_unmap_*
  events for the engine's observed 0x7b2000-byte anonymous allocator
  arena. These records carry the build ID and three hexadecimal arguments
  (a, b, c) and live in the same constructor-watchdog.txt file. The
  mmap entry arguments are length/flags/fd; success is
  base/length/kernel result. For mprotect, the entry arguments are
  base/length/protection and the return arguments are return
  code/errno/kernel result. For constructors, the arguments are
  index/guest target/zero. A last entry without its corresponding return
  narrows an early fatal but is not proof of where the fault occurred.
  In link-58 (0.99.6), constructors 0-2 returned, constructor 3 allocated
  a 0x7b2000-byte arena and the permission change returned success, but no
  constructor 3 completion or three-second watchdog sample followed.
  The follow-up probe records `arena_mmap_guest_lr` and
  `arena_protect_guest_lr` (return address of the guest call site), plus
  `arena_protect_query` (kernel query Result, actual permission and state).
  Watchdog snapshots are now scheduled at approximately 20, 100, 300,
  1,000, and 3,000 ms, before falling back to the former long intervals;
  their `elapsed_ms` fields distinguish early samples from older reports.
  This captures evidence without changing `libroblox.so` or bypassing a
  failed constructor. Host tests validate the schedule only: actual
  Switch liveness and gameplay remain unverified.
- `exception-last.txt` - best-effort userland fault context. Starting with
  the next diagnostic build, the exception records its own build ID, system
  tick and runtime address of `__libnx_exception_handler`. Match the build
  ID and tick against the numbered link report before attributing a fault:
  this file is only replaced on an exception and can belong to an earlier
  run. The handler address is an ASLR anchor for the separate
  `morrow-switch-jni-debug-symbols` CI artifact (`cordial-switched.elf`);
  comparing the fault PC with the handler address can distinguish a host
  code location from a guest code-mapping address. An exception may still
  bypass userland entirely and produce no fresh file.
- `system_property_read name=... result=empty` lines in the diagnostic log:
  the exact "what device is this?" questions the game asked, ready to drop
  into the presented-property table.

## What to send back

The whole `sdmc:/switch/cordial-switched/` folder (or the newest link-NN.txt,
constructor-watchdog.txt, exception-last.txt, and any crash-NN.txt). No
interpretation needed - the report is the data.

## Honest expectations

This is a measurement run, not a playability milestone. Best case: the
report shows all constructors completed and JNI_OnLoad returned. Likely
case: a named blocker or a crash marker tells us where to work next.
A hard crash with no new report lines means the failure is before or
inside the journaling itself - also useful, differently.
