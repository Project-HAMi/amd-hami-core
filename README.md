# amd-hami-core

[![CI](https://github.com/Project-HAMi/amd-hami-core/actions/workflows/ci.yml/badge.svg)](https://github.com/Project-HAMi/amd-hami-core/actions/workflows/ci.yml)
[![OpenSSF Scorecard](https://api.securityscorecards.dev/projects/github.com/Project-HAMi/amd-hami-core/badge)](https://securityscorecards.dev/viewer/?uri=github.com/Project-HAMi/amd-hami-core)
[![License](https://img.shields.io/github/license/Project-HAMi/amd-hami-core)](LICENSE)

The `LD_AUDIT` library (`libamvgpu.so`) that [amd-device-plugin](https://github.com/Project-HAMi/amd-device-plugin) uses to enforce a per-pod AMD GPU memory limit, as part of [HAMi](https://github.com/Project-HAMi/HAMi)'s GPU sharing.

## What it does

- Intercepts HIP memory calls (`hipMalloc`, `hipFree`, `hipMemGetInfo`, and related allocators listed under [Limits](#limits)) to enforce a per-pod memory limit.
- Does **not** do compute unit (CU) masking itself. That is enforced by the ROCm runtime via `HSA_CU_MASK`, which amd-device-plugin sets per pod.
- Protects that `HSA_CU_MASK` value: it reads the mask amd-device-plugin set in the pod spec (from `/proc/1/environ`) and pins child processes to it, so a process cannot widen its own slice by changing the environment variable before the runtime starts. See "Environment variable restoration" in `libamvgpu_audit.c`.

**Known gap:** a child process that re-execs itself without `LD_AUDIT` (for example via `env -i`) runs outside this library entirely, with no memory limit. Only a CU/memory limit enforced in the kernel driver itself can close that gap; this library cannot.

## Build

```bash
# Match ROCM_IMAGE to your host ROCm version.
# ROCm 6.x: rocm/dev-ubuntu-22.04:6.2
# ROCm 7.x: rocm/dev-ubuntu-24.04:7.2

docker build -f Dockerfile.hip \
  --build-arg ROCM_IMAGE=rocm/dev-ubuntu-24.04:7.2 \
  -t libamvgpu-builder .

# Extract artifacts
docker run --rm -v $(pwd)/dist:/dist libamvgpu-builder
# Output: dist/libamvgpu.so, dist/test_memory_limit
```

## Usage

Requires AMD GPU + ROCm. Clear the shared memory cache before each run
to avoid stale limits from previous sessions:

```bash
rm -f /tmp/hipdevshr.cache
```

```bash
# Memory limit test
rm -f /tmp/hipdevshr.cache
LD_AUDIT=dist/libamvgpu.so HIP_DEVICE_MEMORY_LIMIT_0=1G LIBHIP_LOG_LEVEL=3 dist/test_memory_limit

# With PyTorch (requires: pip install --pre torch --index-url https://download.pytorch.org/whl/nightly/rocm7.2)
rm -f /tmp/hipdevshr.cache
LD_AUDIT=dist/libamvgpu.so HIP_DEVICE_MEMORY_LIMIT_0=4G python3 -c "import torch; print(torch.cuda.mem_get_info())"

# With a CU slice (enforced by the ROCm runtime, not this library).
# amd-device-plugin sets HSA_CU_MASK per pod, for example GPU 0, CUs 0-15;
# this library pins it to the pod spec value, see "Environment variables".
rm -f /tmp/hipdevshr.cache
LD_AUDIT=dist/libamvgpu.so HIP_DEVICE_MEMORY_LIMIT_0=1G HSA_CU_MASK=0:0-15 LIBHIP_LOG_LEVEL=3 dist/test_memory_limit
```

### Environment variables

| Variable | Description |
|----------|-------------|
| `LD_AUDIT` | Path to `libamvgpu.so` |
| `HIP_DEVICE_MEMORY_LIMIT_<i>` | Memory limit for device `i` (0-63): a whole number with an optional `K`, `M`, `G` or `T` suffix, e.g. `4G`, `4096m`. An invalid value is logged and means no limit. |
| `HSA_CU_MASK` | Per-GPU CU slice (set by amd-device-plugin; enforced by the ROCm runtime, pinned to the pod spec value by this library) |
| `LIBHIP_LOG_LEVEL` | Log level: 1=ERROR, 2=WARN (default), 3=INFO, 4=DEBUG |

### Limits

The limit is enforced on `hipMalloc`, `hipMallocManaged`, `hipMallocAsync`, `hipMallocPitch` and `hipExtMallocWithFlags`, and checked atomically across threads and processes. Allocations made through `hipMemCreate`/`hipMemMap`, `hipMallocFromPoolAsync`, `hipMalloc3D` or `hipMallocArray` are not intercepted yet; the dmem cgroup cap set by amd-device-plugin, where available, still covers them.

## Test

```bash
# Unit tests (no GPU required)
for t in test_alloc_tracker test_env_policy test_memory_size; do gcc -o /tmp/$t test/$t.c -I src/hip && /tmp/$t; done
for t in test_reserve test_shrreg test_init; do gcc -O2 -pthread -o /tmp/$t test/$t.c src/multiprocess/hip_multiprocess_memory_limit.c && /tmp/$t; done

# On-GPU test (requires AMD GPU + ROCm)
rm -f /tmp/hipdevshr.cache
LD_AUDIT=dist/libamvgpu.so HIP_DEVICE_MEMORY_LIMIT_0=1G LIBHIP_LOG_LEVEL=3 dist/test_memory_limit

# glibc ABI check: fails if the build picked up a symbol version newer
# than this library's glibc 2.34 baseline (no GPU required)
test/check_glibc_abi.sh dist/libamvgpu.so
```

## Verified on

- AMD Instinct MI300X (192GB HBM3e)
- AMD Radeon RX 9060 XT (gfx1200) and RX 9070 XT (gfx1201), through amd-device-plugin
- ROCm 6.2, 7.0, 7.1, 7.2
