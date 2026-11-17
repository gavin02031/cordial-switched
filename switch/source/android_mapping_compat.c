#define _GNU_SOURCE
#include "android_mapping_compat.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#ifdef __SWITCH__
#include <malloc.h>
#include <switch.h>
#else
#include <sys/mman.h>
#endif

enum { PAGE_BYTES = 4096, PRIVATE_ANONYMOUS = 0x22 };
#define FAILED_ADDRESS ((void *)(intptr_t)-1)
#ifdef __SWITCH__
typedef struct {
    unsigned char *storage;
    size_t pieces;
} Backing;
#endif

typedef struct Region {
    void *base;
    size_t size;
#ifdef __SWITCH__
    Backing *backing;
    size_t backing_offset;
#endif
    struct Region *next;
} Region;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static Region *regions;
static Region *rollback_pending;

static size_t page_bytes(size_t length)
{
    if (!length || length > SIZE_MAX - (PAGE_BYTES - 1)) return 0;
    return (length + PAGE_BYTES - 1) & ~(size_t)(PAGE_BYTES - 1);
}

static int reject(int error)
{
    errno = error;
    return -1;
}

static bool allowed(int protection)
{
    return protection == 0 || protection == 1 || protection == 3;
}

static int protect_pages(void *base, size_t size, int protection)
{
#ifdef __SWITCH__
    u32 perm = protection == 3 ? Perm_Rw : protection == 1 ? Perm_R : Perm_None;
    return R_SUCCEEDED(svcSetMemoryPermission(base, size, perm)) ? 0 : reject(EACCES);
#else
    return mprotect(base, size, protection);
#endif
}

static int release_pages(Region *region, size_t offset, size_t size)
{
    void *target = (unsigned char *)region->base + offset;
#ifdef __SWITCH__
    void *source = region->backing->storage + region->backing_offset + offset;
    if (R_FAILED(svcUnmapMemory(target, source, size))) return reject(EBUSY);
    return 0;
#else
    return munmap(target, size);
#endif
}

static void drop_piece(Region *region)
{
#ifdef __SWITCH__
    if (--region->backing->pieces == 0) {
        free(region->backing->storage);
        free(region->backing);
    }
#endif
    free(region);
}

// A failed kernel rollback still owns its alias. Keep it unreachable from
// guest operations and retry cleanup before the next allocation.
static void retry_rollbacks(void)
{
    pthread_mutex_lock(&lock);
    Region **slot = &rollback_pending;
    while (*slot) {
        Region *r = *slot;
        if (release_pages(r, 0, r->size)) { slot = &r->next; continue; }
        *slot = r->next;
        drop_piece(r);
    }
    pthread_mutex_unlock(&lock);
}

static int allocate_pages(Region *region)
{
#ifdef __SWITCH__
    region->backing = calloc(1, sizeof(*region->backing));
    if (!region->backing) return reject(ENOMEM);
    region->backing->storage = memalign(PAGE_BYTES, region->size);
    if (!region->backing->storage) { free(region->backing); return reject(ENOMEM); }
    region->backing->pieces = 1;
    memset(region->backing->storage, 0, region->size);
    // As with libnx thread stacks, the alias is the accessible address.
    // Never touch or release the backing allocation while it is borrowed.
    virtmemLock();
    region->base = virtmemFindStack(region->size, PAGE_BYTES);
    bool mapped = region->base &&
        R_SUCCEEDED(svcMapMemory(region->base, region->backing->storage, region->size));
    virtmemUnlock();
    if (!mapped) {
        free(region->backing->storage);
        free(region->backing);
        return reject(ENOMEM);
    }
#else
    region->base = mmap(NULL, region->size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region->base == MAP_FAILED) return -1;
#endif
    return 0;
}

static void *guest_mmap(void *hint, size_t length, int protection, int flags,
                        int fd, int64_t offset)
{
    (void)hint; // A non-fixed hint may be ignored.
    size_t size = page_bytes(length);
    if (!size || offset) { errno = EINVAL; return FAILED_ADDRESS; }
    if (flags != PRIVATE_ANONYMOUS || fd != -1 || !allowed(protection)) {
        errno = ENOTSUP;
        return FAILED_ADDRESS;
    }
    retry_rollbacks();
    Region *region = calloc(1, sizeof(*region));
    if (!region) { errno = ENOMEM; return FAILED_ADDRESS; }
    region->size = size;
    if (allocate_pages(region)) { free(region); return FAILED_ADDRESS; }
    if (protection != 3 && protect_pages(region->base, size, protection)) {
        int saved = errno;
        // Rollback must never free backing that the kernel still owns.
        if (release_pages(region, 0, size) == 0) drop_piece(region);
        else {
            pthread_mutex_lock(&lock);
            region->next = rollback_pending;
            rollback_pending = region;
            pthread_mutex_unlock(&lock);
        }
        errno = saved;
        return FAILED_ADDRESS;
    }
    pthread_mutex_lock(&lock);
    region->next = regions;
    regions = region;
    void *base = region->base;
    pthread_mutex_unlock(&lock);
    return base;
}

static int guest_mprotect(void *base, size_t length, int protection)
{
    size_t size = page_bytes(length);
    uintptr_t start = (uintptr_t)base;
    if (!size || start % PAGE_BYTES) return reject(EINVAL);
    if (!allowed(protection)) return reject(ENOTSUP);
    pthread_mutex_lock(&lock);
    int result = -1;
    errno = ENOMEM;
    for (Region *r = regions; r; r = r->next) {
        uintptr_t origin = (uintptr_t)r->base;
        if (start >= origin && size <= r->size && start - origin <= r->size - size) {
            result = protect_pages(base, size, protection);
            break;
        }
    }
    pthread_mutex_unlock(&lock);
    return result;
}

static int guest_munmap(void *base, size_t length)
{
    size_t size = page_bytes(length);
    if (!size || (uintptr_t)base % PAGE_BYTES) return reject(EINVAL);
    pthread_mutex_lock(&lock);
    Region **slot = &regions;
    int result = -1;
    errno = EINVAL;
    while (*slot) {
        Region *r = *slot;
        uintptr_t start = (uintptr_t)base, origin = (uintptr_t)r->base;
        if (start < origin || size > r->size || start - origin > r->size - size) {
            slot = &r->next;
            continue;
        }
        size_t before = start - origin;
        size_t after = r->size - before - size;
        // Allocate split metadata before changing the actual mapping, so
        // allocation failure leaves both the registry and the pages intact.
        Region *tail = before && after ? malloc(sizeof(*tail)) : NULL;
        if (before && after && !tail) { errno = ENOMEM; break; }
        result = release_pages(r, before, size);
        if (result) { free(tail); break; }
        if (!before && !after) {
            *slot = r->next;
            drop_piece(r);
        } else {
            if (tail) {
                *tail = *r;
#ifdef __SWITCH__
                tail->backing->pieces++;
#endif
                r->next = tail;
            }
            if (before) r->size = before;
            if (after) {
                Region *right = tail ? tail : r;
                right->base = (unsigned char *)base + size;
                right->size = after;
#ifdef __SWITCH__
                right->backing_offset += before + size;
#endif
            }
        }
        break;
    }
    pthread_mutex_unlock(&lock);
    return result;
}

bool android_mapping_compat_resolve(const char *name, const char *version,
                                    const char *library, uint64_t *address)
{
    if (!name || !version || !library || !address ||
        strcmp(version, "LIBC") || strcmp(library, "libc.so")) return false;
    if (!strcmp(name, "mmap")) *address = (uintptr_t)&guest_mmap;
    else if (!strcmp(name, "munmap")) *address = (uintptr_t)&guest_munmap;
    else if (!strcmp(name, "mprotect")) *address = (uintptr_t)&guest_mprotect;
    else return false;
    return true;
}
