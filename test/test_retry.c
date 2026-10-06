/*
 * With no limit in the environment, every hip_shrreg_init call retries
 * reading one. Allocating threads call it concurrently, so the retry must
 * not read the environment into a shared buffer outside the lock.
 *
 * Build and run:
 *   gcc -O2 -pthread -o test_retry test_retry.c \
 *       ../src/multiprocess/hip_multiprocess_memory_limit.c && ./test_retry
 */

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "../src/multiprocess/hip_multiprocess_memory_limit.h"

#define THREADS 8
#define CALLS 200

static pthread_barrier_t start;

static void *init(void *arg) {
    (void)arg;
    pthread_barrier_wait(&start);
    for (int i = 0; i < CALLS; i++)
        assert(hip_shrreg_init() == 0);
    return NULL;
}

int main(void) {
    char cache[] = "/tmp/test_retry_XXXXXX";
    int fd = mkstemp(cache);
    assert(fd >= 0);
    close(fd);
    unlink(cache);
    setenv("HIP_DEVICE_MEMORY_SHARED_CACHE", cache, 1);
    unsetenv("HIP_DEVICE_MEMORY_LIMIT");
    unsetenv("HIP_DEVICE_MEMORY_LIMIT_0");

    pthread_t th[THREADS];
    pthread_barrier_init(&start, NULL, THREADS);
    for (int i = 0; i < THREADS; i++)
        pthread_create(&th[i], NULL, init, NULL);
    for (int i = 0; i < THREADS; i++)
        pthread_join(th[i], NULL);
    assert(hip_get_device_memory_limit(0) == 0);
    unlink(cache);
    printf("  PASS: retry (%d threads x %d inits without a limit)\n", THREADS, CALLS);
    return 0;
}
