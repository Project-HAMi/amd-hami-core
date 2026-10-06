# Contributing

Thanks for your interest in amd-hami-core, the LD_AUDIT memory-limiting
library AMD device plugin uses for HAMi. This repo follows the community
norms of the broader [HAMi project](https://github.com/Project-HAMi/HAMi):
please read its [Code of Conduct](https://github.com/Project-HAMi/HAMi/blob/master/CODE_OF_CONDUCT.md)
and [Contributing guide](https://github.com/Project-HAMi/HAMi/blob/master/CONTRIBUTING.md)
before opening a PR here, in particular the AI assistance disclosure
requirement.

## Building and testing

```bash
# Build libamvgpu.so (requires ROCm; set ROCM_HOME if not /opt/rocm)
make -f Makefile.hip

# Build and run the on-GPU test (requires an AMD GPU)
make -f Makefile.hip test
rm -f /tmp/hipdevshr.cache
LD_AUDIT=build-hip/libamvgpu.so HIP_DEVICE_MEMORY_LIMIT_0=1G LIBHIP_LOG_LEVEL=3 build-hip/test_memory_limit

# Unit tests (no GPU required)
gcc -o /tmp/test test/test_alloc_tracker.c -I src/hip -lpthread && /tmp/test

# glibc ABI regression check (no GPU required)
test/check_glibc_abi.sh build-hip/libamvgpu.so
```

See the [README](README.md) for the Docker-based build and more usage
examples.

## Submitting a change

1. Fork the repo and create a branch for your change.
2. Keep changes focused; unrelated cleanup belongs in its own PR.
3. If you touch `src/hip/libamvgpu_audit.c` or
   `src/multiprocess/hip_multiprocess_memory_limit.c`, run the on-GPU test
   on real AMD hardware before opening the PR and say so in the PR
   description. This library runs inside other people's containers, so a
   change that only compiles but was never exercised against a real GPU is
   not reviewable.
4. Open a pull request describing what changed and why, and which of the
   checks above you ran.
5. A maintainer will review and may ask for changes before merging.

## Reporting issues

Open a GitHub issue with reproduction steps, the ROCm version, and the GPU
model. For anything that looks like a security issue (for example, a way to
defeat the memory limit), see the main
[HAMi security policy](https://github.com/Project-HAMi/HAMi/blob/master/SECURITY.md)
instead of filing a public issue.
