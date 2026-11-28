#include "engine_exception_format.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    char record[384];
    assert(engine_exception_format(record, sizeof(record), 0x301,
                                   0x12345678, 98, 0xdeadbeef, 0xaabbccdd,
                                   "0.99.6+testbuild", 123456, 0x87654321));
    assert(strstr(record, "kind=0x00000301"));
    assert(strstr(record, "pc=0x0000000012345678"));
    assert(strstr(record, "x8=0x0000000000000062"));
    assert(strstr(record, "esr=0xdeadbeef"));
    assert(strstr(record, "far=0x00000000aabbccdd"));
    assert(strstr(record, "build=0.99.6+testbuild"));
    assert(strstr(record, "tick=123456"));
    assert(strstr(record, "handler_pc=0x0000000087654321"));
    char small[16];
    assert(!engine_exception_format(small, sizeof(small), 0x301, 1, 2, 3, 4,
                                    "test", 5, 6));
    assert(!engine_exception_format(NULL, 0, 0, 0, 0, 0, 0, NULL, 0, 0));
    puts("engine_exception_format_test: PC, x8, ESR, FAR and build identity passed");
    return 0;
}
