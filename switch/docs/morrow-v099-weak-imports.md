# v0.99: optional ELF imports

Run Loader → L3 and inspect the newest `sdmc:/switch/cordial-switched/link-XX.txt`.
This version permits absent `STB_WEAK` imports for AArch64 ABS64, GLOB_DAT and
JUMP_SLOT relocations, using ELF's zero symbol value plus the explicit addend.
Available providers still take precedence. Required imports, invalid metadata,
TLS and IFUNC retain their existing checks.

The report adds `strict_weak_undefined_relocations` and
`strict_required_missing_relocations`, with corresponding `full_audit_*`
fields when strict linking fails. These count relocation sites, not unique
symbols. Existing `*_missing_imports` counts still describe failed provider
lookups, including optional weak imports; they are not counts of mandatory
link failures. No new Android provider is claimed by this change.

Synthetic tests verify all three relocation types, provider precedence,
required-symbol rejection, malformed version rejection and absent weak dyninit
boundaries. The host Switch test job passes. The Linux Cargo build and tests
could not run in this workspace because Cargo is not installed.

**INFERRED / awaiting hardware:** whether this removes the observed
`__start_ls_rbx_dyninit1` blocker depends on its binding in the actual Android
library. A strong reference still requires a verified section or real provider.
No newer hardware log was available during implementation. The last observed
hardware count remains 534/629 supported and 95 missing in v0.97. v0.99 has not
been shown to execute Roblox, render a frame, sign in or join a game.
