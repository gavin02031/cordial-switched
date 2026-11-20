#include "android_jump_compat.h"

#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// AOSP's ARM64 jmp_buf has 32 u64 slots: the saved return address and stack
// pointer start at word 2, x28..x19 occupy words 4..15, and d14..d8 occupy
// words 16..23. Word 24 holds our checksum; words 25..31 stay reserved. The two
// functions below share this layout but do not consume Bionic's cookie format.
// An Android buffer created by a different provider must fail closed.
enum {
    JUMP_MAGIC = 0,
    JUMP_THREAD = 1,
    JUMP_RETURN_PC = 2,
    JUMP_STACK = 3,
    JUMP_CHECKSUM = 24
};

static const uint64_t jump_magic = UINT64_C(0x4d4f52524a4d5031); // MORRJMP1

_Static_assert(MORROW_BIONIC_JUMP_WORDS == 32, "unexpected Bionic jump buffer");
_Static_assert(sizeof(pthread_t) <= sizeof(uint64_t), "unsupported thread identity");

static uint64_t native_thread_token(void)
{
    pthread_t native = pthread_self();
    uint64_t token = 0;
    // pthread_t need not be a pointer or integer. Compare its full native
    // representation without guessing what Horizon/newlib stores inside.
    memcpy(&token, &native, sizeof(native));
    return token;
}

static uint64_t jump_checksum(const uint64_t *buffer)
{
    uint64_t hash = UINT64_C(0x9e3779b97f4a7c15);
    for (unsigned i = 0; i < JUMP_CHECKSUM; ++i) {
        hash ^= buffer[i];
        hash = (hash << 11) | (hash >> 53);
        hash *= UINT64_C(0x100000001b3);
    }
    return hash;
}

void morrow_jump_seal(uint64_t *buffer)
{
    if (!buffer) abort();
    buffer[JUMP_MAGIC] = jump_magic;
    buffer[JUMP_THREAD] = native_thread_token();
    buffer[JUMP_CHECKSUM] = jump_checksum(buffer);
}

bool morrow_jump_valid(const uint64_t *buffer)
{
    return buffer && buffer[JUMP_MAGIC] == jump_magic &&
           buffer[JUMP_THREAD] == native_thread_token() &&
           buffer[JUMP_RETURN_PC] != 0 && buffer[JUMP_STACK] != 0 &&
           (buffer[JUMP_STACK] & 15u) == 0 &&
           buffer[JUMP_CHECKSUM] == jump_checksum(buffer);
}

void morrow_jump_require_valid(const uint64_t *buffer)
{
    // Aborting at this boundary makes a foreign, corrupted or cross-thread
    // buffer observable instead of branching into arbitrary guest memory.
    if (!morrow_jump_valid(buffer)) abort();
}

bool android_jump_compat_resolve(const char *name, const char *version,
                                 const char *library, uint64_t *address)
{
    if (!name || !version || !library || !address ||
        strcmp(version, "LIBC") != 0 || strcmp(library, "libc.so") != 0)
        return false;
#if defined(__aarch64__)
    if (strcmp(name, "setjmp") == 0) {
        *address = (uint64_t)(uintptr_t)&morrow_bionic_setjmp;
        return true;
    }
    if (strcmp(name, "longjmp") == 0) {
        *address = (uint64_t)(uintptr_t)&morrow_bionic_longjmp;
        return true;
    }
#endif
    // sigsetjmp/siglongjmp need a signal-mask bridge. Horizon does not have
    // Android's process signal-mask semantics, so leave them unresolved.
    return false;
}
