/*
 * Unit tests for alloc_size.h.
 *
 * Build and run:
 *   gcc -o test_alloc_size test_alloc_size.c && ./test_alloc_size
 */

#include <assert.h>
#include <stdio.h>
#include "../src/hip/alloc_size.h"

int main(void) {
    /* float4: four 32-bit channels = 16 bytes per element. */
    assert(array_bytes(32, 32, 32, 32, 8192, 8192, 0) == (size_t)16 * 8192 * 8192);
    /* A zero height or depth counts as one (1D / 2D arrays). */
    assert(array_bytes(32, 0, 0, 0, 1000, 0, 0) == 4000);
    assert(array_bytes(8, 8, 8, 8, 10, 10, 3) == (size_t)4 * 10 * 10 * 3);
    /* Odd channel layouts round up to whole bytes. */
    assert(array_bytes(5, 6, 5, 0, 4, 4, 0) == (size_t)2 * 4 * 4);
    /* Nothing to account for an empty element or width, or a bad channel. */
    assert(array_bytes(0, 0, 0, 0, 10, 10, 1) == 0);
    assert(array_bytes(32, 0, 0, 0, 0, 10, 1) == 0);
    assert(array_bytes(-1, 0, 0, 0, 10, 10, 1) == 0);
    /* Overflow is reported as 0, never wrapped. */
    assert(array_bytes(32, 32, 32, 32, SIZE_MAX / 2, 4, 1) == 0);

    assert(extent_bytes(1 << 20, 1024, 2) == (size_t)2 << 30);
    assert(extent_bytes(SIZE_MAX, 2, 1) == 0);
    assert(extent_bytes(0, 5, 5) == 0);

    assert(clamp_total((size_t)16 << 30, (uint64_t)1 << 30) == (size_t)1 << 30);
    assert(clamp_total((size_t)16 << 30, 0) == (size_t)16 << 30);                  /* no limit set */
    assert(clamp_total((size_t)1 << 30, (uint64_t)16 << 30) == (size_t)1 << 30);   /* limit above the GPU */
    printf("  PASS: alloc_size\n");
    return 0;
}
