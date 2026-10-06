#!/usr/bin/env bash
#
# Fails if libamvgpu.so requires a glibc symbol version newer than 2.34.
# The library targets glibc >= 2.34 (see src/include/glibc_compat.h); a
# newer requirement silently breaks containers with older glibc instead
# of failing the build.
#
# Usage: test/check_glibc_abi.sh path/to/libamvgpu.so

set -euo pipefail

lib="${1:?usage: $0 path/to/libamvgpu.so}"
max_allowed="2.34"

bad=$(objdump -T "$lib" \
  | grep -oE '\(GLIBC_[0-9]+\.[0-9]+(\.[0-9]+)?\)' \
  | tr -d '()' \
  | sed 's/^GLIBC_//' \
  | sort -V -u \
  | awk -F. -v max_major="${max_allowed%%.*}" -v max_minor="${max_allowed#*.}" '
      { major=$1+0; minor=$2+0
        if (major > max_major || (major == max_major && minor > max_minor)) print
      }')

if [ -n "$bad" ]; then
  echo "FAIL: $lib requires glibc symbol versions newer than $max_allowed:" >&2
  echo "$bad" | sed 's/^/  GLIBC_/' >&2
  exit 1
fi

echo "OK: $lib requires glibc <= $max_allowed"
