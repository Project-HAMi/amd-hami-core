/*
 * Shared region recovery tests: a slot left by an earlier process with our
 * pid, and a lock whose owner died. The second takes one lock timeout
 * (SEM_LOCK_TIMEOUT_SEC).
 *
 * Build and run:
 *   gcc -O2 -pthread -o test_shrreg test_shrreg.c \
 *       ../src/multiprocess/hip_multiprocess_memory_limit.c && ./test_shrreg
 */

#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../src/multiprocess/hip_multiprocess_memory_limit.h"

static void *lock_unlock(void *arg) {
    (void)arg;
    assert(hip_shrreg_lock() == 0);
    hip_shrreg_unlock();
    return NULL;
}

int main(void) {
    char cache[] = "/tmp/test_shrreg_XXXXXX";
    int fd = mkstemp(cache);
    assert(fd >= 0);
    assert(ftruncate(fd, sizeof(hip_shared_region_t)) == 0);
    hip_shared_region_t *r = mmap(NULL, sizeof(*r), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(r != MAP_FAILED);
    close(fd);

    /* A region persisted from an earlier process that had our pid and
     * held 512 MiB: none of that usage is ours. */
    memset(r, 0, sizeof(*r));
    assert(sem_init(&r->sem, 1, 1) == 0);
    r->limit[0] = 1ULL << 30;
    r->procs[0].pid = getpid();
    r->procs[0].used[0].data_size = r->procs[0].used[0].total = 512ULL << 20;
    r->proc_num = 1;
    r->initialized_flag = HIP_SHRREG_MAGIC;

    setenv("HIP_DEVICE_MEMORY_SHARED_CACHE", cache, 1);
    assert(hip_shrreg_init() == 0);
    assert(hip_shrreg_lock() == 0);
    assert(hip_get_device_memory_usage(0) == 0);
    hip_shrreg_unlock();

    /* The lock holder died without unlocking. Two waiters that both see it
     * dead must recover the semaphore once, not twice. */
    assert(sem_trywait(&r->sem) == 0);
    r->owner_pid = 999999999;
    pthread_t a, b;
    pthread_create(&a, NULL, lock_unlock, NULL);
    pthread_create(&b, NULL, lock_unlock, NULL);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    int val = -1;
    assert(sem_getvalue(&r->sem, &val) == 0);
    assert(val == 1);

    munmap(r, sizeof(*r));
    unlink(cache);
    printf("  PASS: shrreg (stale slot reset, dead owner recovered once)\n");
    return 0;
}
