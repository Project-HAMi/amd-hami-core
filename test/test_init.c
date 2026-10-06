/*
 * hip_shrreg_init from many threads at once: one thread initializes, the
 * others wait for it, and the process takes exactly one slot.
 *
 * Build and run:
 *   gcc -O2 -pthread -o test_init test_init.c \
 *       ../src/multiprocess/hip_multiprocess_memory_limit.c && ./test_init
 */

#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../src/multiprocess/hip_multiprocess_memory_limit.h"

#define THREADS 16

static pthread_barrier_t start;
static int rc[THREADS];

static void *init(void *arg) {
    int i = (int)(long)arg;
    pthread_barrier_wait(&start);
    rc[i] = hip_shrreg_init();
    /* every caller returns with the limit already loaded */
    assert(hip_get_device_memory_limit(0) == 1ULL << 30);
    return NULL;
}

int main(void) {
    char cache[] = "/tmp/test_init_XXXXXX";
    int fd = mkstemp(cache);
    assert(fd >= 0);
    close(fd);
    unlink(cache);
    setenv("HIP_DEVICE_MEMORY_SHARED_CACHE", cache, 1);
    setenv("HIP_DEVICE_MEMORY_LIMIT_0", "1g", 1);

    pthread_t th[THREADS];
    pthread_barrier_init(&start, NULL, THREADS);
    for (long i = 0; i < THREADS; i++)
        pthread_create(&th[i], NULL, init, (void *)i);
    for (int i = 0; i < THREADS; i++) {
        pthread_join(th[i], NULL);
        assert(rc[i] == 0);
    }

    fd = open(cache, O_RDONLY);
    assert(fd >= 0);
    hip_shared_region_t *r = mmap(NULL, sizeof(*r), PROT_READ, MAP_SHARED, fd, 0);
    assert(r != MAP_FAILED);
    close(fd);
    int slots = 0;
    for (int i = 0; i < HIP_MAX_PROCS; i++)
        slots += r->procs[i].pid == getpid();
    assert(slots == 1);
    munmap(r, sizeof(*r));
    unlink(cache);
    printf("  PASS: init (%d concurrent callers, one slot)\n", THREADS);
    return 0;
}
