/*
 * HIP_OVERSUBSCRIBE: the device plugin sets it on pods placed on a GPU whose
 * registered memory was scaled above the physical VRAM. hipMalloc is then
 * served from managed memory, which the driver can migrate to host RAM when
 * VRAM runs out. Kept free of I/O so it can be unit tested
 * (test/test_oversubscribe.c).
 */
#ifndef OVERSUBSCRIBE_H
#define OVERSUBSCRIBE_H

#include <string.h>
#include <strings.h>

static inline int oversubscribe_on(const char *value) {
    return value && (strcasecmp(value, "true") == 0 || strcmp(value, "1") == 0);
}

#endif /* OVERSUBSCRIBE_H */
