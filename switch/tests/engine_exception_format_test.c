#include "engine_exception_format.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    char record[256];
    assert(engine_exception_format(record, sizeof(record), 0x301,
                                   0x12345678, 98, 0xdeadbeef, 0xaabbccdd));
    assert(strstr(record, "kind=0x00000301"));
    assert(strstr(record, "pc=0x0000000012345678"));
    assert(strstr(record, "x8=0x0000000000000062"));
    assert(strstr(record, "esr=0xdeadbeef"));
    assert(strstr(record, "far=0x00000000aabbccdd"));
    char small[16];
    assert(!engine_exception_format(small, sizeof(small), 0x301, 1, 2, 3, 4));
    assert(!engine_exception_format(NULL, 0, 0, 0, 0, 0, 0));
    puts("engine_exception_format_test: PC, x8, ESR, FAR encoding passed");
    return 0;
}
