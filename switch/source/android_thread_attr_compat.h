#ifndef CORDIAL_ANDROID_THREAD_ATTR_COMPAT_H
#define CORDIAL_ANDROID_THREAD_ATTR_COMPAT_H

#include <stdbool.h>
#include <pthread.h>
#include <stdint.h>

// Android's LP64 pthread_attr_t is 56 bytes and is not interchangeable with
// a host pthread_attr_t. Only proven Bionic attribute operations are exposed.
bool android_thread_attr_compat_resolve(const char *name, const char *version,
                                        const char *library, uint64_t *address);

// A caller owns and destroys the native attr on success. No Android object is
// ever passed directly to the host pthread implementation.
int android_thread_attr_compat_to_native(const void *address, pthread_attr_t *native);

#endif
