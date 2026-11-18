// Test the allocator extension's ownership and rollback under deterministic
// libnx failures. Kernel memory states are measured separately on console.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

typedef int Result;
typedef struct { void *base; size_t size; } VirtmemReservation;
#define MAKERESULT(module, code) 1
#define R_SUCCEEDED(rc) ((rc) == 0)

static bool locked, fail_record, fail_reserve, fail_map, fail_unmap, fail_find;
static size_t reservations, requested;
static uintptr_t next_base = 0x10000000;
static void virtmemLock(void) { assert(!locked); locked = true; }
static void virtmemUnlock(void) { assert(locked); locked = false; }

static void *virtmemFindStack(size_t size, size_t gap)
{
    assert(locked && gap == 16384);
    requested = size;
    next_base += 0x100000;
    return fail_find ? NULL : (void *)next_base;
}

static VirtmemReservation *virtmemAddReservation(void *base, size_t size)
{
    assert(locked && base == (void *)next_base && size != 0);
    if (fail_reserve) return NULL;
    VirtmemReservation *r = malloc(sizeof(*r));
    assert(r);
    *r = (VirtmemReservation){base, size};
    ++reservations;
    return r;
}

static void virtmemRemoveReservation(VirtmemReservation *r)
{
    assert(locked && reservations);
    --reservations;
    free(r);
}

static void *__libnx_alloc(size_t n) { return fail_record ? NULL : malloc(n); }
#define __libnx_free free
static Result svcMapMemory(void *stack, void *source, size_t n)
{
    assert(locked && source && (uintptr_t)stack == next_base + requested - n);
    return fail_map;
}
static Result svcUnmapMemory(void *stack, void *source, size_t n)
{
    assert(locked && stack && source && n);
    return fail_unmap;
}

#include "../source/horizon_stack_guard.inc"

static Result create(void **stack, size_t guard)
{
    size_t previous = morrowSetThreadGuard(guard);
    virtmemLock();
    Result rc = morrowMapStack(stack, (void *)1, 32768);
    virtmemUnlock();
    assert(morrowSetThreadGuard(previous) == guard);
    return rc;
}

int main(void)
{
    void *a = NULL, *b = NULL, *plain = NULL;
    bool *failures[] = {&fail_find, &fail_record, &fail_reserve, &fail_map};
    for (unsigned i = 0; i < sizeof(failures)/sizeof(failures[0]); ++i) {
        *failures[i] = true;
        assert(create(&a, 16384) && reservations == 0);
        *failures[i] = false;
    }
    assert(create(&a, SIZE_MAX - 4095) && reservations == 0);
    assert(create(&a, 1) && reservations == 0);
    assert(!create(&a, 16384) && requested == 49152);
    assert(!create(&b, 4096) && requested == 36864);
    assert(!create(&plain, 0) && requested == 32768 && reservations == 2);
    assert(morrowStackGuardSize(a) == 16384 && morrowStackGuardSize(b) == 4096);
    fail_unmap = true;
    assert(morrowUnmapStack(a, (void *)1, 32768) && reservations == 2);
    fail_unmap = false;
    assert(!morrowUnmapStack(a, (void *)1, 32768) && morrowStackGuardSize(a) == 0);
    assert(!morrowUnmapStack(plain, (void *)1, 32768) && reservations == 1);
    assert(!morrowUnmapStack(b, (void *)1, 32768) && reservations == 0);
    puts("Guard reservation, mapping failure and out-of-order release passed");
}
