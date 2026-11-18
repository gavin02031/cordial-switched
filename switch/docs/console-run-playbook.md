# Console run playbook (draft for the next supervised run)

What the next run on Gavin's Switch Lite answers, where the evidence lands,
and how to read it. Nothing here has executed on hardware yet.

## What to install

The CI "Horizon preview" workflow's `cordial-switched-switch-preview`
artifact (the .nro) from the latest GREEN run on branch
feat/switch-horizon-smoke-test, plus libroblox.so at the path the engine
probes (ENGINE_LIBRARY_PATH). Copy the .nro onto the SD card and launch
through the Homebrew Menu as before.

## What the run will do, in order

1. Probe and audit libroblox.so, map it, bind the 576 imports.
2. Run the engine's startup constructors (milestone B), each leaving a
   durable marker before and after it runs.
3. Only if every constructor completes cleanly: enter JNI_OnLoad.
4. Refuse honestly, with a named blocker, at the first thing that fails.

## Where the evidence lands (SD card)

`sdmc:/switch/cordial-switched/`
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
  - `event=entering_real_engine_jni_onload` - only appears if every
    constructor finished; then `engine_jni_onload_return=0x00010006` is a
    clean return.
  - `blocker=...` - the named refusal if it stops.
- `crash-NN.txt` - archived marker from a run that died mid-step, with the
  phase (`constructors` or `jni_onload`) it died in. The app recovers this
  automatically on the next launch; a fresh process re-running constructors
  is correct (their side effects died with the process).
- `system_property_read name=... result=empty` lines in the diagnostic log:
  the exact "what device is this?" questions the game asked, ready to drop
  into the presented-property table.

## What to send back

The whole `sdmc:/switch/cordial-switched/` folder (or the newest link-NN.txt
and any crash-NN.txt). No interpretation needed - the report is the data.

## Honest expectations

This is a measurement run, not a playability milestone. Best case: the
report shows all constructors completed and JNI_OnLoad returned. Likely
case: a named blocker or a crash marker tells us where to work next.
A hard crash with no new report lines means the failure is before or
inside the journaling itself - also useful, differently.
