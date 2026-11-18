# Morrow development handoff — Flint (Instinct)

**Prepared:** 2026-11-18 (America/Chicago)

**Repository:** [gavin02031/cordial-switched](https://github.com/gavin02031/cordial-switched)

**Active development PR:** [#1](https://github.com/gavin02031/cordial-switched/pull/1), draft, `feat/switch-horizon-smoke-test`

## Decision: Flint leads implementation; Codex researches hourly

Gavin changed the collaboration model on 2026-11-18. **Flint has exclusive ownership of implementation**, including code, tests, build configurations, CI, integration, releases and the development branch. **Codex is a research and review contributor only**. It will inspect evidence hourly and may commit substantive, evidence-backed suggestions to one dedicated plain-text file:

`switch/docs/CODEX_RESEARCH_NOTES.txt`

No separate code-development branch or merge protocol is requested for Codex. Codex must not edit any other repository file, including this handoff. Flint decides whether and how to implement a recommendation. Research must never be represented as implemented or hardware-verified. If no substantive finding exists, Codex makes no commit.

The existing scheduled task, `Automation_1992e3039e0081919eb976303a2073cc`, was changed to **Morrow Hourly Research & Suggestions**. It remains hourly (minute 28) and research-only; its repository write scope is restricted to the dedicated notes file. The task should report to Gavin even when a new finding does not warrant a commit.

## Source baseline and outstanding local state

The verified GitHub source baseline before this *documentation-only* handoff is:

| Item | Value |
| --- | --- |
| Remote branch | `feat/switch-horizon-smoke-test` |
| Source commit | [`1153d906243240e58716d2cce1f31d01bd081ad7`](https://github.com/gavin02031/cordial-switched/commit/1153d906243240e58716d2cce1f31d01bd081ad7) |
| Preview version | `0.99.6` (`switch/Makefile`) |
| Git tree | `081356d1d3115e1dd501c640312331e926054d68` |
| Local Codex HEAD | `7d3da9b9f2c4caf5852cd94e3ec25ae510e856b9` |
| Local uncommitted/untracked files before handoff preparation | None |
| Local code difference from remote | None; both source commits have the same tree |

The local commit object differs from the published GitHub commit even though their file trees are identical. **Use GitHub's remote HEAD as the integration base.** Do not push the local commit over Flint's changes. The handoff and research-notes files are the only repository additions authorised for this transition.

## What has actually been verified

On source commit `1153d906…`, all **five Horizon-specific CI jobs passed**: standard NRO build, JNI-enabled NRO cross-build, ARM64 compatibility tests, ELF/provider tests, and JNI provider host tests. [Horizon CI run](https://github.com/gavin02031/cordial-switched/actions/runs/44255111852).

Codex also re-executed the 15 offline audit/export Python tests and the host process compatibility unit test successfully. These are host/synthetic tests; they do **not** execute Roblox.

The broader repository [test run](https://github.com/gavin02031/cordial-switched/actions/runs/44255111758) finished **failure**: the x86-64, scripts and issue-form jobs passed, but the ARM64 desktop `cordial-linker-sys` build failed in the third-party linker compilation with `unknown type name '__u128'` from `/usr/include/asm/sigcontext.h`. This is a separate desktop CI failure, not a failure of the Horizon NRO workflow.

### Last available Switch hardware evidence

The latest actual console report made available in the development conversation was the **v0.99.4** engine readiness diagnostic. Its results are *historical*, not a measurement of v0.99.6:

| v0.99.4 hardware field | Observed value |
| --- | --- |
| ELF file size | 108,360,288 bytes |
| Load segments / dependencies | 3 / 10 |
| Final code address reserved | Yes |
| Total / resolved / missing import lookups | 629 / 562 / 67 |
| Weak unresolved references | 2 |
| Required missing relocation sites in full audit | 65 |
| Strict first required missing name | `geteuid@LIBC` |
| Initialization-array entries | 2,505 |
| Synthetic JNI_OnLoad probe | Passed; **not Roblox code** |
| Guest memory probe | `zero=1 read_only=0 data=0 none=0 rw=0 released=1` |
| Guest constructors / real JNI_OnLoad / guest EGL calls | None observed |
| `engine_entry_called` / `engine_executed` | `no` / `no` |

The raw report was supplied directly by Gavin and has no stable public GitHub link; the related findings and definitions are documented in [playability-milestones.md](playability-milestones.md). No subsequent physical Switch execution report is available in this handoff.

**Last confirmed real Roblox engine-execution milestone: none.** The last confirmed hardware milestone is ELF inspection, synthetic JNI probing, final code-address reservation, and a completed read-only relocation audit. Successful NRO builds, the launcher triangle, and synthetic JNI callbacks must not be described as a Roblox frame or engine execution.

**Unknown:** exact remaining import count on v0.99.6; actual guest executable mapping; whether constructor or SVC paths run; JNI startup; EGL guest surface/context; login, online play, and FPS.

## Runtime critical path for Flint

The following paths and functions exist in the verified source baseline. They are not claims that the functionality is complete.

1. **Required imports and relocation:** `switch/source/horizon_real_link.c` → `horizon_real_link_run()` calls `engine_stage_stream_resolved_at()` / `engine_stage_stream_audit_at()` in `switch/source/engine_stage.c`. The v0.99.4 strict pass stopped at `geteuid`; v0.99.6 added a *fail-closed* versioned provider in `android_process_compat.c`, but a new complete target-observed provider inventory has not established Milestone A. Resolve groups of missing functions with truthful semantics, preserving symbol versions and provider identities. The audit tooling is `switch/tools/export_horizon_providers.py` and `audit_android_elf.py`.
2. **Guest memory and permission transitions:** `switch/source/android_mapping_compat.c` → `protect_pages()` / `guest_mprotect()` and `switch/source/horizon_code_memory.c` → `horizon_code_memory_map_reserved()`. The v0.99.4 permission probe returned false for all four protection/data checks despite successful allocation and release; record actual kernel return codes before inferring a cause.
3. **Constructors and dependency initialization:** `switch/source/engine_constructor_plan.c` → `engine_constructor_plan_prepare()` validates targets. `switch/source/engine_constructor_run.c` → `engine_constructor_run_once()` is a synthetic callback controller, not wired to live guest code. `switch/source/engine_execution_attempt.c` → `engine_execution_decide()` refuses real JNI_OnLoad while DT_INIT/DT_INIT_ARRAY remain unrun. The observed library had 2,505 initialization-array entries. Do not bypass the gate without durable crash journaling, dependency ordering, mapping lifetime and guest thread/TLS correctness.
4. **Raw Linux syscall candidates:** the independent private audit documented 16 `svc #0` candidate instructions in `.text`, seven with immediately preceding `getpid`, `gettid` or `futex` syscall-number loads. Runtime reachability is unknown. `switch/source/horizon_exception_log.c` → `__libnx_exception_handler()` is best-effort diagnostics, not a syscall translator; it may not receive faults from dynamically mapped code. No guest instruction patching or prohibited hook path should be introduced.
5. **Android runtime and activity lifecycle:** `switch/source/morrow_jni_runtime.cpp` contains the JNI VM/class bridge, partial typed App Bridge parameters and synthetic probes. `switch/source/horizon_real_link.c` contains guarded GameActivity, lifecycle and App Bridge calls. APK classpath, application initialization, Java callbacks and real account/authentication semantics remain incomplete. A synthetic JNI success is not a real Roblox JNI_OnLoad return.
6. **Real guest graphics:** `switch/source/android_egl_gles_compat.c` → `checked_create_window_surface()` and `switch/source/android_native_window_compat.c` bridge guest EGL window references to Horizon. Symbol linkage alone does not establish a guest-created EGL surface, active context or visible Roblox frame. `switch/source/main.c` manages the exclusive graphics/engine path; last hardware report used diagnostic mode and did not enter it.
7. **Game join and performance:** No verified native Roblox login, online session, joined game, gameplay inputs or FPS exist. Reserve v1.0 for working game execution, not a passing emulator/launcher smoke test.

## File ownership and workflow

| Owner | Scope |
| --- | --- |
| **Flint** | All `switch/source/` code, `switch/tests/`, ELF/JNI/GLES integrations, `switch/Makefile`, workflows, UI, user documentation, the active PR and releases |
| **Codex** | Read-only research/review across the repository; **only** writes `switch/docs/CODEX_RESEARCH_NOTES.txt` when a new substantiated recommendation exists |
| **Gavin** | Priorities, hardware testing when uniquely necessary, final acceptance of playable v1.0 |

The import resolver (`switch/source/android_import_resolver.c`), central loader (`switch/source/horizon_real_link.c`), CI workflow and milestone document are **Flint-owned**. Codex will propose changes to them as notes, never apply them. Flint need not cherry-pick or merge anything produced by the hourly task. Findings should include priority, source/evidence URL, precise file/function, actionable implementation suggestion, proposed verification, and unknowns.

No proprietary `libroblox.so`, APK, credentials, private download URL or game assets belong in the repository, GitHub comments or CI artifacts. Use `AGENTS.md` as the controlling project safety and test guidance.

**Immediate next work for Flint:** verify the latest full target import inventory, investigate the observed permission-probe failures, then work toward a real journaled constructor run and only afterward a guarded Roblox JNI_OnLoad. Progress is measured by new target observations, not version-number increments.
