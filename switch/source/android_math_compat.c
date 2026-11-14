#include "android_math_compat.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

bool android_math_compat_resolve(const char *name, const char *version,
                                 const char *library, uint64_t *address)
{
    if (!name || !version || !library || !address ||
        strcmp(version, "LIBC") != 0 || strcmp(library, "libm.so") != 0)
        return false;
    *address = 0;
    // Bionic's libm symbol map exports these under LIBC. Only math routines
    // with matching plain AArch64 float/double signatures are eligible.
#define MAP_MATH(symbol) \
    if (strcmp(name, #symbol) == 0) *address = (uint64_t)(uintptr_t)&symbol; else
    MAP_MATH(acos)
    MAP_MATH(acosf)
    MAP_MATH(asinf)
    MAP_MATH(tanf)
    MAP_MATH(atan2f)
    MAP_MATH(nextafterf)
    MAP_MATH(nan)
    MAP_MATH(ldexpf)
    MAP_MATH(frexp)
    MAP_MATH(modf)
    MAP_MATH(atan)
    MAP_MATH(asin)
    MAP_MATH(atan2)
    MAP_MATH(cosh)
    MAP_MATH(sinh)
    MAP_MATH(tanh)
    MAP_MATH(tan)
    MAP_MATH(atanf)
    MAP_MATH(remquof)
    MAP_MATH(cbrt)
    MAP_MATH(cbrtf)
    MAP_MATH(scalbn)
    MAP_MATH(hypotf)
    MAP_MATH(modff)
    MAP_MATH(coshf)
    MAP_MATH(sinhf)
    MAP_MATH(tanhf)
    MAP_MATH(erfcf)
    MAP_MATH(exp)
    MAP_MATH(pow)
    MAP_MATH(fmod)
    MAP_MATH(log)
    MAP_MATH(log2)
    MAP_MATH(log10)
    MAP_MATH(sin)
    MAP_MATH(cos)
    MAP_MATH(frexpf)
    MAP_MATH(remainderf)
    MAP_MATH(erff)
    MAP_MATH(ilogb)
    MAP_MATH(hypot)
    MAP_MATH(expm1)
    MAP_MATH(sinf)
    MAP_MATH(exp2)
    MAP_MATH(exp2f)
    MAP_MATH(log10f)
    MAP_MATH(logf)
    MAP_MATH(powf)
    MAP_MATH(fmodf)
    MAP_MATH(log2f)
    MAP_MATH(expf)
    MAP_MATH(cosf)
    *address = 0;
#undef MAP_MATH
    return *address != 0;
}
