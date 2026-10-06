/*
 * Copyright 2024 HAMi Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Parse a memory size such as "4096m", "16G" or "1073741824". Kept free of
 * I/O so it can be unit tested (test/test_memory_size.c).
 */

#ifndef MEMORY_SIZE_H
#define MEMORY_SIZE_H

#include <stdint.h>

/*
 * Returns the size in bytes, or 0 (no limit) for anything that is not a
 * whole number with an optional K, M, G or T suffix (either case), or that
 * overflows 64 bits. A malformed value must never become a tiny or huge
 * limit: "1.5G" used to become 1 byte and "-1" effectively unlimited.
 */
static inline uint64_t parse_memory_size(const char *str) {
    uint64_t val = 0, mult = 1;
    const char *p = str;

    if (p == NULL || *p < '0' || *p > '9')
        return 0;
    for (; *p >= '0' && *p <= '9'; p++) {
        if (val > (UINT64_MAX - (uint64_t)(*p - '0')) / 10)
            return 0;
        val = val * 10 + (uint64_t)(*p - '0');
    }
    switch (*p) {
    case 'T': case 't': mult = 1ULL << 40; p++; break;
    case 'G': case 'g': mult = 1ULL << 30; p++; break;
    case 'M': case 'm': mult = 1ULL << 20; p++; break;
    case 'K': case 'k': mult = 1ULL << 10; p++; break;
    default: break;
    }
    if (*p == '\n')
        p++;
    if (*p != '\0' || val > UINT64_MAX / mult)
        return 0;
    return val * mult;
}

#endif /* MEMORY_SIZE_H */
