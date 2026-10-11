---
name: Bug Report
about: Report a problem encountered while using amd-hami-core (libamvgpu.so)
labels: bug
---

<!-- Please use this template while reporting a bug and provide as much info as possible. Not doing so may result in your bug not being addressed in a timely manner. Thanks!
-->

**What happened**:

**What you expected to happen**:

**How to reproduce it (as minimally and precisely as possible)**:

**Anything else we need to know?**:

- Relevant, time-bounded excerpts of the `[HAMI-core-hip ...]` lines the library prints (set `LIBHIP_LOG_LEVEL=4` for debug output)
- The values of `LD_AUDIT`, `HIP_DEVICE_MEMORY_LIMIT_*`, `HSA_CU_MASK`, `AMD_TASK_PRIORITY` and `HIP_OVERSUBSCRIBE` inside the container
- Relevant excerpts from AMD SMI or ROCm SMI output; mask GPU UUIDs, PCI addresses, and host or node identifiers
- The HIP call that misbehaved and a minimal program that reproduces it, if you have one
- Whether `/tmp/hipdevshr.cache` was cleared between runs

Before posting, remove or mask credentials, tokens, GPU identifiers, PCI addresses, node or host identifiers, and other sensitive data from configuration and logs.

**Environment**:
- amd-hami-core version or amd-device-plugin image that ships it:
- AMD GPU model:
- ROCm and AMD SMI version:
- `amdgpu` kernel driver version:
- Workload image and glibc version (`ldd --version`):
- Kernel version from `uname -a`:
- Others:
