/*
 * Copyright 2024 HAMi Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Multi-process memory limiting for AMD GPUs via shared memory IPC.
 *
 * This module manages a shared memory region (mmap'd file) that tracks
 * GPU memory usage across all processes sharing the same GPU device.
 * Each process registers itself in a slot and updates its memory usage
 * atomically. When a process attempts to allocate GPU memory that would
 * exceed the configured limit, the allocation is rejected with OOM.
 *
 * The design follows HAMi's multiprocess_memory_limit.c for NVIDIA GPUs,
 * adapted for AMD HIP runtime.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "../include/glibc_compat.h"
#include "hip_multiprocess_memory_limit.h"
#include "../include/hip_log_utils.h"
#include "../include/libamvgpu.h"
#include "../include/memory_size.h"
#include "../include/proc_file.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Global shared region pointer */
static hip_shared_region_t *g_shrreg = NULL;

/* Current process slot index (-1 = not registered) */
static int g_proc_slot = -1;

/* Initialization guard: 0=not started, 2=in progress, 1=ready. A plain
 * atomic flag, not pthread_once: la_objopen flags every object
 * BINDTO|BINDFROM, so this library's own first pthread_* call can re-enter
 * la_symbind64 during lazy PLT resolution while the dynamic linker holds
 * its load lock, same reentrancy hazard libamvgpu_audit.c's tracker_lock()
 * avoids by using a spinlock instead of pthread_mutex. */
static volatile int g_shrreg_init_state = 0;

/* Flag to track if cleanup was already called */
static volatile int g_cleanup_done = 0;

/*
 * Read an environment variable, with /proc/self/environ fallback.
 *
 * During early LD_AUDIT loading (before glibc's __libc_start_main),
 * the C library's 'environ' pointer may not be initialized, causing
 * getenv() to return NULL even though the variable was passed via
 * execve(). This happens in vLLM/SGLang engine processes where a
 * library constructor triggers HIP calls before main().
 *
 * Fallback: parse /proc/self/environ (null-separated KEY=VALUE pairs),
 * which reflects the actual process environment from execve().
 */
/* Search a /proc environ buffer for a variable.
 * Returns pointer to value (within buf) or NULL.
 */
static const char *search_proc_environ(char *buf, ssize_t n,
                                       const char *name) {
    size_t name_len = strlen(name);
    char *p = buf;
    char *end = buf + n;
    while (p < end) {
        if (strncmp(p, name, name_len) == 0 && p[name_len] == '=')
            return p + name_len + 1;
        p += strlen(p) + 1;
    }
    return NULL;
}

static const char *safe_getenv(const char *name) {
    /* Fast path: standard getenv */
    const char *val = getenv(name);
    if (val != NULL)
        return val;

    /* Fallback: read /proc/self/environ, then /proc/1/environ.
     *
     * In containers, vLLM/SGLang engine processes may be spawned with
     * a clean environment (no HAMi env vars), even though the container's
     * PID 1 has them. This happens because:
     * - vLLM uses multiprocessing "fork" context
     * - The forked child re-execs a Python interpreter
     * - The exec may use a limited environment
     *
     * /proc/1/environ always reflects the container runtime's env vars
     * (set by containerd from the pod spec), so it's a reliable fallback.
     */
    static char *proc_env_buf = NULL;
    static size_t proc_env_cap = 0;
    static const char *proc_paths[] = {"/proc/self/environ", "/proc/1/environ", NULL};

    for (const char **pp = proc_paths; *pp; pp++) {
        ssize_t n = read_proc_file(*pp, &proc_env_buf, &proc_env_cap);
        if (n <= 0)
            continue;

        val = search_proc_environ(proc_env_buf, n, name);
        if (val != NULL) {
            LOG_INFO("safe_getenv: found %s via %s", name, *pp);
            return val;
        }
    }
    return NULL;
}

/*
 * Read per-device memory limits from environment variables.
 * Format: HIP_DEVICE_MEMORY_LIMIT_{idx}=<size>[G|M|K]
 * Falls back to HIP_DEVICE_MEMORY_LIMIT for global limit.
 */
static void read_memory_limits(hip_shared_region_t *region) {
    char env_name[64];
    const char *env_val;
    uint64_t global_limit = 0;

    /* Global limit as fallback */
    env_val = safe_getenv(HIP_DEVICE_MEMORY_LIMIT_ENV);
    LOG_INFO("read_memory_limits: HIP_DEVICE_MEMORY_LIMIT=%s",
             env_val ? env_val : "(null)");
    if (env_val != NULL) {
        global_limit = parse_memory_size(env_val);
        if (global_limit == 0)
            LOG_WARN("Ignoring invalid %s=%s", HIP_DEVICE_MEMORY_LIMIT_ENV, env_val);
    }

    for (int i = 0; i < HIP_MAX_DEVICES; i++) {
        snprintf(env_name, sizeof(env_name),
                 HIP_DEVICE_MEMORY_LIMIT_ENV_FMT, i);
        env_val = safe_getenv(env_name);
        if (env_val != NULL) {
            region->limit[i] = parse_memory_size(env_val);
            if (region->limit[i] == 0)
                LOG_WARN("Ignoring invalid %s=%s", env_name, env_val);
            LOG_INFO("Device %d memory limit: %lu bytes (%s)",
                     i, region->limit[i], env_val);
        } else {
            region->limit[i] = global_limit;
        }
    }
}

/*
 * Check if a process is still alive by reading /proc/<pid>/stat.
 * Returns 1 if alive, 0 if dead.
 */
static int is_process_alive(pid_t pid) {
    char path[32];
    struct stat st;

    if (pid <= 0)
        return 0;

    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    return (stat(path, &st) == 0) ? 1 : 0;
}

/*
 * Find or allocate a process slot for the current process.
 * Must be called with the lock held.
 * Returns slot index, or -1 if no slots available.
 */
static int find_or_create_proc_slot(hip_shared_region_t *region) {
    pid_t mypid = getpid();
    int free_slot = -1;

    for (int i = 0; i < HIP_MAX_PROCS; i++) {
        if (region->procs[i].pid == mypid) {
            /* Called once per process, so a slot with our pid belongs to an
             * earlier process that had the same pid (a restarted container
             * reusing a persisted region); its usage is not ours. */
            memset(region->procs[i].used, 0, sizeof(region->procs[i].used));
            return i;
        }
        if (free_slot < 0 && region->procs[i].pid == 0) {
            free_slot = i;
        }
    }

    if (free_slot < 0) {
        /* Try to reclaim dead process slots */
        for (int i = 0; i < HIP_MAX_PROCS; i++) {
            if (region->procs[i].pid != 0 &&
                !is_process_alive(region->procs[i].pid)) {
                LOG_INFO("Reclaiming dead process slot %d (pid %d)",
                         i, region->procs[i].pid);
                memset(&region->procs[i], 0, sizeof(hip_shrreg_proc_slot_t));
                region->proc_num--;
                if (free_slot < 0) {
                    free_slot = i;
                }
            }
        }
    }

    if (free_slot < 0) {
        LOG_ERROR("No available process slots (max %d)", HIP_MAX_PROCS);
        return -1;
    }

    memset(&region->procs[free_slot], 0, sizeof(hip_shrreg_proc_slot_t));
    region->procs[free_slot].pid = mypid;
    region->procs[free_slot].hostpid = mypid; /* pid in this pid namespace */
    region->procs[free_slot].status = 1;  /* Running */
    region->proc_num++;

    LOG_INFO("Registered process %d in slot %d (total: %d)",
             mypid, free_slot, region->proc_num);

    return free_slot;
}

/*
 * Clear the current process's slot in the shared region.
 * Must be called with the lock held.
 */
static void clear_proc_slot_locked(hip_shared_region_t *region, int slot) {
    if (slot < 0 || slot >= HIP_MAX_PROCS)
        return;

    pid_t pid = region->procs[slot].pid;
    memset(&region->procs[slot], 0, sizeof(hip_shrreg_proc_slot_t));
    if (region->proc_num > 0)
        region->proc_num--;

    LOG_INFO("Cleared process slot %d (pid %d, remaining: %d)",
             slot, pid, region->proc_num);
}

/* Flag: set to 1 if init completed but found no limits
 * (env vars not yet available during early LD_AUDIT init).
 * We retry on next call to hip_shrreg_init() to pick up the limits. */
static int g_needs_limit_retry = 0;

/*
 * Initialize shared memory region from cache file.
 */
static void do_shrreg_init(void) {
    const char *cache_path;
    int fd;
    int created = 0;
    struct stat st;

    LOG_INFO("do_shrreg_init ENTRY (pid %d, g_shrreg=%p)", getpid(), (void *)g_shrreg);

    cache_path = safe_getenv(HIP_DEVICE_MEMORY_SHARED_CACHE);
    if (cache_path == NULL)
        cache_path = DEFAULT_SHARED_CACHE_PATH;

    LOG_INFO("Shared cache: %s", cache_path);

    fd = open(cache_path, O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        LOG_ERROR("Failed to open shared cache '%s': %s",
                  cache_path, strerror(errno));
        return;
    }

    /* Hold exclusive lock during initialization to prevent races between
     * multiple processes creating/resizing the file simultaneously. */
    if (flock(fd, LOCK_EX) < 0) {
        LOG_ERROR("Failed to lock shared cache: %s", strerror(errno));
        close(fd);
        return;
    }

    /* Ensure file is large enough */
    if (fstat(fd, &st) == 0 && st.st_size < (off_t)sizeof(hip_shared_region_t)) {
        if (ftruncate(fd, sizeof(hip_shared_region_t)) < 0) {
            LOG_ERROR("Failed to resize shared cache: %s", strerror(errno));
            flock(fd, LOCK_UN);
            close(fd);
            return;
        }
        created = 1;
    }

    g_shrreg = (hip_shared_region_t *)mmap(
        NULL, sizeof(hip_shared_region_t),
        PROT_READ | PROT_WRITE, MAP_SHARED,
        fd, 0);

    if (g_shrreg == MAP_FAILED) {
        LOG_ERROR("Failed to mmap shared cache: %s", strerror(errno));
        g_shrreg = NULL;
        flock(fd, LOCK_UN);
        close(fd);
        return;
    }

    /* Initialize on first creation */
    LOG_DEBUG("do_shrreg_init: created=%d, magic_match=%d",
              created, (g_shrreg->initialized_flag == HIP_SHRREG_MAGIC));
    if (created || g_shrreg->initialized_flag != HIP_SHRREG_MAGIC) {
        memset(g_shrreg, 0, sizeof(hip_shared_region_t));

        if (sem_init(&g_shrreg->sem, 1 /* pshared */, 1 /* initial */) < 0) {
            LOG_ERROR("Failed to init semaphore: %s", strerror(errno));
            munmap(g_shrreg, sizeof(hip_shared_region_t));
            g_shrreg = NULL;
            flock(fd, LOCK_UN);
            close(fd);
            return;
        }

        read_memory_limits(g_shrreg);

        __sync_synchronize();
        g_shrreg->initialized_flag = HIP_SHRREG_MAGIC;

        LOG_INFO("Created new shared region, limit[0]=%lu",
                 (unsigned long)g_shrreg->limit[0]);

        /* If all limits are zero, env vars may not be available yet
         * (common during early LD_AUDIT loading). Mark for retry. */
        if (g_shrreg->limit[0] == 0) {
            int any_limit = 0;
            for (int i = 0; i < HIP_MAX_DEVICES; i++) {
                if (g_shrreg->limit[i] > 0) { any_limit = 1; break; }
            }
            if (!any_limit) {
                g_needs_limit_retry = 1;
                LOG_INFO("No limits found - will retry when env becomes available");
            }
        }
    } else {
        LOG_INFO("Attached to existing shared region (procs: %d, limit[0]=%lu)",
                 g_shrreg->proc_num, (unsigned long)g_shrreg->limit[0]);
        /* If attached region has no limits, env vars may not have been
         * available when the region was created. Mark for retry. */
        if (g_shrreg->limit[0] == 0) {
            int any_limit = 0;
            for (int i = 0; i < HIP_MAX_DEVICES; i++) {
                if (g_shrreg->limit[i] > 0) { any_limit = 1; break; }
            }
            if (!any_limit) {
                g_needs_limit_retry = 1;
                LOG_INFO("Attached region has no limits - will retry when env becomes available");
            }
        }
    }

    flock(fd, LOCK_UN);
    close(fd);

    /* Register current process */
    if (hip_shrreg_lock() == 0) {
        g_proc_slot = find_or_create_proc_slot(g_shrreg);
        hip_shrreg_unlock();
    }

    /* Register cleanup on process exit */
    atexit(hip_shrreg_cleanup);

    LOG_INFO("do_shrreg_init DONE: slot=%d, limit[0]=%lu",
             g_proc_slot, (unsigned long)g_shrreg->limit[0]);
}

/*
 * Reinitialize after fork in child process.
 */
static void child_reinit(void) {
    LOG_INFO("child_reinit: pid %d, g_shrreg=%p, slot=%d",
             getpid(), (void *)g_shrreg, g_proc_slot);
    g_shrreg_init_state = 0;
    g_proc_slot = -1;
    g_cleanup_done = 0;

    if (g_shrreg != NULL && hip_shrreg_lock() == 0) {
        g_proc_slot = find_or_create_proc_slot(g_shrreg);
        hip_shrreg_unlock();
        LOG_INFO("child_reinit: re-registered in slot %d, limit[0]=%lu",
                 g_proc_slot, (unsigned long)g_shrreg->limit[0]);
    }
}

/* Register fork handler during library load */
__attribute__((constructor))
static void register_fork_handler(void) {
    if (pthread_atfork(NULL, NULL, child_reinit) != 0) {
        LOG_ERROR("Failed to register fork handler: memory limits may not work in child processes");
    }
}

int hip_shrreg_init(void) {
    /* compiler builtins, not libc calls, so nothing here goes through the
     * PLT; release/acquire publishes g_shrreg to the waiting threads */
    int expected = 0;
    if (__atomic_compare_exchange_n(&g_shrreg_init_state, &expected, 2, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        do_shrreg_init();
        __atomic_store_n(&g_shrreg_init_state, 1, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(&g_shrreg_init_state, __ATOMIC_ACQUIRE) == 2)
            ;
    }

    /* If shared region was created but limits were all zero (env not available
     * during early LD_AUDIT loading), retry reading limits now that the
     * environment should be available. */
    if (g_shrreg != NULL && g_needs_limit_retry) {
        const char *env_val = safe_getenv("HIP_DEVICE_MEMORY_LIMIT_0");
        if (env_val != NULL) {
            LOG_INFO("hip_shrreg_init: retrying read_memory_limits (env now available: %s)", env_val);
            if (hip_shrreg_lock() == 0) {
                read_memory_limits(g_shrreg);
                hip_shrreg_unlock();
            }
            g_needs_limit_retry = 0;
        }
    }

    return (g_shrreg != NULL) ? 0 : -1;
}

int hip_shrreg_lock(void) {
    struct timespec ts;
    int retries = 0;

    if (g_shrreg == NULL)
        return -1;

    while (retries < SEM_LOCK_MAX_RETRIES) {
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += SEM_LOCK_TIMEOUT_SEC;

        if (sem_timedwait(&g_shrreg->sem, &ts) == 0) {
            __sync_synchronize();
            g_shrreg->owner_pid = (size_t)getpid();
            return 0;
        }

        if (errno != ETIMEDOUT) {
            LOG_ERROR("sem_timedwait failed: %s", strerror(errno));
            return -1;
        }

        /* Deadlock detection: check if lock owner is still alive */
        size_t owner = g_shrreg->owner_pid;
        /* Only the waiter that clears owner_pid posts: two waiters that both
         * saw the dead owner would otherwise raise the semaphore to 2 and
         * let two holders in for the rest of the region's life. */
        if (owner > 0 && !is_process_alive((pid_t)owner) &&
            __sync_bool_compare_and_swap(&g_shrreg->owner_pid, owner, 0)) {
            LOG_WARN("Lock owner pid %zu is dead, recovering semaphore",
                     owner);
            sem_post(&g_shrreg->sem);
            /* Retry immediately */
            continue;
        }

        retries++;
        LOG_WARN("Lock timeout (attempt %d/%d, owner: %zu)",
                 retries, SEM_LOCK_MAX_RETRIES, owner);
    }

    LOG_ERROR("Failed to acquire lock after %d retries", SEM_LOCK_MAX_RETRIES);
    return -1;
}

void hip_shrreg_unlock(void) {
    if (g_shrreg == NULL)
        return;

    g_shrreg->owner_pid = 0;
    __sync_synchronize();
    sem_post(&g_shrreg->sem);
}

uint64_t hip_get_device_memory_limit(int dev) {
    if (g_shrreg == NULL || dev < 0 || dev >= HIP_MAX_DEVICES)
        return 0;
    return g_shrreg->limit[dev];
}

uint64_t hip_get_device_memory_usage(int dev) {
    uint64_t total = 0;

    if (g_shrreg == NULL || dev < 0 || dev >= HIP_MAX_DEVICES)
        return 0;

    for (int i = 0; i < HIP_MAX_PROCS; i++) {
        if (g_shrreg->procs[i].pid > 0 &&
            is_process_alive(g_shrreg->procs[i].pid)) {
            total += g_shrreg->procs[i].used[dev].total;
        }
    }

    return total;
}

int hip_oom_check(int dev, uint64_t addon) {
    uint64_t limit, usage;

    limit = hip_get_device_memory_limit(dev);
    if (limit == 0)
        return 0;  /* No limit configured */

    usage = hip_get_device_memory_usage(dev) + addon;

    if (usage > limit) {
        LOG_ERROR("Device %d OOM: usage %lu + alloc %lu = %lu > limit %lu",
                  dev, usage - addon, addon, usage, limit);

        /* Optionally kill dead processes to reclaim memory */
        const char *oom_killer = getenv(ACTIVE_OOM_KILLER_ENV);
        if (oom_killer != NULL && strcmp(oom_killer, "true") == 0) {
            for (int i = 0; i < HIP_MAX_PROCS; i++) {
                if (g_shrreg->procs[i].pid > 0 &&
                    !is_process_alive(g_shrreg->procs[i].pid)) {
                    LOG_INFO("OOM: reclaiming dead slot %d (pid %d)",
                             i, g_shrreg->procs[i].pid);
                    clear_proc_slot_locked(g_shrreg, i);
                }
            }
            /* Re-check after cleanup */
            usage = hip_get_device_memory_usage(dev) + addon;
            if (usage <= limit) {
                LOG_INFO("OOM resolved after reclaiming dead processes");
                return 0;
            }
        }

        return -1;
    }

    return 0;
}

int hip_reserve_device_memory(int dev, uint64_t size) {
    if (hip_get_device_memory_limit(dev) == 0)
        return 1;
    if (hip_shrreg_lock() != 0)
        return 1;  /* fail open: a stuck region must not stop every allocation */
    int ret = hip_oom_check(dev, size);
    if (ret == 0 && hip_add_device_memory_usage(dev, size, MEM_TYPE_DATA) != 0)
        ret = 1;  /* not registered in a slot: nothing to account against */
    hip_shrreg_unlock();
    return ret;
}

void hip_release_device_memory(int dev, uint64_t size) {
    if (hip_shrreg_lock() != 0)
        return;
    hip_rm_device_memory_usage(dev, size, MEM_TYPE_DATA);
    hip_shrreg_unlock();
}

int hip_add_device_memory_usage(int dev, uint64_t size, int type) {
    if (g_shrreg == NULL || g_proc_slot < 0)
        return -1;
    if (dev < 0 || dev >= HIP_MAX_DEVICES)
        return -1;

    hip_shrreg_proc_slot_t *slot = &g_shrreg->procs[g_proc_slot];
    hip_device_memory_t *mem = &slot->used[dev];

    switch (type) {
    case MEM_TYPE_CONTEXT:
        mem->context_size += size;
        break;
    case MEM_TYPE_MODULE:
        mem->module_size += size;
        break;
    case MEM_TYPE_DATA:
        mem->data_size += size;
        break;
    default:
        LOG_WARN("Unknown memory type %d", type);
        mem->data_size += size;
        break;
    }

    mem->total = mem->context_size + mem->module_size + mem->data_size;
    __sync_synchronize();

    LOG_DEBUG("Device %d: added %lu bytes (type %d), total: %lu",
              dev, size, type, mem->total);

    return 0;
}

void hip_rm_device_memory_usage(int dev, uint64_t size, int type) {
    if (g_shrreg == NULL || g_proc_slot < 0)
        return;
    if (dev < 0 || dev >= HIP_MAX_DEVICES)
        return;

    hip_shrreg_proc_slot_t *slot = &g_shrreg->procs[g_proc_slot];
    hip_device_memory_t *mem = &slot->used[dev];

    switch (type) {
    case MEM_TYPE_CONTEXT:
        mem->context_size = (mem->context_size >= size) ?
                            mem->context_size - size : 0;
        break;
    case MEM_TYPE_MODULE:
        mem->module_size = (mem->module_size >= size) ?
                           mem->module_size - size : 0;
        break;
    case MEM_TYPE_DATA:
        mem->data_size = (mem->data_size >= size) ?
                         mem->data_size - size : 0;
        break;
    default:
        mem->data_size = (mem->data_size >= size) ?
                         mem->data_size - size : 0;
        break;
    }

    mem->total = mem->context_size + mem->module_size + mem->data_size;
    __sync_synchronize();

    LOG_DEBUG("Device %d: removed %lu bytes (type %d), total: %lu",
              dev, size, type, mem->total);
}

void hip_shrreg_cleanup(void) {
    struct timespec ts;

    if (g_cleanup_done || g_shrreg == NULL || g_proc_slot < 0)
        return;

    g_cleanup_done = 1;

    /* Try to acquire lock with a short timeout for cleanup */
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 3;

    if (sem_timedwait(&g_shrreg->sem, &ts) == 0) {
        clear_proc_slot_locked(g_shrreg, g_proc_slot);
        g_shrreg->owner_pid = 0;
        __sync_synchronize();
        sem_post(&g_shrreg->sem);
    } else {
        LOG_WARN("Cleanup: could not acquire lock, slot may leak");
    }

    g_proc_slot = -1;
}
