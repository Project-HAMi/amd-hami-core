/*
 * Unit tests for env_policy.h.
 *
 * Build and run:
 *   gcc -o test_env_policy test_env_policy.c && ./test_env_policy
 */

#include <assert.h>
#include <stdio.h>
#include "../src/hip/env_policy.h"

int main(void) {
    /* The pod's CU slice wins over a value the process set itself... */
    assert(strcmp(env_resolve("HSA_CU_MASK", "0:0-63", "0:0-3", 1), "0:0-3") == 0);
    /* ...and over the process unsetting it. */
    assert(strcmp(env_resolve("HSA_CU_MASK", NULL, "0:0-3", 1), "0:0-3") == 0);
    /* No slice in the pod spec: leave the process alone. */
    assert(strcmp(env_resolve("HSA_CU_MASK", "0:0-63", NULL, 1), "0:0-63") == 0);
    /* Other tracked vars only fall back when the process lost them. */
    assert(strcmp(env_resolve("ROCR_VISIBLE_DEVICES", "0", "1", 1), "0") == 0);
    assert(strcmp(env_resolve("ROCR_VISIBLE_DEVICES", NULL, "1", 1), "1") == 0);
    assert(env_resolve("HOME", NULL, "/root", 0) == NULL);
    /* The pod's priority class cannot be rewritten by the process either. */
    assert(strcmp(env_resolve("AMD_TASK_PRIORITY", "0", "1", 1), "1") == 0);
    assert(strcmp(env_resolve("AMD_TASK_PRIORITY", NULL, "1", 1), "1") == 0);
    assert(strcmp(env_resolve("AMD_TASK_PRIORITY", "0", NULL, 1), "0") == 0);
    assert(strcmp(env_resolve("HIP_OVERSUBSCRIBE", "0", "true", 1), "true") == 0);
    printf("  PASS: env_policy\n");
    return 0;
}
