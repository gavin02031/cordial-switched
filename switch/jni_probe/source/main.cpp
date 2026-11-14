// This program verifies a genuine jnivm JavaVM/JNIEnv on Horizon hardware.
// It does not load Android libraries or run any Roblox engine instructions.
#include <string> // jnivm's pinned headers require std::hash<string> first (GCC 15)
#include <jnivm.h>

#include <switch.h>

#include <cerrno>
#include <cstdio>
#include <exception>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr const char *REPORT_DIR = "sdmc:/switch/cordial-switched";

FILE *create_report(char *path, size_t length)
{
    if (mkdir(REPORT_DIR, 0777) != 0 && errno != EEXIST)
        return nullptr;
    for (unsigned number = 1; number <= 99; ++number) {
        const int chars = snprintf(path, length, "%s/jni-%02u.txt", REPORT_DIR, number);
        if (chars <= 0 || static_cast<size_t>(chars) >= length)
            break;
        const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            if (errno == EEXIST)
                continue;
            break;
        }
        FILE *report = fdopen(fd, "w");
        if (report)
            return report;
        close(fd);
        break;
    }
    path[0] = 0;
    return nullptr;
}

void checkpoint(FILE *report, const char *event)
{
    if (!report)
        return;
    fprintf(report, "event=%s\n", event);
    fflush(report); // Preserve the last completed step if the hardware faults.
}

struct WorkerResult {
    JavaVM *vm = nullptr;
    JNIEnv *main_env = nullptr;
    jint attach = JNI_ERR;
    jint get_env = JNI_ERR;
    jint get_vm = JNI_ERR;
    jint detach = JNI_ERR;
    jint version = 0;
    bool different_env = false;
    bool same_env_on_lookup = false;
    bool same_vm_on_lookup = false;
    bool exception = false;
};

void *probe_worker(void *argument)
{
    auto *worker = static_cast<WorkerResult *>(argument);
    try {
        JNIEnv *env = nullptr;
        worker->attach = worker->vm->AttachCurrentThread(&env, nullptr);
        if (worker->attach == JNI_OK && env && env->functions) {
            worker->different_env = env != worker->main_env;
            worker->version = env->GetVersion();
            void *looked_up_env = nullptr;
            worker->get_env = worker->vm->GetEnv(&looked_up_env, JNI_VERSION_1_6);
            worker->same_env_on_lookup = looked_up_env == env;
            JavaVM *looked_up_vm = nullptr;
            worker->get_vm = env->GetJavaVM(&looked_up_vm);
            worker->same_vm_on_lookup = looked_up_vm == worker->vm;
        }
        if (worker->attach == JNI_OK)
            worker->detach = worker->vm->DetachCurrentThread();
    } catch (...) {
        // A C++ exception must not escape a libnx pthread entry point.
        worker->exception = true;
    }
    return nullptr;
}

bool probe_worker_thread(JavaVM *vm, JNIEnv *main_env, FILE *report)
{
    WorkerResult worker;
    worker.vm = vm;
    worker.main_env = main_env;
    pthread_t thread;
    checkpoint(report, "before_worker_create");
    const int created = pthread_create(&thread, nullptr, probe_worker, &worker);
    if (report) {
        fprintf(report, "worker_create_status=%d\n", created);
        fflush(report);
    }
    if (created != 0)
        return false;
    checkpoint(report, "before_worker_join");
    const int joined = pthread_join(thread, nullptr);
    if (report) {
        fprintf(report, "worker_join_status=%d\n", joined);
        fprintf(report, "worker_attach_status=%d\n", worker.attach);
        fprintf(report, "worker_get_env_status=%d\n", worker.get_env);
        fprintf(report, "worker_get_vm_status=%d\n", worker.get_vm);
        fprintf(report, "worker_detach_status=%d\n", worker.detach);
        fprintf(report, "worker_jni_version=0x%x\n", static_cast<unsigned>(worker.version));
        fprintf(report, "worker_env_is_distinct=%s\n", worker.different_env ? "yes" : "no");
        fprintf(report, "worker_env_lookup_matches=%s\n", worker.same_env_on_lookup ? "yes" : "no");
        fprintf(report, "worker_vm_lookup_matches=%s\n", worker.same_vm_on_lookup ? "yes" : "no");
        fprintf(report, "worker_exception=%s\n", worker.exception ? "yes" : "no");
        fflush(report);
    }
    return joined == 0 && !worker.exception &&
           worker.attach == JNI_OK && worker.get_env == JNI_OK &&
           worker.get_vm == JNI_OK && worker.detach == JNI_OK &&
           worker.version == JNI_VERSION_1_6 && worker.different_env &&
           worker.same_env_on_lookup && worker.same_vm_on_lookup;
}

void show_result(bool success, FILE *report, const char *path)
{
    puts(success ? "JNI probe: PASS" : "JNI probe: FAIL");
    puts("No Roblox engine code was executed.");
    if (report) {
        fprintf(report, "jni_probe_pass=%s\n", success ? "yes" : "no");
        fflush(report);
        fclose(report);
        printf("Saved: %s\n", path);
    } else {
        puts("Warning: could not save JNI report on SD.");
    }
    puts("\nPress PLUS to exit.");
    PadState pad;
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            break;
        consoleUpdate(nullptr);
    }
}

} // namespace

int main()
{
    consoleInit(nullptr);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    puts("Morrow v0.43 — native JNI probe\n");
    char report_path[128] = {};
    FILE *report = create_report(report_path, sizeof(report_path));
    if (report) {
        fputs("# Native Morrow libjnivm hardware probe\n", report);
        fputs("roblox_engine_executed=no\nroblox_frame_rendered=no\n", report);
        fflush(report);
    }

    bool success = false;
    try {
        checkpoint(report, "before_vm_construction");
        jnivm::VM vm;
        checkpoint(report, "vm_constructed");
        JavaVM *java_vm = vm.GetJavaVM();
        checkpoint(report, "java_vm_retrieved");
        JNIEnv *env = vm.GetJNIEnv();
        checkpoint(report, "jni_env_retrieved");
        void *attached_env = nullptr;
        const jint status = java_vm ? java_vm->GetEnv(&attached_env, JNI_VERSION_1_6)
                                    : JNI_ERR;
        checkpoint(report, "java_vm_get_env_returned");
        const jint version = env ? env->GetVersion() : 0;
        checkpoint(report, "jni_get_version_returned");
        success = java_vm && env && env->functions &&
                  status == JNI_OK && attached_env == env &&
                  version == JNI_VERSION_1_6;
        printf("VM: %s\nJNIEnv: %s\n", java_vm ? "created" : "missing",
               env && env->functions ? "ready" : "missing");
        printf("GetEnv: %d, version: 0x%x\n", status, static_cast<unsigned>(version));
        if (report) {
            fprintf(report, "java_vm_created=%s\n", java_vm ? "yes" : "no");
            fprintf(report, "thread_jni_env_ready=%s\n", env && env->functions ? "yes" : "no");
            fprintf(report, "get_env_status=%d\n", status);
            fprintf(report, "jni_version=0x%x\n", static_cast<unsigned>(version));
            fprintf(report, "get_env_same_thread=%s\n", attached_env == env ? "yes" : "no");
            fprintf(report, "core_jni_pass=%s\n", success ? "yes" : "no");
            fflush(report);
        }
        if (success) {
            // Keep the VM alive until the worker detaches: the real engine
            // attaches other threads during JNI_OnLoad, and the desktop MCP
            // cannot observe the Switch's pthread/JNIEnv ownership.
            const bool worker_pass = probe_worker_thread(java_vm, env, report);
            printf("Worker JNI: %s\n", worker_pass ? "PASS" : "FAIL");
            if (report) {
                fprintf(report, "worker_jni_pass=%s\n", worker_pass ? "yes" : "no");
                fflush(report);
            }
        } else {
            puts("Worker JNI: not tested (core VM failed)");
        }
    } catch (const std::exception &exception) {
        printf("JNI exception: %s\n", exception.what());
        if (report)
            fprintf(report, "blocker=jnivm_exception\n");
    } catch (...) {
        puts("JNI initialization failed with an unknown exception.");
        if (report)
            fprintf(report, "blocker=unknown_exception\n");
    }

    show_result(success, report, report_path);
    consoleExit(nullptr);
    return success ? 0 : 1;
}
