#include "android_jni_vm_bridge.h"

#include <pthread.h>
#include <stddef.h>

typedef struct {
    void *reserved0;
    void *reserved1;
    void *reserved2;
    int32_t (*DestroyJavaVM)(AndroidJniVm *vm);
    int32_t (*AttachCurrentThread)(AndroidJniVm *vm, void **environment,
                                    void *arguments);
    int32_t (*DetachCurrentThread)(AndroidJniVm *vm);
    int32_t (*GetEnv)(AndroidJniVm *vm, void **environment, int32_t version);
    int32_t (*AttachCurrentThreadAsDaemon)(AndroidJniVm *vm,
                                            void **environment, void *arguments);
} JniInvokeTable;

struct AndroidJniVm {
    const JniInvokeTable *functions;
};

static int32_t destroy_vm(AndroidJniVm *vm);
static int32_t attach_vm(AndroidJniVm *vm, void **environment, void *arguments);
static int32_t detach_vm(AndroidJniVm *vm);
static int32_t get_vm_env(AndroidJniVm *vm, void **environment, int32_t version);
static int32_t attach_daemon(AndroidJniVm *vm, void **environment, void *arguments);

static const JniInvokeTable invocation = {
    NULL, NULL, NULL, destroy_vm, attach_vm, detach_vm, get_vm_env, attach_daemon
};
static AndroidJniVm java_vm = { &invocation };
static pthread_mutex_t provider_lock = PTHREAD_MUTEX_INITIALIZER;
static AndroidJniVmProvider installed_provider;
static void *provider_context;
static bool provider_installed;

_Static_assert(offsetof(JniInvokeTable, DestroyJavaVM) == 3 * sizeof(void *),
               "JNI invocation function-table slot mismatch");
_Static_assert(offsetof(JniInvokeTable, GetEnv) == 6 * sizeof(void *),
               "JNI invocation GetEnv slot mismatch");
_Static_assert(offsetof(JniInvokeTable, AttachCurrentThreadAsDaemon) == 7 * sizeof(void *),
               "JNI invocation daemon slot mismatch");
_Static_assert(sizeof(AndroidJniVm) == sizeof(void *),
               "JavaVM must consist of a single invocation-table pointer");

AndroidJniVm *android_jni_vm_bridge_instance(void)
{
    return &java_vm;
}

bool android_jni_vm_bridge_install_provider(const AndroidJniVmProvider *provider,
                                             void *context)
{
    if (!provider || !provider->get_env || !provider->attach || !provider->detach)
        return false;
    pthread_mutex_lock(&provider_lock);
    const bool can_install = !provider_installed;
    if (can_install) {
        installed_provider = *provider;
        provider_context = context;
        provider_installed = true;
    }
    pthread_mutex_unlock(&provider_lock);
    return can_install;
}

bool android_jni_vm_bridge_has_provider(void)
{
    pthread_mutex_lock(&provider_lock);
    const bool installed = provider_installed;
    pthread_mutex_unlock(&provider_lock);
    return installed;
}

static bool get_provider(AndroidJniVmProvider *provider, void **context)
{
    pthread_mutex_lock(&provider_lock);
    const bool installed = provider_installed;
    if (installed) {
        *provider = installed_provider;
        *context = provider_context;
    }
    pthread_mutex_unlock(&provider_lock);
    return installed;
}

static bool supported_version(int32_t version)
{
    // JNI 1.1, 1.2, 1.4, 1.6 and 1.8 are defined versions. A real provider
    // may reject one it does not support (JNI_EVERSION).
    return version == 0x00010001 || version == 0x00010002 ||
           version == 0x00010004 || version == 0x00010006 ||
           version == 0x00010008;
}

static int32_t get_vm_env(AndroidJniVm *vm, void **environment, int32_t version)
{
    if (environment) *environment = NULL;
    if (vm != &java_vm || !environment) return ANDROID_JNI_ERR;
    if (!supported_version(version)) return ANDROID_JNI_EVERSION;
    AndroidJniVmProvider provider = {0};
    void *context = NULL;
    if (!get_provider(&provider, &context)) return ANDROID_JNI_EDETACHED;
    const int32_t status = provider.get_env(context, environment, version);
    if (status != ANDROID_JNI_OK || !*environment) {
        *environment = NULL;
        return status == ANDROID_JNI_OK ? ANDROID_JNI_ERR : status;
    }
    return ANDROID_JNI_OK;
}

static int32_t attach_common(AndroidJniVm *vm, void **environment,
                             void *arguments, bool daemon)
{
    if (environment) *environment = NULL;
    if (vm != &java_vm || !environment) return ANDROID_JNI_ERR;
    AndroidJniVmProvider provider = {0};
    void *context = NULL;
    if (!get_provider(&provider, &context)) return ANDROID_JNI_ERR;
    const int32_t status = provider.attach(context, environment, arguments, daemon);
    if (status != ANDROID_JNI_OK || !*environment) {
        *environment = NULL;
        return status == ANDROID_JNI_OK ? ANDROID_JNI_ERR : status;
    }
    return ANDROID_JNI_OK;
}

static int32_t attach_vm(AndroidJniVm *vm, void **environment, void *arguments)
{
    return attach_common(vm, environment, arguments, false);
}

static int32_t attach_daemon(AndroidJniVm *vm, void **environment, void *arguments)
{
    return attach_common(vm, environment, arguments, true);
}

static int32_t detach_vm(AndroidJniVm *vm)
{
    if (vm != &java_vm) return ANDROID_JNI_ERR;
    AndroidJniVmProvider provider = {0};
    void *context = NULL;
    if (!get_provider(&provider, &context)) return ANDROID_JNI_ERR;
    return provider.detach(context);
}

static int32_t destroy_vm(AndroidJniVm *vm)
{
    if (vm != &java_vm) return ANDROID_JNI_ERR;
    AndroidJniVmProvider provider = {0};
    void *context = NULL;
    if (!get_provider(&provider, &context) || !provider.destroy)
        return ANDROID_JNI_ERR;
    return provider.destroy(context);
}
