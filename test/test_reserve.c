/*
 * Concurrency test for hip_reserve_device_memory: the limit check and the
 * usage update must be one step, or threads allocating at the same time all
 * pass the check and together exceed the limit.
 *
 * Build and run:
 *   gcc -O2 -pthread -o test_reserve test_reserve.c \
 *       ../src/multiprocess/hip_multiprocess_memory_limit.c && ./test_reserve
 */

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "../src/multiprocess/hip_multiprocess_memory_limit.h"

#define THREADS 8
#define CHUNK (300ULL << 20)

static pthread_barrier_t start;
static int granted[THREADS];

static void *grab(void *arg) {
    int i = (int)(long)arg;
    pthread_barrier_wait(&start);
    granted[i] = hip_reserve_device_memory(0, CHUNK) == 0;
    return NULL;
}

int main(void) {
    char cache[] = "/tmp/test_reserve_XXXXXX";
    int fd = mkstemp(cache);
    assert(fd >= 0);
    close(fd);
    unlink(cache);
    setenv("HIP_DEVICE_MEMORY_SHARED_CACHE", cache, 1);
    setenv("HIP_DEVICE_MEMORY_LIMIT_0", "1g", 1);
    assert(hip_shrreg_init() == 0);
    assert(hip_get_device_memory_limit(0) == 1ULL << 30);

    pthread_t th[THREADS];
    pthread_barrier_init(&start, NULL, THREADS);
    for (long i = 0; i < THREADS; i++)
        pthread_create(&th[i], NULL, grab, (void *)i);
    int n = 0;
    for (int i = 0; i < THREADS; i++) {
        pthread_join(th[i], NULL);
        n += granted[i];
    }
    /* 3 x 300 MiB fit in 1 GiB, a fourth does not. */
    assert(n == 3);
    assert(hip_shrreg_lock() == 0);
    assert(hip_get_device_memory_usage(0) == 3 * CHUNK);
    hip_shrreg_unlock();

    for (int i = 0; i < n; i++)
        hip_release_device_memory(0, CHUNK);
    assert(hip_shrreg_lock() == 0);
    assert(hip_get_device_memory_usage(0) == 0);
    hip_shrreg_unlock();

    /* No limit on device 1: nothing reserved, nothing to undo. */
    assert(hip_reserve_device_memory(1, CHUNK) == 1);
    unlink(cache);
    printf("  PASS: reserve (%d of %d concurrent reservations granted)\n", n, THREADS);
    return 0;
}
