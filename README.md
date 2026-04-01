# amd-hami-core

LD_AUDIT library (`libamvgpu.so`) for AMD GPU slicing for HAMi project.
Intercepts HIP API calls (`hipMalloc`, `hipFree`, `hipMemGetInfo`) to enforce
per-pod GPU memory limit and compute unit masking.

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

```bash
# Memory limit only
LD_AUDIT=dist/libamvgpu.so \
  HIP_DEVICE_MEMORY_LIMIT_0=4G \
  python3 -c "import torch; print(torch.cuda.mem_get_info())"

# Memory limit + CU mask
# ROC_GLOBAL_CU_MASK limits which compute units the process can use.
# The mask is a hex bitmask where each bit corresponds to one CU.
# In HAMi, the scheduler calculates the mask for exclusive CU partitioning.
LD_AUDIT=dist/libamvgpu.so \
  HIP_DEVICE_MEMORY_LIMIT_0=48G \
  ROC_GLOBAL_CU_MASK=0xFFFFFFFFFFFFFFFFFFF \
  ./your_app
```

### Environment variables

| Variable | Description |
|----------|-------------|
| `LD_AUDIT` | Path to `libamvgpu.so` |
| `HIP_DEVICE_MEMORY_LIMIT_0` | Memory limit for device 0 (e.g. `4G`, `512m`) |
| `ROC_GLOBAL_CU_MASK` | CU bitmask (set by HAMi scheduler) |
| `LIBHIP_LOG_LEVEL` | Log level: 1=ERROR, 2=WARN (default), 3=INFO, 4=DEBUG |

## Test

```bash
# Unit test (no GPU required)
docker run --rm libamvgpu-builder bash -c \
  "gcc -o /tmp/test test/test_alloc_tracker.c -I src/hip -lpthread && /tmp/test"

# On-GPU test (requires AMD GPU + ROCm)
LD_AUDIT=dist/libamvgpu.so \
  HIP_DEVICE_MEMORY_LIMIT_0=1G \
  LIBHIP_LOG_LEVEL=3 \
  dist/test_memory_limit
```

## Verified on

- AMD Instinct MI300X (192GB HBM3e)
- ROCm 6.2, 7.0, 7.1, 7.2
