# amd-hami-core

LD_AUDIT library (`libamvgpu.so`) for AMD GPU slicing for HAMi project.
Intercepts HIP API calls (`hipMalloc`, `hipFree`, `hipMemGetInfo`) to enforce
a per-pod GPU memory limit. Compute unit masking is done by the HIP runtime
itself via `ROC_GLOBAL_CU_MASK`, set by amd-device-plugin; this library only
restores that variable for child processes that exec with a clean
environment (see "Environment variable restoration" in `libamvgpu_audit.c`).

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

# With CU mask (enforced by the HIP runtime, not this library)
# ROC_GLOBAL_CU_MASK limits which compute units the process can use.
# The mask is a hex bitmask where each bit corresponds to one CU.
# In HAMi, the scheduler calculates the mask for exclusive CU partitioning;
# this library just makes sure the variable survives into child processes.
rm -f /tmp/hipdevshr.cache
LD_AUDIT=dist/libamvgpu.so HIP_DEVICE_MEMORY_LIMIT_0=1G ROC_GLOBAL_CU_MASK=0xFFFFFFFFFFFFFFFFFFF LIBHIP_LOG_LEVEL=3 dist/test_memory_limit
```

### Environment variables

| Variable | Description |
|----------|-------------|
| `LD_AUDIT` | Path to `libamvgpu.so` |
| `HIP_DEVICE_MEMORY_LIMIT_0` | Memory limit for device 0 (e.g. `4G`, `512m`) |
| `ROC_GLOBAL_CU_MASK` | CU bitmask (set by HAMi scheduler; enforced by the HIP runtime, restored by this library for exec'd child processes) |
| `LIBHIP_LOG_LEVEL` | Log level: 1=ERROR, 2=WARN (default), 3=INFO, 4=DEBUG |

## Test

```bash
# Unit test (no GPU required)
docker run --rm libamvgpu-builder bash -c \
  "gcc -o /tmp/test test/test_alloc_tracker.c -I src/hip -lpthread && /tmp/test"

# On-GPU test (requires AMD GPU + ROCm)
rm -f /tmp/hipdevshr.cache
LD_AUDIT=dist/libamvgpu.so HIP_DEVICE_MEMORY_LIMIT_0=1G LIBHIP_LOG_LEVEL=3 dist/test_memory_limit
```

## Verified on

- AMD Instinct MI300X (192GB HBM3e)
- ROCm 6.2, 7.0, 7.1, 7.2
