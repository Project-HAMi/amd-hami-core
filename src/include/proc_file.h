/*
 * Copyright 2024 HAMi Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Read a whole /proc file such as /proc/1/environ.
 */

#ifndef PROC_FILE_H
#define PROC_FILE_H

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * Reads all of path into *buf, growing it (and *cap) as needed, and
 * NUL-terminates it. Returns the length, or -1. A fixed-size read cut a
 * large environment mid-value, turning "LIMIT_0=4096m" into "409".
 */
static inline ssize_t read_proc_file(const char *path, char **buf, size_t *cap) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    size_t n = 0;

    if (fd < 0)
        return -1;
    for (;;) {
        if (n + 1 >= *cap) {
            size_t ncap = *cap ? *cap * 2 : 16384;
            char *nb = (char *)realloc(*buf, ncap);
            if (nb == NULL) {
                close(fd);
                return -1;
            }
            *buf = nb;
            *cap = ncap;
        }
        ssize_t r = read(fd, *buf + n, *cap - n - 1);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0) {
            close(fd);
            return -1;
        }
        if (r == 0)
            break;
        n += (size_t)r;
    }
    close(fd);
    (*buf)[n] = '\0';
    return (ssize_t)n;
}

#endif /* PROC_FILE_H */
