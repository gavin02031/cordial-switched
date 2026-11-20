#define _POSIX_C_SOURCE 200809L
#include "android_process_compat.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

// Horizon does not offer a Linux process tree, executable replacement or
// Android application UID. Native host UIDs can be observed on desktop, but
// inventing an Android UID on Switch would make permission checks lie. Keep
// identity queries and process operations explicitly unsupported there.
static uint32_t guest_getuid(void)
{
#ifdef __SWITCH__
    errno = ENOSYS;
    return UINT32_MAX;
#else
    return (uint32_t)getuid();
#endif
}

static uint32_t guest_geteuid(void)
{
#ifdef __SWITCH__
    errno = ENOSYS;
    return UINT32_MAX;
#else
    return (uint32_t)geteuid();
#endif
}

static int32_t guest_getppid(void)
{
#ifdef __SWITCH__
    errno = ENOSYS;
    return -1;
#else
    return (int32_t)getppid();
#endif
}

static int guest_system(const char *command)
{
    // POSIX system(NULL) tests for a command interpreter. None exists in the
    // single-process guest, and a command must never be passed to Horizon.
    if (!command) return 0;
    errno = ENOSYS;
    return -1;
}

static int32_t guest_fork(void)
{
    errno = ENOSYS;
    return -1;
}

static int32_t guest_waitpid(int32_t pid, int *status, int options)
{
    (void)pid;
    (void)status;
    (void)options;
    errno = ENOSYS;
    return -1;
}

static int guest_execv(const char *path, char *const argv[])
{
    (void)path;
    (void)argv;
    errno = ENOSYS;
    return -1;
}

static int guest_execve(const char *path, char *const argv[], char *const envp[])
{
    (void)path;
    (void)argv;
    (void)envp;
    errno = ENOSYS;
    return -1;
}

static long guest_ptrace(int request, int32_t pid, void *address, void *data)
{
    (void)request;
    (void)pid;
    (void)address;
    (void)data;
    errno = ENOSYS;
    return -1;
}

static int guest_register_atfork(void (*prepare)(void), void (*parent)(void),
                                 void (*child)(void), void *dso)
{
    (void)prepare;
    (void)parent;
    (void)child;
    (void)dso;
    // Unlike libc's unversioned pthread_atfork, Bionic's __register_atfork
    // is a versioned registration with DSO lifetime. Pretending success would
    // lose required callbacks if the process ever acquired fork support.
    return ENOSYS;
}

bool android_process_compat_resolve(const char *name, const char *version,
                                    const char *library, uint64_t *address)
{
    if (!name || !version || !library || !address ||
        strcmp(version, "LIBC") || strcmp(library, "libc.so"))
        return false;
    *address = 0;
#define MAP_GUEST(symbol, target) \
    if (!strcmp(name, symbol)) *address = (uint64_t)(uintptr_t)&target; else
    MAP_GUEST("getuid", guest_getuid)
    MAP_GUEST("geteuid", guest_geteuid)
    MAP_GUEST("getppid", guest_getppid)
    MAP_GUEST("system", guest_system)
    MAP_GUEST("fork", guest_fork)
    MAP_GUEST("waitpid", guest_waitpid)
    MAP_GUEST("execv", guest_execv)
    MAP_GUEST("execve", guest_execve)
    MAP_GUEST("ptrace", guest_ptrace)
    MAP_GUEST("__register_atfork", guest_register_atfork)
    (void)0;
#undef MAP_GUEST
    return *address != 0;
}
