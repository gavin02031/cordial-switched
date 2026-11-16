#include "android_wchar_compat.h"

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
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

int main(void)
{
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

    puts("android_wchar_compat_test: UTF-8 guest state and bounds passed");
    return 0;
}
