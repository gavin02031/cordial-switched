#ifndef MORROW_ANDROID_JUMP_COMPAT_H
#define MORROW_ANDROID_JUMP_COMPAT_H

#include <stdbool.h>
#include <stdint.h>

// Bionic ARM64 reserves 32 eight-byte words for jmp_buf. Our two guest
// functions must use one private format; a native newlib jmp_buf is not it.
enum { MORROW_BIONIC_JUMP_WORDS = 32 };

bool android_jump_compat_resolve(const char *name, const char *version,
                                 const char *library, uint64_t *address);

// Invoked by the ARM64 assembly after saving the guest callee-saved state.
void morrow_jump_seal(uint64_t *buffer);
bool morrow_jump_valid(const uint64_t *buffer);
void morrow_jump_require_valid(const uint64_t *buffer);

#if defined(__aarch64__)
__attribute__((returns_twice)) int morrow_bionic_setjmp(uint64_t *buffer);
__attribute__((noreturn)) void morrow_bionic_longjmp(uint64_t *buffer, int value);
#endif

#endif
