# Constructor-3 init path and shared once-guard mechanics (2026-11-20)

Static analysis of the guest ELF only (sha256 2ce7db6d..., as in the
syscall audit). No hardware observations here; external run claims
(link-46/48/50/52/58) remain unverified and are referenced only as
patterns to reconcile, not evidence.

## The ctor-3 chain, grounded

- ctor[3] = 0x1b2fe48 (init_array slot, relocation-derived; no direct
  callers, as expected for a constructor entry).
- Its first call (BL at 0x1b2fe7c) targets 0x26dc68c, a common runtime
  helper with 8,988 direct callers - its presence in the chain carries
  little discriminating weight.
- Within that helper's address range, BL at 0x26dc77c targets 0x26dce4c;
  within 0x26dce4c, BL at 0x26dcf38 targets the once-function at
  0x26dcfdc. That BL is the once-function's ONLY direct caller, and no
  relocation-derived pointer to 0x26dcfdc exists. (Intra-function
  attribution is by address ranges, a heuristic.)
- The once-function's init body calls the arena-reserve function
  0x26e63a8 (BL at 0x26dd04c). Arena-reserve's 9 direct callers are
  exactly the 9 ensure-init sites described below.

## The once-guard protocol

Guard word: 0x677bf90 (guest .bss, so fresh per process). The
once-function entry is 0x26dcfdc (earlier notes cited 0x26dcffc; that is
the slow-path block start, reached by fallthrough):

- Fast path: ldar guard; if == 2, return immediately (b.eq 0x26dd158).
- Claim: helper 0x2747a50 is a plain atomic CAS(expected=0, desired=1)
  returning the OLD value (LSE casa, or ldaxr/stxr fallback selected by
  the feature byte at 0x677ddb8). Old == 0: this thread runs init.
  Old == 2: already published, done. Otherwise spin at
  0x26dd144-0x26dd154 (ldar; cmp #2; b.ne) until publication.
- The CAS has NO recursion detection and no abort path: if the claiming
  thread re-enters ANY ensure-site on the same guard before publishing,
  its own claim returns 1 and it spins forever. Self-deadlock, exact
  mechanism confirmed. (Earlier structural finding, now with the exact
  atomic semantics.)
- Publish: after init, the struct pointer is stored at guard+8
  (0x677bf98) and stlr releases guard = 2 (0x26dd12c).

## One arena, one guard, twelve ensure-sites

Twelve code sites reference guard 0x677bf90: the once-function and
others at 0x26ea710, 0x26eba34, 0x26ebb54, 0x26ebb80, 0x26eccfc,
0x26ecf00, 0x26ed044, 0x26ed188, 0x26ed2cc, 0x26ed410 (plus the
once-function's two reference points). Nine of them immediately precede
the nine arena-reserve call sites and run the identical
ldar/CAS/spin/publish protocol. The once-function and site 0x26ea710
both initialize the SAME global struct at 0x6760e80 (verified);
the remaining sites are presumed to as well by their identical protocol
(INFERRED for the unexamined ones).

Consequence: the deadlock surface is wider than recursion into the
once-function. Re-entry through ANY ensure-site - for example an
allocation inside an error or logging callback during init - deadlocks
the claiming thread on its own claim.

## Reconciling the external claim patterns (conditional)

Within a single fresh process: if ctors 0-2 completed without touching
this guard, ctor 3 is plausibly the first claimer. Two static mechanisms
then fit the claimed observations:

1. Fault inside init after the successful mprotect. The statically first
   guest dereferences of the arena on the reviewed success path are the
   slot-loop stores at 0x26e649c-0x26e64a0 (see the syscall audit,
   Revision 3; the intervening call is a verified leaf atomic-add that
   cannot touch the arena). A fault there ends the run with the guard
   left at 1. Fits link-58's claimed "mprotect success, then silence".
2. Re-entry self-deadlock: something in init calls back into an
   ensure-path; the claiming thread spins at 0x26dd14c (the site whose
   instructions link-52 claimed to sample). Fits a stable-PC-at-spin
   observation directly.

A spin observed on a DIFFERENT thread than the claimer would fit (1)
with a concurrent ensure-path call. Choosing between these requires the
original hardware evidence (exception records, sampled thread identity
and stacks), which is not in our possession. No causal diagnosis is made
here.

## Cross-references

- switch/docs/morrow-v099-syscall-audit.md (Revision 3): post-mprotect
  path, first-dereference analysis, census scope.
- switch/docs/console-run-playbook.md: report-field glossary; link-58
  narrative as recorded by its external author (unverified).
