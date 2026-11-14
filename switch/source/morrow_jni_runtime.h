#ifndef MORROW_JNI_RUNTIME_H
#define MORROW_JNI_RUNTIME_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opt-in Horizon libjnivm runtime. This creates JNI environments, not Android
// classes, an Activity or any executable Roblox engine image.
bool morrow_jni_runtime_start(void);

// Return the actual libjnivm JavaVM, not the separate diagnostic invocation
// bridge. JNIEnv::GetJavaVM must return the same JavaVM passed to JNI_OnLoad.
void *morrow_jni_runtime_java_vm(void);

#ifdef __cplusplus
}
#endif

#endif
