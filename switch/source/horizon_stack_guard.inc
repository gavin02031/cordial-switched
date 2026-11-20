// Included by the pinned libnx thread.c, inside its normal stack allocator.
// The creator's request is thread-local. Newlib calls threadCreate on the
// creating thread before returning from pthread_create.
static __thread size_t morrow_requested_guard;

size_t morrowSetThreadGuard(size_t size)
{
    size_t previous = morrow_requested_guard;
    morrow_requested_guard = size;
    return previous;
}

typedef struct MorrowGuard {
    void *stack;
    size_t size;
    VirtmemReservation *reservation;
    struct MorrowGuard *next;
} MorrowGuard;

static MorrowGuard *morrow_guards;

size_t morrowStackGuardSize(void *stack)
{
    size_t size = 0;
    virtmemLock();
    for (MorrowGuard *record = morrow_guards; record; record = record->next) {
        if (record->stack == stack) { size = record->size; break; }
    }
    virtmemUnlock();
    return size;
}

// Both functions run under the libnx virtual-memory mutex. Reserve the
// unmapped prefix before exposing the neighbouring stack to another finder.
static Result morrowMapStack(void **stack, void *backing, size_t size)
{
    size_t guard = morrow_requested_guard;
    Result no_space = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    if ((guard & 4095) || guard > SIZE_MAX - size) return no_space;
    unsigned char *base = virtmemFindStack(size + guard, 0x4000);
    if (!base) return no_space;
    *stack = base + guard;
    MorrowGuard *record = NULL;
    if (guard) {
        record = __libnx_alloc(sizeof(*record));
        if (!record) return no_space;
        record->reservation = virtmemAddReservation(base, guard);
        if (!record->reservation) { __libnx_free(record); return no_space; }
        record->stack = *stack;
        record->size = guard;
    }
    Result rc = svcMapMemory(*stack, backing, size);
    if (record) {
        if (R_SUCCEEDED(rc)) {
            record->next = morrow_guards;
            morrow_guards = record;
        } else {
            virtmemRemoveReservation(record->reservation);
            __libnx_free(record);
        }
    }
    return rc;
}

static Result morrowUnmapStack(void *stack, void *backing, size_t size)
{
    virtmemLock();
    Result rc = svcUnmapMemory(stack, backing, size);
    if (R_SUCCEEDED(rc)) {
        for (MorrowGuard **slot = &morrow_guards; *slot; slot = &(*slot)->next) {
            MorrowGuard *record = *slot;
            if (record->stack != stack) continue;
            *slot = record->next;
            virtmemRemoveReservation(record->reservation);
            __libnx_free(record);
            break;
        }
    }
    virtmemUnlock();
    return rc;
}
