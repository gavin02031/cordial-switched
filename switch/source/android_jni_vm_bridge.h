#ifndef CORDIAL_SWITCH_ANDROID_JNI_VM_BRIDGE_H
#define CORDIAL_SWITCH_ANDROID_JNI_VM_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

// The JNI C ABI passes JavaVM* as a pointer to an invocation-function table.
// Keep this independent of an Android NDK header so the same ABI can be tested
// on Linux and compiled for Horizon. This is NOT a Java object model or ART.
typedef struct AndroidJniVm AndroidJniVm;

typedef struct {
    // Each callback must use the *calling thread's* actual JNIEnv. A JNIEnv
    // from another thread must never be returned, even if the version matches.
    int32_t (*get_env)(void *context, void **environment, int32_t version);
    int32_t (*attach)(void *context, void **environment, void *arguments,
                      bool daemon);
    int32_t (*detach)(void *context);
    // Optional: not all embedded JVMs can be destroyed from their host.
    int32_t (*destroy)(void *context);
} AndroidJniVmProvider;

enum {
    ANDROID_JNI_OK = 0,
    ANDROID_JNI_ERR = -1,
    ANDROID_JNI_EDETACHED = -2,
    ANDROID_JNI_EVERSION = -3,
    ANDROID_JNI_VERSION_1_6 = 0x00010006,
};

// The invocation table is exported only as a JavaVM ABI argument. There is no
// corresponding JNIEnv yet and it must not be passed into engine JNI_OnLoad.
AndroidJniVm *android_jni_vm_bridge_instance(void);

// One-time registration by a future, verified JNI implementation. All three
// mandatory callbacks must exist; context must remain valid for the process.
// There is deliberately no way to unregister or swap the live provider.
bool android_jni_vm_bridge_install_provider(const AndroidJniVmProvider *provider,
                                             void *context);
bool android_jni_vm_bridge_has_provider(void);

#endif
