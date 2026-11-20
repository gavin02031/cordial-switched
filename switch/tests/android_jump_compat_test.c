#include "android_jump_compat.h"

#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void *validate_other_thread(void *opaque)
{
    assert(!morrow_jump_valid((const uint64_t *)opaque));
    return NULL;
}

static void expect_invalid_jump_aborts(uint64_t *buffer)
{
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        // The guard is expected to abort; an ordinary return would be a bug.
        FILE *redirected = freopen("/dev/null", "w", stderr);
        assert(redirected == stderr);
        morrow_jump_require_valid(buffer);
        _exit(0);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}

#if defined(__aarch64__)
__attribute__((noinline)) static void jump_back(uint64_t *buffer, int value)
{
    morrow_bionic_longjmp(buffer, value);
}

static void test_real_aarch64_jumps(void)
{
    uint64_t buffer[MORROW_BIONIC_JUMP_WORDS] = {0};
    volatile unsigned visits = 0;
    int returned = morrow_bionic_setjmp(buffer);
    if (returned == 0) {
        assert(morrow_jump_valid(buffer));
        visits++;
        jump_back(buffer, 47);
    }
    assert(returned == 47 && visits == 1);

    returned = morrow_bionic_setjmp(buffer);
    if (returned == 0) {
        visits++;
        jump_back(buffer, 0);
    }
    assert(returned == 1 && visits == 2);
}
#endif

int main(void)
{
    uint64_t address = UINT64_C(0x12345678);
    assert(!android_jump_compat_resolve("setjmp", "LIBC_N", "libc.so", &address));
    assert(address == UINT64_C(0x12345678));
    assert(!android_jump_compat_resolve("longjmp", "LIBC", "libm.so", &address));
    assert(!android_jump_compat_resolve("sigsetjmp", "LIBC", "libc.so", &address));
    assert(!android_jump_compat_resolve("siglongjmp", "LIBC", "libc.so", &address));
    assert(!android_jump_compat_resolve("setjmp", NULL, NULL, &address));
#if defined(__aarch64__)
    assert(android_jump_compat_resolve("setjmp", "LIBC", "libc.so", &address));
    assert(address == (uint64_t)(uintptr_t)&morrow_bionic_setjmp);
    assert(android_jump_compat_resolve("longjmp", "LIBC", "libc.so", &address));
    assert(address == (uint64_t)(uintptr_t)&morrow_bionic_longjmp);
#else
    assert(!android_jump_compat_resolve("setjmp", "LIBC", "libc.so", &address));
    assert(!android_jump_compat_resolve("longjmp", "LIBC", "libc.so", &address));
#endif

    // These guard tests run on both native x86 hosts and emulated ARM64.
    // They do not reinterpret a newlib/glibc jmp_buf as an Android one.
    uint64_t buffer[MORROW_BIONIC_JUMP_WORDS] = {0};
    buffer[2] = UINT64_C(0x1234);
    buffer[3] = (uintptr_t)buffer & ~(uintptr_t)15;
    morrow_jump_seal(buffer);
    assert(morrow_jump_valid(buffer));
    pthread_t thread;
    assert(pthread_create(&thread, NULL, validate_other_thread, buffer) == 0);
    assert(pthread_join(thread, NULL) == 0);
    buffer[4] ^= 0x11;
    assert(!morrow_jump_valid(buffer));
    expect_invalid_jump_aborts(buffer);
    buffer[4] ^= 0x11;
    assert(morrow_jump_valid(buffer));
    buffer[3] |= 1;
    morrow_jump_seal(buffer);
    assert(!morrow_jump_valid(buffer));

#if defined(__aarch64__)
    test_real_aarch64_jumps();
#endif
    puts("Bionic jump-compat guard tests passed");
    return 0;
}
