#!/bin/bash -eu
# build every fuzz target of fuzz/ into $OUT
for f in fuzz/fuzz_*.c; do
  $CC $CFLAGS -fsanitize=fuzzer -o "$OUT/$(basename "${f%.c}")" "$f" $LIB_FUZZING_ENGINE
done
