#define _GNU_SOURCE
#include "android_mapping_compat.h"
#include "engine_early_trace.h"

#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdatomic.h>
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
static uint32_t last_kernel_result;
static atomic_uintptr_t traced_arena;

// The first allocator arena uses anonymous PROT_NONE mmap with fd=0.
// A console run crashed before the watchdog could sample after the fd fix;
// keep only this arena's transitions in an independently durable trace.
// Restore errno so diagnostic I/O cannot change the guest-visible result.
static void trace_arena_event(const char *event, uint64_t a, uint64_t b,
                              uint64_t c)
{
    const int saved_errno = errno;
    engine_early_trace_append("sdmc:/switch/cordial-switched", event, a, b, c);
    errno = saved_errno;
}

typedef struct {
    unsigned char *storage;
    size_t size;
    size_t pieces;
} Backing;
#endif

typedef struct Region {
    void *base;
    size_t size;
    bool private_file;
    int protection; // -1 when subranges have different or unknown permissions.
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
    Result rc = svcSetMemoryPermission(base, size, perm);
    last_kernel_result = rc;
    return R_SUCCEEDED(rc) ? 0 : reject(EACCES);
#else
    return mprotect(base, size, protection);
#endif
}

#ifdef __SWITCH__
uint32_t android_mapping_compat_last_kernel_result(void)
{
    return last_kernel_result;
}
#endif

static int release_pages(Region *region, size_t offset, size_t size)
{
    void *target = (unsigned char *)region->base + offset;
#ifdef __SWITCH__
    // Heap pages cannot be unmapped individually, but they can be fenced:
    // the Normal state carries FlagCanReprotect, so released subranges
    // become inaccessible until the whole backing returns to the heap.
    Result rc = svcSetMemoryPermission(target, size, Perm_None);
    last_kernel_result = rc;
    return R_SUCCEEDED(rc) ? 0 : reject(EBUSY);
#else
    return munmap(target, size);
#endif
}

static void drop_piece(Region *region)
{
#ifdef __SWITCH__
    if (--region->backing->pieces == 0) {
        // newlib's free writes its bookkeeping into the block, so the whole
        // backing must be writable again first. A failed restore leaks the
        // allocation rather than crash the allocator on a fenced page.
        Result rc = svcSetMemoryPermission(region->backing->storage,
                                           region->backing->size, Perm_Rw);
        last_kernel_result = rc;
        if (R_SUCCEEDED(rc)) {
            free(region->backing->storage);
            free(region->backing);
        }
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
    region->backing->size = region->size;
    region->backing->pieces = 1;
    memset(region->backing->storage, 0, region->size);
    // The guest uses the heap pages directly. svcMapMemory aliases were
    // tried first, but the kernel maps them with state Stack (no
    // FlagCanReprotect), so every svcSetMemoryPermission on them failed
    // with 0x0000d401 (Atmosphere KPageTableBase::MapMemory /
    // SetMemoryPermission). The heap's Normal state carries
    // FlagCanReprotect, so protection changes work on these pages.
    region->base = region->backing->storage;
#else
    region->base = mmap(NULL, region->size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region->base == MAP_FAILED) return -1;
#endif
    return 0;
}

// Caller holds lock. If kernel cleanup fails, preserve the backing reference.
static void discard_private(Region *region)
{
    int saved = errno;
    if (release_pages(region, 0, region->size) == 0) drop_piece(region);
    else {
        region->next = rollback_pending;
        rollback_pending = region;
    }
    errno = saved;
}

static Region *new_region(size_t size)
{
    Region *region = calloc(1, sizeof(*region));
    if (!region) { errno = ENOMEM; return NULL; }
    region->size = size;
    region->protection = 3;
    if (allocate_pages(region)) { free(region); return NULL; }
    return region;
}

static Region **find_range(void *base, size_t size)
{
    uintptr_t start = (uintptr_t)base;
    for (Region **slot = &regions; *slot; slot = &(*slot)->next) {
        uintptr_t origin = (uintptr_t)(*slot)->base;
        if (start >= origin && size <= (*slot)->size &&
            start - origin <= (*slot)->size - size) return slot;
    }
    return NULL;
}

// Read the file through positional I/O. The guest may close its descriptor
// immediately after mmap or have another thread using the same file offset.
static int copy_file_pages(Region *region, int fd, int64_t offset)
{
#if defined(__SWITCH__) && !defined(MORROW_HAS_POSITIONAL_IO)
    (void)region; (void)fd; (void)offset;
    return reject(ENOTSUP);
#else
#ifdef __SWITCH__
    extern ssize_t morrowFsdevPread(int, void *, size_t, off_t);
#endif
    struct stat st;
    if (fstat(fd, &st)) return -1;
    if (!S_ISREG(st.st_mode)) return reject(ENODEV);
    if (offset >= st.st_size) return reject(ENOTSUP);
    uint64_t available = (uint64_t)(st.st_size - offset);
    // Beyond the final partial page Linux raises SIGBUS. Restrict our
    // snapshot to ranges we can serve without fabricating data there.
    if (available <= region->size - PAGE_BYTES) return reject(ENOTSUP);
    size_t bytes = available < region->size ? (size_t)available : region->size;
    for (size_t done = 0; done < bytes;) {
        size_t chunk = bytes - done;
        if (chunk > 1024 * 1024) chunk = 1024 * 1024;
        unsigned char *out = (unsigned char *)region->base + done;
#ifdef __SWITCH__
        ssize_t n = morrowFsdevPread(fd, out, chunk, (off_t)(offset + (int64_t)done));
#else
        ssize_t n = pread(fd, out, chunk, (off_t)(offset + (int64_t)done));
#endif
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return -1;
        if (!n) return reject(EIO); // concurrent truncation during initialisation
        done += (size_t)n;
    }
    return 0;
#endif
}

static void *guest_mmap(void *hint, size_t length, int protection, int flags,
                        int fd, int64_t offset)
{
#ifdef __SWITCH__
    const bool trace = !hint && length == 0x7b2000 && protection == 0 &&
                       flags == PRIVATE_ANONYMOUS && fd == 0 && offset == 0;
    if (trace)
        trace_arena_event("arena_mmap_enter", length, (unsigned)flags,
                          (unsigned)fd);
#endif
    (void)hint; // A non-fixed hint may be ignored.
    size_t size = page_bytes(length);
    bool file = flags == 2; // MAP_PRIVATE without MAP_ANONYMOUS
    if (!size || offset < 0 || offset % PAGE_BYTES || (!file && offset)) {
        errno = EINVAL;
#ifdef __SWITCH__
        if (trace) trace_arena_event("arena_mmap_reject", errno, 0, 0);
#endif
        return FAILED_ADDRESS;
    }
    // Linux/Android ignore fd when MAP_ANONYMOUS is set. In particular,
    // libroblox's early allocator passes fd=0 for its PROT_NONE arena;
    // rejecting that valid call makes its error logger recursively enter
    // the allocator initializer and spin on the still-pending guard.
    if ((!file && flags != PRIVATE_ANONYMOUS) || !allowed(protection)) {
        errno = ENOTSUP;
#ifdef __SWITCH__
        if (trace) trace_arena_event("arena_mmap_reject", errno, 0, 0);
#endif
        return FAILED_ADDRESS;
    }
    retry_rollbacks();
    Region *region = new_region(size);
    if (!region) {
#ifdef __SWITCH__
        if (trace) trace_arena_event("arena_mmap_alloc_error", errno, 0, 0);
#endif
        return FAILED_ADDRESS;
    }
    region->private_file = file;
    if ((file && copy_file_pages(region, fd, offset)) ||
        (protection != 3 && protect_pages(region->base, size, protection))) {
#ifdef __SWITCH__
        if (trace)
            trace_arena_event("arena_mmap_protect_error", errno,
                              last_kernel_result, (uintptr_t)region->base);
#endif
        pthread_mutex_lock(&lock);
        discard_private(region);
        pthread_mutex_unlock(&lock);
        return FAILED_ADDRESS;
    }
    region->protection = protection;
    pthread_mutex_lock(&lock);
    region->next = regions;
    regions = region;
    void *base = region->base;
    pthread_mutex_unlock(&lock);
#ifdef __SWITCH__
    if (trace) {
        atomic_store_explicit(&traced_arena, (uintptr_t)base,
                              memory_order_relaxed);
        trace_arena_event("arena_mmap_ok", (uintptr_t)base, size,
                          last_kernel_result);
        // The return site identifies the guest allocator call even when its
        // first memory access faults before the watchdog can sample the PC.
        trace_arena_event("arena_mmap_guest_lr",
                          (uintptr_t)__builtin_extract_return_addr(
                              __builtin_return_address(0)),
                          (uintptr_t)base, size);
    }
#endif
    return base;
}

static int guest_mprotect(void *base, size_t length, int protection)
{
    size_t size = page_bytes(length);
#ifdef __SWITCH__
    const bool trace = (uintptr_t)base &&
        (uintptr_t)base == atomic_load_explicit(&traced_arena,
                                                memory_order_relaxed);
    if (trace)
        trace_arena_event("arena_protect_enter", (uintptr_t)base, size,
                          (unsigned)protection);
#endif
    uintptr_t start = (uintptr_t)base;
    if (!size || start % PAGE_BYTES) return reject(EINVAL);
    if (!allowed(protection)) return reject(ENOTSUP);
    pthread_mutex_lock(&lock);
    Region **slot = find_range(base, size);
    int result = slot ? protect_pages(base, size, protection) : reject(ENOMEM);
    if (!result) {
        Region *r = *slot;
        if (base == r->base && size == r->size) r->protection = protection;
        else if (r->protection != protection) r->protection = -1;
    }
    pthread_mutex_unlock(&lock);
#ifdef __SWITCH__
    if (trace) {
        trace_arena_event("arena_protect_return",
                          (uint32_t)result, result ? (unsigned)errno : 0,
                          last_kernel_result);
        if (!result) {
            // A successful SetMemoryPermission call is not evidence that the
            // guest's next dereference will be valid. Save the actual kernel
            // memory state and guest return PC before handing control back.
            MemoryInfo info = {0};
            u32 page_info = 0;
            const Result query = svcQueryMemory(&info, &page_info,
                                                 (u64)(uintptr_t)base);
            trace_arena_event("arena_protect_query", (unsigned)query,
                              R_SUCCEEDED(query) ? info.perm : 0,
                              R_SUCCEEDED(query) ? info.type : 0);
        }
        trace_arena_event("arena_protect_guest_lr",
                          (uintptr_t)__builtin_extract_return_addr(
                              __builtin_return_address(0)),
                          (uintptr_t)base, size);
    }
#endif
    return result;
}

static int remove_range(Region **slot, void *base, size_t size)
{
    Region *r = *slot;
    uintptr_t start = (uintptr_t)base, origin = (uintptr_t)r->base;
    size_t before = start - origin;
    size_t after = r->size - before - size;
    // Allocate split metadata before changing the actual mapping, so
    // allocation failure leaves both the registry and the pages intact.
    Region *tail = before && after ? malloc(sizeof(*tail)) : NULL;
    if (before && after && !tail) return reject(ENOMEM);
    int result = release_pages(r, before, size);
    if (result) { free(tail); return result; }
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
    return 0;
}

static int guest_munmap(void *base, size_t length)
{
    size_t size = page_bytes(length);
#ifdef __SWITCH__
    const bool trace = (uintptr_t)base &&
        (uintptr_t)base == atomic_load_explicit(&traced_arena,
                                                memory_order_relaxed);
    if (trace)
        trace_arena_event("arena_unmap_enter", (uintptr_t)base, size, 0);
#endif
    if (!size || (uintptr_t)base % PAGE_BYTES) return reject(EINVAL);
    pthread_mutex_lock(&lock);
    Region **slot = find_range(base, size);
    int result = slot ? remove_range(slot, base, size) : reject(EINVAL);
    pthread_mutex_unlock(&lock);
#ifdef __SWITCH__
    if (trace) {
        trace_arena_event("arena_unmap_return", (uint32_t)result,
                          result ? (unsigned)errno : 0, last_kernel_result);
        if (!result)
            atomic_store_explicit(&traced_arena, 0, memory_order_relaxed);
    }
#endif
    return result;
}

static void *guest_mremap(void *base, size_t old_length, size_t new_length,
                          int flags, ...)
{
    size_t old_size = page_bytes(old_length), new_size = page_bytes(new_length);
    if (!old_size || !new_size || (uintptr_t)base % PAGE_BYTES) {
        errno = EINVAL;
        return FAILED_ADDRESS;
    }
    if (flags & ~1) { errno = ENOTSUP; return FAILED_ADDRESS; }
    pthread_mutex_lock(&lock);
    Region **slot = find_range(base, old_size);
    void *out = FAILED_ADDRESS;
    if (!slot || (*slot)->base != base || (*slot)->size != old_size) errno = EFAULT;
    else if (new_size == old_size) out = base;
    else if (new_size < old_size) {
        if (!remove_range(slot, (unsigned char *)base + new_size, old_size - new_size))
            out = base;
    } else if (!flags) errno = ENOMEM; // Additional adjacent pages are not reserved.
    else if ((*slot)->private_file || (*slot)->protection != 3) errno = ENOTSUP;
    else {
        Region *old = *slot;
        Region *replacement = new_region(new_size);
        if (replacement) {
            memcpy(replacement->base, old->base, old_size);
            if (release_pages(old, 0, old_size)) discard_private(replacement);
            else {
                replacement->next = old->next;
                *slot = replacement;
                out = replacement->base;
                drop_piece(old);
            }
        }
    }
    pthread_mutex_unlock(&lock);
    return out;
}

static int guest_madvise(void *base, size_t length, int advice)
{
    size_t size = page_bytes(length);
    if (!size || (uintptr_t)base % PAGE_BYTES) return reject(EINVAL);
    if (advice != 4) return reject(ENOTSUP); // MADV_DONTNEED
    pthread_mutex_lock(&lock);
    Region **slot = find_range(base, size);
    int result = 0;
    if (!slot) result = reject(ENOMEM);
    else if ((*slot)->private_file || (*slot)->protection != 3) result = reject(ENOTSUP);
    else {
#ifdef __SWITCH__
        // Anonymous private data reads back zero after discard. This eager
        // backend keeps its heap allocation until unmap.
        memset(base, 0, size);
#else
        result = madvise(base, size, MADV_DONTNEED);
#endif
    }
    pthread_mutex_unlock(&lock);
    return result;
}

static int guest_msync(void *base, size_t length, int flags)
{
    size_t size = page_bytes(length);
    if (!size || (uintptr_t)base % PAGE_BYTES || (flags & ~7) ||
        (flags & 5) == 5) return reject(EINVAL);
    if (flags & 2) return reject(ENOTSUP); // MS_INVALIDATE
    pthread_mutex_lock(&lock);
    // All supported mappings are private. No dirty data is owed to a file.
    int result = find_range(base, size) ? 0 : reject(ENOMEM);
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
    else if (!strcmp(name, "mremap")) *address = (uintptr_t)&guest_mremap;
    else if (!strcmp(name, "msync")) *address = (uintptr_t)&guest_msync;
    else if (!strcmp(name, "madvise")) *address = (uintptr_t)&guest_madvise;
    else return false;
    return true;
}
