#define _POSIX_C_SOURCE 200809L
#include "android_wchar_compat.h"

#include <errno.h>
#include <float.h>
#include <locale.h>
#include <stdlib.h>
#include <wctype.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

typedef struct {
    unsigned char seq[4];
    unsigned char reserved[4];
} BionicMbstate;

_Static_assert(sizeof(BionicMbstate) == 8, "Bionic arm64 mbstate_t is eight bytes");
_Static_assert(sizeof(wchar_t) == 4, "Android arm64 wchar_t is UTF-32");
#ifdef __aarch64__
_Static_assert(sizeof(long double) == 16 && LDBL_MANT_DIG == 113,
               "Android arm64 strtold_l returns IEEE binary128");
#endif

// Bionic's UTF-8 decoder buffers at most three prefix bytes across calls.
// A separate state type avoids writing beyond the actual Android mbstate_t.
static BionicMbstate private_decode_state;
static BionicMbstate private_encode_state;

static size_t illegal(BionicMbstate *state)
{
    memset(state->seq, 0, sizeof(state->seq));
    errno = EILSEQ;
    return (size_t)-1;
}

static unsigned utf8_width(unsigned char lead)
{
    if (lead < 0x80) return 1;
    if (lead >= 0xc2 && lead <= 0xdf) return 2;
    if (lead >= 0xe0 && lead <= 0xef) return 3;
    if (lead >= 0xf0 && lead <= 0xf4) return 4;
    return 0;
}

static size_t bionic_mbrtowc(wchar_t *out, const char *source, size_t count,
                             BionicMbstate *state)
{
    if (!state) state = &private_decode_state;
    if (!source) { source = ""; count = 1; out = NULL; }
    if (!count) return (size_t)-2;

    unsigned saved = 0;
    if (state->seq[3]) return illegal(state);
    for (unsigned i = 0; i < 3 && state->seq[i]; i++) saved++;
    if ((saved == 0 && (state->seq[1] || state->seq[2])) ||
        (saved == 1 && state->seq[2]))
        return illegal(state);
    unsigned char bytes[4] = {0};
    for (unsigned i = 0; i < saved; i++) bytes[i] = state->seq[i];
    unsigned used = 0;
    if (!saved) {
        bytes[0] = (unsigned char)source[used++];
        saved = 1;
    }
    const unsigned width = utf8_width(bytes[0]);
    if (!width || saved > width) return illegal(state);
    while (saved < width && used < count) {
        unsigned char byte = (unsigned char)source[used++];
        if ((byte & 0xc0) != 0x80) return illegal(state);
        bytes[saved++] = byte;
    }
    if (saved < width) {
        memcpy(state->seq, bytes, saved);
        memset(state->seq + saved, 0, sizeof(state->seq) - saved);
        return (size_t)-2;
    }
    uint32_t codepoint = width == 1 ? bytes[0] :
                         width == 2 ? bytes[0] & 0x1f :
                         width == 3 ? bytes[0] & 0x0f : bytes[0] & 7;
    for (unsigned i = 1; i < width; i++)
        codepoint = (codepoint << 6) | (bytes[i] & 0x3f);
    if ((width == 2 && codepoint < 0x80) ||
        (width == 3 && codepoint < 0x800) ||
        (width == 4 && codepoint < 0x10000) ||
        codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff))
        return illegal(state);
    memset(state->seq, 0, sizeof(state->seq));
    if (out) *out = (wchar_t)codepoint;
    return codepoint ? used : 0;
}

static size_t bionic_mbrlen(const char *s, size_t n, BionicMbstate *state)
{
    return bionic_mbrtowc(NULL, s, n, state);
}

static int bionic_mbtowc(wchar_t *out, const char *s, size_t n)
{
    if (!s) return 0; // UTF-8 has no persistent shift state.
    BionicMbstate state = {0};
    const size_t result = bionic_mbrtowc(out, s, n, &state);
    if (result == (size_t)-2) { errno = EILSEQ; return -1; }
    return result == (size_t)-1 ? -1 : (int)result;
}

static size_t bionic_wcrtomb(char *output, wchar_t wide, BionicMbstate *state)
{
    if (!state) state = &private_encode_state;
    char temporary[4];
    if (!output) { output = temporary; wide = 0; }
    const uint32_t point = (uint32_t)wide;
    if (point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff))
        return illegal(state);
    size_t count;
    if (point < 0x80) {
        output[0] = (char)point;
        count = 1;
    } else if (point < 0x800) {
        output[0] = (char)(0xc0 | (point >> 6));
        output[1] = (char)(0x80 | (point & 0x3f));
        count = 2;
    } else if (point < 0x10000) {
        output[0] = (char)(0xe0 | (point >> 12));
        output[1] = (char)(0x80 | ((point >> 6) & 0x3f));
        output[2] = (char)(0x80 | (point & 0x3f));
        count = 3;
    } else {
        output[0] = (char)(0xf0 | (point >> 18));
        output[1] = (char)(0x80 | ((point >> 12) & 0x3f));
        output[2] = (char)(0x80 | ((point >> 6) & 0x3f));
        output[3] = (char)(0x80 | (point & 0x3f));
        count = 4;
    }
    memset(state->seq, 0, sizeof(state->seq));
    return count;
}

static size_t bionic_mbsnrtowcs(wchar_t *destination, const char **source,
                                 size_t max_input, size_t max_output,
                                 BionicMbstate *state)
{
    if (!source || !*source) { errno = EINVAL; return (size_t)-1; }
    BionicMbstate temporary_state = {0};
    if (!state) state = &temporary_state;
    // Counting mode may not change the source pointer or supplied state.
    BionicMbstate copy = *state;
    BionicMbstate *working = destination ? state : &copy;
    const char *cursor = *source;
    size_t input = 0, output = 0;
    while (input < max_input && (!destination || output < max_output)) {
        const size_t available = max_input - input > 4 ? 4 : max_input - input;
        wchar_t wide;
        const size_t consumed = bionic_mbrtowc(&wide, cursor, available, working);
        if (consumed == (size_t)-1 || consumed == (size_t)-2) {
            if (consumed == (size_t)-2) {
                memset(working->seq, 0, sizeof(working->seq));
                errno = EILSEQ;
            }
            if (destination) *source = cursor;
            return (size_t)-1;
        }
        if (consumed == 0) {
            if (destination) { destination[output] = 0; *source = NULL; }
            return output;
        }
        if (destination) destination[output] = wide;
        cursor += consumed;
        input += consumed;
        output++;
    }
    if (destination) *source = cursor;
    return output;
}

static size_t bionic_mbsrtowcs(wchar_t *destination, const char **source,
                                size_t max_output, BionicMbstate *state)
{
    return bionic_mbsnrtowcs(destination, source, SIZE_MAX, max_output, state);
}

static size_t bionic_wcsnrtombs(char *destination, const wchar_t **source,
                                size_t max_input, size_t max_output,
                                BionicMbstate *state)
{
    if (!source || !*source) { errno = EINVAL; return (size_t)-1; }
    BionicMbstate temporary_state = {0};
    if (!state) state = &temporary_state;
    BionicMbstate copy = *state;
    BionicMbstate *working = destination ? state : &copy;
    const wchar_t *cursor = *source;
    size_t input = 0, output = 0;
    while (input < max_input && (!destination || output < max_output)) {
        char bytes[4];
        const size_t encoded = bionic_wcrtomb(bytes, cursor[0], working);
        if (encoded == (size_t)-1) {
            if (destination) *source = cursor;
            return (size_t)-1;
        }
        if (destination && encoded > max_output - output)
            break;
        if (cursor[0] == 0) {
            if (destination) { destination[output] = 0; *source = NULL; }
            return output;
        }
        if (destination) memcpy(destination + output, bytes, encoded);
        output += encoded;
        cursor++;
        input++;
    }
    if (destination) *source = cursor;
    return output;
}

// Locale objects remain opaque native handles throughout this provider.
// Bionic category bits differ from newlib (including LC_ALL_MASK).
static locale_t bionic_newlocale(int categories, const char *name, locale_t base)
{
    if (!name) { errno = EINVAL; return (locale_t)0; }
    int native = 0;
    if (categories == 0x1fbf) native = LC_ALL_MASK;
    else {
        if (categories & ~0x3f) { errno = EINVAL; return (locale_t)0; }
        if (categories & 1) native |= LC_CTYPE_MASK;
        if (categories & 2) native |= LC_NUMERIC_MASK;
        if (categories & 4) native |= LC_TIME_MASK;
        if (categories & 8) native |= LC_COLLATE_MASK;
        if (categories & 16) native |= LC_MONETARY_MASK;
        if (categories & 32) native |= LC_MESSAGES_MASK;
    }
    return newlocale(native, name, base);
}

// Bionic's public stdlib.h explicitly defines the _l parsing family as
// equivalent to the corresponding non-_l routine. Do not pass the opaque
// Android locale handle into an unrelated newlib _l implementation.
static long bionic_strtol_l(const char *s, char **end, int base, locale_t locale)
{
    (void)locale;
    return strtol(s, end, base);
}

static long long bionic_strtoll_l(const char *s, char **end, int base, locale_t locale)
{
    (void)locale;
    return strtoll(s, end, base);
}

static unsigned long bionic_strtoul_l(const char *s, char **end, int base, locale_t locale)
{
    (void)locale;
    return strtoul(s, end, base);
}

static unsigned long long bionic_strtoull_l(const char *s, char **end,
                                             int base, locale_t locale)
{
    (void)locale;
    return strtoull(s, end, base);
}

static float bionic_strtof_l(const char *s, char **end, locale_t locale)
{
    (void)locale;
    return strtof(s, end);
}

static double bionic_strtod_l(const char *s, char **end, locale_t locale)
{
    (void)locale;
    return strtod(s, end);
}

static long double bionic_strtold_l(const char *s, char **end, locale_t locale)
{
    (void)locale;
    return strtold(s, end);
}

bool android_wchar_compat_resolve(const char *name, const char *version,
                                  const char *library, uint64_t *address)
{
    if (!name || !version || !library || !address ||
        strcmp(version, "LIBC") || strcmp(library, "libc.so"))
        return false;
    *address = 0;
#define MAP(name_, target_) if (!strcmp(name, name_)) *address = (uint64_t)(uintptr_t)&target_; else
    MAP("newlocale", bionic_newlocale)
    MAP("strtol_l", bionic_strtol_l)
    MAP("strtoll_l", bionic_strtoll_l)
    MAP("strtoul_l", bionic_strtoul_l)
    MAP("strtoull_l", bionic_strtoull_l)
    MAP("strtof_l", bionic_strtof_l)
    MAP("strtod_l", bionic_strtod_l)
    MAP("strtold_l", bionic_strtold_l)
    MAP("freelocale", freelocale)
    MAP("uselocale", uselocale)
    MAP("strcoll_l", strcoll_l)
    MAP("strxfrm_l", strxfrm_l)
    MAP("wcscoll_l", wcscoll_l)
    MAP("wcsxfrm_l", wcsxfrm_l)
    MAP("iswlower_l", iswlower_l)
    MAP("iswspace_l", iswspace_l)
    MAP("iswprint_l", iswprint_l)
    MAP("iswblank_l", iswblank_l)
    MAP("iswcntrl_l", iswcntrl_l)
    MAP("iswupper_l", iswupper_l)
    MAP("iswalpha_l", iswalpha_l)
    MAP("iswdigit_l", iswdigit_l)
    MAP("iswpunct_l", iswpunct_l)
    MAP("iswxdigit_l", iswxdigit_l)
    MAP("towupper_l", towupper_l)
    MAP("towlower_l", towlower_l)
    MAP("mbrtowc", bionic_mbrtowc)
    MAP("mbrlen", bionic_mbrlen)
    MAP("mbtowc", bionic_mbtowc)
    MAP("wcrtomb", bionic_wcrtomb)
    MAP("mbsnrtowcs", bionic_mbsnrtowcs)
    MAP("mbsrtowcs", bionic_mbsrtowcs)
    MAP("wcsnrtombs", bionic_wcsnrtombs)
    { return false; }
#undef MAP
    return true;
}
