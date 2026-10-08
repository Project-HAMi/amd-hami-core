/*
 * Sizes the HIP allocators that take a shape instead of a byte count, and the
 * clamp for the total memory a device reports. Kept free of I/O so it can be
 * unit tested (test/test_alloc_size.c).
 */
#ifndef ALLOC_SIZE_H
#define ALLOC_SIZE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Bytes of a width x height x depth array whose elements have the channel
 * widths x, y, z, w (bits). A zero height or depth counts as 1, as for
 * hipMallocArray. Returns 0 when the size overflows or the element is empty,
 * which callers treat as "cannot be accounted".
 */
static inline size_t array_bytes(int x, int y, int z, int w, size_t width, size_t height, size_t depth) {
    if (x < 0 || y < 0 || z < 0 || w < 0)
        return 0;
    size_t bits = (size_t)x + (size_t)y + (size_t)z + (size_t)w;
    size_t elem = (bits + 7) / 8;
    size_t n = elem;
    if (elem == 0 || width == 0)
        return 0;
    if (height == 0) height = 1;
    if (depth == 0) depth = 1;
    if (__builtin_mul_overflow(n, width, &n) || __builtin_mul_overflow(n, height, &n) ||
        __builtin_mul_overflow(n, depth, &n))
        return 0;
    return n;
}

/*
 * Bytes of a pitched 3D allocation of the given extent: width bytes per row,
 * height rows per slice, depth slices. 0 on overflow.
 */
static inline size_t extent_bytes(size_t width, size_t height, size_t depth) {
    size_t n = width;
    if (__builtin_mul_overflow(n, height, &n) || __builtin_mul_overflow(n, depth, &n))
        return 0;
    return n;
}

/*
 * Bytes per channel of a driver-API array format (hipArray_Format). 0 for an
 * unknown format, which callers treat as "cannot be accounted".
 */
static inline size_t array_format_bytes(int format) {
    switch (format) {
    case 0x01: case 0x08: return 1;  /* UNSIGNED_INT8, SIGNED_INT8 */
    case 0x02: case 0x09: case 0x10: return 2;  /* UNSIGNED_INT16, SIGNED_INT16, HALF */
    case 0x03: case 0x0a: case 0x20: return 4;  /* UNSIGNED_INT32, SIGNED_INT32, FLOAT */
    default: return 0;
    }
}

/* Bytes of a driver-API array: width x height x depth elements of channels x format. 0 when it cannot be sized. */
static inline size_t driver_array_bytes(size_t width, size_t height, size_t depth, int format, unsigned int channels) {
    size_t n = array_format_bytes(format);
    if (n == 0 || channels == 0 || width == 0)
        return 0;
    if (height == 0) height = 1;
    if (depth == 0) depth = 1;
    if (__builtin_mul_overflow(n, (size_t)channels, &n) || __builtin_mul_overflow(n, width, &n) ||
        __builtin_mul_overflow(n, height, &n) || __builtin_mul_overflow(n, depth, &n))
        return 0;
    return n;
}

/* The total memory to report for a device: its limit when one is set and smaller than the real total. */
static inline size_t clamp_total(size_t real_total, uint64_t limit) {
    return (limit > 0 && limit < real_total) ? (size_t)limit : real_total;
}

#endif /* ALLOC_SIZE_H */
