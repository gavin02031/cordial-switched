// The pinned libjnivm VM is used here, rather than imitating Java objects.
#include <string> // Pinned vm.h requires std::hash<string> before unordered_map.
#include <jnivm/vm.h>
#include <jnivm/env.h>
#include <jnivm/class.h>
#include <jnivm/method.h>
#include <jnivm/jnitypes.h>

#include "morrow_jni_unknown_observer.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <unistd.h>

// Bounds, by design: each distinct unknown class / (class, method, sig) key
// is logged once, capped at kMaxEntries; further unknowns are only counted.
// The log names classes and JNI signatures only - never call arguments, so
// no user data reaches the file. Per-lookup cost is a short linear scan.
// Races against concurrent jnivm registration can lose or duplicate a line;
// that is acceptable for a census diagnostic and matches libjnivm's own
// unsynchronized release-build behavior.

namespace {

constexpr size_t kMaxEntries = 192;
constexpr size_t kKeyMax = 192; // class + method + sig, truncated beyond

std::mutex observer_mutex;
jnivm::VM *observer_vm;
char observer_path[320];
char observer_keys[kMaxEntries][kKeyMax];
size_t observer_key_count;
unsigned long observer_overflow;

jclass (JNICALL *orig_find_class)(JNIEnv *, const char *);
jmethodID (JNICALL *orig_get_method_id)(JNIEnv *, jclass, const char *, const char *);
jmethodID (JNICALL *orig_get_static_method_id)(JNIEnv *, jclass, const char *, const char *);

// Mirror note_link_opened() in horizon_real_link.c: best-effort, fsync'd,
// silently skipped on failure. Epoch seconds suffice to order entries
// within a run; this translation unit stays free of libnx so it can be
// syntax-checked on the host.
void note_unknown(const char *key)
{
    FILE *marker = std::fopen(observer_path, "a");
    if (!marker)
        return;
    std::fprintf(marker, "%ld jni_unknown %s\n", (long)std::time(nullptr), key);
    std::fflush(marker);
    fsync(fileno(marker));
    std::fclose(marker);
}

void record(const char *kind, const char *detail)
{
    char key[kKeyMax];
    std::snprintf(key, sizeof key, "%s %s", kind, detail);
    std::lock_guard<std::mutex> lock(observer_mutex);
    for (size_t i = 0; i < observer_key_count; ++i) {
        if (std::strcmp(observer_keys[i], key) == 0)
            return;
    }
    if (observer_key_count >= kMaxEntries) {
        ++observer_overflow;
        return;
    }
    std::snprintf(observer_keys[observer_key_count], kKeyMax, "%s", key);
    ++observer_key_count;
    note_unknown(key);
}

const char *class_prefix(JNIEnv *env, jclass cl)
{
    if (!env || !cl)
        return "?";
    auto c = jnivm::JNITypes<std::shared_ptr<jnivm::Class>>::JNICast(
        jnivm::ENV::FromJNIEnv(env), cl);
    return c ? c->nativeprefix.data() : "?";
}

jclass JNICALL observed_find_class(JNIEnv *env, const char *name)
{
    // A class absent from the VM table before the call is one libjnivm is
    // about to auto-create empty: that is an unmodeled class.
    const bool unknown = name && observer_vm &&
        observer_vm->classes.find(name) == observer_vm->classes.end();
    jclass result = orig_find_class(env, name);
    if (unknown)
        record("class", name);
    return result;
}

jmethodID JNICALL observed_get_method_id(JNIEnv *env, jclass cl, const char *name, const char *sig)
{
    jmethodID id = orig_get_method_id(env, cl, name, sig);
    if (id) {
        auto *method = reinterpret_cast<jnivm::Method *>(id);
        if (!method->native && !method->nativehandle) {
            char detail[kKeyMax];
            std::snprintf(detail, sizeof detail, "%s.%s%s",
                          class_prefix(env, cl), name ? name : "?", sig ? sig : "?");
            record("method", detail);
        }
    }
    return id;
}

jmethodID JNICALL observed_get_static_method_id(JNIEnv *env, jclass cl, const char *name, const char *sig)
{
    jmethodID id = orig_get_static_method_id(env, cl, name, sig);
    if (id) {
        auto *method = reinterpret_cast<jnivm::Method *>(id);
        if (!method->native && !method->nativehandle) {
            char detail[kKeyMax];
            std::snprintf(detail, sizeof detail, "%s.%s%s",
                          class_prefix(env, cl), name ? name : "?", sig ? sig : "?");
            record("smethod", detail);
        }
    }
    return id;
}

void wrap_interface(JNINativeInterface &iface)
{
    if (!orig_find_class) {
        orig_find_class = iface.FindClass;
        orig_get_method_id = iface.GetMethodID;
        orig_get_static_method_id = iface.GetStaticMethodID;
    }
    iface.FindClass = observed_find_class;
    iface.GetMethodID = observed_get_method_id;
    iface.GetStaticMethodID = observed_get_static_method_id;
}

} // namespace

void morrow_jni_unknown_observer_install(jnivm::VM *vm, const char *report_dir)
{
    if (!vm || !report_dir || observer_vm)
        return;
    observer_vm = vm;
    std::snprintf(observer_path, sizeof observer_path, "%s/jni-unknown-surface.txt", report_dir);
    // Must run before VM::initialize(): CreateEnv applies the hooks, and the
    // no-argument VM constructor creates the first env itself.
    vm->AddHook(wrap_interface);
}
