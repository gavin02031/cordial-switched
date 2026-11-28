#ifndef CORDIAL_ANDROID_FILE_COMPAT_H
#define CORDIAL_ANDROID_FILE_COMPAT_H
#include <stdbool.h>
#include <stdint.h>

// Fixed LP64 wire layout, independent of newlib's struct stat and dirent.
// Layout reference: AOSP bionic libc/include/sys/stat.h and dirent.h.
typedef struct {
    uint64_t device, inode;
    uint32_t mode, links, uid, gid;
    uint64_t rdevice, padding1;
    int64_t size;
    int32_t block_size, padding2;
    int64_t blocks;
    int64_t access_sec, access_nsec, modify_sec, modify_nsec, change_sec, change_nsec;
    uint32_t unused4, unused5;
} AndroidFileStat;

typedef struct {
    uint64_t inode;
    int64_t offset;
    uint16_t record_bytes;
    uint8_t type;
    char name[256];
} AndroidDirectoryEntry;

bool android_file_compat_resolve(const char *name, const char *version,
                                const char *library, uint64_t *address);
#endif
