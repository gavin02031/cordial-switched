#pragma once

namespace jnivm {
class VM;
}

// Compiled diagnostic, not yet observed on console: logs the first
// resolution of each JNI class or method the pinned VM has no model for,
// so a hardware run can reveal the guest's real JNI usage instead of us
// guessing which classes to model. See
// switch/docs/morrow-v099-import-census.md (JNI surface section).
void morrow_jni_unknown_observer_install(jnivm::VM *vm, const char *report_dir);
