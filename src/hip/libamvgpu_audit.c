/*
 * Copyright 2024 HAMi Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * AMD GPU (HIP) memory virtualization via LD_AUDIT.
 *
 * This library uses the LD_AUDIT interface (la_symbind64) to intercept
 * HIP memory allocation calls. Unlike LD_PRELOAD, LD_AUDIT intercepts
 * at the dynamic linker level and can selectively redirect only
 * cross-library bindings, leaving HIP-internal calls untouched.
 *
 * This is required for ROCm 7.x, where LD_PRELOAD breaks HIP runtime
 * initialization due to internal dlsym/symbol resolution conflicts.
 *
 * Key design:
 *   - la_objopen: Identifies libamdhip64.so by its cookie ID
 *   - la_symbind64: Captures real function pointers from sym->st_value,
 *     redirects non-HIP callers to our wrappers, passes through HIP
 *     internal calls unchanged
 *   - Multi-process memory tracking via shared memory region (mmap'd file)
 *
 * Usage:
 *   LD_AUDIT=libamvgpu.so HIP_DEVICE_MEMORY_LIMIT_0=48G ./my_app
 *
 * Environment variables:
 *   HIP_DEVICE_MEMORY_LIMIT       - Global memory limit (with G/M/K suffix)
 *   HIP_DEVICE_MEMORY_LIMIT_{N}   - Per-device memory limit
 *   HIP_DEVICE_MEMORY_SHARED_CACHE - Shared region file path
 *   LIBHIP_LOG_LEVEL              - Log level (1=error, 2=warn, 3=info, 4=debug)
 *   ACTIVE_OOM_KILLER             - Reclaim dead process memory on OOM ("true")
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "../include/glibc_compat.h"

#include <dlfcn.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

#include "../include/hip_log_utils.h"
#include "../multiprocess/hip_multiprocess_memory_limit.h"
#include "alloc_tracker.h"
#include "env_policy.h"
#include "../include/proc_file.h"

/* HIP error codes (subset - we don't link against HIP) */
typedef int hipError_t;
#define hipSuccess 0
#define hipErrorOutOfMemory 2
#define hipErrorNotInitialized 3

typedef void *hipStream_t;

/* ====================================================================
 * Real HIP function pointers - captured from la_symbind64
 * ==================================================================== */

static hipError_t (*real_hipMalloc)(void **, size_t) = NULL;
static hipError_t (*real_hipFree)(void *) = NULL;
static hipError_t (*real_hipMemGetInfo)(size_t *, size_t *) = NULL;
static hipError_t (*real_hipSetDevice)(int) = NULL;
static hipError_t (*real_hipMallocManaged)(void **, size_t, unsigned int) = NULL;
static hipError_t (*real_hipMallocAsync)(void **, size_t, hipStream_t) = NULL;
static hipError_t (*real_hipFreeAsync)(void *, hipStream_t) = NULL;
static hipError_t (*real_hipMallocPitch)(void **, size_t *, size_t, size_t) = NULL;
static hipError_t (*real_hipExtMallocWithFlags)(void **, size_t, unsigned int) = NULL;
static char *(*real_getenv)(const char *) = NULL;

/* Current device per-thread */
static __thread int current_device = 0;

/* Shared region initialization state */
static int g_shrreg_ready = 0;

/* Pointer-to-size allocation tracker.
 *
 * We use a GCC atomic spinlock instead of pthread_mutex because
 * LD_AUDIT's la_symbind64 intercepts ALL symbol bindings.  When
 * pthread_mutex_lock is lazily resolved via PLT, the dynamic linker
 * re-enters la_symbind64 while holding its internal lock, causing
 * deadlock.  Atomic builtins are compiler intrinsics with no PLT. */
static alloc_tracker_t g_alloc_tracker;
static volatile int g_tracker_initialized = 0;
static volatile int g_tracker_spinlock = 0;

static inline void tracker_lock(void) {
    while (__sync_lock_test_and_set(&g_tracker_spinlock, 1)) {
        while (g_tracker_spinlock)
#if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#elif defined(__aarch64__)
            __asm__ volatile("yield");
#endif
    }
}

static inline void tracker_unlock(void) {
    __sync_lock_release(&g_tracker_spinlock);
}

/* Must be called under tracker_lock() or during single-threaded init */
static inline alloc_tracker_t *get_tracker(void) {
    if (!g_tracker_initialized) {
        alloc_tracker_init(&g_alloc_tracker);
        __sync_synchronize();
        g_tracker_initialized = 1;
    }
    return &g_alloc_tracker;
}

/* ====================================================================
 * Shared region lazy initialization
 *
 * Cannot init in la_version() because the dynamic linker is still
 * resolving symbols at that point. Defer to first memory operation.
 * ==================================================================== */

static void restore_container_env(void);

static void ensure_shrreg_init(void) {
    /* restore_container_env() must NOT run from la_version(): at that
     * point the dynamic linker is still loading the main executable, and
     * __libc_start_main has not yet pointed glibc's "environ" at this
     * process's real envp. Any setenv() done before that assignment is
     * silently discarded once it runs. Deferring to the first real HIP
     * call (guaranteed to be after the host program's own main() has
     * started, so "environ" is already the live one) was verified fixed
     * on real hardware; calling it from la_version() logged "restored"
     * but the child never actually saw the variable. */
    if (!g_shrreg_ready) {
        restore_container_env();
    }
    /* Always call hip_shrreg_init(): it's lightweight when already
     * initialized (pthread_once + flag check), and must be called
     * even after first init to trigger the env var retry mechanism
     * for multiprocess scenarios (vLLM/SGLang engine processes). */
    if (hip_shrreg_init() == 0) {
        if (!g_shrreg_ready) {
            g_shrreg_ready = 1;
            LOG_INFO("Shared region initialized for LD_AUDIT mode (pid %d)", getpid());
        }
    }
}

/* ====================================================================
 * Wrapper functions
 * ==================================================================== */

/*
 * Finish an allocation reserved with hip_reserve_device_memory: track the
 * pointer on success, return the reservation on failure. If the tracker is
 * full the memory is freed again and reported as out of memory, because its
 * free could never be accounted and the usage would leak for good.
 */
static hipError_t finish_alloc(hipError_t ret, int reserved, void **ptr,
                               size_t size, int dev, const char *what) {
    if (reserved != 0)
        return ret;  /* no limit on dev: nothing reserved or tracked */
    if (ret != hipSuccess) {
        hip_release_device_memory(dev, size);
        return ret;
    }
    tracker_lock();
    int full = alloc_tracker_insert(get_tracker(), *ptr, size, dev);
    tracker_unlock();
    if (full != 0) {
        LOG_ERROR("%s: allocation tracker full, refusing %zu bytes on device %d", what, size, dev);
        if (real_hipFree)
            real_hipFree(*ptr);
        hip_release_device_memory(dev, size);
        return hipErrorOutOfMemory;
    }
    LOG_DEBUG("%s: %zu bytes -> %p (dev %d)", what, size, *ptr, dev);
    return hipSuccess;
}

/* Reserve size on the current device, logging a rejection. */
static int reserve(int dev, size_t size, const char *what) {
    ensure_shrreg_init();
    int reserved = hip_reserve_device_memory(dev, size);
    if (reserved < 0)
        LOG_WARN("%s: OOM rejected %zu bytes on device %d", what, size, dev);
    return reserved;
}

/*
 * Free ptr with free_fn and return its accounted size to the device it was
 * allocated on (not the current one). The entry leaves the tracker before
 * the free, so a concurrent allocation that reuses the address cannot have
 * its own entry removed, and goes back in if the free fails.
 */
static hipError_t untracked_free(void *ptr, hipError_t (*free_fn)(void *, hipStream_t),
                                 hipStream_t stream) {
    int dev = 0;
    tracker_lock();
    size_t size = alloc_tracker_remove(get_tracker(), ptr, &dev);
    tracker_unlock();

    hipError_t ret = free_fn(ptr, stream);
    if (size == 0)
        return ret;
    if (ret == hipSuccess) {
        hip_release_device_memory(dev, size);
        LOG_DEBUG("free: %p released %zu bytes (dev %d)", ptr, size, dev);
    } else {
        tracker_lock();
        alloc_tracker_insert(get_tracker(), ptr, size, dev);
        tracker_unlock();
    }
    return ret;
}

static hipError_t sync_free(void *ptr, hipStream_t stream) {
    (void)stream;
    return real_hipFree(ptr);
}

static hipError_t wrap_hipMalloc(void **ptr, size_t size) {
    if (!real_hipMalloc) return hipErrorNotInitialized;
    int dev = current_device;
    int reserved = reserve(dev, size, "hipMalloc");
    if (reserved < 0)
        return hipErrorOutOfMemory;
    return finish_alloc(real_hipMalloc(ptr, size), reserved, ptr, size, dev, "hipMalloc");
}

static hipError_t wrap_hipFree(void *ptr) {
    if (!real_hipFree) return hipErrorNotInitialized;
    if (ptr == NULL) return real_hipFree(ptr);
    return untracked_free(ptr, sync_free, NULL);
}

static hipError_t wrap_hipMemGetInfo(size_t *free_mem, size_t *total_mem) {
    if (!real_hipMemGetInfo) return hipErrorNotInitialized;

    hipError_t ret = real_hipMemGetInfo(free_mem, total_mem);

    if (ret == hipSuccess) {
        ensure_shrreg_init();
        uint64_t limit = hip_get_device_memory_limit(current_device);
        if (limit > 0) {
            uint64_t usage = 0;
            if (hip_shrreg_lock() == 0) {
                usage = hip_get_device_memory_usage(current_device);
                hip_shrreg_unlock();
            }
            uint64_t left = (usage < limit) ? limit - usage : 0;
            /* Never report more than the GPU really has free: other pods,
             * and memory this library does not account, use it too. */
            if (left < *free_mem)
                *free_mem = (size_t)left;
            *total_mem = (size_t)limit;
            LOG_DEBUG("hipMemGetInfo: free=%zu total=%zu (virtualized, dev=%d)",
                      *free_mem, *total_mem, current_device);
        }
    }

    return ret;
}

static hipError_t wrap_hipSetDevice(int device) {
    if (!real_hipSetDevice) return hipErrorNotInitialized;

    hipError_t ret = real_hipSetDevice(device);
    if (ret == hipSuccess) {
        current_device = device;
        LOG_DEBUG("hipSetDevice: %d", device);
    }
    return ret;
}

static hipError_t wrap_hipMallocManaged(void **ptr, size_t size,
                                         unsigned int flags) {
    if (!real_hipMallocManaged) return hipErrorNotInitialized;
    int dev = current_device;
    int reserved = reserve(dev, size, "hipMallocManaged");
    if (reserved < 0)
        return hipErrorOutOfMemory;
    return finish_alloc(real_hipMallocManaged(ptr, size, flags), reserved, ptr, size, dev, "hipMallocManaged");
}

static hipError_t wrap_hipMallocAsync(void **ptr, size_t size,
                                       hipStream_t stream) {
    if (!real_hipMallocAsync) return hipErrorNotInitialized;
    int dev = current_device;
    int reserved = reserve(dev, size, "hipMallocAsync");
    if (reserved < 0)
        return hipErrorOutOfMemory;
    return finish_alloc(real_hipMallocAsync(ptr, size, stream), reserved, ptr, size, dev, "hipMallocAsync");
}

static hipError_t wrap_hipFreeAsync(void *ptr, hipStream_t stream) {
    if (!real_hipFreeAsync) return hipErrorNotInitialized;
    if (ptr == NULL) return real_hipFreeAsync(ptr, stream);
    return untracked_free(ptr, real_hipFreeAsync, stream);
}

static hipError_t wrap_hipMallocPitch(void **ptr, size_t *pitch,
                                       size_t width, size_t height) {
    if (!real_hipMallocPitch) return hipErrorNotInitialized;
    int dev = current_device;

    size_t estimated;
    if (__builtin_mul_overflow(width, height, &estimated))
        return hipErrorOutOfMemory;
    int reserved = reserve(dev, estimated, "hipMallocPitch");
    if (reserved < 0)
        return hipErrorOutOfMemory;

    hipError_t ret = real_hipMallocPitch(ptr, pitch, width, height);
    size_t actual = estimated;
    if (reserved == 0 && ret == hipSuccess && pitch != NULL &&
        !__builtin_mul_overflow(*pitch, height, &actual) && actual > estimated) {
        /* The row padding is real memory too; it has to fit the limit. */
        if (hip_reserve_device_memory(dev, actual - estimated) < 0) {
            LOG_WARN("hipMallocPitch: OOM rejected %zu padded bytes on device %d", actual, dev);
            if (real_hipFree)
                real_hipFree(*ptr);
            hip_release_device_memory(dev, estimated);
            return hipErrorOutOfMemory;
        }
    } else {
        actual = estimated;
    }
    return finish_alloc(ret, reserved, ptr, actual, dev, "hipMallocPitch");
}

static hipError_t wrap_hipExtMallocWithFlags(void **ptr, size_t size,
                                              unsigned int flags) {
    if (!real_hipExtMallocWithFlags) return hipErrorNotInitialized;
    int dev = current_device;
    int reserved = reserve(dev, size, "hipExtMallocWithFlags");
    if (reserved < 0)
        return hipErrorOutOfMemory;
    return finish_alloc(real_hipExtMallocWithFlags(ptr, size, flags), reserved, ptr, size, dev, "hipExtMallocWithFlags");
}

/* ====================================================================
 * Environment variable restoration for child processes
 *
 * When inference servers (vLLM, SGLang) exec engine processes with a
 * clean environment, HAMi env vars are lost.
 *
 * This library's own code (hip_multiprocess_memory_limit.c's
 * safe_getenv) and the code below both restore from /proc/.../environ,
 * but they fix two DIFFERENT problems and must stay separate:
 *
 *   - internal_restore_vars: read by THIS library's own code, in its
 *     own LD_AUDIT link-map namespace. setenv() here is visible to
 *     that same code's later getenv() calls, so restoring them once
 *     (via setenv) is correct and sufficient.
 *
 *   - getenv_fallback_vars: read by the HIP/HSA runtime itself
 *     (libamdhip64.so, loaded normally, in the DEFAULT namespace) via
 *     standard getenv(). LD_AUDIT loads this library into a SEPARATE
 *     link-map namespace with its OWN private glibc "environ": verified
 *     on real hardware that setenv() here and getenv() in the default
 *     namespace see two different "environ" pointers in the same
 *     process. A setenv() in this file can never make the HIP runtime's
 *     getenv() see the value, no matter how early or late it runs.
 *     These must instead be served by intercepting getenv() itself
 *     through la_symbind64 (see wrap_getenv), the same technique this
 *     file already uses for hipMalloc and friends: it works across
 *     namespaces because la_symbind64 rewrites the DEFAULT namespace's
 *     own PLT entry, not this namespace's environment.
 * ==================================================================== */

static const char *internal_restore_vars[] = {
    "HIP_DEVICE_MEMORY_LIMIT",
    "HIP_DEVICE_MEMORY_LIMIT_0",
    "HIP_DEVICE_MEMORY_LIMIT_1",
    "HIP_DEVICE_MEMORY_SHARED_CACHE",
    "CUDA_DEVICE_MEMORY_LIMIT",
    "CUDA_DEVICE_MEMORY_LIMIT_0",
    "LIBHIP_LOG_LEVEL",
    "ACTIVE_OOM_KILLER",
    NULL
};

static const char *getenv_fallback_vars[] = {
    "ROCR_VISIBLE_DEVICES",
    "HIP_VISIBLE_DEVICES",
    "HSA_CU_MASK",
    "ROC_GLOBAL_CU_MASK",
    NULL
};

static char *proc1_environ_buf = NULL;
static size_t proc1_environ_len = 0;
/* 0=not loaded, 2=loading, 1=ok, -1=failed */
static volatile int proc1_environ_state = 0;

/* Load /proc/1/environ once (container PID 1 always has the full pod
 * spec env); cheap to call repeatedly after the first time. getenv() runs
 * on any thread, so one loader wins an atomic flag and the rest wait for it
 * (no pthread calls: their PLT resolution re-enters la_symbind64). */
static void load_proc1_environ(void) {
    int expected = 0;
    if (__atomic_compare_exchange_n(&proc1_environ_state, &expected, 2, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        size_t cap = 0;
        ssize_t n = read_proc_file("/proc/1/environ", &proc1_environ_buf, &cap);
        if (n > 0)
            proc1_environ_len = (size_t)n;
        /* release publishes the buffer to readers that acquire the state */
        __atomic_store_n(&proc1_environ_state, (n > 0) ? 1 : -1, __ATOMIC_RELEASE);
        return;
    }
    while (__atomic_load_n(&proc1_environ_state, __ATOMIC_ACQUIRE) == 2)
        ;
}

/*
 * Search a null-separated environ buffer for NAME=VALUE.
 * Returns a pointer to VALUE (within buf) or NULL.
 */
static const char *audit_search_proc_environ(const char *buf, size_t len,
                                              const char *name) {
    size_t name_len = strlen(name);
    const char *end = buf + len;
    const char *p = buf;
    while (p < end) {
        if (strncmp(p, name, name_len) == 0 && p[name_len] == '=')
            return p + name_len + 1;
        p += strlen(p) + 1;
    }
    return NULL;
}

/*
 * Restore internal_restore_vars missing from THIS process's own
 * environment, via /proc/1/environ. Only correct for vars consumed by
 * this library's own code; see the block comment above.
 */
static void restore_container_env(void) {
    load_proc1_environ();
    if (proc1_environ_state != 1)
        return;

    int restored = 0;
    for (const char **vp = internal_restore_vars; *vp; vp++) {
        if (getenv(*vp) != NULL)
            continue;  /* already present */
        const char *val = audit_search_proc_environ(proc1_environ_buf, proc1_environ_len, *vp);
        if (val) {
            setenv(*vp, val, 0);
            LOG_INFO("restore_container_env: %s restored from /proc/1/environ", *vp);
            restored++;
        }
    }
    if (restored > 0)
        LOG_INFO("restore_container_env: restored %d env vars (pid %d)", restored, getpid());
}

/*
 * getenv() interceptor for the DEFAULT namespace (installed via
 * la_symbind64, like the HIP function wrappers). Tries the real getenv
 * first; only for the small getenv_fallback_vars set, on a miss, falls
 * back to /proc/1/environ. HSA_CU_MASK is served from /proc/1/environ
 * even when the process set its own, see env_policy.h. This is the only way to hand the HIP/HSA
 * runtime a value this library learned outside its own environment;
 * setenv() cannot cross the link-map namespace boundary (see the block
 * comment above).
 */
static char *wrap_getenv(const char *name) {
    char *val = real_getenv ? real_getenv(name) : NULL;
    if (!name)
        return val;

    int tracked = 0;
    for (const char **vp = getenv_fallback_vars; *vp; vp++) {
        if (strcmp(*vp, name) == 0) {
            tracked = 1;
            break;
        }
    }
    if (!tracked || (val && !env_is_pinned(name)))
        return val;

    load_proc1_environ();
    const char *pod_val = proc1_environ_state == 1
        ? audit_search_proc_environ(proc1_environ_buf, proc1_environ_len, name)
        : NULL;
    /* Cast away const: callers never write through a getenv() result. */
    return (char *)env_resolve(name, val, pod_val, tracked);
}

/* ====================================================================
 * LD_AUDIT interface
 * ==================================================================== */

__attribute__((visibility("default")))
unsigned int la_version(unsigned int version) {
    LOG_INFO("libamvgpu LD_AUDIT loaded (pid %d, LAV %u)", getpid(), version);
    return LAV_CURRENT;
}

/* Cookie to identify libamdhip64.so */
static int hip_cookie = -1;

__attribute__((visibility("default")))
unsigned int la_objopen(struct link_map *map, Lmid_t lmid,
                        uintptr_t *cookie) {
    static int counter = 0;
    int id = counter++;
    *cookie = (uintptr_t)id;

    if (map->l_name && strstr(map->l_name, "libamdhip64")) {
        hip_cookie = id;
        LOG_DEBUG("Identified HIP library: %s (cookie %d)", map->l_name, id);
    }

    return LA_FLG_BINDTO | LA_FLG_BINDFROM;
}

/*
 * Symbol binding interceptor.
 *
 * For each HIP function we care about:
 *   1. Capture the real function pointer from sym->st_value (first time only)
 *   2. If the caller is HIP itself (from_hip), pass through unchanged
 *      to avoid breaking HIP internal initialization
 *   3. If the caller is external (PyTorch, user app, etc.), redirect to wrapper
 *
 * This function always intercepts once loaded; it does not itself check
 * whether a memory limit is configured. A whole-GPU pod skips interception
 * one level up instead: amd-device-plugin does not set LD_AUDIT for it, so
 * this library, and la_symbind64, never run in that process at all.
 */
#if __ELF_NATIVE_CLASS == 64
__attribute__((visibility("default")))
uintptr_t la_symbind64(Elf64_Sym *sym, unsigned int ndx,
                        uintptr_t *refcook, uintptr_t *defcook,
                        unsigned int *flags, const char *symname)
#else
__attribute__((visibility("default")))
uintptr_t la_symbind32(Elf32_Sym *sym, unsigned int ndx,
                        uintptr_t *refcook, uintptr_t *defcook,
                        unsigned int *flags, const char *symname)
#endif
{
    if (!symname)
        return sym->st_value;

    /* Skip interception if caller is HIP itself */
    int from_hip = ((int)(uintptr_t)*refcook == hip_cookie);

    /*
     * Macro to handle each intercepted function:
     * - Capture real pointer on first encounter
     * - Return wrapper for external callers, real for HIP-internal
     */
    #define INTERCEPT(func_name, real_ptr, wrapper_fn) \
        if (strcmp(symname, #func_name) == 0) { \
            if (!(real_ptr)) \
                (real_ptr) = (void *)sym->st_value; \
            return from_hip ? sym->st_value : (uintptr_t)(wrapper_fn); \
        }

    /* Always intercept, even HIP-internal calls (read-only queries safe
     * to virtualize unconditionally; skipping from_hip would let
     * unvirtualized values leak to callers like PyTorch/vLLM). */
    #define INTERCEPT_ALWAYS(func_name, real_ptr, wrapper_fn) \
        if (strcmp(symname, #func_name) == 0) { \
            if (!(real_ptr)) \
                (real_ptr) = (void *)sym->st_value; \
            return (uintptr_t)(wrapper_fn); \
        }

    INTERCEPT(hipMalloc,             real_hipMalloc,             wrap_hipMalloc)
    INTERCEPT(hipFree,               real_hipFree,               wrap_hipFree)
    INTERCEPT_ALWAYS(hipMemGetInfo,  real_hipMemGetInfo,         wrap_hipMemGetInfo)
    INTERCEPT(hipSetDevice,          real_hipSetDevice,          wrap_hipSetDevice)
    INTERCEPT(hipMallocManaged,      real_hipMallocManaged,      wrap_hipMallocManaged)
    INTERCEPT(hipMallocAsync,        real_hipMallocAsync,        wrap_hipMallocAsync)
    INTERCEPT(hipFreeAsync,          real_hipFreeAsync,          wrap_hipFreeAsync)
    INTERCEPT(hipMallocPitch,        real_hipMallocPitch,        wrap_hipMallocPitch)
    INTERCEPT(hipExtMallocWithFlags, real_hipExtMallocWithFlags, wrap_hipExtMallocWithFlags)
    /* getenv must stay unconditional (INTERCEPT_ALWAYS), including for
     * from_hip callers: the HIP runtime reading ROC_GLOBAL_CU_MASK IS
     * the from_hip caller this fallback exists for. */
    INTERCEPT_ALWAYS(getenv,         real_getenv,                wrap_getenv)

    #undef INTERCEPT_ALWAYS
    #undef INTERCEPT

    return sym->st_value;
}
