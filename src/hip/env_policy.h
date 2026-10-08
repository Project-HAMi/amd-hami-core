/*
 * Which value getenv() hands the HIP/HSA runtime for a variable libamvgpu
 * manages. Kept free of I/O so it can be unit tested (test/test_env_policy.c).
 */
#ifndef ENV_POLICY_H
#define ENV_POLICY_H

#include <string.h>

/*
 * HSA_CU_MASK is the pod's CU slice. ROCr reads it once at init and already
 * clamps later hsa_amd_queue_cu_set_mask/hipExtStreamCreateWithCUMask calls
 * to it, so the only way out of the slice is to change the variable itself
 * (setenv/unsetenv/os.environ before the runtime starts). Pin it to the pod
 * spec value so that does not work. AMD_TASK_PRIORITY is pinned for the same
 * reason: a process that could rewrite it could raise its own priority.
 */
static inline int env_is_pinned(const char *name) {
    return strcmp(name, "HSA_CU_MASK") == 0 || strcmp(name, "AMD_TASK_PRIORITY") == 0;
}

/*
 * process_val: the process's own getenv() result. pod_val: the value in the
 * pod spec (/proc/1/environ), or NULL. tracked: whether name may fall back
 * to the pod spec when the process lost it.
 */
static inline const char *env_resolve(const char *name, const char *process_val,
                                      const char *pod_val, int tracked) {
    if (pod_val && env_is_pinned(name))
        return pod_val;
    if (process_val)
        return process_val;
    return tracked ? pod_val : NULL;
}

#endif /* ENV_POLICY_H */
