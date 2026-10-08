/*
 * Unit tests for queue_priority.h.
 *
 * Build and run:
 *   gcc -o test_queue_priority test_queue_priority.c && ./test_queue_priority
 */

#include <assert.h>
#include <stdio.h>
#include "../src/hip/queue_priority.h"

int main(void) {
    assert(queue_priority_level("0") == QP_HIGH);
    assert(queue_priority_level("1") == QP_LOW);
    assert(queue_priority_level("7") == QP_LOW);
    /* Nothing asked for, or nothing usable: the queue keeps its default. */
    assert(queue_priority_level(NULL) == QP_NONE);
    assert(queue_priority_level("") == QP_NONE);
    assert(queue_priority_level("-1") == QP_NONE);
    assert(queue_priority_level("high") == QP_NONE);
    assert(queue_priority_level("1x") == QP_NONE);
    assert(queue_priority_level("99999999999999999999") == QP_NONE);
    printf("  PASS: queue_priority\n");
    return 0;
}
