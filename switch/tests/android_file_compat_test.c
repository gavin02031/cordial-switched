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
    LOAD(fcntl, int, (int, int, ...));
    LOAD(stat, int, (const char *, AndroidFileStat *));
    LOAD(lstat, int, (const char *, AndroidFileStat *));
    LOAD(fstat, int, (int, AndroidFileStat *));
    LOAD(lseek, int64_t, (int, int64_t, int));
    LOAD(ftruncate, int, (int, int64_t));
    LOAD(fchmod, int, (int, mode_t));
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
    assert(guest_fchmod(fd, 0640) == 0);
    struct stat permissions;
    assert(fstat(fd, &permissions) == 0 && (permissions.st_mode & 0777) == 0640);
    assert(guest_fchmod(fd, 0100000) == -1 && errno == EINVAL);
    assert(fstat(fd, &permissions) == 0 && (permissions.st_mode & 0777) == 0640);
    assert(guest_fchmod(-1, 0600) == -1 && errno == EBADF);
    uint64_t ignored_owner = 0;
    assert(!android_file_compat_resolve("fchown", "LIBC", "libc.so", &ignored_owner));
    assert(guest_open(path, 2 | 0100 | 0200, 0600u) == -1 && errno == EEXIST);
    assert(write(fd, "abc", 3) == 3);
    assert(guest_lseek(fd, 1, 0) == 1);
    assert(write(fd, "Z", 1) == 1);
    assert(guest_lseek(fd, 0, 2) == 3);
    assert(guest_lseek(fd, 0, 0) == 0);
    char bytes[8] = {0};
    assert(read(fd, bytes, 3) == 3 && !strcmp(bytes, "aZc"));
    assert(guest_lseek(fd, 0, 1) == 3);
    assert(guest_lseek(fd, 0, 17) == -1 && errno == EINVAL);
    assert(guest_lseek(-1, 0, 0) == -1 && errno == EBADF);
    uint64_t positional = 0;
    assert(android_file_compat_resolve("pread", "LIBC", "libc.so", &positional));
    ssize_t (*guest_pread)(int, void *, size_t, off_t) = (void *)(uintptr_t)positional;
    assert(android_file_compat_resolve("pread64", "LIBC", "libc.so", &positional));
    assert(positional == (uintptr_t)guest_pread);
    assert(android_file_compat_resolve("pwrite", "LIBC", "libc.so", &positional));
    ssize_t (*guest_pwrite)(int, const void *, size_t, off_t) = (void *)(uintptr_t)positional;
    assert(guest_pread(fd, bytes, 2, 1) == 2 && !memcmp(bytes, "Zc", 2));
    assert(guest_lseek(fd, 0, 1) == 3);
    assert(guest_pwrite(fd, "Z", 1, 1) == 1);
    assert(guest_lseek(fd, 0, 1) == 3);
    assert(guest_pread(fd, bytes, 2, 3) == 0);
    assert(guest_pread(-1, bytes, 1, 0) == -1 && errno == EBADF);
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
    assert(guest_lseek(fd, large, 0) == large);
    assert(write(fd, "Q", 1) == 1);
    assert(guest_fstat(fd, &guarded.stat) == 0 && guarded.stat.size == large + 1);
    assert(guest_lseek(fd, large, 0) == large);
    assert(read(fd, bytes, 1) == 1 && bytes[0] == 'Q');
    assert(guest_ftruncate(fd, 3) == 0);
    assert(guest_ftruncate(fd, -1) == -1);
    // Bionic command numbers 0..4 and O_* bits, not host constants.
    assert(guest_fcntl(fd, 3) == 2); // F_GETFL: O_RDWR
    assert(guest_fcntl(fd, 4, 2 | 00002000 | 00004000) == 0);
    assert(guest_fcntl(fd, 3) == (2 | 00002000 | 00004000));
    assert((fcntl(fd, F_GETFL) & (O_APPEND | O_NONBLOCK)) ==
           (O_APPEND | O_NONBLOCK));
    assert(guest_fcntl(fd, 4, 2) == 0);
    assert(guest_fcntl(fd, 3) == 2);
    assert(guest_fcntl(fd, 1) == 0); // F_GETFD
    assert(guest_fcntl(fd, 2, 1) == 0); // F_SETFD: FD_CLOEXEC
    assert(guest_fcntl(fd, 1) == 1);
    assert((fcntl(fd, F_GETFD) & FD_CLOEXEC) == FD_CLOEXEC);
    int duplicate = guest_fcntl(fd, 0, 0); // F_DUPFD
    assert(duplicate >= 0 && duplicate != fd);
    assert(close(duplicate) == 0);
    assert(guest_fcntl(fd, 2, 2) == -1 && errno == EINVAL);
    assert(guest_fcntl(fd, 4, 2 | 01000) == -1 && errno == EINVAL);
    assert(guest_fcntl(fd, 3) == 2); // unsupported F_SETFL changed nothing
    assert(guest_fcntl(fd, 999) == -1 && errno == EOPNOTSUPP);
    assert(guest_fcntl(-1, 3) == -1 && errno == EBADF);
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
    puts("android_file_compat_test: layouts, descriptor flags, 64-bit offsets, directories and failures passed");
}
