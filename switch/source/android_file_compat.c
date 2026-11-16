#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "android_file_compat.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

_Static_assert(sizeof(AndroidFileStat) == 128, "Android ARM64 stat size");
_Static_assert(offsetof(AndroidFileStat, size) == 48, "Android stat size offset");
_Static_assert(offsetof(AndroidFileStat, access_sec) == 72, "Android stat time offset");
_Static_assert(offsetof(AndroidDirectoryEntry, name) == 19, "Android dirent name offset");
_Static_assert(sizeof(AndroidDirectoryEntry) == 280, "Android dirent size");
_Static_assert(sizeof(off_t) == 8, "64-bit file offsets required");

static bool stat_fields_fit(int64_t block_size, uint64_t links)
{
    return block_size >= INT32_MIN && block_size <= INT32_MAX && links <= UINT32_MAX;
}

static int copy_stat(const struct stat *native, AndroidFileStat *guest)
{
    if (!stat_fields_fit(native->st_blksize, native->st_nlink)) {
        errno = EOVERFLOW;
        return -1;
    }
    AndroidFileStat value = {0};
    value.device = native->st_dev;
    value.inode = native->st_ino;
    value.mode = native->st_mode;
    value.links = native->st_nlink;
    value.uid = native->st_uid;
    value.gid = native->st_gid;
    value.rdevice = native->st_rdev;
    value.size = native->st_size;
    value.block_size = native->st_blksize;
    value.blocks = native->st_blocks;
    value.access_sec = native->st_atim.tv_sec;
    value.access_nsec = native->st_atim.tv_nsec;
    value.modify_sec = native->st_mtim.tv_sec;
    value.modify_nsec = native->st_mtim.tv_nsec;
    value.change_sec = native->st_ctim.tv_sec;
    value.change_nsec = native->st_ctim.tv_nsec;
    *guest = value;
    return 0;
}

static int file_stat(const char *path, AndroidFileStat *guest)
{
    if (!guest) { errno = EFAULT; return -1; }
    struct stat native;
    if (stat(path, &native) < 0) return -1;
    return copy_stat(&native, guest);
}

static int file_lstat(const char *path, AndroidFileStat *guest)
{
    if (!guest) { errno = EFAULT; return -1; }
    struct stat native;
    if (lstat(path, &native) < 0) return -1;
    return copy_stat(&native, guest);
}

static int file_fstat(int fd, AndroidFileStat *guest)
{
    if (!guest) { errno = EFAULT; return -1; }
    struct stat native;
    if (fstat(fd, &native) < 0) return -1;
    return copy_stat(&native, guest);
}

// AOSP asm-generic/fcntl.h assigns different bits from newlib. Unknown
// requests fail before open can create or truncate anything.
static bool open_flags(int guest, int *native)
{
    int remaining = guest & ~3;
    switch (guest & 3) {
    case 0: *native = O_RDONLY; break;
    case 1: *native = O_WRONLY; break;
    case 2: *native = O_RDWR; break;
    default: errno = EINVAL; return false;
    }
#define FLAG(android, host) if (remaining & (android)) { *native |= (host); remaining &= ~(android); }
    FLAG(00000100, O_CREAT)
    FLAG(00000200, O_EXCL)
    FLAG(00000400, O_NOCTTY)
    FLAG(00001000, O_TRUNC)
    FLAG(00002000, O_APPEND)
    FLAG(00004000, O_NONBLOCK)
#ifdef O_DSYNC
    FLAG(00010000, O_DSYNC)
#endif
#ifdef O_DIRECTORY
    FLAG(00200000, O_DIRECTORY)
#endif
#ifdef O_NOFOLLOW
    FLAG(00400000, O_NOFOLLOW)
#endif
#ifdef O_CLOEXEC
    FLAG(02000000, O_CLOEXEC)
#endif
#ifdef O_SYNC
    FLAG(04000000, O_SYNC)
#endif
#undef FLAG
    // O_LARGEFILE has no effect in the LP64 guest/host pair.
    remaining &= ~00100000;
    if (remaining) { errno = EINVAL; return false; }
    return true;
}

static int file_open(const char *path, int flags, ...)
{
    int native;
    if (!open_flags(flags, &native)) return -1;
    unsigned mode = 0;
    if (flags & 00000100) {
        va_list arguments;
        va_start(arguments, flags);
        mode = va_arg(arguments, unsigned);
        va_end(arguments);
    }
    return open(path, native, (mode_t)mode);
}

static int file_open_2(const char *path, int flags)
{
    // FORTIFY's two-argument entry must never read a missing mode argument.
    if (flags & (00000100 | 020000000)) abort();
    return file_open(path, flags);
}

static int64_t file_lseek(int fd, int64_t offset, int whence)
{
    int native;
    switch (whence) {
    case 0: native = SEEK_SET; break;
    case 1: native = SEEK_CUR; break;
    case 2: native = SEEK_END; break;
    default: errno = EINVAL; return -1;
    }
    return lseek(fd, (off_t)offset, native);
}

// These offset-based calls must not be emulated with seek/read/seek: another
// engine thread may share the descriptor's current offset.
static int64_t file_pread(int fd, void *buffer, uint64_t count, int64_t offset)
{
    if (count > INT64_MAX) { errno = EINVAL; return -1; }
    return pread(fd, buffer, (size_t)count, (off_t)offset);
}
static int64_t file_pwrite(int fd, const void *buffer, uint64_t count, int64_t offset)
{
    if (count > INT64_MAX) { errno = EINVAL; return -1; }
    return pwrite(fd, buffer, (size_t)count, (off_t)offset);
}
static int file_ftruncate(int fd, int64_t length)
{
    return ftruncate(fd, (off_t)length);
}

typedef struct {
    DIR *native;
    AndroidDirectoryEntry entry;
} AndroidDirectory;

static AndroidDirectory *file_opendir(const char *path)
{
    DIR *native = opendir(path);
    if (!native) return NULL;
    AndroidDirectory *directory = calloc(1, sizeof(*directory));
    if (!directory) { closedir(native); errno = ENOMEM; return NULL; }
    directory->native = native;
    return directory;
}

static AndroidDirectoryEntry *file_readdir(AndroidDirectory *directory)
{
    if (!directory) { errno = EBADF; return NULL; }
    struct dirent *native = readdir(directory->native);
    if (!native) return NULL;
    size_t length = strlen(native->d_name);
    if (length >= sizeof(directory->entry.name)) { errno = ENAMETOOLONG; return NULL; }
    memset(&directory->entry, 0, sizeof(directory->entry));
    directory->entry.inode = native->d_ino;
    // telldir supplies the stream's next-entry cookie on both implementations.
    directory->entry.offset = telldir(directory->native);
    directory->entry.record_bytes = (uint16_t)((19 + length + 1 + 7) & ~7u);
    // DT_UNKNOWN is valid when the host does not expose a portable type.
#ifndef __SWITCH__
    directory->entry.type = native->d_type;
#endif
    memcpy(directory->entry.name, native->d_name, length + 1);
    return &directory->entry;
}

static int file_closedir(AndroidDirectory *directory)
{
    if (!directory) { errno = EBADF; return -1; }
    int result = closedir(directory->native);
    free(directory);
    return result;
}

bool android_file_compat_resolve(const char *name, const char *version,
                                const char *library, uint64_t *address)
{
    if (!name || !version || !library || !address ||
        strcmp(version, "LIBC") || strcmp(library, "libc.so")) return false;
    *address = 0;
#define MAP(symbol, target) if (!strcmp(name, symbol)) *address = (uint64_t)(uintptr_t)&target; else
    MAP("stat", file_stat)
    MAP("fstat", file_fstat)
    MAP("lstat", file_lstat)
    MAP("open", file_open)
    MAP("__open_2", file_open_2)
    MAP("lseek", file_lseek)
    MAP("pread", file_pread)
    MAP("pread64", file_pread)
    MAP("pwrite", file_pwrite)
    MAP("ftruncate", file_ftruncate)
    MAP("opendir", file_opendir)
    MAP("readdir", file_readdir)
    MAP("closedir", file_closedir)
    { return false; }
#undef MAP
    return true;
}
