/*
 * Which HSA queue priority a pod's AMD_TASK_PRIORITY asks for. Kept free of
 * I/O so it can be unit tested (test/test_queue_priority.c).
 */
#ifndef QUEUE_PRIORITY_H
#define QUEUE_PRIORITY_H

#include <errno.h>
#include <stdlib.h>

/* hsa_amd_queue_priority_t from hsa_ext_amd.h; this library does not link HSA. */
#define QP_LOW 0
#define QP_NORMAL 1
#define QP_HIGH 2
/* The pod asked for nothing: leave the queue at the runtime's default. */
#define QP_NONE (-1)

/*
 * 0 is the high class and anything above it the low class, the order of
 * nvidia.com/priority. NULL, empty, negative and non-numeric values ask for
 * nothing, so a bad value cannot demote or promote a pod by accident.
 */
static inline int queue_priority_level(const char *value) {
    if (!value || !*value)
        return QP_NONE;
    char *end = NULL;
    errno = 0;
    long v = strtol(value, &end, 10);
    if (errno != 0 || *end != '\0' || v < 0)
        return QP_NONE;
    return v == 0 ? QP_HIGH : QP_LOW;
}

#endif /* QUEUE_PRIORITY_H */
