// This program verifies a genuine jnivm JavaVM/JNIEnv on Horizon hardware.
// It does not load Android libraries or run any Roblox engine instructions.
#include <string> // jnivm's pinned headers require std::hash<string> first (GCC 15)
#include <jnivm.h>

#include <switch.h>

#include <cerrno>
#include <cstdio>
#include <exception>
#include <fcntl.h>
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
    puts("Morrow v0.42 — native JNI probe\n");
    char report_path[128] = {};
    FILE *report = create_report(report_path, sizeof(report_path));
    if (report) {
        fputs("# Native Morrow libjnivm hardware probe\n", report);
        fputs("roblox_engine_executed=no\nroblox_frame_rendered=no\n", report);
        fflush(report);
    }

    bool success = false;
    try {
        jnivm::VM vm;
        JavaVM *java_vm = vm.GetJavaVM();
        JNIEnv *env = vm.GetJNIEnv();
        void *attached_env = nullptr;
        const jint status = java_vm ? java_vm->GetEnv(&attached_env, JNI_VERSION_1_6)
                                    : JNI_ERR;
        const jint version = env ? env->GetVersion() : 0;
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
