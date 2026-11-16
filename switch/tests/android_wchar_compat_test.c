#define _POSIX_C_SOURCE 200809L
#include "android_wchar_compat.h"

#include <assert.h>
#include <locale.h>
#include <limits.h>
#include <wctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef struct { unsigned char state[8]; uint64_t guard; } ProtectedState;

static uint64_t lookup(const char *name)
{
    uint64_t address = 0;
    assert(android_wchar_compat_resolve(name, "LIBC", "libc.so", &address));
    assert(address);
    return address;
}

static void numeric_locale_test(locale_t locale)
{
    long (*signed_long)(const char *, char **, int, locale_t) =
        (void *)(uintptr_t)lookup("strtol_l");
    long long (*signed_wide)(const char *, char **, int, locale_t) =
        (void *)(uintptr_t)lookup("strtoll_l");
    unsigned long (*unsigned_long)(const char *, char **, int, locale_t) =
        (void *)(uintptr_t)lookup("strtoul_l");
    unsigned long long (*unsigned_wide)(const char *, char **, int, locale_t) =
        (void *)(uintptr_t)lookup("strtoull_l");
    float (*float_value)(const char *, char **, locale_t) =
        (void *)(uintptr_t)lookup("strtof_l");
    double (*double_value)(const char *, char **, locale_t) =
        (void *)(uintptr_t)lookup("strtod_l");
    long double (*long_double_value)(const char *, char **, locale_t) =
        (void *)(uintptr_t)lookup("strtold_l");

    char *end = NULL;
    assert(signed_long("0x2a!", &end, 0, locale) == 42 && *end == '!');
    assert(signed_wide("-9223372036854775808!", &end, 10, locale) == LLONG_MIN && *end == '!');
    assert(unsigned_long("0377!", &end, 0, locale) == 255 && *end == '!');
    assert(unsigned_wide("18446744073709551615!", &end, 10, locale) == ULLONG_MAX && *end == '!');
    errno = 0;
    assert(signed_wide("9223372036854775808!", &end, 10, locale) == LLONG_MAX);
    assert(errno == ERANGE && *end == '!');
    errno = 0;
    assert(unsigned_wide("18446744073709551616!", &end, 10, locale) == ULLONG_MAX);
    assert(errno == ERANGE && *end == '!');
    assert(float_value("1.25!", &end, locale) == 1.25f && *end == '!');
    assert(double_value("2.5!", &end, locale) == 2.5 && *end == '!');
    assert(long_double_value("3.75!", &end, locale) == 3.75L && *end == '!');
    // These APIs are specified by Bionic to use the non-_l parser, so the
    // opaque locale argument cannot be reinterpreted as a host locale.
    assert(signed_long("17", NULL, 10, (locale_t)0) == 17);
    uint64_t ignored = 0;
    assert(!android_wchar_compat_resolve("strtoll_l", "LIBC_N", "libc.so", &ignored));
}

static void locale_test(void)
{
    locale_t (*create)(int, const char *, locale_t) = (void *)(uintptr_t)lookup("newlocale");
    void (*release)(locale_t) = (void *)(uintptr_t)lookup("freelocale");
    locale_t (*activate)(locale_t) = (void *)(uintptr_t)lookup("uselocale");
    locale_t locale = create(0x1fbf, "C", (locale_t)0);
    assert(locale);
    numeric_locale_test(locale);
    locale_t previous = activate(locale);
    assert(previous && activate((locale_t)0) == locale);
    assert(activate(previous) == locale);
    const char *classifiers[] = {"iswlower_l", "iswspace_l", "iswprint_l", "iswblank_l",
        "iswcntrl_l", "iswupper_l", "iswalpha_l", "iswdigit_l", "iswpunct_l", "iswxdigit_l"};
    const wint_t yes[] = {L'a', L' ', L'A', 9, 10, L'A', L'z', L'5', L'!', L'F'};
    for (size_t i = 0; i < sizeof(yes)/sizeof(yes[0]); ++i) {
        int (*classify)(wint_t, locale_t) = (void *)(uintptr_t)lookup(classifiers[i]);
        assert(classify(yes[i], locale) != 0);
        assert(classify(WEOF, locale) == 0);
        uint64_t ignored;
        assert(!android_wchar_compat_resolve(classifiers[i], "LIBC_N", "libc.so", &ignored));
    }
    wint_t (*upper)(wint_t, locale_t) = (void *)(uintptr_t)lookup("towupper_l");
    wint_t (*lower)(wint_t, locale_t) = (void *)(uintptr_t)lookup("towlower_l");
    assert(upper(L'a', locale) == L'A' && lower(L'Z', locale) == L'z');
    int (*compare)(const char *, const char *, locale_t) = (void *)(uintptr_t)lookup("strcoll_l");
    int (*wide_compare)(const wchar_t *, const wchar_t *, locale_t) = (void *)(uintptr_t)lookup("wcscoll_l");
    assert(compare("a", "b", locale) < 0 && wide_compare(L"z", L"a", locale) > 0);
    size_t (*transform)(char *, const char *, size_t, locale_t) = (void *)(uintptr_t)lookup("strxfrm_l");
    size_t (*wide_transform)(wchar_t *, const wchar_t *, size_t, locale_t) = (void *)(uintptr_t)lookup("wcsxfrm_l");
    char text[8]; wchar_t wide[8];
    assert(transform(text, "abc", sizeof(text), locale) == 3 && !strcmp(text, "abc"));
    assert(wide_transform(wide, L"abc", 8, locale) == 3 && !wcscmp(wide, L"abc"));
    release(locale);
    assert(!create(0x40000000, "C", (locale_t)0) && errno == EINVAL);
    assert(!create(1, NULL, (locale_t)0) && errno == EINVAL);
    assert(!create(1, "morrow-no-such-locale", (locale_t)0));
    for (int mask = 1; mask <= 32; mask <<= 1) {
        locale = create(mask, "C", (locale_t)0);
        assert(locale); release(locale);
    }
}

int main(void)
{
    locale_test();
    uint64_t ignored = 0;
    assert(!android_wchar_compat_resolve("mbrtowc", "LIBC_O", "libc.so", &ignored));
    assert(!android_wchar_compat_resolve("mbrtowc", "LIBC", "libm.so", &ignored));
    assert(!android_wchar_compat_resolve("mbrtowc", NULL, "libc.so", &ignored));
    assert(!android_wchar_compat_resolve("unknown", "LIBC", "libc.so", &ignored));

    size_t (*decode)(wchar_t *, const char *, size_t, void *) =
        (size_t (*)(wchar_t *, const char *, size_t, void *))(uintptr_t)lookup("mbrtowc");
    size_t (*length)(const char *, size_t, void *) =
        (size_t (*)(const char *, size_t, void *))(uintptr_t)lookup("mbrlen");
    int (*single)(wchar_t *, const char *, size_t) =
        (int (*)(wchar_t *, const char *, size_t))(uintptr_t)lookup("mbtowc");
    size_t (*encode)(char *, wchar_t, void *) =
        (size_t (*)(char *, wchar_t, void *))(uintptr_t)lookup("wcrtomb");
    size_t (*decode_stream)(wchar_t *, const char **, size_t, void *) =
        (size_t (*)(wchar_t *, const char **, size_t, void *))(uintptr_t)lookup("mbsrtowcs");
    size_t (*decode_bounded)(wchar_t *, const char **, size_t, size_t, void *) =
        (size_t (*)(wchar_t *, const char **, size_t, size_t, void *))(uintptr_t)lookup("mbsnrtowcs");
    size_t (*encode_bounded)(char *, const wchar_t **, size_t, size_t, void *) =
        (size_t (*)(char *, const wchar_t **, size_t, size_t, void *))(uintptr_t)lookup("wcsnrtombs");

    ProtectedState state = {{0}, UINT64_C(0x1122334455667788)};
    const char emoji[] = "\xf0\x9f\x98\x80";
    wchar_t wide = 0;
    assert(decode(&wide, emoji, 2, state.state) == (size_t)-2);
    assert(state.state[0] == 0xf0 && state.state[1] == 0x9f);
    assert(decode(&wide, emoji + 2, 2, state.state) == 2);
    assert(wide == (wchar_t)0x1f600);
    assert(!state.state[0] && state.guard == UINT64_C(0x1122334455667788));
    assert(decode(&wide, emoji, 1, state.state) == (size_t)-2);
    errno = 0;
    assert(decode(&wide, "x", 1, state.state) == (size_t)-1 && errno == EILSEQ);
    assert(!state.state[0]);
    assert(decode(&wide, "A", 1, state.state) == 1 && wide == L'A');
    assert(decode(&wide, "", 1, state.state) == 0 && wide == 0);
    assert(length("\xc3\xa9", 2, state.state) == 2);
    assert(single(&wide, "\xc3\xa9", 2) == 2 && wide == 0xe9);
    assert(single(&wide, "", 1) == 0 && wide == 0);
    errno = 0;
    assert(single(&wide, "\xf0", 1) == -1 && errno == EILSEQ);
    errno = 0;
    assert(decode(&wide, "\xc0\xaf", 2, state.state) == (size_t)-1 && errno == EILSEQ);
    errno = 0;
    assert(decode(&wide, "\xed\xa0\x80", 3, state.state) == (size_t)-1 && errno == EILSEQ);
    errno = 0;
    assert(decode(&wide, "\xf4\x90\x80\x80", 4, state.state) == (size_t)-1 && errno == EILSEQ);
    assert(state.guard == UINT64_C(0x1122334455667788));

    char encoded[8] = {0};
    assert(encode(encoded, 0x1f600, state.state) == 4);
    assert(memcmp(encoded, emoji, 4) == 0);
    assert(encode(encoded, 0, state.state) == 1 && encoded[0] == 0);
    errno = 0;
    assert(encode(encoded, 0xd800, state.state) == (size_t)-1 && errno == EILSEQ);

    const char text[] = "A\xf0\x9f\x98\x80!";
    const char *cursor = text;
    wchar_t converted[4] = {0};
    memset(state.state, 0, sizeof(state.state));
    assert(decode_stream(converted, &cursor, 4, state.state) == 3);
    assert(cursor == NULL && converted[0] == L'A' &&
           converted[1] == 0x1f600 && converted[2] == L'!' && converted[3] == 0);
    cursor = text;
    assert(decode_stream(NULL, &cursor, 0, state.state) == 3 && cursor == text);
    cursor = text;
    assert(decode_bounded(converted, &cursor, 1, 4, state.state) == 1);
    assert(cursor == text + 1 && converted[0] == L'A');
    assert(decode_bounded(converted, &cursor, 4, 4, state.state) == 1);
    assert(cursor == text + 5 && converted[0] == 0x1f600);

    const wchar_t wide_text[] = {L'A', 0x1f600, L'!', 0};
    const wchar_t *wide_cursor = wide_text;
    char output[16] = {0};
    assert(encode_bounded(output, &wide_cursor, 4, 2, state.state) == 1);
    assert(wide_cursor == wide_text + 1 && output[0] == 'A');
    assert(encode_bounded(output, &wide_cursor, 3, sizeof(output), state.state) == 5);
    assert(wide_cursor == NULL && memcmp(output, emoji, 4) == 0 && output[4] == '!');
    wide_cursor = wide_text;
    assert(encode_bounded(NULL, &wide_cursor, 4, 0, state.state) == 6 &&
           wide_cursor == wide_text);
    assert(state.guard == UINT64_C(0x1122334455667788));

    puts("android_wchar_compat_test: UTF-8 guest state, numeric locale and bounds passed");
    return 0;
}
