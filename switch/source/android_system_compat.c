#include "android_system_compat.h"

#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "android_log_compat.h"

#ifdef __SWITCH__
#include <switch.h>
#else
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#endif

// System-side libc entry points that have no honest Horizon implementation
// yet. Two kinds live here. A small number really work: the guest's syslog
// output is forwarded to the diagnostic log, and the abort message the
// engine's libc hands over before a fatal exit is captured for the run
// journal. Everything else is resolved but fails closed, because an
// unresolved import blocks the strict relocation audit entirely while a
// resolved ENOSYS keeps the failure at the call that actually needs the
// facility. Fail-closed providers must never dereference their arguments:
// the guest's structure layouts are not translated here, so a wrong read
// would turn a clean refusal into memory corruption.

static char abort_message[512];
static char syslog_identity[64];
static bool syslog_open;

// Bounded set of system-property names the guest has asked about, so one
// console run records exactly which properties the engine reads. Values are
// never fabricated: every query gets bionic's not-found answer until a
// documented presented-property table exists, and this log is the evidence
// for what belongs in it.
enum { PROPERTY_LOG_MAX = 64, PROPERTY_NAME_BYTES = 96 };
static char property_names_seen[PROPERTY_LOG_MAX][PROPERTY_NAME_BYTES];
static unsigned property_names_count;

static void note_property_name(const char *name)
{
    for (unsigned i = 0; i < property_names_count; i++) {
        if (!strcmp(property_names_seen[i], name)) return;
    }
    if (property_names_count >= PROPERTY_LOG_MAX) return;
    snprintf(property_names_seen[property_names_count],
             PROPERTY_NAME_BYTES, "%s", name);
    property_names_count++;
    char message[128];
    snprintf(message, sizeof(message), "system_property_read name=%s", name);
    cordial_android_log_write(4, "properties", message);
}

// Bionic's __system_property_get answers a missing property with an empty
// value and a zero return (libc/bionic/system_properties.cpp). Until the
// presented environment's properties are documented, every name is missing.
static int guest_system_property_get(const char *name, char *value)
{
    if (!name || !value) return 0;
    note_property_name(name);
    value[0] = '\0';
    return 0;
}

// Bionic's current locale is C.UTF-8 unless a program replaces it, and its
// multibyte maximum is 4 (libc/bionic/locale.cpp). That is also exactly the
// UTF-8 contract android_wchar_compat implements, so 4 is the true answer
// for the environment Morrow presents. Revisit if setlocale ever exists.
static size_t guest_ctype_get_mb_cur_max(void)
{
    return 4;
}

// The Linux kernel's struct sysinfo, which bionic exposes unchanged. Every
// field offset is asserted because the guest reads this memory raw.
typedef struct {
    int64_t uptime;
    uint64_t loads[3];
    uint64_t totalram;
    uint64_t freeram;
    uint64_t sharedram;
    uint64_t bufferram;
    uint64_t totalswap;
    uint64_t freeswap;
    uint16_t procs;
    uint16_t pad;
    uint64_t totalhigh;
    uint64_t freehigh;
    uint32_t mem_unit;
} GuestSysinfo;
_Static_assert(sizeof(GuestSysinfo) == 112, "kernel sysinfo LP64 size");
_Static_assert(offsetof(GuestSysinfo, totalram) == 32, "sysinfo totalram");
_Static_assert(offsetof(GuestSysinfo, procs) == 80, "sysinfo procs");
_Static_assert(offsetof(GuestSysinfo, mem_unit) == 104, "sysinfo mem_unit");

static int guest_sysinfo(void *out)
{
    if (!out) { errno = EFAULT; return -1; }
    GuestSysinfo info;
    memset(&info, 0, sizeof(info));
#ifdef __SWITCH__
    // Real Horizon figures: boot tick uptime and this process's Horizon
    // memory-pool budget. svcGetInfo answers for the current process's
    // pool, so totalram/freeram here mean "bytes this process may still
    // map" - the figure guest allocation pressure actually depends on -
    // and NOT the console's physical RAM, which Horizon does not report
    // to applications and which sysinfo cannot honestly reproduce.
    info.uptime = (int64_t)(armGetSystemTick() / armGetSystemTickFreq());
    uint64_t total = 0, used = 0;
    if (R_FAILED(svcGetInfo(&total, InfoType_TotalMemorySize,
                            CUR_PROCESS_HANDLE, 0)) ||
        R_FAILED(svcGetInfo(&used, InfoType_UsedMemorySize,
                            CUR_PROCESS_HANDLE, 0)) || used > total) {
        errno = EIO;
        return -1;
    }
    info.totalram = total;
    info.freeram = total - used;
    info.procs = 1; // One process exists on Horizon: this one.
    // Remaining fields, per field: totalswap/freeswap are zero because it
    // is true - the process pool has no swap backing. loads, sharedram,
    // bufferram, totalhigh and freehigh have no Horizon metric at all;
    // their zeros are placeholders by presented-environment decision, not
    // measurements, and must never be read as "no shared memory" or "the
    // system is idle".
#else
    struct sysinfo host;
    if (sysinfo(&host)) return -1;
    info.uptime = (int64_t)host.uptime;
    info.totalram = (uint64_t)host.totalram * host.mem_unit;
    info.freeram = (uint64_t)host.freeram * host.mem_unit;
    info.procs = host.procs;
#endif
    info.mem_unit = 1;
    memcpy(out, &info, sizeof(info));
    return 0;
}

enum { UTSNAME_FIELD_BYTES = 65 };

// Bionic's struct utsname is six 65-byte fields: sysname, nodename, release,
// version, machine, domainname. The answers name the real environment -
// Horizon on AArch64 - not a pretend Linux kernel. release carries the
// console's actual Horizon version where one can be read.
static int guest_uname(void *out)
{
    if (!out) { errno = EFAULT; return -1; }
    char (*field)[UTSNAME_FIELD_BYTES] = out;
    memset(out, 0, 6 * UTSNAME_FIELD_BYTES);
#ifdef __SWITCH__
    snprintf(field[0], UTSNAME_FIELD_BYTES, "Horizon");
    snprintf(field[1], UTSNAME_FIELD_BYTES, "switch");
    u32 version = hosversionGet();
    snprintf(field[2], UTSNAME_FIELD_BYTES, "%u.%u.%u",
             (unsigned)((version >> 16) & 0xff),
             (unsigned)((version >> 8) & 0xff), (unsigned)(version & 0xff));
    snprintf(field[3], UTSNAME_FIELD_BYTES, "morrow");
    snprintf(field[4], UTSNAME_FIELD_BYTES, "aarch64");
#else
    struct utsname host;
    if (uname(&host)) return -1;
    snprintf(field[0], UTSNAME_FIELD_BYTES, "%s", host.sysname);
    snprintf(field[1], UTSNAME_FIELD_BYTES, "%s", host.nodename);
    snprintf(field[2], UTSNAME_FIELD_BYTES, "%s", host.release);
    snprintf(field[3], UTSNAME_FIELD_BYTES, "%s", host.version);
    snprintf(field[4], UTSNAME_FIELD_BYTES, "%s", host.machine);
#endif
    return 0;
}

void android_set_abort_message(const char *message)
{
    if (!message) {
        abort_message[0] = '\0';
        return;
    }
    snprintf(abort_message, sizeof(abort_message), "%s", message);
    cordial_android_log_write(7, "abort", abort_message);
}

const char *android_system_compat_abort_message(void)
{
    return abort_message;
}

static int syslog_android_priority(int priority)
{
    switch (priority & 7) {
    case 0: // LOG_EMERG
    case 1: // LOG_ALERT
    case 2: // LOG_CRIT
        return 7;
    case 3: return 6; // LOG_ERR
    case 4: return 5; // LOG_WARNING
    case 5:           // LOG_NOTICE
    case 6: return 4; // LOG_INFO
    default: return 3; // LOG_DEBUG
    }
}

static void guest_openlog(const char *identity, int option, int facility)
{
    (void)option;
    (void)facility;
    if (identity)
        snprintf(syslog_identity, sizeof(syslog_identity), "%s", identity);
    syslog_open = true;
}

// Bionic routes syslog to logd. Morrow has no logd, so the message joins the
// same sink as the guest's liblog traffic; nothing is acknowledged that was
// not delivered there.
static void guest_syslog(int priority, const char *format, ...)
{
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format ? format : "(null)", args);
    va_end(args);
    cordial_android_log_write(syslog_android_priority(priority),
                              syslog_open ? syslog_identity : "syslog",
                              message);
}

static void guest_closelog(void)
{
    syslog_open = false;
}

// A fail-closed refusal must never be silent on the console: the first call
// to each unsupported facility logs once, so a run journal shows exactly
// which facility the engine reached for, in order, without flooding the
// sink on retry loops.
enum { REFUSAL_LOG_MAX = 64 };
static const char *refusals_seen[REFUSAL_LOG_MAX];
static unsigned refusals_count;

static void note_refusal(const char *facility)
{
    for (unsigned i = 0; i < refusals_count; i++) {
        if (refusals_seen[i] == facility) return; // static literals: pointer-equal
    }
    if (refusals_count >= REFUSAL_LOG_MAX) return;
    refusals_seen[refusals_count++] = facility;
    char message[160];
    snprintf(message, sizeof(message),
             "unsupported facility called: %s (refusing ENOSYS)", facility);
    cordial_android_log_write(5, "unsupported", message); // WARN
}

static int refuse_logged(const char *facility)
{
    note_refusal(facility);
    errno = ENOSYS;
    return -1;
}

// pthread entry points return the error number itself instead of -1/errno.
static int refuse_logged_pthread(const char *facility)
{
    note_refusal(facility);
    return ENOSYS;
}

static int guest_sigaction(int signal_number, const void *action, void *old)
{
    (void)signal_number;
    (void)action;
    (void)old;
    return refuse_logged(__func__);
}

static void *guest_signal(int signal_number, void *handler)
{
    (void)signal_number;
    (void)handler;
    note_refusal(__func__);
    errno = ENOSYS;
    return (void *)(intptr_t)-1; // SIG_ERR
}

static int guest_raise(int signal_number)
{
    (void)signal_number;
    return refuse_logged(__func__);
}

static int guest_pthread_sigmask(int how, const void *set, void *old)
{
    (void)how;
    (void)set;
    (void)old;
    return refuse_logged_pthread(__func__); // pthread entry points return the error number itself.
}

static int guest_sigaltstack(const void *stack, void *old)
{
    (void)stack;
    (void)old;
    return refuse_logged(__func__);
}

static int guest_sched_priority_max(int policy)
{
    (void)policy;
    return refuse_logged("sched_get_priority_max");
}

static int guest_sched_priority_min(int policy)
{
    (void)policy;
    return refuse_logged("sched_get_priority_min");
}

static int guest_sched_getparam(int32_t pid, void *param)
{
    (void)pid;
    (void)param;
    return refuse_logged(__func__);
}

static int guest_sched_getscheduler(int32_t pid)
{
    (void)pid;
    return refuse_logged(__func__);
}

static int guest_sched_setscheduler(int32_t pid, int policy, const void *param)
{
    (void)pid;
    (void)policy;
    (void)param;
    return refuse_logged(__func__);
}

static int guest_sched_getcpu(void)
{
    return refuse_logged(__func__);
}

static int guest_getpriority(int which, unsigned who)
{
    (void)which;
    (void)who;
    return refuse_logged(__func__);
}

static int guest_setpriority(int which, unsigned who, int priority)
{
    (void)which;
    (void)who;
    (void)priority;
    return refuse_logged(__func__);
}

static int guest_pthread_attr_setschedparam(void *attr, const void *param)
{
    (void)attr;
    (void)param;
    return refuse_logged_pthread(__func__);
}

static int guest_pthread_getattr_np(uint64_t thread, void *attr)
{
    (void)thread;
    (void)attr;
    return refuse_logged_pthread(__func__);
}

static int guest_pthread_getschedparam(uint64_t thread, int *policy, void *param)
{
    (void)thread;
    (void)policy;
    (void)param;
    return refuse_logged_pthread(__func__);
}

static int guest_pthread_setschedparam(uint64_t thread, int policy,
                                       const void *param)
{
    (void)thread;
    (void)policy;
    (void)param;
    return refuse_logged_pthread(__func__);
}

static int guest_pthread_setname_np(uint64_t thread, const char *name)
{
    (void)thread;
    (void)name;
    return refuse_logged_pthread(__func__);
}

static int guest_accept4(int fd, void *address, void *length, int flags)
{
    (void)fd;
    (void)address;
    (void)length;
    (void)flags;
    return refuse_logged(__func__);
}

static int guest_eventfd(unsigned initial, int flags)
{
    (void)initial;
    (void)flags;
    return refuse_logged(__func__);
}

static int guest_timerfd_create(int clock, int flags)
{
    (void)clock;
    (void)flags;
    return refuse_logged(__func__);
}

static int guest_timerfd_settime(int fd, int flags, const void *next, void *old)
{
    (void)fd;
    (void)flags;
    (void)next;
    (void)old;
    return refuse_logged(__func__);
}

// Distinct refusal names per import alias: the journal must show which
// entry point the guest actually called, not a shared helper's name.
static int guest_epoll_create(int size)
{
    (void)size;
    return refuse_logged("epoll_create");
}

static int guest_epoll_create1(int flags)
{
    (void)flags;
    return refuse_logged("epoll_create1");
}

static int guest_epoll_ctl(int epfd, int operation, int fd, void *event)
{
    (void)epfd;
    (void)operation;
    (void)fd;
    (void)event;
    return refuse_logged(__func__);
}

static int guest_epoll_wait(int epfd, void *events, int count, int timeout)
{
    (void)epfd;
    (void)events;
    (void)count;
    (void)timeout;
    return refuse_logged(__func__);
}

static int guest_pipe(int *fds)
{
    (void)fds;
    return refuse_logged(__func__);
}

static int guest_socketpair(int domain, int type, int protocol, int *fds)
{
    (void)domain;
    (void)type;
    (void)protocol;
    (void)fds;
    return refuse_logged(__func__);
}

static int guest_ioctl(int fd, unsigned long request, ...)
{
    (void)fd;
    (void)request;
    return refuse_logged(__func__);
}

static long guest_syscall(long number, ...)
{
    (void)number;
    errno = ENOSYS;
    return -1;
}

static int guest_prctl(int option, ...)
{
    (void)option;
    return refuse_logged(__func__);
}

static int guest_mlock(const void *address, size_t size)
{
    (void)address;
    (void)size;
    return refuse_logged(__func__);
}

static int guest_posix_fallocate(int fd, int64_t offset, int64_t size)
{
    (void)fd;
    (void)offset;
    (void)size;
    return refuse_logged_pthread(__func__); // posix_fallocate returns the error number itself.
}

static int guest_statvfs(const char *path, void *result)
{
    (void)path;
    (void)result;
    return refuse_logged(__func__);
}

static int guest_utime(const char *path, const void *times)
{
    (void)path;
    (void)times;
    return refuse_logged(__func__);
}

static int guest_utimes(const char *path, const void *times)
{
    (void)path;
    (void)times;
    return refuse_logged(__func__);
}

static int guest_gethostname(char *name, size_t size)
{
    (void)name;
    (void)size;
    return refuse_logged(__func__);
}

static unsigned guest_if_nametoindex(const char *name)
{
    (void)name;
    errno = ENODEV;
    return 0; // Failure is zero here; there is no error-return variant.
}

static void *guest_if_indextoname(unsigned index, void *name)
{
    (void)index;
    (void)name;
    errno = ENXIO;
    return NULL;
}

static int guest_tcgetattr(int fd, void *termios)
{
    (void)fd;
    (void)termios;
    return refuse_logged(__func__);
}

static int guest_tcsetattr(int fd, int action, const void *termios)
{
    (void)fd;
    (void)action;
    (void)termios;
    return refuse_logged(__func__);
}

bool android_system_compat_resolve(const char *name, const char *version,
                                   const char *library, uint64_t *address)
{
    if (!name || !version || !library || !address ||
        strcmp(version, "LIBC") || strcmp(library, "libc.so"))
        return false;
    *address = 0;
#define MAP_GUEST(symbol, target) \
    if (!strcmp(name, symbol)) *address = (uint64_t)(uintptr_t)&target; else
    MAP_GUEST("android_set_abort_message", android_set_abort_message)
    MAP_GUEST("__system_property_get", guest_system_property_get)
    MAP_GUEST("__ctype_get_mb_cur_max", guest_ctype_get_mb_cur_max)
    MAP_GUEST("sysinfo", guest_sysinfo)
    MAP_GUEST("uname", guest_uname)
    MAP_GUEST("openlog", guest_openlog)
    MAP_GUEST("syslog", guest_syslog)
    MAP_GUEST("closelog", guest_closelog)
    MAP_GUEST("sigaction", guest_sigaction)
    MAP_GUEST("signal", guest_signal)
    MAP_GUEST("raise", guest_raise)
    MAP_GUEST("pthread_sigmask", guest_pthread_sigmask)
    MAP_GUEST("sigaltstack", guest_sigaltstack)
    MAP_GUEST("sched_get_priority_max", guest_sched_priority_max)
    MAP_GUEST("sched_get_priority_min", guest_sched_priority_min)
    MAP_GUEST("sched_getparam", guest_sched_getparam)
    MAP_GUEST("sched_getscheduler", guest_sched_getscheduler)
    MAP_GUEST("sched_setscheduler", guest_sched_setscheduler)
    MAP_GUEST("sched_getcpu", guest_sched_getcpu)
    MAP_GUEST("getpriority", guest_getpriority)
    MAP_GUEST("setpriority", guest_setpriority)
    MAP_GUEST("pthread_attr_setschedparam", guest_pthread_attr_setschedparam)
    MAP_GUEST("pthread_getattr_np", guest_pthread_getattr_np)
    MAP_GUEST("pthread_getschedparam", guest_pthread_getschedparam)
    MAP_GUEST("pthread_setschedparam", guest_pthread_setschedparam)
    MAP_GUEST("pthread_setname_np", guest_pthread_setname_np)
    MAP_GUEST("accept4", guest_accept4)
    MAP_GUEST("eventfd", guest_eventfd)
    MAP_GUEST("timerfd_create", guest_timerfd_create)
    MAP_GUEST("timerfd_settime", guest_timerfd_settime)
    MAP_GUEST("epoll_create", guest_epoll_create)
    MAP_GUEST("epoll_create1", guest_epoll_create1)
    MAP_GUEST("epoll_ctl", guest_epoll_ctl)
    MAP_GUEST("epoll_wait", guest_epoll_wait)
    MAP_GUEST("pipe", guest_pipe)
    MAP_GUEST("socketpair", guest_socketpair)
    MAP_GUEST("ioctl", guest_ioctl)
    MAP_GUEST("syscall", guest_syscall)
    MAP_GUEST("prctl", guest_prctl)
    MAP_GUEST("mlock", guest_mlock)
    MAP_GUEST("posix_fallocate", guest_posix_fallocate)
    MAP_GUEST("statvfs", guest_statvfs)
    MAP_GUEST("utime", guest_utime)
    MAP_GUEST("utimes", guest_utimes)
    MAP_GUEST("gethostname", guest_gethostname)
    MAP_GUEST("if_nametoindex", guest_if_nametoindex)
    MAP_GUEST("if_indextoname", guest_if_indextoname)
    MAP_GUEST("tcgetattr", guest_tcgetattr)
    MAP_GUEST("tcsetattr", guest_tcsetattr)
    (void)0;
#undef MAP_GUEST
    return *address != 0;
}
