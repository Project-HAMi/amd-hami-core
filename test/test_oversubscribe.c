#include <assert.h>
#include <stdio.h>

#include "oversubscribe.h"

int main(void) {
    assert(oversubscribe_on("true"));
    assert(oversubscribe_on("TRUE"));
    assert(oversubscribe_on("1"));
    assert(!oversubscribe_on("false"));
    assert(!oversubscribe_on("0"));
    assert(!oversubscribe_on(""));
    assert(!oversubscribe_on("yes"));
    assert(!oversubscribe_on(NULL));
    printf("  PASS: oversubscribe\n");
    return 0;
}
