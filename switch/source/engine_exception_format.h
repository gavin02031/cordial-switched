#ifndef MORROW_ENGINE_EXCEPTION_FORMAT_H
#define MORROW_ENGINE_EXCEPTION_FORMAT_H

#include <stddef.h>
#include <stdint.h>

// Pure formatter for a best-effort Switch exception breadcrumb.
// Returns false on truncation or invalid arguments.
int engine_exception_format(char *buffer, size_t capacity, uint32_t description,
                            uint64_t pc, uint64_t x8, uint32_t esr,
                            uint64_t far);

#endif
