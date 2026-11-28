# Cordial Switched v0.94: ELF constructor preflight

Android loads each shared library's `DT_INIT` and then the entries in
`DT_INIT_ARRAY` before calling its `JNI_OnLoad`. The native Switch preview
previously only counted these entries; it did not check their relocated
function pointers against its final executable memory map.

The new `engine_constructor_plan` reads the **non-executable** staged ELF
image after strict relocation succeeds and verifies:

- The `DT_INIT_ARRAY` is aligned and fully file-backed within a `PT_LOAD`.
- The intended runtime address matches the loader's final base and alignment.
- `DT_INIT` and each non-sentinel array entry point to aligned RX pages,
  never writable data, unmapped holes or out-of-range addresses.
- Zero and all-ones pointers are skipped, as in Bionic's own linker.
- Call targets are preserved in `DT_INIT`, then array order, for future
  integration with a complete runtime.

The `link-XX.txt` report gains `constructor_plan_status`,
`constructor_plan_reason`, `constructor_plan_array_entries`,
`constructor_plan_skipped_entries`, `constructor_plan_validated_targets`,
`constructor_plan_first_invalid_index`, and `constructor_plan_executed=no`.
These diagnostics only appear after the **strict** stage succeeds; a failed
import or TLS relocation still blocks it. A failed constructor preflight
also explicitly rejects entry into `JNI_OnLoad`.

Synthetic tests cover valid relocated addresses, null/-1 sentinels, malformed
arrays, unaligned entries, RW/outside-image pointers, a bad final base and an
incomplete staging image. No Roblox code, private engine binary, or game
constructor is executed by these tests.

**Still blocked:** Android thread-pointer/TLS setup, unsupported relocations,
unresolved imports and the real Android JNI/graphics/lifecycle runtime. The
preflight **does not execute constructors**, so it cannot lift the current
startup guard. No first Roblox frame or login has been observed on Switch.
The last hardware-confirmed import result remains v0.86's 437/629 supported,
192 missing; v0.94 does not claim to change it.

Reference: [AOSP Bionic linker constructor ordering and sentinel handling](https://android.googlesource.com/platform/bionic.git/+/3d4918e0d3e5b38ed00e7d048be266257137f90a/linker/linker_soinfo.cpp).
