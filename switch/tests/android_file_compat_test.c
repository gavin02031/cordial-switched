#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "android_file_compat.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>

static uint64_t resolve(const char *name)
{
    uint64_t address = 0;
    assert(android_file_compat_resolve(name, "LIBC", "libc.so", &address));
    assert(address);
    assert(!android_file_compat_resolve(name, "LIBC_N", "libc.so", &address));
    assert(!android_file_compat_resolve(name, "LIBC", "libm.so", &address));
    return address;
}
#define LOAD(name, result, args) result (*guest_##name) args = (result (*) args)(uintptr_t)resolve(#name)
int main(void)
{
    LOAD(open, int, (const char *, int, ...));
    LOAD(__open_2, int, (const char *, int));
    LOAD(stat, int, (const char *, AndroidFileStat *));
    LOAD(lstat, int, (const char *, AndroidFileStat *));
    LOAD(fstat, int, (int, AndroidFileStat *));
    LOAD(lseek, int64_t, (int, int64_t, int));
    LOAD(pread, int64_t, (int, void *, uint64_t, int64_t));
    LOAD(pread64, int64_t, (int, void *, uint64_t, int64_t));
    LOAD(pwrite, int64_t, (int, const void *, uint64_t, int64_t));
    LOAD(ftruncate, int, (int, int64_t));
    LOAD(opendir, void *, (const char *));
    LOAD(readdir, AndroidDirectoryEntry *, (void *));
    LOAD(closedir, int, (void *));
    char directory[] = "/tmp/morrow-files-XXXXXX";
    assert(mkdtemp(directory));
    char path[256], link[256];
    snprintf(path, sizeof(path), "%s/data", directory);
    snprintf(link, sizeof(link), "%s/link", directory);
    // Android O_RDWR|O_CREAT|O_EXCL, intentionally not host constants.
    int fd = guest_open(path, 2 | 0100 | 0200, 0600u);
    assert(fd >= 0);
    assert(guest_open(path, 2 | 0100 | 0200, 0600u) == -1 && errno == EEXIST);
    assert(write(fd, "abc", 3) == 3);
    assert(guest_pwrite(fd, "Z", 1, 1) == 1);
    assert(guest_lseek(fd, 0, 1) == 3);
    char bytes[8] = {0};
    assert(guest_pread(fd, bytes, 3, 0) == 3 && !strcmp(bytes, "aZc"));
    assert(guest_pread64(fd, bytes, 3, 0) == 3);
    assert(guest_lseek(fd, 0, 1) == 3);
    assert(guest_lseek(fd, 0, 17) == -1 && errno == EINVAL);
    assert(guest_pread(fd, bytes, UINT64_MAX, 0) == -1 && errno == EINVAL);
    assert(guest_pread(-1, bytes, 1, 0) == -1 && errno == EBADF);
    assert(guest_pwrite(fd, "x", 1, -1) == -1 && errno == EINVAL);
    struct { uint64_t before; AndroidFileStat stat; uint64_t after; } guarded;
    memset(&guarded, 0xa5, sizeof(guarded));
    assert(guest_fstat(fd, &guarded.stat) == 0);
    assert(guarded.before == UINT64_C(0xa5a5a5a5a5a5a5a5));
    assert(guarded.after == guarded.before);
    assert(guarded.stat.size == 3 && (guarded.stat.mode & 0170000) == 0100000);
    assert(!guarded.stat.padding1 && !guarded.stat.padding2 && !guarded.stat.unused4);
    struct stat native;
    assert(fstat(fd, &native) == 0);
    assert(guarded.stat.inode == native.st_ino);
    assert(guarded.stat.modify_sec == native.st_mtim.tv_sec);
    assert(guarded.stat.modify_nsec == native.st_mtim.tv_nsec);
    // A high offset catches accidental 32-bit truncation without allocating gigabytes.
    const int64_t large = INT64_C(4294967296) + 7;
    assert(guest_pwrite(fd, "Q", 1, large) == 1);
    assert(guest_fstat(fd, &guarded.stat) == 0 && guarded.stat.size == large + 1);
    assert(guest_pread64(fd, bytes, 1, large) == 1 && bytes[0] == 'Q');
    assert(guest_ftruncate(fd, 3) == 0);
    assert(guest_ftruncate(fd, -1) == -1);
    assert(close(fd) == 0);
    // Unsupported flags must be rejected before O_TRUNC changes the file.
    assert(guest_open(path, 1 | 01000 | 010000000) == -1 && errno == EINVAL);
    assert(guest_stat(path, &guarded.stat) == 0 && guarded.stat.size == 3);
    assert(guest_open(path, 3 | 01000) == -1 && errno == EINVAL);
    assert(symlink(path, link) == 0);
    assert(guest_lstat(link, &guarded.stat) == 0);
    assert((guarded.stat.mode & 0170000) == 0120000);
    assert(guest_stat(link, &guarded.stat) == 0 && guarded.stat.size == 3);
    assert(guest_stat(path, NULL) == -1 && errno == EFAULT);
    memset(&guarded.stat, 0x5a, sizeof(guarded.stat));
    AndroidFileStat sentinel = guarded.stat;
    assert(guest_fstat(-1, &guarded.stat) == -1 && errno == EBADF);
    assert(!memcmp(&sentinel, &guarded.stat, sizeof(sentinel)));
    fd = guest___open_2(path, 0);
    assert(fd >= 0 && close(fd) == 0);
    void *dir = guest_opendir(directory);
    assert(dir);
    int found = 0;
    AndroidDirectoryEntry *entry;
    while ((entry = guest_readdir(dir))) {
        assert(entry->record_bytes >= 24 && !(entry->record_bytes & 7));
        assert(entry->record_bytes <= sizeof(*entry));
        if (!strcmp(entry->name, "data")) found++;
    }
    assert(found == 1);
    errno = EDOM;
    assert(guest_readdir(dir) == NULL && errno == EDOM);
    assert(guest_closedir(dir) == 0);
    assert(guest_opendir(path) == NULL && errno == ENOTDIR);
    struct rlimit no_core = {0, 0};
    assert(setrlimit(RLIMIT_CORE, &no_core) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) { guest___open_2(path, 0100); _exit(99); }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    assert(unlink(link) == 0 && unlink(path) == 0 && rmdir(directory) == 0);
    puts("android_file_compat_test: layouts, flags, 64-bit offsets, directories and failures passed");
}
