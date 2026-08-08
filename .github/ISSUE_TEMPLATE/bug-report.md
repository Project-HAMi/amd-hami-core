---
name: Bug Report
about: Report a problem encountered while using amd-hami-core
labels: bug
---

<!-- Please use this template while reporting a bug and provide as much info as possible. Not doing so may result in your bug not being addressed in a timely manner. Thanks!
-->

**What happened**:

**What you expected to happen**:

**How to reproduce it (as minimally and precisely as possible)**:

**Anything else we need to know?**:

- Relevant excerpts from AMD SMI or ROCm SMI output; mask GPU UUIDs, PCI addresses, and host identifiers
- Relevant, time-bounded excerpts from the workload and `libamvgpu.so` logs
- Relevant `LD_AUDIT`, `HIP_DEVICE_MEMORY_LIMIT_*`, `ROC_GLOBAL_CU_MASK`, and `LIBHIP_LOG_LEVEL` values
- The exact build and workload reproduction commands
- Whether `/tmp/hipdevshr.cache` was cleared before reproduction
- Relevant, time-bounded AMD driver or kernel output

Before posting, remove or mask credentials, tokens, GPU identifiers, PCI addresses, process or workload identifiers, host paths, internal image names, and other sensitive data from configuration and logs.

**Environment**:
- amd-hami-core version or commit:
- AMD GPU model:
- ROCm and `amdgpu` driver version:
- Workload framework and version:
- Build image and tag:
- Container runtime and workload image:
- Operating system and libc version:
- Kernel version from `uname -a`:
- Others:
