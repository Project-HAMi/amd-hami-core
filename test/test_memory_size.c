/*
 * Unit tests for memory_size.h.
 *
 * Build and run:
 *   gcc -o test_memory_size test_memory_size.c && ./test_memory_size
 */

#include <assert.h>
#include <stdio.h>
#include "../src/include/memory_size.h"

int main(void) {
    /* What amd-device-plugin emits, and the documented forms. */
    assert(parse_memory_size("4096m") == 4096ULL << 20);
    assert(parse_memory_size("16G") == 16ULL << 30);
    assert(parse_memory_size("512k") == 512ULL << 10);
    assert(parse_memory_size("1T") == 1ULL << 40);
    assert(parse_memory_size("1073741824") == 1073741824ULL);
    assert(parse_memory_size("2g\n") == 2ULL << 30);
    /* Malformed values mean no limit, never a wrong one. */
    assert(parse_memory_size("1.5G") == 0);
    assert(parse_memory_size("-1") == 0);
    assert(parse_memory_size("4096 M") == 0);
    assert(parse_memory_size("4096MB") == 0);
    assert(parse_memory_size("abc") == 0);
    assert(parse_memory_size("") == 0);
    assert(parse_memory_size(NULL) == 0);
    /* Overflow. */
    assert(parse_memory_size("99999999999G") == 0);
    assert(parse_memory_size("99999999999999999999") == 0);
    printf("  PASS: memory_size\n");
    return 0;
}
