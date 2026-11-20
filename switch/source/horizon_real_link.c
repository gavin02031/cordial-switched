#include "horizon_real_link.h"
#include "android_import_resolver.h"
#include "engine_module_registry.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <malloc.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "android_log_compat.h"
#include "android_media_codec_compat.h"
#include "android_image_reader_compat.h"
#include "android_coverage_compat.h"
#include "android_cxa_compat.h"
#include "android_looper_compat.h"
#include "android_asset_compat.h"
#include "android_configuration_compat.h"
#include "android_egl_gles_compat.h"
#include "android_jni_vm_bridge.h"
#ifdef MORROW_HAS_JNIVM
#include "morrow_jni_runtime.h"
#endif
#include "android_native_window_compat.h"
#include "android_dl_compat.h"
#include "android_media_format_compat.h"
#include "android_math_compat.h"
#include "android_libc_compat.h"
#include "android_wchar_compat.h"
#include "android_socket_compat.h"
#include "android_file_compat.h"
#include "android_mapping_compat.h"
#include "android_jump_compat.h"
#include "android_stdio_compat.h"
#include "android_fortify_compat.h"
#include "android_pthread_compat.h"
#include "android_sem_compat.h"
#include "android_tls_compat.h"
#include "android_thread_attr_compat.h"
#include "android_thread_lifecycle_compat.h"
#include "elf_probe.h"
#include "engine_entry_probe.h"
#include "engine_constructor_plan.h"
#include "engine_constructor_run.h"
#include "engine_execution_attempt.h"
#include "engine_run_journal.h"
#include "engine_memory_plan.h"
#include "engine_stage.h"
#include "engine_tls_scratch.h"
#include "engine_guest_tpidr.h"
#include "engine_tls_thread.h"
#include "horizon_code_address.h"
#include "horizon_code_memory.h"

#ifndef MORROW_BUILD_VERSION
#define MORROW_BUILD_VERSION "development"
#endif

static HorizonCodeMemory reservation;
// JNI_OnLoad can register callbacks whose function pointers live in the
// mapped library. After entering it, neither the RX mapping nor its source
// can safely be unloaded by a second L3; the OS reclaims both on app exit.
static bool engine_entry_started;
static bool engine_mapping_pinned;
static bool engine_jni_onload_succeeded;
// The engine's own TLS world for its main thread, prepared once and kept for
// the process lifetime: guest TLS is never freed while the engine runs.
static EngineTlsThread engine_main_tls;
static bool engine_main_tls_ready;
static bool game_activity_attempt_consumed;
static bool game_activity_entry_called;
static uintptr_t live_game_activity_target;
static uintptr_t live_app_bridge_targets[5];
static uintptr_t live_activity_lifecycle_targets[ENGINE_ACTIVITY_LIFECYCLE_STAGES];
static bool activity_lifecycle_attempt_consumed;
static uintptr_t live_app_bridge_surface_update_target;
static bool app_bridge_attempt_consumed;
static bool app_bridge_sequence_returned;
static uintptr_t live_main_thread_message_target;
static bool main_thread_message_attempt_consumed;
static bool exclusive_graphics_ownership;
static void log_line(FILE *out, const char *format, ...);

void horizon_real_link_enable_exclusive_graphics(void)
{
    // The caller must already have destroyed SDL's renderer and window.
    // Never toggle this back while guest code or GPU workers may be running.
    exclusive_graphics_ownership = true;
}

bool horizon_real_link_jni_onload_ready(void)
{
    return engine_jni_onload_succeeded;
}

bool horizon_real_link_app_bridge_ready(void)
{
    return app_bridge_sequence_returned;
}

bool horizon_real_link_pump_exclusive(FILE *trace, const char *report_path,
                                      unsigned call_number)
{
    if (!trace || !report_path || !*report_path || call_number < 1 ||
        call_number > 120) return false;
    if (!exclusive_graphics_ownership || !engine_mapping_pinned ||
        !engine_jni_onload_succeeded || !app_bridge_sequence_returned ||
        !live_main_thread_message_target) {
        log_line(trace, "pump_blocker=engine_or_message_callback_not_ready");
        return false;
    }
#ifdef MORROW_HAS_JNIVM
    if (!morrow_jni_runtime_jni_env()) {
        log_line(trace, "pump_blocker=jni_environment_missing");
        return false;
    }
    char phase[80];
    snprintf(phase, sizeof(phase), "app_bridge_pump_tick_%03u", call_number);
    if (!engine_run_journal_begin(ENGINE_REPORT_DIR, phase, report_path)) {
        log_line(trace, "pump_blocker=crash_journal_not_durable");
        return false;
    }
    log_line(trace, "pump_tick_%03u_entered_guest=yes", call_number);
    if (fflush(trace) != 0 || fsync(fileno(trace)) != 0) {
        // Guest code has not been entered. Avoid leaving a false crash
        // breadcrumb if only the report sync failed.
        engine_run_journal_finish(ENGINE_REPORT_DIR);
        log_line(trace, "pump_blocker=link_report_not_durable");
        return false;
    }
    MorrowAppBridgeCallResult callback = {0};
    const bool returned = morrow_jni_runtime_call_app_bridge(
        live_main_thread_message_target,
        MORROW_APP_BRIDGE_MAIN_THREAD_MESSAGES, &callback);
    log_line(trace, "pump_tick_%03u_returned=%s", call_number,
             returned && callback.entry_returned ? "yes" : "no");
    log_line(trace, "pump_tick_%03u_java_exception=%s", call_number,
             callback.java_exception_pending ? "yes" : "no");
    log_line(trace, "pump_tick_%03u_cpp_exception=%s", call_number,
             callback.cpp_exception_caught ? "yes" : "no");
    const bool marker_cleared = engine_run_journal_finish_report(
        ENGINE_REPORT_DIR, trace);
    if (!marker_cleared) {
        log_line(trace, "pump_blocker=return_report_or_crash_marker_not_durable");
        return false;
    }
    if (!returned || !callback.entry_invoked || !callback.entry_returned ||
        callback.java_exception_pending || callback.cpp_exception_caught) {
        log_line(trace, "pump_blocker=message_callback_failed");
        return false;
    }
    return true;
#else
    log_line(trace, "pump_blocker=jni_runtime_not_compiled");
    return false;
#endif
}

static const char *reserve_status_name(HorizonCodeReserveStatus status)
{
    switch (status) {
    case HORIZON_RESERVE_OK: return "ok";
    case HORIZON_RESERVE_INVALID_PLAN: return "invalid_memory_plan";
    case HORIZON_RESERVE_SYSCALL_UNAVAILABLE: return "required_syscall_unavailable";
    case HORIZON_RESERVE_NO_FREE_SPAN: return "no_contiguous_code_address_span";
    case HORIZON_RESERVE_NO_RESERVATION_MEMORY: return "reservation_allocation_failed";
    case HORIZON_RESERVE_INVALID_STATE: return "reservation_already_active";
    }
    return "unknown";
}

static const char *entry_status_name(EngineEntryProbeStatus status)
{
    switch (status) {
    case ENGINE_ENTRY_INVALID: return "invalid_elf_metadata";
    case ENGINE_ENTRY_IO_ERROR: return "io_error";
    case ENGINE_ENTRY_NO_DYNAMIC_SYMBOLS: return "export_symbols_unknown";
    case ENGINE_ENTRY_INSPECTED: return "export_symbols_inspected";
    }
    return "unknown";
}

typedef struct {
    char name[80];
    char version[48];
    char library[48];
} MissingImport;

typedef struct {
    MissingImport first;
    MissingImport examples[24];
    MissingImport *complete_list;
    unsigned complete_capacity;
    MissingImport *resolved_list;
    unsigned resolved_capacity;
    bool resolved_keys_truncated;
    unsigned lookups;
    unsigned supported;
    unsigned missing;
} ImportDiagnostics;

// The SD report is a source of evidence about the *actual* Horizon resolver,
// not an inferred list of MAP names. Refuse a complete manifest if the fixed
// report fields cannot contain any of the accepted ABI keys.
static bool snapshot_import(MissingImport *value, const char *name,
                            const char *version, const char *library)
{
    if (!value) return false;
    const char *entry_name = name ? name : "?";
    const char *entry_version = version ? version : "unversioned";
    const char *entry_library = library ? library : "unspecified";
    const bool fits = strlen(entry_name) < sizeof(value->name) &&
                      strlen(entry_version) < sizeof(value->version) &&
                      strlen(entry_library) < sizeof(value->library);
    snprintf(value->name, sizeof(value->name), "%s", entry_name);
    snprintf(value->version, sizeof(value->version), "%s", entry_version);
    snprintf(value->library, sizeof(value->library), "%s", entry_library);
    return fits;
}

static FILE *new_log(char *path, size_t capacity)
{
    for (unsigned i = 1; i <= 99; i++) {
        const int n = snprintf(path, capacity,
                               ENGINE_REPORT_DIR "/link-%02u.txt", i);
        if (n <= 0 || (size_t)n >= capacity) break;
        const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            if (errno == EEXIST) continue;
            break;
        }
        FILE *stream = fdopen(fd, "w");
        if (stream) return stream;
        close(fd);
        remove(path);
        break;
    }
    path[0] = 0;
    return NULL;
}
static void log_line(FILE *out, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(out, format, args);
    va_end(args);
    fputc('\n', out);
    fflush(out);
    // fsync too: fflush only reaches the fs service's buffers; fsync maps
    // to fsFileFlush, which commits the data. After a fatal, only
    // committed bytes are visible on SD. Diagnostics exist to survive
    // crashes, though a fatal can still lose unflushed kernel state.
    fsync(fileno(out));
}
static void log_engine_export(FILE *log, const char *name,
                              const EngineEntryProbe *entry,
                              bool found, uint64_t virtual_address)
{
    log_line(log, "%s_exported=%s", name,
             entry->status != ENGINE_ENTRY_INSPECTED ? "unknown" :
             found ? "yes" : "no");
    if (entry->status == ENGINE_ENTRY_INSPECTED && found)
        log_line(log, "%s_virtual_address=0x%llx", name,
                 (unsigned long long)virtual_address);
}
static void detail(RealLinkResult *result, const char *message)
{
    snprintf(result->detail, sizeof(result->detail), "%.110s", message);
}
static bool resolve_compatible(const char *name, const char *version,
                               const char *library, uint64_t *address,
                               void *context)
{
    ImportDiagnostics *diagnostics = context;
    if (diagnostics && diagnostics->lookups < UINT32_MAX)
        diagnostics->lookups++;
    // Match the same real providers used by ZR's import audit, without
    // fabricating pointers for Android services that do not exist on Horizon.
    const bool supported = android_import_resolve(name, version, library, address);
    if (supported) {
        if (diagnostics) {
            if (diagnostics->resolved_list &&
                diagnostics->supported < diagnostics->resolved_capacity &&
                !snapshot_import(&diagnostics->resolved_list[diagnostics->supported],
                                 name, version, library))
                diagnostics->resolved_keys_truncated = true;
            if (diagnostics->supported < UINT32_MAX)
                diagnostics->supported++;
        }
    } else if (diagnostics) {
        MissingImport value = {0};
        // A name alone cannot establish ABI compatibility: keep the version
        // and owning Android DSO for every rejected provider lookup.
        snapshot_import(&value, name, version, library);
        if (!diagnostics->first.name[0]) diagnostics->first = value;
        if (diagnostics->complete_list &&
            diagnostics->missing < diagnostics->complete_capacity)
            diagnostics->complete_list[diagnostics->missing] = value;
        if (diagnostics->missing < 24)
            diagnostics->examples[diagnostics->missing] = value;
        if (diagnostics->missing < UINT32_MAX) diagnostics->missing++;
    }
    return supported;
}
static void printable(char *out, size_t size, const char *message)
{
    size_t i = 0;
    while (message && message[i] && i + 1 < size) {
        const unsigned char ch = (unsigned char)message[i];
        out[i] = (ch >= 32 && ch <= 126) ? (char)ch : '?';
        i++;
    }
    out[i] = 0;
}

static void log_required_import(FILE *log, const char *prefix, const EngineStage *stage)
{
    char name[76], version[45], library[45];
    printable(name, sizeof(name), stage->first_required_name);
    printable(version, sizeof(version), stage->first_required_version);
    printable(library, sizeof(library), stage->first_required_library);
    log_line(log, "%s_first_required_missing_name=%s", prefix, name[0] ? name : "none");
    log_line(log, "%s_first_required_missing_version=%s", prefix, version[0] ? version : "none");
    log_line(log, "%s_first_required_missing_provider=%s", prefix, library[0] ? library : "none");
}

static void log_import_summary(FILE *log, const char *prefix,
                               const ImportDiagnostics *diagnostics)
{
    log_line(log, "%s_import_lookups=%u", prefix, diagnostics->lookups);
    log_line(log, "%s_supported_imports=%u", prefix, diagnostics->supported);
    log_line(log, "%s_missing_imports=%u", prefix, diagnostics->missing);
    if (diagnostics->first.name[0]) {
        char name[80], version[48], library[48];
        printable(name, sizeof(name), diagnostics->first.name);
        printable(version, sizeof(version), diagnostics->first.version);
        printable(library, sizeof(library), diagnostics->first.library);
        log_line(log, "%s_first_missing_name=%s", prefix, name);
        log_line(log, "%s_first_missing_version=%s", prefix, version);
        log_line(log, "%s_first_missing_provider=%s", prefix, library);
    }
    const unsigned limit = diagnostics->complete_list ?
        diagnostics->complete_capacity : 24;
    const unsigned recorded = diagnostics->missing < limit ? diagnostics->missing : limit;
    log_line(log, "%s_missing_imports_reported=%u", prefix, recorded);
    log_line(log, "%s_missing_import_list_complete=%s", prefix,
             recorded == diagnostics->missing ? "yes" : "no");
    for (unsigned i = 0; i < recorded; i++) {
        const MissingImport *entry = diagnostics->complete_list ?
            &diagnostics->complete_list[i] : &diagnostics->examples[i];
        char name[80], version[48], library[48];
        printable(name, sizeof(name), entry->name);
        printable(version, sizeof(version), entry->version);
        printable(library, sizeof(library), entry->library);
        log_line(log, "%s_missing_import_%03u=%s @ %s from %s",
                 prefix, i + 1, name, version, library);
    }
}

static void log_resolved_imports(FILE *log, const char *prefix,
                                 const ImportDiagnostics *diagnostics)
{
    const unsigned recorded = diagnostics->resolved_list ?
        (diagnostics->supported < diagnostics->resolved_capacity ?
         diagnostics->supported : diagnostics->resolved_capacity) : 0;
    log_line(log, "%s_resolved_imports_reported=%u", prefix, recorded);
    log_line(log, "%s_resolved_import_list_complete=%s", prefix,
             recorded == diagnostics->supported &&
             !diagnostics->resolved_keys_truncated ? "yes" : "no");
    for (unsigned i = 0; i < recorded; i++) {
        const MissingImport *entry = &diagnostics->resolved_list[i];
        char name[80], version[48], library[48];
        printable(name, sizeof(name), entry->name);
        printable(version, sizeof(version), entry->version);
        printable(library, sizeof(library), entry->library);
        log_line(log, "%s_resolved_import_%03u=%s @ %s from %s",
                 prefix, i + 1, name, version, library);
    }
}

static void audit_thread_tls(FILE *library, const EngineProbeResult *profile,
                             FILE *log)
{
    EngineTlsScratch template_image = {0};
    const EngineTlsScratchStatus status =
        engine_tls_scratch_prepare(library, profile, &template_image);
    log_line(log, "tls_template_status=%d", status);
    log_line(log, "tls_thread_pointer_installed=no");
    log_line(log, "tls_dynamic_modules_supported=no");
    if (status == ENGINE_TLS_PREPARED && template_image.size) {
        EngineTlsThread thread = {0};
        const bool copied = engine_tls_thread_prepare(&template_image, NULL, &thread);
        log_line(log, "tls_thread_image_prepared=%s", copied ? "yes" : "no");
        if (copied) {
            log_line(log, "tls_thread_image_bytes=%zu", thread.size);
            log_line(log, "tls_thread_image_alignment=%zu", thread.alignment);
            log_line(log, "tls_thread_image_is_private=%s",
                     thread.data != template_image.data ? "yes" : "no");
            log_line(log, "tls_dtv_slot_1_prepared=%s",
                     thread.tcb.dtv == &thread.dtv &&
                     thread.dtv.modules[1] == thread.data ? "yes" : "no");
        }
        engine_tls_thread_release(&thread);
    } else if (status == ENGINE_TLS_PREPARED) {
        log_line(log, "tls_thread_image_prepared=empty_template");
    } else {
        log_line(log, "tls_thread_image_prepared=no");
    }
    engine_tls_scratch_release(&template_image);
}

#ifdef MORROW_HAS_STACK_GUARDS
extern size_t morrowStackGuardSize(void *stack);

static void *guard_probe_worker(void *unused)
{
    (void)unused;
    Thread *thread = threadGetSelf();
    if (!thread) return NULL;
    uintptr_t base = (uintptr_t)thread->stack_mirror;
    uintptr_t local = (uintptr_t)&thread;
    size_t guard = morrowStackGuardSize(thread->stack_mirror);
    if (guard != 16384 || guard > base || local < base || local - base >= thread->stack_sz)
        return NULL;
    MemoryInfo info;
    u32 page;
    if (R_FAILED(svcQueryMemory(&info, &page, base - guard)) ||
        info.type != MemType_Unmapped || info.addr > base - guard ||
        info.size < base - info.addr)
        return NULL;
    return thread->stack_mirror;
}

static void probe_guest_thread_guard(FILE *log)
{
    uint64_t c, j, thread;
    if (!android_import_resolve("pthread_create", "LIBC", "libc.so", &c) ||
        !android_import_resolve("pthread_join", "LIBC", "libc.so", &j)) {
        log_line(log, "guest_stack_guard_probe=missing_pthreads");
        return;
    }
    int (*create)(uint64_t *, const void *, void *(*)(void *), void *) = (void *)(uintptr_t)c;
    int (*join)(uint64_t, void **) = (void *)(uintptr_t)j;
    int error = create(&thread, NULL, guard_probe_worker, NULL);
    if (error) { log_line(log, "guest_stack_guard_create_error=%d", error); return; }
    void *stack = NULL;
    error = join(thread, &stack);
    log_line(log, "guest_stack_guard_join=%d unmapped_16k=%d released=%d", error,
             !error && stack, !error && stack && morrowStackGuardSize(stack) == 0);
}
#endif

// Inspect MMU permissions without deliberately faulting the console.
static void probe_guest_data_mapping(FILE *log)
{
    uint64_t map, protect, unmap;
    if (!android_mapping_compat_resolve("mmap", "LIBC", "libc.so", &map) ||
        !android_mapping_compat_resolve("mprotect", "LIBC", "libc.so", &protect) ||
        !android_mapping_compat_resolve("munmap", "LIBC", "libc.so", &unmap)) {
        log_line(log, "guest_mapping_probe=provider_unavailable");
        return;
    }
    void *(*map_fn)(void *, size_t, int, int, int, int64_t) = (void *)(uintptr_t)map;
    int (*protect_fn)(void *, size_t, int) = (void *)(uintptr_t)protect;
    int (*unmap_fn)(void *, size_t) = (void *)(uintptr_t)unmap;
    unsigned char *p = map_fn(NULL, 4096, 3, 0x22, -1, 0);
    if (p == (void *)-1) {
        log_line(log, "guest_mapping_probe=map_failed_errno_%d", errno);
        return;
    }
    bool contents = p[0] == 0 && p[4095] == 0;
    p[4095] = 73;
    MemoryInfo info;
    u32 page_info;
    // Record each transition's raw kernel answer and queried state. The
    // v0.99.4 console run saw every protection check fail but could not say
    // whether svcSetMemoryPermission refused or the query disagreed.
    int protect_rc = protect_fn(p, 4096, 1);
    bool queried = R_SUCCEEDED(svcQueryMemory(&info, &page_info, (u64)(uintptr_t)p));
    log_line(log, "guest_mapping_protect_read_only=%d errno=%d kernel=0x%08x query=%d perm=0x%x type=0x%x attr=0x%x",
             protect_rc, protect_rc ? errno : 0,
             (unsigned)android_mapping_compat_last_kernel_result(),
             queried, queried ? info.perm : 0, queried ? info.type : 0,
             queried ? info.attr : 0);
    bool read_only = !protect_rc && queried && info.perm == Perm_R;
    bool preserved = read_only && p[4095] == 73;
    protect_rc = protect_fn(p, 4096, 0);
    queried = R_SUCCEEDED(svcQueryMemory(&info, &page_info, (u64)(uintptr_t)p));
    log_line(log, "guest_mapping_protect_none=%d errno=%d kernel=0x%08x query=%d perm=0x%x type=0x%x attr=0x%x",
             protect_rc, protect_rc ? errno : 0,
             (unsigned)android_mapping_compat_last_kernel_result(),
             queried, queried ? info.perm : 0, queried ? info.type : 0,
             queried ? info.attr : 0);
    bool inaccessible = !protect_rc && queried && info.perm == Perm_None;
    protect_rc = protect_fn(p, 4096, 3);
    queried = R_SUCCEEDED(svcQueryMemory(&info, &page_info, (u64)(uintptr_t)p));
    log_line(log, "guest_mapping_protect_rw=%d errno=%d kernel=0x%08x query=%d perm=0x%x type=0x%x attr=0x%x",
             protect_rc, protect_rc ? errno : 0,
             (unsigned)android_mapping_compat_last_kernel_result(),
             queried, queried ? info.perm : 0, queried ? info.type : 0,
             queried ? info.attr : 0);
    bool writable = !protect_rc && queried && info.perm == Perm_Rw;
    bool released = unmap_fn(p, 4096) == 0;
    log_line(log, "guest_mapping_probe_zero=%d read_only=%d data=%d none=%d rw=%d released=%d",
             contents, read_only, preserved, inaccessible, writable, released);

    // The mirror-alias failures above are source-grounded: Atmosphere's
    // KPageTableBase::MapMemory hardcodes the destination to UserReadWrite
    // with memory state Stack, and KMemoryState_Stack lacks the
    // FlagCanReprotect bit that KPageTableBase::SetMemoryPermission
    // requires, so every reprotect returns 0x0000d401. This matches the
    // observed type 0xb (Stack), attr 0x0 and unchanged RW perm. The heap's
    // Normal state does carry FlagCanReprotect, so a plain heap page is
    // probed below as the backing the mapping provider now uses; raw
    // kernel answers confirm it on this unit.
    unsigned char *heap = memalign(4096, 4096);
    if (!heap) {
        log_line(log, "guest_heap_protect=alloc_failed");
    } else {
        heap[0] = 1;
        Result heap_rc = svcSetMemoryPermission(heap, 4096, Perm_R);
        bool heap_q = R_SUCCEEDED(svcQueryMemory(&info, &page_info,
                                                 (u64)(uintptr_t)heap));
        log_line(log,
                 "guest_heap_protect_read_only=0x%08x query=%d perm=0x%x type=0x%x attr=0x%x",
                 (unsigned)heap_rc, heap_q, heap_q ? info.perm : 0,
                 heap_q ? info.type : 0, heap_q ? info.attr : 0);
        // Attempt the RW restore on every path. Only hand the page back to
        // the allocator when it is provably writable again: the read-only
        // attempt failed and changed nothing, the restore succeeded, or a
        // fresh query confirms RW. Otherwise retain the page rather than
        // let the allocator hand out a read-only page.
        Result heap_back = svcSetMemoryPermission(heap, 4096, Perm_Rw);
        bool heap_q2 = R_SUCCEEDED(svcQueryMemory(&info, &page_info,
                                                  (u64)(uintptr_t)heap));
        bool safe_to_free = R_FAILED(heap_rc) || R_SUCCEEDED(heap_back) ||
                            (heap_q2 && info.perm == Perm_Rw);
        log_line(log,
                 "guest_heap_protect_restore_rw=0x%08x query=%d perm=0x%x freed=%d",
                 (unsigned)heap_back, heap_q2, heap_q2 ? info.perm : 0,
                 safe_to_free);
        if (safe_to_free) {
            free(heap);
        } else {
            log_line(log, "guest_heap_protect_page_retained=1");
        }
    }
    // envIsSyscallHinted only reports that this process may attempt these
    // privileged code-memory syscalls; it says nothing about whether the
    // calls are granted or would succeed. The RX path's actual svc results
    // remain the only capability evidence.
    log_line(log, "guest_code_memory_syscalls_hinted=map:%d unmap:%d setperm:%d",
             envIsSyscallHinted(0x77) ? 1 : 0, envIsSyscallHinted(0x78) ? 1 : 0,
             envIsSyscallHinted(0x73) ? 1 : 0);

    // Hints only permit attempting the code-memory syscalls. Map one
    // private test page RX and read it back to learn whether they are
    // actually granted; nothing is executed, guest or otherwise.
    if (envGetOwnProcessHandle() == INVALID_HANDLE) {
        log_line(log, "guest_code_mapping_probe=process_handle_unavailable");
        return;
    }
    unsigned char *code_src = memalign(4096, 4096);
    if (!code_src) {
        log_line(log, "guest_code_mapping_probe=alloc_failed");
        return;
    }
    for (size_t i = 0; i < 4096; i++) code_src[i] = (unsigned char)(i ^ 0x5a);
    armDCacheFlush(code_src, 4096);
    virtmemLock();
    void *span = virtmemFindCodeMemory(4096, 4096);
    void *rsv = span ? virtmemAddReservation(span, 4096) : NULL;
    virtmemUnlock();
    if (!rsv) {
        log_line(log, "guest_code_mapping_probe=reservation_failed");
        free(code_src);
        return;
    }
    const Handle self = envGetOwnProcessHandle();
    Result map_rc = svcMapProcessCodeMemory(self, (u64)(uintptr_t)span,
                                            (u64)(uintptr_t)code_src, 4096);
    // Per-step lines: a fatal between lines localizes the failing stage.
    log_line(log, "guest_code_mapping_map=0x%08x", (unsigned)map_rc);
    if (R_FAILED(map_rc)) {
        virtmemLock();
        virtmemRemoveReservation(rsv);
        virtmemUnlock();
        free(code_src);
        return;
    }
    Result rx_rc = svcSetProcessMemoryPermission(self, (u64)(uintptr_t)span,
                                                 4096, Perm_Rx);
    log_line(log, "guest_code_mapping_rx=0x%08x", (unsigned)rx_rc);
    bool rx_query = false, content_match = false;
    unsigned rx_perm = 0, rx_type = 0;
    if (R_SUCCEEDED(rx_rc)) {
        armICacheInvalidate(span, 4096);
        rx_query = R_SUCCEEDED(svcQueryMemory(&info, &page_info,
                                              (u64)(uintptr_t)span));
        rx_perm = rx_query ? info.perm : 0;
        rx_type = rx_query ? info.type : 0;
        // The kernel marks the SOURCE pages NotMapped+Locked while they are
        // mapped as code (KPageTableBase::MapCodeMemory), so reading
        // code_src here would fault. Verify against the destination only,
        // regenerating the known pattern locally.
        const unsigned char *dst = (const unsigned char *)span;
        content_match = true;
        for (size_t i = 0; i < 4096; i++) {
            if (dst[i] != (unsigned char)(i ^ 0x5a)) { content_match = false; break; }
        }
    }
    log_line(log, "guest_code_mapping_query=%d perm=0x%x type=0x%x content_match=%d",
             rx_query, rx_perm, rx_type, content_match);
    // Roll back: writable again, then unmap the code pages. A rollback
    // failure retains the reservation rather than losing track of it.
    Result back_rc = svcSetProcessMemoryPermission(self, (u64)(uintptr_t)span,
                                                   4096, Perm_Rw);
    Result unmap_rc = R_SUCCEEDED(back_rc)
        ? svcUnmapProcessCodeMemory(self, (u64)(uintptr_t)span,
                                    (u64)(uintptr_t)code_src, 4096)
        : back_rc;
    log_line(log, "guest_code_mapping_rollback=0x%08x", (unsigned)unmap_rc);
    if (R_SUCCEEDED(unmap_rc)) {
        virtmemLock();
        virtmemRemoveReservation(rsv);
        virtmemUnlock();
        // The unmap should have returned the source to heap RW; verify
        // before handing the page back to the allocator.
        bool src_q = R_SUCCEEDED(svcQueryMemory(&info, &page_info,
                                                (u64)(uintptr_t)code_src));
        if (src_q && info.perm == Perm_Rw) {
            // RW restored: reading the source is legal again. Confirm the
            // bytes survived the map/unmap round trip intact.
            bool src_match = true;
            for (size_t i = 0; i < 4096; i++) {
                if (code_src[i] != (unsigned char)(i ^ 0x5a)) {
                    src_match = false;
                    break;
                }
            }
            log_line(log, "guest_code_mapping_source_restored=1 content_match=%d",
                     src_match ? 1 : 0);
            free(code_src);
        } else {
            log_line(log, "guest_code_mapping_source_retained=1 query=%d perm=0x%x",
                     src_q, src_q ? info.perm : 0);
        }
    } else {
        // Keep both ends of the possibly-live alias: the reservation stays
        // tracked and the source page is never returned to the allocator.
        log_line(log,
                 "guest_code_mapping_reservation_retained=1 source_retained=1");
    }
}

typedef struct {
    FILE *log;
    char report_path[112];
    atomic_size_t active_index;
    atomic_bool stop_watchdog;
} ConstructorRunContext;

// The 0.99.6 hardware run returned from constructors 0-2 then went black
// after constructor_begin_003. A marker on the main thread cannot tell an
// endless guest call from a process that died. An independent, low-frequency
// monitor records liveness without cancelling, skipping or patching guest code.
// Keep its output separate from the main FILE so a guest-blocked main thread
// cannot hold the report's stdio lock while the monitor is writing.
static void constructor_watchdog_record(const ConstructorRunContext *ctx,
                                        size_t index, unsigned elapsed)
{
    const int fd = open(ENGINE_REPORT_DIR "/constructor-watchdog.txt",
                        O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) return;
    char line[240];
    const int n = index == SIZE_MAX
        ? snprintf(line, sizeof(line),
                   "%ld.%llu link_report=%.95s watchdog_started=yes\n",
                   (long)time(NULL), (unsigned long long)armGetSystemTick(),
                   ctx->report_path)
        : snprintf(line, sizeof(line),
                   "%ld.%llu link_report=%.95s active_constructor=%zu approx_seconds=%u\n",
                   (long)time(NULL), (unsigned long long)armGetSystemTick(),
                   ctx->report_path, index, elapsed);
    if (n > 0 && (size_t)n < sizeof(line)) {
        size_t written = 0;
        while (written < (size_t)n) {
            const ssize_t chunk = write(fd, line + written, (size_t)n - written);
            if (chunk < 0 && errno == EINTR) continue;
            if (chunk <= 0) break;
            written += (size_t)chunk;
        }
        if (written == (size_t)n) fsync(fd);
    }
    close(fd);
}

static void constructor_watchdog_thread(void *argument)
{
    ConstructorRunContext *ctx = argument;
    size_t last_index = SIZE_MAX;
    unsigned elapsed = 0;
    while (!atomic_load_explicit(&ctx->stop_watchdog, memory_order_acquire)) {
        svcSleepThread(1000000000LL);
        if (atomic_load_explicit(&ctx->stop_watchdog, memory_order_acquire))
            break;
        const size_t index = atomic_load_explicit(&ctx->active_index,
                                                   memory_order_acquire);
        if (index == SIZE_MAX) {
            last_index = SIZE_MAX;
            elapsed = 0;
            continue;
        }
        if (index != last_index) {
            last_index = index;
            elapsed = 0;
        }
        if (elapsed < UINT_MAX) elapsed++;
        if (elapsed == 3 || elapsed == 10 || elapsed == 30 ||
            (elapsed > 30 && elapsed % 30 == 0))
            constructor_watchdog_record(ctx, index, elapsed);
    }
}

// Gate proof for the constructor runner: the RX mapping is pinned for the
// process lifetime. TLS is no part of the gate: the caller installs the
// bridge only when the ELF carries a PT_TLS template, and an ELF without
// TLS has no thread-pointer state to bridge before its constructors run.
static bool constructor_run_gate(void *context)
{
    (void)context;
    return engine_mapping_pinned;
}

static bool constructor_begin(size_t index, uintptr_t target, void *context)
{
    ConstructorRunContext *ctx = context;
    atomic_store_explicit(&ctx->active_index, index, memory_order_release);
    log_line(ctx->log, "constructor_begin_%03zu=0x%016llX", index,
             (unsigned long long)target);
    // Durability is the attribution: a crash after this line leaves the
    // constructor's index as the report's last durable marker.
    return fflush(ctx->log) == 0 && fsync(fileno(ctx->log)) == 0;
}

static bool constructor_completed(size_t index, uintptr_t target, void *context)
{
    ConstructorRunContext *ctx = context;
    (void)target;
    atomic_store_explicit(&ctx->active_index, SIZE_MAX, memory_order_release);
    log_line(ctx->log, "constructor_completed_%03zu=yes", index);
    return true;
}

static void constructor_call(uintptr_t target, void *context)
{
    (void)context;
    engine_execution_call_constructor(target);
}

void horizon_real_link_run(RealLinkResult *result, unsigned action)
{
    if (!result) return;
    memset(result, 0, sizeof(*result));
    result->attempted = true;
    if (mkdir(ENGINE_REPORT_DIR, 0777) != 0 && errno != EEXIST) {
        detail(result, "Cannot create the SD diagnostic folder.");
        return;
    }
    FILE *log = new_log(result->log_path, sizeof(result->log_path));
    if (!log) {
        detail(result, "Cannot save a numbered link report.");
        return;
    }
    log_line(log, "# Morrow native engine entry readiness attempt");
    log_line(log, "morrow_build_version=%s", MORROW_BUILD_VERSION);
    log_line(log, "attempt_epoch=%ld attempt_tick=%llu", (long)time(NULL),
             (unsigned long long)armGetSystemTick());
    if (action == 0) {
        probe_guest_data_mapping(log);
#ifdef MORROW_HAS_STACK_GUARDS
        probe_guest_thread_guard(log);
#endif
    }
    log_line(log, "# JNI_OnLoad is opt-in and blocked unless strict mapping and runtime gates pass.");
    log_line(log, "engine_execution_action=%s",
             action == 1 ? "jni_onload" : action == 2 ? "game_activity" :
             action == 3 ? "app_bridge_v2" :
             action == 4 ? "app_bridge_main_thread_message_once" : "diagnostic");
    log_line(log, "exclusive_graphics_ownership=%s",
             exclusive_graphics_ownership ? "yes" : "no");
    if (action >= 1 && action <= 4 && !exclusive_graphics_ownership) {
        detail(result, "Close Morrow UI first: use ZL+ZR+Minus on Loader.");
        log_line(log, "blocker=ui_sdl_renderer_owns_horizon_window");
        goto done;
    }
    if (action == 4) {
        log_line(log, "message_pump_mode=manual_one_shot_not_android_lifecycle");
        log_line(log, "app_bridge_sequence_returned=%s",
                 app_bridge_sequence_returned ? "yes" : "no");
        log_line(log, "main_thread_message_mapped_rx_ready=%s",
                 live_main_thread_message_target ? "yes" : "no");
        if (!engine_mapping_pinned || !engine_jni_onload_succeeded ||
            !app_bridge_sequence_returned) {
            detail(result, "Run successful JNI_OnLoad and App Bridge first.");
            log_line(log, "blocker=app_bridge_sequence_not_ready");
            goto done;
        }
        if (!live_main_thread_message_target) {
            detail(result, "Message callback absent or not in mapped RX pages.");
            log_line(log, "blocker=message_callback_not_executable");
            goto done;
        }
        if (main_thread_message_attempt_consumed) {
            detail(result, "Message callback already attempted; restart Morrow.");
            log_line(log, "blocker=main_thread_message_already_attempted");
            goto done;
        }
#ifdef MORROW_HAS_JNIVM
        if (!morrow_jni_runtime_jni_env()) {
            detail(result, "Message callback blocked: JNI environment missing.");
            log_line(log, "blocker=message_callback_jni_environment_missing");
            goto done;
        }
        // No background thread or repeating UI-frame callback: the observed
        // ABI expects main-thread ownership, and Android event-loop readiness
        // cannot be claimed on Horizon yet. One user-requested call per boot.
        main_thread_message_attempt_consumed = true;
        if (!engine_run_journal_begin(ENGINE_REPORT_DIR,
                                      "app_bridge_main_thread_message_once",
                                      result->log_path)) {
            detail(result, "Message callback blocked: crash marker failed.");
            log_line(log, "blocker=crash_journal_not_durable");
            goto done;
        }
        log_line(log, "event=entering_nativeCallMessagesFromMainThread");
        if (fflush(log) != 0 || fsync(fileno(log)) != 0) {
            engine_run_journal_finish(ENGINE_REPORT_DIR);
            detail(result, "Message callback blocked: SD report sync failed.");
            log_line(log, "blocker=link_report_not_durable");
            goto done;
        }
        MorrowAppBridgeCallResult callback = {0};
        const bool returned = morrow_jni_runtime_call_app_bridge(
            live_main_thread_message_target,
            MORROW_APP_BRIDGE_MAIN_THREAD_MESSAGES, &callback);
        log_line(log, "main_thread_message_invoked=%s",
                 callback.entry_invoked ? "yes" : "no");
        log_line(log, "main_thread_message_returned=%s",
                 callback.entry_returned ? "yes" : "no");
        log_line(log, "main_thread_message_java_exception=%s",
                 callback.java_exception_pending ? "yes" : "no");
        log_line(log, "main_thread_message_cpp_exception=%s",
                 callback.cpp_exception_caught ? "yes" : "no");
        const bool marker_cleared = engine_run_journal_finish_report(
            ENGINE_REPORT_DIR, log);
        log_line(log, "main_thread_message_crash_marker_cleared=%s",
                 marker_cleared ? "yes" : "no");
        log_line(log, "roblox_login_verified=no");
        log_line(log, "roblox_frame_presented=no");
        if (marker_cleared && returned && !callback.java_exception_pending)
            detail(result, "Message callback returned; no Roblox frame verified.");
        else {
            detail(result, "Message callback failed; inspect numbered report.");
            log_line(log, "blocker=message_callback_failed");
        }
#else
        detail(result, "JNI-enabled Morrow artifact required for message callback.");
        log_line(log, "blocker=jni_runtime_not_compiled");
#endif
        goto done;
    }
    if (action == 3) {
        log_line(log, "app_bridge_prior_jni_onload_succeeded=%s",
                 engine_jni_onload_succeeded ? "yes" : "no");
        if (!engine_mapping_pinned || !engine_jni_onload_succeeded) {
            detail(result, "First complete ZL+L3 JNI_OnLoad before App Bridge.");
            log_line(log, "blocker=engine_jni_onload_not_ready");
            goto done;
        }
        if (app_bridge_attempt_consumed) {
            detail(result, "App Bridge already attempted; restart Morrow.");
            log_line(log, "blocker=app_bridge_already_attempted");
            goto done;
        }
        for (unsigned i = 0; i < 5; ++i) {
            log_line(log, "app_bridge_step_%u_rx_target=%s", i + 1,
                     live_app_bridge_targets[i] ? "yes" : "no");
            if (!live_app_bridge_targets[i]) {
                detail(result, "App Bridge export absent or outside mapped RX pages.");
                log_line(log, "blocker=app_bridge_entry_not_executable");
                goto done;
            }
        }
        if (activity_lifecycle_attempt_consumed) {
            detail(result, "Activity lifecycle already attempted; restart Morrow.");
            log_line(log, "blocker=activity_lifecycle_already_attempted");
            goto done;
        }
        for (unsigned i = 0; i < ENGINE_ACTIVITY_LIFECYCLE_STAGES; ++i) {
            const char *name = engine_activity_lifecycle_stage_name(i);
            log_line(log, "activity_lifecycle_%s_rx_ready=%s", name,
                     live_activity_lifecycle_targets[i] ? "yes" : "no");
            if (!live_activity_lifecycle_targets[i]) {
                detail(result, "Android lifecycle native export missing or non-RX.");
                log_line(log, "blocker=activity_lifecycle_entry_not_executable");
                goto done;
            }
        }
        log_line(log, "app_bridge_surface_update_rx_target=%s",
                 live_app_bridge_surface_update_target ? "yes" : "no");
        log_line(log, "app_bridge_surface_update_invoked=no_no_resize_event");
#ifdef MORROW_HAS_JNIVM
        const char *const names[] = {
            "nativeGameGlobalInit", "nativeUpdateAdapterInit",
            "nativeAppBridgeV2InitWithParams", "nativeAppBridgeStartLuaAppDM",
            "nativeAppBridgeV2StartAppWithParams"
        };
        if (!morrow_jni_runtime_jni_env() ||
            !morrow_jni_runtime_asset_manager_bound() ||
            !morrow_jni_runtime_surface_bound() ||
            !morrow_jni_runtime_app_bridge_init_params() ||
            !morrow_jni_runtime_app_bridge_start_params()) {
            detail(result, "App Bridge blocked: missing JNI, assets or Surface.");
            log_line(log, "blocker=app_bridge_runtime_prerequisite_missing");
            goto done;
        }
        log_line(log, "app_bridge_params=partial_missing_android_application_and_account");
        log_line(log, "app_bridge_message_pump=not_started_android_event_loop_unimplemented");
        log_line(log, "app_bridge_execution_mode=explicit_experimental_user_request");
        // Cordial's observed ActivityNativeMain launch fires these nine JNI
        // callbacks in order before App Bridge. Each guest entry is journaled
        // to SD before invocation; never skip a missing stage, and never
        // mark the Android Application as implemented by these calls alone.
        activity_lifecycle_attempt_consumed = true;
        for (unsigned i = 0; i < ENGINE_ACTIVITY_LIFECYCLE_STAGES; ++i) {
            const char *name = engine_activity_lifecycle_stage_name(i);
            char phase[96];
            snprintf(phase, sizeof(phase), "activity_lifecycle_%u_%s", i + 1, name);
            if (!engine_run_journal_begin(ENGINE_REPORT_DIR, phase, result->log_path)) {
                log_line(log, "blocker=activity_lifecycle_crash_journal_not_durable");
                detail(result, "Lifecycle blocked: cannot save crash marker.");
                goto done;
            }
            log_line(log, "event=entering_%s", phase);
            if (fflush(log) != 0 || fsync(fileno(log)) != 0) {
                engine_run_journal_finish(ENGINE_REPORT_DIR);
                log_line(log, "blocker=activity_lifecycle_report_not_durable");
                detail(result, "Lifecycle blocked: SD report could not sync.");
                goto done;
            }
            MorrowActivityLifecycleResult stage = {0};
            const bool returned = morrow_jni_runtime_call_activity_lifecycle(
                live_activity_lifecycle_targets[i], &stage);
            log_line(log, "activity_lifecycle_%s_invoked=%s", name,
                     stage.entry_invoked ? "yes" : "no");
            log_line(log, "activity_lifecycle_%s_returned=%s", name,
                     stage.entry_returned ? "yes" : "no");
            log_line(log, "activity_lifecycle_%s_java_exception=%s", name,
                     stage.java_exception_pending ? "yes" : "no");
            log_line(log, "activity_lifecycle_%s_cpp_exception=%s", name,
                     stage.cpp_exception_caught ? "yes" : "no");
            const bool durable = engine_run_journal_finish_report(
                ENGINE_REPORT_DIR, log);
            if (!durable || !returned || !stage.entry_returned) {
                detail(result, "Lifecycle stopped; see last recorded callback.");
                log_line(log, "blocker=activity_lifecycle_callback_failed");
                goto done;
            }
        }
        log_line(log, "activity_lifecycle_nine_callbacks_returned=yes");
        log_line(log, "android_application_lifecycle_complete=no");
        app_bridge_attempt_consumed = true;
        detail(result, "App Bridge attempted; inspect report and crash marker.");
        for (unsigned i = 0; i < 5; ++i) {
            char phase[92];
            snprintf(phase, sizeof(phase), "app_bridge_%u_%s", i + 1, names[i]);
            if (!engine_run_journal_begin(ENGINE_REPORT_DIR, phase, result->log_path)) {
                log_line(log, "blocker=crash_journal_not_durable");
                detail(result, "App Bridge blocked: cannot save crash marker.");
                break;
            }
            log_line(log, "event=entering_%s", phase);
            if (fflush(log) != 0 || fsync(fileno(log)) != 0) {
                engine_run_journal_finish(ENGINE_REPORT_DIR);
                log_line(log, "blocker=link_report_not_durable");
                detail(result, "App Bridge blocked: report could not sync to SD.");
                break;
            }
            MorrowAppBridgeCallResult call = {0};
            const bool returned = morrow_jni_runtime_call_app_bridge(
                live_app_bridge_targets[i], (MorrowAppBridgeStep)i, &call);
            log_line(log, "app_bridge_step_%u_invoked=%s", i + 1,
                     call.entry_invoked ? "yes" : "no");
            log_line(log, "app_bridge_step_%u_returned=%s", i + 1,
                     call.entry_returned ? "yes" : "no");
            log_line(log, "app_bridge_step_%u_java_exception=%s", i + 1,
                     call.java_exception_pending ? "yes" : "no");
            log_line(log, "app_bridge_step_%u_cpp_exception=%s", i + 1,
                     call.cpp_exception_caught ? "yes" : "no");
            const bool marker_cleared = engine_run_journal_finish_report(
                ENGINE_REPORT_DIR, log);
            if (!marker_cleared || !returned || call.java_exception_pending) {
                log_line(log, "blocker=app_bridge_step_failed");
                detail(result, "App Bridge stopped; read last completed step.");
                break;
            }
            if (i == 4) {
                app_bridge_sequence_returned = true;
                log_line(log, "app_bridge_sequence_returned=yes");
                detail(result, "Five native entries returned; no gameplay verified.");
            }
        }
        log_line(log, "roblox_login_verified=no");
        log_line(log, "engine_gameplay_started=no");
#else
        detail(result, "JNI-enabled Morrow artifact required for App Bridge.");
        log_line(log, "blocker=jni_runtime_not_compiled");
#endif
        goto done;
    }
    if (action == 2) {
        log_line(log, "game_activity_prior_jni_onload_succeeded=%s",
                 engine_jni_onload_succeeded ? "yes" : "no");
        log_line(log, "game_activity_entry_pointer_valid=%s",
                 live_game_activity_target ? "yes" : "no");
        if (!engine_mapping_pinned || !engine_jni_onload_succeeded ||
            !live_game_activity_target) {
            detail(result, "First complete ZL+L3 JNI_OnLoad before GameActivity.");
            log_line(log, "blocker=engine_jni_onload_not_ready");
            goto done;
        }
        if (game_activity_attempt_consumed) {
            detail(result, "Activity entry already attempted. Restart Morrow.");
            log_line(log, "blocker=game_activity_already_attempted");
            goto done;
        }
#ifdef MORROW_HAS_JNIVM
        const bool env_ready = morrow_jni_runtime_jni_env() != NULL;
        const bool activity_ready = morrow_jni_runtime_game_activity_ready();
        const bool config_ready = morrow_jni_runtime_configuration_bound();
        const bool assets_ready = morrow_jni_runtime_asset_manager_bound();
        const bool surface_ready = morrow_jni_runtime_surface_bound();
        log_line(log, "activity_jni_env_ready=%s", env_ready ? "yes" : "no");
        log_line(log, "activity_java_object_ready=%s", activity_ready ? "yes" : "no");
        log_line(log, "activity_configuration_ready=%s", config_ready ? "yes" : "no");
        log_line(log, "activity_real_asset_manager_ready=%s", assets_ready ? "yes" : "no");
        log_line(log, "activity_horizon_surface_ready=%s", surface_ready ? "yes" : "no");
        if (!env_ready || !activity_ready || !config_ready ||
            !assets_ready || !surface_ready) {
            detail(result, "GameActivity blocked: missing actual JNI/assets/surface.");
            log_line(log, "blocker=game_activity_jni_prerequisite_missing");
            goto done;
        }
        game_activity_attempt_consumed = true;
        MorrowGameActivityInitResult init = {0};
        bool returned = false;
        if (!engine_run_journal_begin(ENGINE_REPORT_DIR,
                                      "game_activity_initialize_native", result->log_path)) {
            detail(result, "Cannot save GameActivity crash marker; call blocked.");
            log_line(log, "blocker=crash_journal_not_durable");
            goto done;
        }
        log_line(log, "event=entering_real_game_activity_initialize_native");
        if (fflush(log) != 0 || fsync(fileno(log)) != 0) {
            engine_run_journal_finish(ENGINE_REPORT_DIR);
            log_line(log, "blocker=link_report_not_durable");
            goto done;
        }
        detail(result, "GameActivity attempted. Inspect link report; restart to retry.");
        returned = morrow_jni_runtime_call_game_activity(
            live_game_activity_target, ENGINE_REPORT_DIR, &init);
        game_activity_entry_called = init.entry_invoked;
        log_line(log, "game_activity_initializer_returned=%s",
                 returned && init.entry_returned ? "yes" : "no");
        log_line(log, "game_activity_jni_exception_pending=%s",
                 init.java_exception_pending ? "yes" : "no");
        if (returned)
            log_line(log, "game_activity_native_handle=0x%llx",
                     (unsigned long long)init.native_handle);
        const bool marker_cleared = engine_run_journal_finish_report(
            ENGINE_REPORT_DIR, log);
        log_line(log, "game_activity_crash_marker_cleared=%s",
                 marker_cleared ? "yes" : "no");
        if (!marker_cleared) {
            detail(result, "GameActivity returned but its report was not durable.");
            log_line(log, "blocker=game_activity_return_report_not_durable");
        }
        log_line(log, "engine_gameplay_started=no");
#else
        detail(result, "JNI-enabled Morrow artifact required for GameActivity.");
        log_line(log, "blocker=jni_runtime_not_compiled");
#endif
        goto done;
    }
    if (engine_mapping_pinned) {
        detail(result, "Engine code mapping held. Restart Morrow before retrying.");
        log_line(log, "blocker=prior_engine_mapping_requires_process_restart");
        goto done;
    }
    bool java_surface_bound = false;
#ifdef MORROW_HAS_JNIVM
    java_surface_bound = morrow_jni_runtime_surface_bound();
#endif
    log_line(log, "android_java_surface_registered=%s",
             java_surface_bound ? "yes" : "no");
    log_line(log, "android_java_surface_alias_resolution=%s",
             java_surface_bound ? "known_vm_references_only" : "unavailable");
    log_line(log, "android_java_surface_used_by_engine=no");
    bool java_assets_bound = false;
#ifdef MORROW_HAS_JNIVM
    java_assets_bound = morrow_jni_runtime_asset_manager_bound();
#endif
    log_line(log, "android_java_asset_manager_registered=%s",
             java_assets_bound ? "yes" : "no");
    bool java_configuration_bound = false;
#ifdef MORROW_HAS_JNIVM
    java_configuration_bound = morrow_jni_runtime_configuration_bound();
#endif
    log_line(log, "jni_java_configuration_object_registered=%s",
             java_configuration_bound ? "yes" : "no");
    log_line(log, "jni_java_configuration_primitive_fields=%s",
             java_configuration_bound ?
             (java_surface_bound ? "18_registered_viewport_1x_layout" :
                                   "18_registered_no_surface_geometry") : "unavailable");
    log_line(log, "jni_java_configuration_locales=%s",
             java_configuration_bound ? "device_locale_if_available" : "unavailable");
    log_line(log, "android_java_asset_manager_root=optional_user_assets");
    bool ndk_asset_configuration_ready = false;
#ifdef MORROW_HAS_JNIVM
    ndk_asset_configuration_ready = morrow_jni_runtime_asset_configuration_bound();
#endif
    log_line(log, "android_asset_configuration_registered=%s",
             ndk_asset_configuration_ready ? "yes" : "no");
    log_line(log, "android_asset_configuration_values=%s",
             !ndk_asset_configuration_ready ? "unavailable" :
             java_surface_bound ? "measured_1x_viewport_and_known_locale" :
                                  "known_locale_no_surface_geometry");
    log_line(log, "android_configuration_ndk_functions=9");
    log_line(log, "jni_vm_invocation_abi=%s",
             android_jni_vm_bridge_instance() ? "available" : "unavailable");
    const bool jni_provider_ready = android_jni_vm_bridge_has_provider();
    log_line(log, "jni_vm_provider_installed=%s", jni_provider_ready ? "yes" : "no");
    log_line(log, "jni_env_provider_connected=%s", jni_provider_ready ? "yes" : "no");
    bool synthetic_onload_passed = false;
    bool synthetic_vm_available = false;
    bool synthetic_entry_accepted = false;
    bool synthetic_native_invoked = false;
    bool synthetic_cleanup = false;
#ifdef MORROW_HAS_JNIVM
    MorrowJniOnLoadProbe synthetic_onload = {0};
    synthetic_onload_passed = morrow_jni_runtime_probe_onload(&synthetic_onload);
    synthetic_vm_available = synthetic_onload.vm_available;
    synthetic_entry_accepted = synthetic_onload.jni_onload_accepted;
    synthetic_native_invoked = synthetic_onload.native_invoked;
    synthetic_cleanup = synthetic_onload.native_unregistered &&
                        synthetic_onload.local_references_released;
#endif
    const char *synthetic_status = !synthetic_vm_available ? "vm_unavailable" :
                                   !synthetic_onload_passed ? "registration_or_cleanup_failed" :
                                   !synthetic_native_invoked ? "native_dispatch_missing" : "passed";
    log_line(log, "morrow_synthetic_jni_onload_probe=%s", synthetic_status);
    log_line(log, "morrow_synthetic_jni_onload_vm_available=%s",
             synthetic_vm_available ? "yes" : "no");
    log_line(log, "morrow_synthetic_jni_onload_entry_accepted=%s",
             synthetic_entry_accepted ? "yes" : "no");
    log_line(log, "morrow_synthetic_jni_onload_native_invoked=%s",
             synthetic_native_invoked ? "yes" : "no");
    log_line(log, "morrow_synthetic_jni_onload_cleanup=%s",
             synthetic_cleanup ? "yes" : "no");
    log_line(log, "morrow_synthetic_jni_onload_roblox_code_executed=no");
    log_line(log, "jni_runtime_hardware_verified=no");
    log_line(log, "jni_engine_class_callbacks_complete=no");
    bool game_activity_ready = false;
#ifdef MORROW_HAS_JNIVM
    game_activity_ready = morrow_jni_runtime_game_activity_ready();
#endif
    log_line(log, "jni_game_activity_object_registered=%s",
             game_activity_ready ? "yes" : "no");
#ifdef MORROW_HAS_JNIVM
    log_line(log, "jni_system_locale_bound=%s",
             morrow_jni_runtime_system_locale_bound() ? "yes" : "no");
    int64_t sd_bytes = 0;
    const bool sd_measured = morrow_jni_runtime_storage_available_bytes(&sd_bytes);
    log_line(log, "jni_storage_free_space_measured=%s",
             sd_measured ? "yes" : "no");
    if (sd_measured)
        log_line(log, "jni_storage_available_bytes=%lld", (long long)sd_bytes);
    log_line(log, "jni_local_storage_manager_registered=%s",
             morrow_jni_runtime_local_storage_manager() ? "yes" : "no");
#else
    log_line(log, "jni_system_locale_bound=no");
    log_line(log, "jni_storage_free_space_measured=no");
    log_line(log, "jni_local_storage_manager_registered=no");
#endif
    log_line(log, "roblox_storage_initialised=no");
    log_line(log, "jni_game_activity_class_loader=%s",
             game_activity_ready ? "registered_classes_only" : "unavailable");
    log_line(log, "jni_apk_classpath_installed=no");
    bool partial_app_params = false;
    bool partial_init_params = false;
#ifdef MORROW_HAS_JNIVM
    partial_app_params =
        morrow_jni_runtime_app_bridge_start_params() != NULL &&
        morrow_jni_runtime_app_bridge_start_params_bound();
    partial_init_params =
        morrow_jni_runtime_app_bridge_init_params() != NULL &&
        morrow_jni_runtime_app_bridge_init_params_bound();
#endif
    log_line(log, "app_bridge_init_params_typed_partial=%s",
             partial_init_params ? "yes" : "no");
    log_line(log, "app_bridge_init_params_device_fields=%s",
             partial_init_params ? "21_typed_some_values_unknown" : "unavailable");
    log_line(log, "app_bridge_init_params_user_agent=unknown_unset");
    log_line(log, "app_bridge_init_params_android_api=unknown_unset");
    log_line(log, "app_bridge_start_app_params_typed_partial=%s",
             partial_app_params ? "yes" : "no");
    log_line(log, "app_bridge_start_app_params_surface_from_horizon=%s",
             partial_app_params ? "yes" : "no");
    log_line(log, "app_bridge_start_app_params_platform_assets=%s",
             partial_app_params ? "bound_read_only_directory" : "unavailable");
    log_line(log, "app_bridge_init_params_complete=no");
    log_line(log, "app_bridge_android_app_callbacks_complete=no");
    log_line(log, "app_bridge_account_login_available=no");
    log_line(log, "jni_game_activity_insets_callback=%s",
             game_activity_ready ? "zero_fullscreen_insets_only" : "unavailable");
    log_line(log, "jni_game_activity_get_assets_callback=%s",
             game_activity_ready ? "bound_read_only_manager_or_null" : "unavailable");
    log_line(log, "jni_game_activity_get_resources_callback=%s",
             game_activity_ready ? "measured_horizon_surface_or_null" : "unavailable");
    log_line(log, "jni_resources_get_configuration_callback=%s",
             java_configuration_bound && java_surface_bound ?
             "live_viewport_configuration" : "null_until_both_bound");
    log_line(log, "jni_display_metrics_density=neutral_1x_not_physical_dpi");
    log_line(log, "jni_game_activity_window_ime_callbacks=%s",
             game_activity_ready ? "registered_observation_only" : "unavailable");
    uint32_t window_flag_requests = 0;
    uint32_t ime_editor_info_requests = 0;
#ifdef MORROW_HAS_JNIVM
    MorrowGameActivityRequests activity_requests = {0};
    if (morrow_jni_runtime_activity_requests(&activity_requests)) {
        window_flag_requests = activity_requests.window_flag_requests;
        ime_editor_info_requests = activity_requests.ime_editor_info_requests;
    }
#endif
    log_line(log, "jni_game_activity_window_flag_request_count=%u", window_flag_requests);
    log_line(log, "jni_game_activity_ime_editor_info_request_count=%u", ime_editor_info_requests);
    log_line(log, "jni_game_activity_window_flags_applied=no");
    log_line(log, "jni_game_activity_ime_editor_info_applied=no");
    log_line(log, "jni_asset_reference_alias_resolution=%s",
             jni_provider_ready ? "known_vm_references_only" : "unavailable");
    log_line(log, "jni_game_activity_bootstrap_implemented=no");
    log_line(log, "jni_onload_call_eligible=requires_runtime_preflight");
    log_line(log, "native_egl_gles_symbols_linked=%s",
             android_egl_gles_compat_has_backend() ? "yes" : "no");
    log_line(log, "native_egl_gles_symbol_catalogue=%u",
             android_egl_gles_compat_known_count());
    log_line(log, "guest_egl_window_surface_bridge=registered_surfaces_only");
    log_line(log, "guest_egl_window_surface_registered=no");
    log_line(log, "guest_egl_context_current=no");
    log_line(log, "event=preflight_started");

    // On a rare release failure retain the reservation for the next L3.
    Result kernel_error = 0;
    if (reservation.reservation &&
        !horizon_code_memory_unmap(&reservation, &kernel_error)) {
        detail(result, "Previous code-address reservation could not be released.");
        log_line(log, "blocker=previous_reservation_cleanup_failed");
        goto done;
    }

    FILE *file = fopen(ENGINE_LIBRARY_PATH, "rb");
    if (!file) {
        detail(result, "No ARM64 libroblox.so in the SD engine folder.");
        log_line(log, "blocker=engine_file_missing");
        goto done;
    }

    EngineProbeResult probe;
    engine_probe_stream(file, &probe);
    log_line(log, "inspect_status=%d", probe.status);
    if (probe.status != ENGINE_PROBE_INSPECTED) {
        detail(result, probe.detail);
        log_line(log, "blocker=elf_inspection_failed");
        fclose(file);
        goto done;
    }
    log_line(log, "file_bytes=%llu", (unsigned long long)probe.file_bytes);
    log_line(log, "load_segments=%u", probe.load_segments);
    log_line(log, "max_segment_alignment=%llu",
             (unsigned long long)probe.max_segment_alignment);
    log_line(log, "android_dependencies=%u", probe.needed_count);
    log_line(log, "tls_segment=%s", probe.has_tls_segment ? "present" : "absent");
    audit_thread_tls(file, &probe, log);

    EngineEntryProbe entry = {0};
    engine_entry_probe_stream(file, &entry);
    log_line(log, "entry_probe_status=%d", entry.status);
    log_line(log, "entry_probe_status_name=%s", entry_status_name(entry.status));
    char entry_detail[sizeof(entry.detail)];
    printable(entry_detail, sizeof(entry_detail), entry.detail);
    log_line(log, "entry_probe_detail=%s", entry_detail);
    log_line(log, "jni_onload_exported=%s",
             entry.status != ENGINE_ENTRY_INSPECTED ? "unknown" :
             entry.has_jni_onload ? "yes" : "no");
    if (entry.status == ENGINE_ENTRY_INSPECTED && entry.has_jni_onload)
        log_line(log, "jni_onload_virtual_address=0x%llx",
                 (unsigned long long)entry.jni_onload_virtual_address);
    log_line(log, "game_activity_initialize_native_exported=%s",
             entry.status != ENGINE_ENTRY_INSPECTED ? "unknown" :
             entry.has_game_activity_initialize_native ? "yes" : "no");
    if (entry.status == ENGINE_ENTRY_INSPECTED &&
        entry.has_game_activity_initialize_native)
        log_line(log, "game_activity_initialize_native_virtual_address=0x%llx",
                 (unsigned long long)entry.game_activity_initialize_native_virtual_address);
    log_line(log, "game_activity_initialize_native_call_eligible=only_after_successful_jni_onload");
    log_line(log, "native_activity_exported=%s",
             entry.status != ENGINE_ENTRY_INSPECTED ? "unknown" :
             entry.has_native_activity_oncreate ? "yes" : "no");
    if (entry.status == ENGINE_ENTRY_INSPECTED && entry.has_native_activity_oncreate)
        log_line(log, "native_activity_virtual_address=0x%llx",
                 (unsigned long long)entry.native_activity_virtual_address);
    // Cordial's Android launch trace and app-bridge investigation agree that
    // the Roblox home shell uses NativeGLInterface's V2 bridge, not AGDK's
    // GameActivity surface callbacks. Inspect these actual exports before
    // attempting another ABI-dependent guest entry point.
    log_engine_export(log, "app_bridge_global_init", &entry,
                      entry.has_app_bridge_global_init,
                      entry.app_bridge_global_init_virtual_address);
    log_engine_export(log, "app_bridge_update_adapter_init", &entry,
                      entry.has_app_bridge_update_adapter_init,
                      entry.app_bridge_update_adapter_init_virtual_address);
    log_engine_export(log, "app_bridge_v2_init", &entry,
                      entry.has_app_bridge_v2_init,
                      entry.app_bridge_v2_init_virtual_address);
    log_engine_export(log, "app_bridge_lua_shell_start", &entry,
                      entry.has_app_bridge_lua_shell_start,
                      entry.app_bridge_lua_shell_start_virtual_address);
    log_engine_export(log, "app_bridge_surface_update", &entry,
                      entry.has_app_bridge_surface_update,
                      entry.app_bridge_surface_update_virtual_address);
    log_engine_export(log, "app_bridge_start_app", &entry,
                      entry.has_app_bridge_start_app,
                      entry.app_bridge_start_app_virtual_address);
    log_engine_export(log, "app_bridge_main_thread_messages", &entry,
                      entry.has_app_bridge_main_thread_messages,
                      entry.app_bridge_main_thread_messages_virtual_address);
    for (unsigned i = 0; i < ENGINE_ACTIVITY_LIFECYCLE_STAGES; ++i) {
        char label[92];
        snprintf(label, sizeof(label), "activity_lifecycle_%s",
                 engine_activity_lifecycle_stage_name(i));
        log_engine_export(log, label, &entry, entry.has_activity_lifecycle[i],
                          entry.activity_lifecycle_virtual_address[i]);
    }
    log_line(log, "app_bridge_startup_call_eligible=no_missing_real_autovalue_params_and_lifecycle");
    log_line(log, "app_bridge_startup_called=no");
    log_line(log, "elf_dt_init=%s", entry.has_init ? "present" : "absent");
    if (entry.has_init)
        log_line(log, "elf_dt_init_virtual_address=0x%llx",
                 (unsigned long long)entry.init_virtual_address);
    log_line(log, "elf_init_array_entries=%u", entry.init_array_entries);
    log_line(log, "engine_initializer_called=no");

    EngineMemoryPlan plan = {0};
    if (!engine_memory_plan_from_probe(&probe, &plan)) {
        detail(result, "Cannot construct safe Horizon RX/R/RW plan.");
        log_line(log, "blocker=memory_plan_invalid");
        fclose(file);
        goto done;
    }
    log_line(log, "virtual_image_bytes=%zu", plan.image_bytes);
    size_t search_bytes = 0;
    if (horizon_code_address_search_size(&plan, &search_bytes))
        log_line(log, "code_address_search_bytes=%zu", search_bytes);
    log_line(log, "code_address_alignment_phase=%llu",
             (unsigned long long)(plan.virtual_page_base % plan.alignment_bytes));
    log_line(log, "event=reserve_final_address");
    HorizonCodeReserveStatus reserve_status = HORIZON_RESERVE_INVALID_STATE;
    if (!horizon_code_memory_reserve_detailed(&plan, &reservation, &reserve_status)) {
        if (reserve_status == HORIZON_RESERVE_NO_FREE_SPAN)
            detail(result, "No contiguous code address found. Try title override / restart.");
        else if (reserve_status == HORIZON_RESERVE_SYSCALL_UNAVAILABLE)
            detail(result, "Horizon code-memory syscalls unavailable in this launch mode.");
        else
            detail(result, "Horizon could not reserve a final code address.");
        log_line(log, "reserve_failure_reason=%s", reserve_status_name(reserve_status));
        log_line(log, "blocker=final_address_reservation_failed");
    } else {
        log_line(log, "event=final_address_reserved");
        log_line(log, "reserved_code_address_phase=%zu",
                 (size_t)((uintptr_t)reservation.mapped % plan.alignment_bytes));
        // These are address calculations only; reservation.mapped has not
        // been mapped RX and none of these function pointers are callable.
        if (entry.status == ENGINE_ENTRY_INSPECTED && entry.has_jni_onload &&
            entry.jni_onload_virtual_address >= plan.virtual_page_base) {
            const uint64_t delta = entry.jni_onload_virtual_address -
                                   plan.virtual_page_base;
            if (delta < plan.image_bytes && delta <= UINTPTR_MAX -
                (uintptr_t)reservation.mapped)
                log_line(log, "jni_onload_unmapped_candidate_address=0x%llx",
                         (unsigned long long)((uintptr_t)reservation.mapped + delta));
        }
        if (entry.status == ENGINE_ENTRY_INSPECTED &&
            entry.has_game_activity_initialize_native &&
            entry.game_activity_initialize_native_virtual_address >= plan.virtual_page_base) {
            const uint64_t delta = entry.game_activity_initialize_native_virtual_address -
                                   plan.virtual_page_base;
            if (delta < plan.image_bytes && delta <= UINTPTR_MAX -
                (uintptr_t)reservation.mapped)
                log_line(log, "game_activity_initialize_native_unmapped_candidate_address=0x%llx",
                         (unsigned long long)((uintptr_t)reservation.mapped + delta));
        }
        if (entry.status == ENGINE_ENTRY_INSPECTED &&
            entry.has_app_bridge_start_app &&
            entry.app_bridge_start_app_virtual_address >= plan.virtual_page_base) {
            const uint64_t delta = entry.app_bridge_start_app_virtual_address -
                                   plan.virtual_page_base;
            if (delta < plan.image_bytes && delta <= UINTPTR_MAX -
                (uintptr_t)reservation.mapped)
                log_line(log, "app_bridge_start_app_unmapped_candidate_address=0x%llx",
                         (unsigned long long)((uintptr_t)reservation.mapped + delta));
        }
        EngineStage stage = {0};
        ImportDiagnostics missing = {0};
        log_line(log, "event=strict_final_base_relocation_started");
        const bool ready = engine_stage_stream_resolved_at(
            file, &stage, resolve_compatible, &missing,
            (uint64_t)(uintptr_t)reservation.mapped);
        log_line(log, "strict_stage_status=%d", stage.status);
        char safe[120];
        printable(safe, sizeof(safe), stage.detail);
        log_line(log, "strict_stage_detail=%s", safe);
        log_import_summary(log, "strict", &missing);
        log_required_import(log, "strict", &stage);
        log_line(log, "rela_applied=%u", stage.rela_applied);
        log_line(log, "relr_applied=%u", stage.relr_applied);
        log_line(log, "aps2_applied=%u", stage.packed_applied);
        log_line(log, "symbol_applied=%u", stage.symbol_applied);
        log_line(log, "strict_weak_undefined_relocations=%u", stage.weak_undefined_relocations);
        log_line(log, "strict_required_missing_relocations=%u", stage.required_missing_relocations);
        log_line(log, "dyninit_section_verified=%u",
                 stage.dyninit_section_verified ? 1u : 0u);
        log_line(log, "dyninit_section_status=%s",
                 engine_dyninit_status_name(stage.dyninit_status));
        if (ready) {
            result->stage_prepared = true;
            detail(result, "Strict link passed; Android JNI/graphics runtime still missing.");
            log_line(log, "event=strict_stage_prepared");
            EngineConstructorPlan constructors = {0};
            const bool constructors_verified = engine_constructor_plan_prepare(
                &stage, &entry, &plan, &constructors);
            log_line(log, "constructor_plan_status=%u", (unsigned)constructors.status);
            log_line(log, "constructor_plan_reason=%s",
                     constructors.reason ? constructors.reason : "unknown");
            log_line(log, "constructor_plan_array_entries=%u", constructors.array_entries);
            log_line(log, "constructor_plan_skipped_entries=%u", constructors.skipped_entries);
            log_line(log, "constructor_plan_validated_targets=%zu", constructors.call_count);
            log_line(log, "constructor_plan_first_invalid_index=%u",
                     constructors.first_invalid_index);
            bool constructors_ran = false;
            if (action != 1)
                log_line(log, "execution_gate=not_requested");
            if (action == 1) {
                bool vm_ready = false, env_ready = false, activity_ready = false;
                void *vm = NULL;
#ifdef MORROW_HAS_JNIVM
                vm = morrow_jni_runtime_java_vm();
                vm_ready = vm != NULL;
                env_ready = morrow_jni_runtime_jni_env() != NULL;
                activity_ready = morrow_jni_runtime_game_activity_ready();
#endif
                // Evaluate mapping eligibility prospectively: no privileged
                // map syscall runs until the constructor/TLS/runtime gates pass.
                const EngineExecutionDecision candidate = engine_execution_decide(
                    true, true, true, &probe, &entry, &plan,
                    (uint64_t)(uintptr_t)reservation.mapped,
                    vm_ready, env_ready, activity_ready,
                    false /* constructors_run: pre-constructor gate */);
                log_line(log, "experimental_jni_onload_preflight=%s", candidate.reason);
                // The constructor run itself lives behind this gate, so an
                // ELF whose ONLY blocker is unrun constructors must proceed:
                // refusing here would strand every init-array ELF forever
                // (observed on hardware: preflight=elf_constructors_not_run
                // with constructor_plan_executed=no). Any other refusal
                // skips the whole path as before.
                const bool constructors_pending_only =
                    !candidate.eligible && candidate.reason &&
                    strcmp(candidate.reason, "elf_constructors_not_run") == 0;
                // dlopen ordering says WHEN constructors run, not WHAT they
                // may touch: these arbitrary constructors could use any
                // runtime service. Every non-constructor prerequisite
                // (runtime readiness, memory plan, RX target) is knowable
                // now, so all of them gate BEFORE the first guest
                // constructor executes; none has a reason to await the run.
                const EngineExecutionDecision prerequisites =
                    engine_execution_decide(
                        true, true, true, &probe, &entry, &plan,
                        (uint64_t)(uintptr_t)reservation.mapped,
                        vm_ready, env_ready, activity_ready,
                        true /* constructors_run: evaluate every other rule */);
                log_line(log, "experimental_jni_onload_prerequisites=%s",
                         prerequisites.reason);
                if (!candidate.eligible && !constructors_pending_only) {
                    log_line(log, "experimental_jni_onload_called=no");
                } else if (!prerequisites.eligible) {
                    log_line(log, "blocker=jni_entry_prerequisites_not_met");
                    detail(result,
                           "Engine blocked: JNI entry prerequisites not met.");
                } else if (!constructors_verified) {
                    log_line(log, "experimental_jni_onload_blocker=constructor_plan_invalid");
                } else if (stage.image_size != plan.image_bytes ||
                           stage.lowest_virtual_address != plan.virtual_page_base ||
                           stage.intended_runtime_base !=
                               (uint64_t)(uintptr_t)reservation.mapped) {
                    log_line(log, "experimental_jni_onload_blocker=stage_plan_mismatch");
                } else {
                    Result map_error = 0;
                    log_line(log, "event=experimental_engine_rx_mapping_started");
                    if (!horizon_code_memory_map_reserved(&plan, stage.image,
                                                          &reservation, &map_error)) {
                        log_line(log, "experimental_jni_onload_blocker=rx_mapping_failed");
                        log_line(log, "experimental_mapping_kernel_result=0x%08X",
                                 (unsigned)map_error);
                        if (reservation.active) {
                            // A kernel rollback error can leave the image
                            // mapped. Never free its backing source or try
                            // another guest map in the same process.
                            engine_mapping_pinned = true;
                            stage.image = NULL;
                            log_line(log, "blocker=mapped_code_rollback_failed_restart_required");
                        }
                    } else {
                        // Engine-owned JNI registrations can outlive the call.
                        // Retain both mappings even if JNI_OnLoad returns error.
                        engine_mapping_pinned = true;
                        stage.image = NULL;
                        log_line(log, "engine_mapped_executable=yes");
                        // Register the mapped image so a guest
                        // dl_iterate_phdr (its C++ unwinder needs it) sees
                        // the real module, with the header table read back
                        // from the mapped bytes rather than trusted input.
                        {
                            const unsigned char *mapped =
                                (const unsigned char *)reservation.mapped;
                            uint64_t phoff = 0;
                            uint16_t phnum = 0;
                            memcpy(&phoff, mapped + 0x20, sizeof(phoff));
                            memcpy(&phnum, mapped + 0x38, sizeof(phnum));
                            uint64_t bias = (uint64_t)(uintptr_t)mapped -
                                stage.lowest_virtual_address;
                            if (phnum && phoff <= plan.image_bytes &&
                                (uint64_t)phnum * 56 <= plan.image_bytes - phoff &&
                                engine_module_register(bias, mapped + phoff,
                                                       phnum, "libroblox.so")) {
                                log_line(log,
                                         "engine_module_registered=bias_0x%llX_phnum_%u",
                                         (unsigned long long)bias, phnum);
                            } else {
                                log_line(log,
                                         "engine_module_registered=no");
                            }
                        }
                        // Milestone B: run the validated constructors before
                        // JNI_OnLoad, as dlopen would. The guest thread
                        // pointer is installed for the whole constructor +
                        // JNI_OnLoad window and restored once at the end.
                        if (!engine_main_tls_ready) {
                            EngineTlsScratch scratch = {0};
                            if (engine_tls_scratch_prepare(file, &probe,
                                                           &scratch) ==
                                    ENGINE_TLS_PREPARED && scratch.size &&
                                engine_tls_thread_prepare(&scratch, NULL,
                                                          &engine_main_tls))
                                engine_main_tls_ready = true;
                            engine_tls_scratch_release(&scratch);
                        }
                        log_line(log, "guest_main_tls_ready=%s",
                                 engine_main_tls_ready ? "yes" : "no");
                        uint64_t saved_tpidr = 0;
                        bool bridge_installed = false;
                        bool constructors_attempted = false;
                        bool constructor_marker_cleared = false;
                        EngineConstructorRunState run_state = {0};
                        if (!constructors.call_count) {
                            log_line(log, "constructor_plan_empty=yes");
                        } else if (!engine_run_journal_begin(ENGINE_REPORT_DIR,
                                                             "constructors",
                                                             result->log_path)) {
                            log_line(log, "blocker=crash_journal_not_durable");
                        } else {
                            constructors_attempted = true;
                            log_line(log, "constructor_crash_marker_created=yes");
                            if (engine_main_tls_ready) {
                                saved_tpidr = engine_guest_tpidr_install(
                                    android_arm64_tcb_shadow_pointer(
                                        &engine_main_tls.tcb));
                                bridge_installed = true;
                                log_line(log, "guest_tpidr_bridge=installed");
                            } else {
                                // libroblox.so ships no PT_TLS and reads no
                                // thread-pointer register in its audited
                                // static code, so constructors run without a
                                // bridge. Dynamically reached code paths keep
                                // this a console-validation obligation.
                                log_line(log,
                                         "guest_tpidr_bridge=not_needed_no_tls_template");
                            }
                            // Static lifetime matters if a kernel thread
                            // refuses to join during shutdown: it must not
                            // keep reading a dead stack object.
                            static ConstructorRunContext ctx;
                            ctx.log = log;
                            snprintf(ctx.report_path, sizeof(ctx.report_path),
                                     "%s", result->log_path);
                            atomic_init(&ctx.active_index, SIZE_MAX);
                            atomic_init(&ctx.stop_watchdog, false);
                            Thread watchdog = {0};
                            const Result watchdog_create = threadCreate(
                                &watchdog, constructor_watchdog_thread, &ctx,
                                NULL, 0x10000, 0x2C, -2);
                            const Result watchdog_start = R_SUCCEEDED(watchdog_create)
                                ? threadStart(&watchdog) : watchdog_create;
                            const bool watchdog_running = R_SUCCEEDED(watchdog_start);
                            log_line(log, "constructor_watchdog_started=%s result=0x%08x",
                                     watchdog_running ? "yes" : "no",
                                     (unsigned)watchdog_start);
                            if (watchdog_running)
                                constructor_watchdog_record(&ctx, SIZE_MAX, 0);
                            if (R_SUCCEEDED(watchdog_create) && !watchdog_running)
                                threadClose(&watchdog);
                            const bool constructors_done =
                                engine_constructor_run_once(
                                    &constructors, &run_state,
                                    constructor_run_gate, constructor_begin,
                                    constructor_completed, constructor_call,
                                    &ctx);
                            if (watchdog_running) {
                                atomic_store_explicit(&ctx.stop_watchdog, true,
                                                      memory_order_release);
                                const Result joined = threadWaitForExit(&watchdog);
                                log_line(log, "constructor_watchdog_join=0x%08x",
                                         (unsigned)joined);
                                if (R_SUCCEEDED(joined)) threadClose(&watchdog);
                            }
                            log_line(log, "constructor_run_completed=%s",
                                     constructors_done ? "yes" : "no");
                            log_line(log, "constructor_run_next_index=%zu",
                                     run_state.next_index);
                            constructor_marker_cleared =
                                engine_run_journal_finish(ENGINE_REPORT_DIR);
                            log_line(log, "constructor_crash_marker_cleared=%s",
                                     constructor_marker_cleared ? "yes" : "no");
                            if (!constructor_marker_cleared)
                                log_line(log,
                                         "blocker=constructor_crash_marker_stuck");
                            // Even a journal-failed partial run executed
                            // guest code; the count above says how far.
                            constructors_ran =
                                constructors_done || run_state.next_index != 0;
                        }
                        // JNI_OnLoad stays closed unless the entry-readiness
                        // rule proves initialization completed durably; a
                        // partial run is diagnostics, not readiness.
                        const bool constructors_ok =
                            engine_constructor_run_entry_ready(
                                &constructors, &run_state,
                                constructors_attempted,
                                constructor_marker_cleared);
                        log_line(log, "constructors_ok_for_jni_onload=%s",
                                 constructors_ok ? "yes" : "no");
                        // The entry decision after constructors re-evaluates
                        // every rule, including the ones the pre-constructor
                        // gate never reached (runtime readiness, memory plan,
                        // RX target). Its target is the one JNI_OnLoad uses.
                        EngineExecutionDecision entry_decision = candidate;
                        if (constructors_ok && constructors.call_count) {
                            entry_decision = engine_execution_decide(
                                true, true, true, &probe, &entry, &plan,
                                (uint64_t)(uintptr_t)reservation.mapped,
                                vm_ready, env_ready, activity_ready,
                                true /* constructors_run */);
                            log_line(log,
                                     "experimental_jni_onload_decision_after_constructors=%s",
                                     entry_decision.reason);
                        }
                        int32_t version = 0;
                        bool onload_called = false;
                        bool onload_reported = false;
                        if (!constructors_ok) {
                            log_line(log, "blocker=constructors_incomplete");
                            detail(result,
                                   "Engine blocked: constructors did not complete durably.");
                        } else if (!entry_decision.eligible) {
                            log_line(log, "blocker=post_constructor_entry_refused");
                            detail(result,
                                   "Engine blocked: entry refused after constructors.");
                        } else if (!engine_run_journal_begin(ENGINE_REPORT_DIR,
                                                      "jni_onload", result->log_path)) {
                            log_line(log, "blocker=crash_journal_not_durable");
                            detail(result, "Engine blocked: cannot save crash marker.");
                        } else {
                            log_line(log, "event=entering_real_engine_jni_onload");
                            if (fflush(log) != 0 || fsync(fileno(log)) != 0) {
                                engine_run_journal_finish(ENGINE_REPORT_DIR);
                                log_line(log, "blocker=link_report_not_durable");
                                detail(result, "Engine blocked: report could not sync.");
                            } else {
                                detail(result, "Entered Roblox JNI_OnLoad; inspect link report and restart.");
                                engine_entry_started = true;
                                onload_called = true;
                                version = engine_execution_call_jni_onload(entry_decision.target, vm);
                                log_line(log, "event=real_engine_jni_onload_returned");
                                log_line(log, "engine_jni_onload_return=0x%08X", (unsigned)version);
                                onload_reported = engine_run_journal_finish_report(
                                    ENGINE_REPORT_DIR, log);
                                log_line(log, "jni_onload_crash_marker_cleared=%s",
                                         onload_reported ? "yes" : "no");
                                if (!onload_reported) {
                                    detail(result, "JNI_OnLoad returned but its report was not durable.");
                                    log_line(log, "blocker=jni_onload_return_report_not_durable");
                                }
                            }
                        }
                        log_line(log, "experimental_jni_onload_called=%s",
                                 onload_called ? "yes" : "no");
                        engine_jni_onload_succeeded = onload_called &&
                            onload_reported && version == 0x00010006;
                        log_line(log, "engine_jni_onload_accepted=%s",
                                 engine_jni_onload_succeeded ? "yes" : "no");
                        if (engine_jni_onload_succeeded &&
                            entry.status == ENGINE_ENTRY_INSPECTED &&
                            entry.has_game_activity_initialize_native &&
                            engine_execution_rx_target(&plan,
                                (uint64_t)(uintptr_t)reservation.mapped,
                                entry.game_activity_initialize_native_virtual_address,
                                &live_game_activity_target)) {
                            log_line(log, "game_activity_mapped_entry_ready=yes");
                        } else {
                            log_line(log, "game_activity_mapped_entry_ready=no");
                        }
                        if (engine_jni_onload_succeeded &&
                            entry.status == ENGINE_ENTRY_INSPECTED) {
                            for (unsigned i = 0; i < ENGINE_ACTIVITY_LIFECYCLE_STAGES; ++i) {
                                if (entry.has_activity_lifecycle[i])
                                    engine_execution_rx_target(&plan,
                                        (uint64_t)(uintptr_t)reservation.mapped,
                                        entry.activity_lifecycle_virtual_address[i],
                                        &live_activity_lifecycle_targets[i]);
                                log_line(log, "activity_lifecycle_%s_mapped_rx_ready=%s",
                                         engine_activity_lifecycle_stage_name(i),
                                         live_activity_lifecycle_targets[i] ? "yes" : "no");
                            }
                            const bool found[] = {
                                entry.has_app_bridge_global_init,
                                entry.has_app_bridge_update_adapter_init,
                                entry.has_app_bridge_v2_init,
                                entry.has_app_bridge_lua_shell_start,
                                entry.has_app_bridge_start_app
                            };
                            const uint64_t addresses[] = {
                                entry.app_bridge_global_init_virtual_address,
                                entry.app_bridge_update_adapter_init_virtual_address,
                                entry.app_bridge_v2_init_virtual_address,
                                entry.app_bridge_lua_shell_start_virtual_address,
                                entry.app_bridge_start_app_virtual_address
                            };
                            for (unsigned i = 0; i < 5; ++i) {
                                if (found[i])
                                    engine_execution_rx_target(&plan,
                                        (uint64_t)(uintptr_t)reservation.mapped,
                                        addresses[i], &live_app_bridge_targets[i]);
                                log_line(log, "app_bridge_step_%u_mapped_rx_ready=%s", i + 1,
                                         live_app_bridge_targets[i] ? "yes" : "no");
                            }
                            if (entry.has_app_bridge_surface_update)
                                engine_execution_rx_target(&plan,
                                    (uint64_t)(uintptr_t)reservation.mapped,
                                    entry.app_bridge_surface_update_virtual_address,
                                    &live_app_bridge_surface_update_target);
                            log_line(log, "app_bridge_surface_update_rx_ready=%s",
                                     live_app_bridge_surface_update_target ? "yes" : "no");
                            log_line(log, "app_bridge_surface_update_called=no_no_resize_event");
                            if (entry.has_app_bridge_main_thread_messages)
                                engine_execution_rx_target(&plan,
                                    (uint64_t)(uintptr_t)reservation.mapped,
                                    entry.app_bridge_main_thread_messages_virtual_address,
                                    &live_main_thread_message_target);
                            log_line(log, "main_thread_message_mapped_rx_ready=%s",
                                     live_main_thread_message_target ? "yes" : "no");
                        }
                        log_line(log, "engine_gameplay_started=no");
                        if (bridge_installed) {
                            engine_guest_tpidr_restore(saved_tpidr);
                            log_line(log, "guest_tpidr_bridge=restored");
                        }
                    }
                }
            }
            log_line(log, "constructor_plan_executed=%s",
                     constructors_ran ? "yes" : "no");
            engine_constructor_plan_release(&constructors);
        } else {
            detail(result, "Real engine entry blocked; see complete link report.");
            // Strict binding stops at the first unknown symbol. Continue a
            // separate, still-NX scan at that *same* final base to reveal the
            // full import gap without manufacturing dummy function pointers.
            EngineStage audit = {0};
            ImportDiagnostics all_imports = {0};
            // The v0.81 hardware report revealed 298 missing imports, but
            // printed only 24. Keep the full read-only gap on the heap so it
            // can be diagnosed in one pass without inflating this stack frame.
            enum { MAX_MISSING_IMPORTS = 512, MAX_RESOLVED_IMPORTS = 1024 };
            all_imports.complete_list = calloc(MAX_MISSING_IMPORTS,
                                               sizeof(MissingImport));
            if (all_imports.complete_list)
                all_imports.complete_capacity = MAX_MISSING_IMPORTS;
            all_imports.resolved_list = calloc(MAX_RESOLVED_IMPORTS,
                                               sizeof(MissingImport));
            if (all_imports.resolved_list)
                all_imports.resolved_capacity = MAX_RESOLVED_IMPORTS;
            log_line(log, "event=full_final_base_audit_started");
            const bool all_ready = engine_stage_stream_audit_at(
                file, &audit, resolve_compatible, &all_imports,
                (uint64_t)(uintptr_t)reservation.mapped);
            log_line(log, "full_audit_status=%d", audit.status);
            log_line(log, "full_audit_relocation_scan_complete=%s",
                     audit.relocation_scan_complete ? "yes" : "no");
            printable(safe, sizeof(safe), audit.detail);
            log_line(log, "full_audit_detail=%s", safe);
            log_import_summary(log, "full_audit", &all_imports);
            log_resolved_imports(log, "full_audit", &all_imports);
            log_required_import(log, "full_audit", &audit);
            log_line(log, "full_audit_weak_undefined_relocations=%u", audit.weak_undefined_relocations);
            log_line(log, "full_audit_required_missing_relocations=%u", audit.required_missing_relocations);
            free(all_imports.complete_list);
            free(all_imports.resolved_list);
            log_line(log, "full_audit_unsupported_relocations=%u",
                     audit.unsupported_relocations_seen);
            log_line(log, "full_audit_tls_relocations=%u", audit.tls_relocations_seen);
            if (all_ready) {
                log_line(log, "execution_gate=blocked_strict_link_inconsistency");
            } else if (audit.required_missing_relocations) {
                log_line(log, "execution_gate=blocked_unresolved_android_imports");
            } else {
                log_line(log, "execution_gate=blocked_relocation_or_elf_incompatibility");
            }
            engine_stage_release(&audit);
            log_line(log, "blocker=strict_relocation_incomplete");
        }
        engine_stage_release(&stage);
    }
    if (reservation.reservation && !engine_mapping_pinned) {
        if (!horizon_code_memory_unmap(&reservation, &kernel_error)) {
            detail(result, "Failed to release reserved code address.");
            log_line(log, "blocker=reservation_release_failed");
            result->stage_prepared = false;
        } else {
            log_line(log, "event=reserved_address_released");
        }
    }
    engine_memory_plan_release(&plan);
    fclose(file);

done:;
    AndroidGuestGraphicsStats graphics = {0};
    android_egl_gles_compat_read_guest_stats(&graphics);
    log_line(log, "guest_egl_window_surface_attempts=%llu",
             (unsigned long long)graphics.create_surface_attempts);
    log_line(log, "guest_egl_window_surface_successes=%llu",
             (unsigned long long)graphics.create_surface_succeeded);
    log_line(log, "guest_egl_make_current_attempts=%llu",
             (unsigned long long)graphics.make_current_attempts);
    log_line(log, "guest_egl_make_current_successes=%llu",
             (unsigned long long)graphics.make_current_succeeded);
    log_line(log, "guest_gl_clear_calls=%llu",
             (unsigned long long)graphics.clear_calls);
    log_line(log, "guest_egl_swap_attempts=%llu",
             (unsigned long long)graphics.swap_attempts);
    log_line(log, "guest_egl_swap_successes=%llu",
             (unsigned long long)graphics.swap_succeeded);
    log_line(log, "guest_graphics_pipeline_candidate=%s",
             graphics.create_surface_succeeded &&
             graphics.make_current_succeeded &&
             graphics.clear_calls && graphics.swap_succeeded ? "yes" : "no");
    log_line(log, "guest_visible_roblox_frame_verified=no");
    log_line(log, "engine_entry_called=%s", engine_entry_started ? "yes" : "no");
    log_line(log, "game_activity_initialize_native_called=%s",
             game_activity_entry_called ? "yes" : "no");
    log_line(log, "engine_executed=%s", engine_entry_started ? "yes" : "no");
    log_line(log, "engine_mapped_executable=%s",
             reservation.active ? "yes" : "no");
    const bool all_written = !ferror(log);
    result->log_saved = all_written && fclose(log) == 0;
    if (!all_written) fclose(log);
}
