# Build environment for libamvgpu.so (AMD GPU memory virtualization)
#
# Builds against the specified ROCm version. Match this to the host ROCm.
#
# Usage:
#   # Default (ROCm 6.2):
#   docker build -f Dockerfile.hip -t libamvgpu-builder .
#
#   # ROCm 7.1:
#   docker build -f Dockerfile.hip \
#     --build-arg ROCM_IMAGE=rocm/dev-ubuntu-24.04:7.1 \
#     -t libamvgpu-builder .
#
#   # Extract artifacts:
#   docker run --rm -v $(pwd)/dist:/dist libamvgpu-builder
#
# Output: dist/libamvgpu.so, dist/test_memory_limit
#
# For development (mount source, iterate):
#   docker run --rm -it -v $(pwd):/workspace libamvgpu-builder bash
#   cd /workspace && make -f Makefile.hip clean all
#

# ROCm 7.x requires Ubuntu 24.04 (glibc 2.38+), 6.x uses Ubuntu 22.04.
# Examples:
#   --build-arg ROCM_IMAGE=rocm/dev-ubuntu-22.04:6.2   (ROCm 6.2)
#   --build-arg ROCM_IMAGE=rocm/dev-ubuntu-24.04:7.1   (ROCm 7.1)
ARG ROCM_IMAGE=rocm/dev-ubuntu-22.04:6.2
FROM ${ROCM_IMAGE} AS builder

RUN apt-get update -qq && \
    apt-get install -y -qq --no-install-recommends \
        cmake build-essential && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY . /build/

RUN make -f Makefile.hip clean all && \
    make -f Makefile.hip test

# Verify: no GLIBC_2.38 dependency
RUN ! objdump -T build-hip/libamvgpu.so | grep -q "GLIBC_2.38" && \
    echo "OK: No GLIBC_2.38 dependency" || \
    (echo "ERROR: GLIBC_2.38 dependency found!" && exit 1)

# Verify: LD_AUDIT symbols exported
RUN nm -D build-hip/libamvgpu.so | grep -q "T la_version" && \
    nm -D build-hip/libamvgpu.so | grep -q "T la_symbind64" && \
    echo "OK: LD_AUDIT symbols exported"

# --- Output stage ---
FROM scratch AS output
COPY --from=builder /build/build-hip/libamvgpu.so /libamvgpu.so
COPY --from=builder /build/build-hip/test_memory_limit /test_memory_limit

# Default: copy artifacts to /dist mount
FROM builder AS default
CMD ["sh", "-c", "mkdir -p /dist && cp build-hip/libamvgpu.so build-hip/test_memory_limit /dist/ && echo 'Copied to /dist:' && ls -la /dist/"]
