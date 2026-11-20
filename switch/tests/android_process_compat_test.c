#define _POSIX_C_SOURCE 200809L
#include "android_process_compat.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static uint64_t resolve(const char *name)
{
    uint64_t address = 0;
    assert(android_process_compat_resolve(name, "LIBC", "libc.so", &address));
    assert(address != 0);
    return address;
}

static void test_guest_identity(void)
{
    uint32_t (*guest_uid)(void) = (void *)(uintptr_t)resolve("getuid");
    uint32_t (*guest_euid)(void) = (void *)(uintptr_t)resolve("geteuid");
    int32_t (*guest_parent)(void) = (void *)(uintptr_t)resolve("getppid");
#ifdef __SWITCH__
    errno = 0;
    assert(guest_uid() == UINT32_MAX && errno == ENOSYS);
    errno = 0;
    assert(guest_euid() == UINT32_MAX && errno == ENOSYS);
    errno = 0;
    assert(guest_parent() == -1 && errno == ENOSYS);
#else
    assert(guest_uid() == (uint32_t)getuid());
    assert(guest_euid() == (uint32_t)geteuid());
    assert(guest_parent() == (int32_t)getppid());
#endif
}

static void test_process_operations_fail_without_creating_children(void)
{
    int (*guest_system)(const char *) = (void *)(uintptr_t)resolve("system");
    int32_t (*guest_fork)(void) = (void *)(uintptr_t)resolve("fork");
    int32_t (*guest_wait)(int32_t, int *, int) =
        (void *)(uintptr_t)resolve("waitpid");
    int (*guest_execv)(const char *, char *const[]) =
        (void *)(uintptr_t)resolve("execv");
    int (*guest_execve)(const char *, char *const[], char *const[]) =
        (void *)(uintptr_t)resolve("execve");
    long (*guest_ptrace)(int, int32_t, void *, void *) =
        (void *)(uintptr_t)resolve("ptrace");
    int (*guest_atfork)(void (*)(void), void (*)(void), void (*)(void), void *) =
        (void *)(uintptr_t)resolve("__register_atfork");

    errno = 0;
    assert(guest_system(NULL) == 0 && errno == 0);
    errno = 0;
    assert(guest_system("echo should-not-execute") == -1 && errno == ENOSYS);
    errno = 0;
    assert(guest_fork() == -1 && errno == ENOSYS);
    int status = 123;
    errno = 0;
    assert(guest_wait(-1, &status, 0) == -1 && errno == ENOSYS);
    assert(status == 123);
    char *args[] = {(char *)"does-not-execute", NULL};
    char *env[] = {(char *)"A=B", NULL};
    errno = 0;
    assert(guest_execv("/bin/true", args) == -1 && errno == ENOSYS);
    errno = 0;
    assert(guest_execve("/bin/true", args, env) == -1 && errno == ENOSYS);
    errno = 0;
    assert(guest_ptrace(0, 1, NULL, NULL) == -1 && errno == ENOSYS);
    assert(guest_atfork(NULL, NULL, NULL, NULL) == ENOSYS);
}

int main(void)
{
    test_guest_identity();
    test_process_operations_fail_without_creating_children();
    const char *names[] = {"getuid", "geteuid", "getppid", "system", "fork",
                           "waitpid", "execv", "execve", "ptrace", "__register_atfork"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        uint64_t address = 0x1234;
        assert(!android_process_compat_resolve(names[i], "LIBC_N", "libc.so", &address));
        assert(!android_process_compat_resolve(names[i], "LIBC", "libdl.so", &address));
        assert(!android_process_compat_resolve(names[i], NULL, NULL, &address));
    }
    uint64_t address = 0;
    assert(!android_process_compat_resolve("getgid", "LIBC", "libc.so", &address));
    assert(!address);
    return 0;
}
