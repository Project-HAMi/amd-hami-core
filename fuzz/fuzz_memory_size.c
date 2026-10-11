/*
 * libFuzzer target for memory_size.h: parse_memory_size must never crash or
 * read past the string, whatever the environment holds.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../src/include/memory_size.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char *s = malloc(size + 1);
    if (!s) return 0;
    memcpy(s, data, size);
    s[size] = '\0';
    (void)parse_memory_size(s);
    free(s);
    return 0;
}
