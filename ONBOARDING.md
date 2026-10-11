# Onboarding: amd-hami-core

This guide is for someone who sees the repo for the first time. It covers `libamvgpu.so`, the `LD_AUDIT` library that amd-device-plugin uses to limit AMD GPU memory under HAMi.

## In one minute

1. `libamvgpu.so` is a small C library that the Linux dynamic linker loads into a GPU program through `LD_AUDIT`.
2. It watches HIP memory calls and refuses an allocation when the pod would go over its memory limit.
3. It also sets queue priority and keeps the pod's `HSA_CU_MASK` value from being changed by the program.
4. It does not mask compute units itself. The ROCm runtime does that.
5. A process that starts without `LD_AUDIT` runs outside the library, so this is cooperative sharing and not a hard security wall.

## 1. What it is and what it does not do

### What it is

- The repo is `Project-HAMi/amd-hami-core`. It holds one shared library, `libamvgpu.so`.
- It is licensed under Apache 2.0.
- amd-device-plugin loads it into pods that get a slice of a GPU.
- It is the AMD counterpart of the HAMi core library for NVIDIA. The accounting code follows the same pattern.
- It is plain C. It does not link against HIP or HSA. It declares the few types it needs by hand.

### What it does

- It enforces a per-pod, per-device GPU memory limit on HIP allocation calls.
- It makes `hipMemGetInfo`, `hipDeviceTotalMem` and the `totalGlobalMem` field of the device properties report the limit as the total memory.
- It counts memory across all processes of one container, through one shared file.
- It sets the priority class of every compute queue that the HIP runtime creates (`AMD_TASK_PRIORITY`).
- It pins three variables to the pod spec value: `HSA_CU_MASK`, `AMD_TASK_PRIORITY` and `HIP_OVERSUBSCRIBE`.
- It can serve `hipMalloc` from managed memory when `HIP_OVERSUBSCRIBE` is on, so a limit above the physical VRAM can be used.
- It restores some variables from the container's PID 1 when a child process lost them.

### What it does not do

- It does not mask compute units. The ROCm runtime enforces `HSA_CU_MASK`. The plugin sets it per pod.
- It is not a hard security boundary. `SECURITY.md` says a workload with enough privilege can bypass it. Examples are unsetting `LD_AUDIT`, linking the HIP runtime statically, or calling `ptrace` on itself.
- It does not count every kind of GPU memory. It counts the allocation calls listed in section 3.
- It does not intercept `hipMallocMipmappedArray`.
- It does not limit a process that runs without `LD_AUDIT`. A child that re-executes itself with a clean environment (for example `env -i`) is outside the library.
- It does not run for a whole-GPU pod. The plugin sets no `LD_AUDIT` for such a pod.

## 2. How it works

### LD_AUDIT in plain words

- `LD_AUDIT` names a library that the dynamic linker calls while it loads other libraries and binds symbols.
- The audit library exports `la_version`, `la_objopen` and `la_symbind64`.
- `la_version` runs first. The library logs a line and returns `LAV_CURRENT`.
- `la_objopen` runs for every loaded object. The library gives each object a cookie (a counter). When the file name contains `libamdhip64`, it remembers that cookie and the path. When the name contains `libhsa-runtime64`, it remembers the path.
- `la_objopen` returns `LA_FLG_BINDTO | LA_FLG_BINDFROM`, so `la_symbind64` is called for every symbol bound to and from each object.
- `la_symbind64` gets the symbol name. If the name is on the list, the library returns the address of its own wrapper in place of the real function.

### Why not LD_PRELOAD

- The source header says `LD_PRELOAD` breaks HIP start-up on ROCm 7.x because of internal `dlsym` and symbol resolution conflicts.
- `LD_AUDIT` works at the linker level. It can send only calls that come from outside the HIP library to the wrappers. Calls inside HIP stay untouched.

### Symbol binding

- The first time a symbol is seen, the library stores the real address (`sym->st_value`) in a function pointer such as `real_hipMalloc`.
- `INTERCEPT` returns the wrapper for outside callers. It returns the real function for calls that come from HIP itself (the caller's cookie equals the HIP cookie).
- `INTERCEPT_ALWAYS` returns the wrapper for every caller, HIP included. It is used for the read-only size queries, for `getenv` and for `hsa_queue_create`.
- Unknown symbols pass through unchanged.
- `la_symbind64` does not check whether a limit is set. If the library is loaded, it intercepts.

### Namespaces

- `LD_AUDIT` loads the library into a separate link-map namespace. That namespace has its own copy of glibc `environ`.
- A `setenv` in the library is therefore invisible to the HIP runtime, which lives in the default namespace.
- This is why the library intercepts `getenv` itself (`wrap_getenv`). The rewritten binding lives in the default namespace, so the runtime sees the value the library returns.
- To call the real HSA or HIP runtime from inside the audit namespace, the library reopens the library by path with `dlmopen(LM_ID_BASE, path, RTLD_NOW | RTLD_NOLOAD)`. `NOLOAD` returns the existing handle and never loads a second copy.

### No pthread calls in hot paths

- The binding code can be re-entered while the linker holds its lock. A lazy PLT lookup of `pthread_mutex_lock` would then deadlock.
- The library uses compiler atomics instead. The allocation tracker uses a spinlock built from `__sync_lock_test_and_set`. Initialization uses an atomic state flag (0 not started, 2 in progress, 1 ready).

### Lazy start-up

- The shared region is not created in `la_version`. The linker is still resolving symbols then.
- It is created on the first memory operation, in `ensure_shrreg_init`.
- The restore of environment variables also waits for that first call. Before `main` starts, glibc's `environ` is not the live one yet, and a `setenv` made too early is lost.

### The accounting flow for one allocation

1. The program calls `hipMalloc(&p, size)`. The wrapper runs.
2. The wrapper reads the thread's current device (set by the `hipSetDevice` wrapper, default 0).
3. `hip_reserve_device_memory` takes the semaphore, checks usage plus size against the limit, and adds the size to this process's slot in one step.
4. If it would go over the limit, the wrapper returns `hipErrorOutOfMemory` and never calls HIP.
5. Otherwise the wrapper calls the real function.
6. On failure, the reservation is given back.
7. On success, the pointer, size and device go into the allocation tracker.
8. `hipFree` removes the pointer from the tracker, calls the real free, and gives the bytes back to the device the memory came from. If the free fails, the entry goes back into the tracker.

The reserve-first step is what makes concurrent allocations safe. Without it, many threads could all pass the check and together exceed the limit.

### The allocation tracker (`alloc_tracker.h`)

- A hash table that maps pointer to size and device.
- Open addressing, linear probing, tombstones for deleted slots.
- Capacity is 16384 live allocations. When it is full, the allocation is freed again and reported as out of memory.
- A duplicate insert of the same pointer updates the entry.

### Pitched and shaped allocations

- `hipMallocPitch`, `hipMalloc3D` and `hipMemAllocPitch` reserve width times height first. After the call they know the real pitch. If the padded size is larger, they reserve the extra bytes. If the extra does not fit, the allocation is freed and the call returns out of memory.
- `hipMallocArray`, `hipMalloc3DArray` and the driver-API array creators compute the size from the channel format (`alloc_size.h`). If the size cannot be computed (empty, unknown format, overflow), the call passes through unaccounted.

### The shared accounting file

- Default path: `/tmp/hipdevshr.cache`. Override with `HIP_DEVICE_MEMORY_SHARED_CACHE`.
- The default path is inside the container's own `/tmp`. Processes of one container share one limit.
- The file is opened with `O_CREAT`, locked with `flock` during creation, sized with `ftruncate` and mapped with `mmap` (`MAP_SHARED`).
- The region has a magic number (`HMI2`), a process-shared semaphore, a limit per device, and 1024 process slots (`HIP_MAX_PROCS`).
- Each slot holds a pid, a status and, for each of 64 devices (`HIP_MAX_DEVICES`), the context, module and data bytes. The library only uses the data category today.
- Device indexes at or above 64 have no limit.
- Each process takes a slot on start-up and clears it at exit (`atexit`). A fork child registers a new slot through a `pthread_atfork` handler.
- Usage of a device is the sum over slots whose pid is still alive (checked through `/proc/<pid>/stat`).
- If all slots are used, dead ones are reclaimed.
- A slot that carries the current pid at start-up belongs to an older process (a restarted container that reused a persisted file). Its usage is reset.
- The semaphore wait has a 10 second timeout and up to 30 retries. If the lock owner is dead, exactly one waiter recovers the semaphore (compare and swap on `owner_pid`).
- A stuck lock or a process without a slot makes the reservation fail open. The allocation is allowed and not counted.
- Clear the file before a test run (`rm -f /tmp/hipdevshr.cache`). An old file keeps old limits.

### Reading variables early

- `safe_getenv` first calls `getenv`. If that returns nothing, it reads `/proc/self/environ`, then `/proc/1/environ` (the container's PID 1 has the full pod spec).
- If the first read finds no limit at all, the library retries on later calls, because some engine processes (vLLM or SGLang style) start HIP before their environment is ready.
- `read_proc_file` reads the whole file in a growing buffer. A fixed buffer once cut `4096m` into `409`.

### Size parsing

- `parse_memory_size` accepts a whole number with an optional `K`, `M`, `G` or `T`, in either case, and one trailing newline.
- Anything else means no limit: `1.5G`, `-1`, `4096 M`, `4096MB`, text, an empty string, a missing value and overflow.
- A bad value is logged and never becomes a tiny or huge limit.

### Environment pinning

- `env_is_pinned` is true for `HSA_CU_MASK`, `AMD_TASK_PRIORITY` and `HIP_OVERSUBSCRIBE`.
- For these, `wrap_getenv` returns the pod spec value from `/proc/1/environ` when it exists. It does so even when the process set its own copy or removed it.
- Without a pod spec value, the process's own value is used.
- Other tracked variables (`ROCR_VISIBLE_DEVICES`, `HIP_VISIBLE_DEVICES`, `ROC_GLOBAL_CU_MASK`) only fall back to the pod spec when the process lost them.
- Why: the ROCm runtime reads `HSA_CU_MASK` once at start and already clamps later mask calls to it. The only way out of the slice is to change the variable before the runtime starts. Pinning closes that way.

### Queue priority

- The HIP runtime creates every stream's queue through `hsa_queue_create`. The library wraps that call, including calls made by HIP itself.
- After a successful create, it reads `AMD_TASK_PRIORITY` through `wrap_getenv` and calls `hsa_amd_queue_set_priority`.
- `queue_priority_level`: `0` gives the high class. Any higher whole number gives the low class. Empty, negative, text and overflow values ask for nothing, and the queue keeps its default.
- The function `hsa_amd_queue_set_priority` is found with `dlmopen` and `dlsym` on the HSA runtime path, on first use.

### Oversubscription

- With `HIP_OVERSUBSCRIBE` set to `true` (any case) or `1`, `wrap_hipMalloc` calls `hipMallocManaged` instead, with the global attach flag.
- The memory limit still applies. The reservation is made before.
- If the program never called `hipMallocManaged`, the function is not bound yet. The wrapper looks it up in the runtime once.
- If it cannot be found, a warning is logged and the normal `hipMalloc` is used.

## 3. Every intercepted call

The table follows `la_symbind64` in `src/hip/libamvgpu_audit.c`. "Outside" means only callers outside the HIP library get the wrapper. "Always" means every caller does.

| Symbol | Mode | What the wrapper does |
|---|---|---|
| `hipMalloc` | outside | Reserve, allocate (from managed memory if oversubscribed), track. |
| `hipFree` | outside | Remove from tracker, free, release bytes. |
| `hipMemGetInfo` | always | Real call, then total becomes the limit and free becomes limit minus usage, never above the real free. |
| `hipSetDevice` | outside | Real call, then remember the device for this thread. |
| `hipMallocManaged` | outside | Reserve, allocate, track. |
| `hipMallocAsync` | outside | Reserve, allocate, track. |
| `hipFreeAsync` | outside | Remove from tracker, free, release bytes. |
| `hipMallocPitch` | outside | Reserve width times height, then the padding. |
| `hipExtMallocWithFlags` | outside | Reserve, allocate, track. |
| `hipMalloc3D` | outside | Reserve extent size, then the padding. |
| `hipMallocArray` | outside | Reserve size from channel format. |
| `hipMalloc3DArray` | outside | Reserve size from channel format. |
| `hipFreeArray` | outside | Remove from tracker, free, release bytes. |
| `hipArrayCreate` | outside | Driver-API array, size from format and channels. |
| `hipArray3DCreate` | outside | Driver-API 3D array. |
| `hipArrayDestroy` | outside | Remove from tracker, destroy, release bytes. |
| `hipMemAllocPitch` | outside | Driver-API pitched allocation, with padding. |
| `hipMallocFromPoolAsync` | outside | Reserve, allocate from the pool, track. |
| `hipMemCreate` | outside | Reserve the physical memory of a virtual memory handle on the current device. |
| `hipMemRelease` | outside | Remove from tracker, release the handle, release bytes. |
| `hipDeviceTotalMem` | always | Real call, then clamp to the limit. |
| `hipGetDevicePropertiesR0600` | always | Clamp `totalGlobalMem` (offset 288). |
| `hipGetDeviceProperties` | always | Clamp `totalGlobalMem` (offset 256, older layout). |
| `hipGetDevicePropertiesR0000` | always | Clamp `totalGlobalMem` (offset 256). |
| `getenv` | always | Serve pinned and fallback variables from the pod spec. |
| `hsa_queue_create` | always | Create the queue, then set its priority. |

Notes on the table:

- The clamp only lowers the total. A limit above the real total changes nothing (`clamp_total`).
- A failed call returns `hipErrorNotInitialized` when the real function pointer is not known yet.
- A pointer that is not in the tracker (for example allocated before the library saw it) is freed without accounting.

## 4. Environment variables

| Variable | Set by | Effect |
|---|---|---|
| `LD_AUDIT` | the plugin (`/usr/local/vgpu/libamvgpu.so`), slices only | Loads the library. |
| `HIP_DEVICE_MEMORY_LIMIT_<i>` | the plugin, one per container-local device index, value like `4096m` | Memory limit of device `i` (0 to 63). Invalid means no limit. |
| `HIP_DEVICE_MEMORY_LIMIT` | optional | Limit for every device without its own variable. |
| `HIP_DEVICE_MEMORY_SHARED_CACHE` | optional | Path of the shared accounting file. Default `/tmp/hipdevshr.cache`. |
| `ACTIVE_OOM_KILLER` | optional | Value `true` clears the slots of dead processes when an allocation would go over the limit, then checks again. |
| `LIBHIP_LOG_LEVEL` | the plugin, when its `-hip_log_level` is above 0 | 1 error, 2 warn (default), 3 info, 4 debug. Values outside 1 to 4 give the default. Messages go to stderr. |
| `HSA_CU_MASK` | the plugin | Read by ROCm to limit compute units. The library serves the pod spec value. |
| `AMD_TASK_PRIORITY` | the user, in the pod spec | `0` high class, higher is low class, unset keeps the default. Pinned. |
| `HIP_OVERSUBSCRIBE` | the plugin, when its memory scale is above 1 | `true` or `1` serves `hipMalloc` from managed memory. Pinned. |
| `ROCR_VISIBLE_DEVICES`, `HIP_VISIBLE_DEVICES`, `ROC_GLOBAL_CU_MASK` | the plugin or the user | Read by ROCm. The library only restores them when the process lost them. |
| `CUDA_DEVICE_MEMORY_LIMIT`, `CUDA_DEVICE_MEMORY_LIMIT_0` | legacy | In the restore list. The library does not read them for limits. |

The restore list for the library's own use (`internal_restore_vars`) is: `HIP_DEVICE_MEMORY_LIMIT`, `HIP_DEVICE_MEMORY_LIMIT_0`, `HIP_DEVICE_MEMORY_LIMIT_1`, `HIP_DEVICE_MEMORY_SHARED_CACHE`, `CUDA_DEVICE_MEMORY_LIMIT`, `CUDA_DEVICE_MEMORY_LIMIT_0`, `LIBHIP_LOG_LEVEL` and `ACTIVE_OOM_KILLER`. They are filled from `/proc/1/environ` when missing.

## 5. How amd-device-plugin uses it

- The plugin ships the library in the image and mounts it at `/usr/local/vgpu/libamvgpu.so` for pods that get a slice.
- For such a pod it sets `LD_AUDIT`, one `HIP_DEVICE_MEMORY_LIMIT_<i>` per device (in MiB, like `4096m`), and `HSA_CU_MASK` for the compute slice.
- It sets `LIBHIP_LOG_LEVEL` when asked, and `HIP_OVERSUBSCRIBE=true` when the device memory scale is above 1.
- The plugin does not set `AMD_TASK_PRIORITY`. The user sets it in the pod spec.
- A whole-GPU pod gets no `LD_AUDIT`. The library never runs in it.
- Where the kernel has the dmem cgroup controller, the plugin also sets a cgroup cap. That cap covers what the library does not intercept, such as `hipMallocMipmappedArray`.
- The plugin documents that workload images need glibc 2.34 or newer. That comes from this library's build baseline (section 8).

## 6. Limits and known gaps

- Calls outside the library: a process without `LD_AUDIT` (for example after `env -i`), a statically linked HIP runtime, and musl-based binaries run with no limit. Only a limit in the kernel driver can close this.
- `hipMallocMipmappedArray` is not intercepted. The dmem cgroup cap helps where it exists.
- Only the data category is counted. Contexts and code objects that HIP allocates on its own are not.
- At most 64 devices and 1024 processes per container. At most 16384 live tracked allocations per process.
- A device index of 64 or more has no limit.
- The device of an allocation is the thread's current device. A thread that never calls `hipSetDevice` uses device 0.
- A virtual memory handle that is shared or retained elsewhere leaves the accounting at its first `hipMemRelease` (a `ponytail:` comment in the source marks this).
- Fail open: if the lock cannot be taken or the process has no slot, the allocation goes through uncounted.
- The check uses a size estimate for shaped arrays. If the format is unknown or the size overflows, the call is not accounted.
- A limit larger than the real VRAM does not make memory appear. Only `HIP_OVERSUBSCRIBE` lets `hipMalloc` use managed memory.
- A limit is read once, when the region is created. A kept old file keeps old limits.
- The pins read `/proc/1/environ`. Outside a container, PID 1 is the host init. Test with care.
- Verified by the README on: MI300X, RX 9060 XT (gfx1200), RX 9070 XT (gfx1201), and ROCm 6.2, 7.0, 7.1 and 7.2.

## 7. What was observed on a real MI325X

These were measured by hand on one MI325X, on a virtual function (so partition mode flips were not possible).

- Compute slice: a compute kernel ran at 215 kernels/s in a 25 percent slice (mask `0:0-75`).
- Setting `HSA_CU_MASK` with `setenv` before HIP starts did not widen the slice with the library (still 215 kernels/s). Without the library it did (1015 kernels/s). This is the pin working.
- Priority: two processes on the same 25 percent slice got 144.7 and 72.3 kernels/s with `AMD_TASK_PRIORITY` 0 and 3. Without it they got 108.7 and 108.5.
- Memory calls: with a 1 GiB limit, `hipMalloc3D`, `hipMallocArray`, `hipMalloc3DArray`, `hipMallocFromPoolAsync` and `hipMemCreate` all returned out of memory. `hipMemGetInfo` reported 1 GiB.
- Concurrency: three concurrent 1.5 GiB allocations under a 4 GiB limit gave exactly two successes, in each of three rounds.
- Not checked: a `hipMalloc` larger than the VRAM, served from managed memory. XNACK was off on the virtual function.

## 8. Repo layout

```
.github/
  ISSUE_TEMPLATE/    bug-report.md, enhancement.md, good-first.md, question.md, config.yml
  PULL_REQUEST_TEMPLATE.md
  dependabot.yml
  rocm/              pinned ROCm build images, one Dockerfile per version
  workflows/         ci.yml, codeql-analysis.yml, scorecard.yaml
src/
  hip/               the audit library and its pure helpers
  include/           small shared headers
  multiprocess/      the shared accounting region
test/                unit tests and the glibc check
```

Every file:

| Path | What it is |
|---|---|
| `README.md` | What it does, build, usage, variables, limits, tests. |
| `CONTRIBUTING.md` | Build and test commands, how to submit a change. |
| `SECURITY.md` | Private reporting through the HAMi advisory form, and what is in scope. |
| `CODE_OF_CONDUCT.md` | Community rules. |
| `LICENSE` | Apache 2.0. |
| `security-insights.yml` | Project security metadata. |
| `Makefile.hip` | Entry point for the build. Links the CMake file into `build-hip/` and runs cmake and make. Targets: `all`, `test`, `clean`, `install`. |
| `CMakeLists.txt.hip` | The CMake build. It is not named `CMakeLists.txt` on purpose. |
| `Dockerfile.hip` | Builds in a ROCm image, runs the checks, exports the files. |
| `.gitignore` | Ignore rules. |
| `.github/PULL_REQUEST_TEMPLATE.md` | PR template: kind, what and why, issue, notes, user-facing change, AI disclosure. |
| `.github/dependabot.yml` | Keeps actions and the pinned image digests current. |
| `.github/rocm/rocm-6.2.Dockerfile` | One `FROM` line: pinned ROCm 6.2 image on Ubuntu 22.04. CI reads it. |
| `.github/rocm/rocm-7.2.Dockerfile` | One `FROM` line: pinned ROCm 7.2 image on Ubuntu 24.04. |
| `.github/workflows/ci.yml` | Unit tests, build matrix, glibc check. |
| `.github/workflows/codeql-analysis.yml` | CodeQL scan of the C code. |
| `.github/workflows/scorecard.yaml` | OpenSSF Scorecard run. |
| `src/hip/libamvgpu_audit.c` | The audit library: LD_AUDIT hooks, wrappers, env pinning, queue priority. |
| `src/hip/alloc_tracker.h` | Pointer to size hash table. |
| `src/hip/alloc_size.h` | Size of shaped allocations, and the total clamp. |
| `src/hip/env_policy.h` | Which variables are pinned, and which value `getenv` returns. |
| `src/hip/oversubscribe.h` | Parses `HIP_OVERSUBSCRIBE`. |
| `src/hip/queue_priority.h` | Maps `AMD_TASK_PRIORITY` to an HSA priority. |
| `src/hip/test_memory_limit.c` | The on-GPU test program. |
| `src/include/libamvgpu.h` | Variable names and the default cache path. |
| `src/include/glibc_compat.h` | Keeps the build at the glibc 2.34 baseline. |
| `src/include/hip_log_utils.h` | Log macros and `LIBHIP_LOG_LEVEL` handling. |
| `src/include/memory_size.h` | Parses sizes like `4096m`. |
| `src/include/proc_file.h` | Reads a whole `/proc` file. |
| `src/multiprocess/hip_multiprocess_memory_limit.h` | Region layout, constants and function list. |
| `src/multiprocess/hip_multiprocess_memory_limit.c` | Region init, slots, lock, reserve and release, cleanup. |
| `test/check_glibc_abi.sh` | Fails if the library needs a glibc symbol newer than 2.34. |
| `test/test_alloc_size.c` | See section 10. |
| `test/test_alloc_tracker.c` | See section 10. |
| `test/test_env_policy.c` | See section 10. |
| `test/test_init.c` | See section 10. |
| `test/test_memory_size.c` | See section 10. |
| `test/test_oversubscribe.c` | See section 10. |
| `test/test_queue_priority.c` | See section 10. |
| `test/test_reserve.c` | See section 10. |
| `test/test_retry.c` | See section 10. |
| `test/test_shrreg.c` | See section 10. |

A good reading order: `README.md`, then `libamvgpu_audit.c` from the bottom (`la_symbind64`) upward, then `hip_multiprocess_memory_limit.c`.

## 9. How to build

### Local build

```
make -f Makefile.hip                     # build-hip/libamvgpu.so
make -f Makefile.hip test                # also build-hip/test_memory_limit (needs hipcc)
make -f Makefile.hip ROCM_HOME=/opt/rocm-6.4.0
make -f Makefile.hip clean
make -f Makefile.hip install             # /usr/local/lib, then ldconfig
```

- You need ROCm headers, `cmake` and a C compiler. `ROCM_HOME` defaults to `/opt/rocm`.
- The makefile creates `build-hip/`, links `CMakeLists.txt.hip` in as `CMakeLists.txt` and links `src` next to it. CMake has no flag for another file name, so this is the trick.
- Flags: `-fPIC -D_GNU_SOURCE -std=gnu11 -Wall -Wextra`. Release adds `-O2 -fvisibility=hidden` and strips local symbols. Debug adds `-g -O0 -DDEBUG`.
- Only `la_version`, `la_objopen` and `la_symbind64` are exported (marked with default visibility).
- The build embeds the short git hash when git is available.
- The library links `dl` and `pthread`.

### Docker build

```
docker build -f Dockerfile.hip \
  --build-arg ROCM_IMAGE=rocm/dev-ubuntu-24.04:7.2 \
  -t libamvgpu-builder .
docker run --rm -v $(pwd)/dist:/dist libamvgpu-builder
# dist/libamvgpu.so and dist/test_memory_limit
```

- Match `ROCM_IMAGE` to the host ROCm. ROCm 6.x uses the Ubuntu 22.04 image. ROCm 7.x uses the Ubuntu 24.04 image.
- The default image is the Ubuntu 22.04 one with ROCm 6.2, pinned by digest. Build on the older Ubuntu to keep the glibc level low.
- The builder stage runs `make clean all`, then `make test`, then `check_glibc_abi.sh`, then checks that `nm -D` shows `la_version` and `la_symbind64`.
- The `output` stage holds only the two files. The default stage copies them to `/dist`.
- For development, mount the source in the builder image and run `make -f Makefile.hip clean all` inside.

### The glibc 2.34 baseline

- Workload images may carry an older glibc (Ubuntu 22.04 ROCm images use 2.35). The library must run there.
- With a new glibc, `_GNU_SOURCE` turns on C23 mode, and `strtol` and `strtoull` are renamed to `__isoc23_*`. That adds a hard `GLIBC_2.38` dependency.
- `src/include/glibc_compat.h` turns that off. It undefines both macro spellings (`__GLIBC_USE_C2X_STRTOL` and `__GLIBC_USE_C23_STRTOL`). It must be included before `<stdlib.h>`.
- `test/check_glibc_abi.sh <lib>` lists the `GLIBC_*` versions in `objdump -T` and fails when one is newer than 2.34. It checks every version, not one fixed release.

## 10. Tests

All unit tests need no GPU. They use `assert`, print a `PASS` line, and exit non-zero on failure. Run them without `-DNDEBUG`.

### Pure helper tests

Build: `gcc -Wall -Wextra -Werror -o /tmp/<name> test/<name>.c -I src/hip`

| Test | What it checks |
|---|---|
| `test_alloc_tracker` | Init, insert and lookup, remove (size and device returned), NULL pointers rejected, duplicate insert updates, free then reuse of an address, 1000 entries, device tracking, 200 colliding pointers with removal and tombstone chains, tombstone reuse, full table (insert fails at 16384, works after one removal), and a 500 allocation interleaved pattern like PyTorch. |
| `test_env_policy` | The pod `HSA_CU_MASK` wins over a changed or removed process value. A missing pod value leaves the process alone. Other tracked variables only fall back when lost. Untracked names return NULL. `AMD_TASK_PRIORITY` and `HIP_OVERSUBSCRIBE` are pinned too. |
| `test_memory_size` | `4096m`, `16G`, `512k`, `1T`, plain bytes and a trailing newline parse. `1.5G`, `-1`, `4096 M`, `4096MB`, text, empty, NULL and overflow return 0. |
| `test_queue_priority` | `0` is high. `1` and `7` are low. NULL, empty, `-1`, `high`, `1x` and a huge number ask for nothing. |
| `test_alloc_size` | Array size from channel bits (float4, 1D and 2D with zero height or depth, odd bit layouts round up), zero results for empty or negative channels, overflow gives 0, driver-API formats and channels, unknown format gives 0, extent size and overflow, and the total clamp (limit lower, no limit, limit above the GPU). |
| `test_oversubscribe` | `true`, `TRUE` and `1` are on. `false`, `0`, empty, `yes` and NULL are off. |

### Shared region tests

Build: `gcc -O2 -Wall -Wextra -Werror -pthread -o /tmp/<name> test/<name>.c src/multiprocess/hip_multiprocess_memory_limit.c`

| Test | What it checks |
|---|---|
| `test_reserve` | Eight threads each reserve 300 MiB under a 1 GiB limit. Exactly three succeed. Usage equals three chunks. After release, usage is zero. A device with no limit returns 1 (nothing reserved). |
| `test_init` | Sixteen threads call `hip_shrreg_init` together. Every caller sees the limit already loaded and the process takes exactly one slot. |
| `test_retry` | With no limit set, eight threads call init 200 times each. This must not race on the shared `/proc` buffer. The limit stays 0. |
| `test_shrreg` | A stale slot with the current pid and 512 MiB is reset. A dead lock owner is recovered once, not twice. The semaphore ends at 1. This test takes about one lock timeout (10 seconds). |

### Run them all

```
for t in test_alloc_tracker test_env_policy test_memory_size test_queue_priority test_alloc_size test_oversubscribe; do
  gcc -Wall -Wextra -Werror -o /tmp/$t test/$t.c -I src/hip && /tmp/$t
done
for t in test_reserve test_shrreg test_init test_retry; do
  gcc -O2 -pthread -o /tmp/$t test/$t.c src/multiprocess/hip_multiprocess_memory_limit.c && /tmp/$t
done
```

Note: the README and CONTRIBUTING list fewer tests than CI runs. They leave out `test_queue_priority`, `test_alloc_size` and `test_oversubscribe`. Run the full list above.

### Sanitizers

CI adds this to every test:

```
san="-Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all"
gcc $san -o /tmp/test_x test/test_x.c -I src/hip
gcc -O2 $san -pthread -o /tmp/test_reserve test/test_reserve.c src/multiprocess/hip_multiprocess_memory_limit.c
```

### ThreadSanitizer

The threaded tests run again under ThreadSanitizer. A data race in the shared region would lose or double count memory. TSan exits non-zero on any report.

```
for t in test_reserve test_init test_retry; do
  gcc -O1 -g -fsanitize=thread -pthread -o /tmp/${t}_tsan test/$t.c src/multiprocess/hip_multiprocess_memory_limit.c
  /tmp/${t}_tsan
done
```

ASLR note:

- TSan needs a fixed memory layout. On some kernels, high address randomization makes it stop with an error like "unexpected memory mapping".
- Fix it for one run: `setarch $(uname -m) -R /tmp/test_reserve_tsan`.
- Or lower the randomization for the machine: `sudo sysctl vm.mmap_rnd_bits=28`.
- Do not mix ASan and TSan in one binary.

### The glibc check

```
test/check_glibc_abi.sh dist/libamvgpu.so
```

### The on-GPU test (needs an AMD GPU and ROCm)

```
rm -f /tmp/hipdevshr.cache
LD_AUDIT=dist/libamvgpu.so HIP_DEVICE_MEMORY_LIMIT_0=1G LIBHIP_LOG_LEVEL=3 dist/test_memory_limit
```

`src/hip/test_memory_limit.c` does this:

1. Prints the device count and name, then calls `hipSetDevice(0)`.
2. Reads `hipMemGetInfo`. With a limit, the total is the limit.
3. Allocates 256 MB twice. Both must work.
4. Tries 2 GB. It expects out of memory. If it succeeds while the limit is below 2.5 GB, the test prints FAIL and returns 1.
5. Frees both, prints the final memory, and prints "All tests passed."

Other runs: PyTorch (`torch.cuda.mem_get_info()` with `HIP_DEVICE_MEMORY_LIMIT_0=4G`) and a CU slice (`HSA_CU_MASK=0:0-15`).

Remember to clear the cache file before each run.

## 11. CI jobs

| Workflow and job | What it does |
|---|---|
| `ci.yml` / `unit-tests` | Builds every unit test with `-Wall -Wextra -Werror` and ASan plus UBSan, runs them, then runs the threaded tests under TSan. |
| `ci.yml` / `build` | Matrix of ROCm 6.2 and 7.2. Reads the pinned image from `.github/rocm/`, builds `Dockerfile.hip` with it. A green build means the glibc check and the `LD_AUDIT` symbol check passed. |
| `ci.yml` / `glibc-abi-check` | Needs `build`. Builds the `output` stage with ROCm 6.2, exports `dist/`, runs `check_glibc_abi.sh` on it. |
| `codeql-analysis.yml` / `analyze` | CodeQL for C and C++, inside the pinned ROCm 6.2 image, with a manual build (`make -f Makefile.hip clean all`). Runs on push, pull request and weekly. |
| `scorecard.yaml` / `analysis` | OpenSSF Scorecard, weekly and on push. Results go to the security tab. |

Facts about the workflows:

- CI runs on pull requests, on pushes to `main` and by hand.
- Actions are pinned by commit hash. Image digests are pinned too. Dependabot updates both.
- CI does not run the on-GPU test. That is on you (section 12).

## 12. How to contribute

- The repo follows the HAMi project's Code of Conduct and Contributing guide. Read them first.
- Fork, branch, keep the change small. Unrelated cleanup goes in its own PR.
- Add a test for every change. It should fail without your fix.
- Run the unit tests (with sanitizers), the glibc check and, for C changes, a build for ROCm 6.2 and 7.2 if you can.
- If you touch `libamvgpu_audit.c` or `hip_multiprocess_memory_limit.c`, run the on-GPU test on real AMD hardware. Say so in the PR. A change that was never run on a real GPU cannot be reviewed.
- DCO: sign every commit with `git commit -s`. To fix one that is not signed: `git commit --amend --no-edit -s`, then push with `--force-with-lease`.
- Do not add AI co-author or assistance trailers to commits.
- PR template (`.github/PULL_REQUEST_TEMPLATE.md`) sections:
  - what type of PR (add a `/kind` line: bug, cleanup, deprecation, design, documentation, failing-test, feature or flake),
  - what this PR does and why,
  - which issues it fixes (`Fixes #`),
  - notes for the reviewer,
  - whether it has a user-facing change,
  - AI disclosure: say briefly if AI helped. Where the guide asks for it, one short line is enough.
- Issue templates and their labels: Bug Report (`bug`), Enhancement Request (`enhancement`), Good First Issue (`good first issue`), Question (`question`). Give the ROCm version and the GPU model in a bug.
- Security issues: do not file a public issue. Use the HAMi advisory form named in `SECURITY.md`. A workload that only exceeds its own quota is not a new vulnerability. A way to reach another tenant's GPU memory is in scope.
- A maintainer reviews and may ask for changes before merge.

## 13. Troubleshooting

| Symptom | Likely cause and fix |
|---|---|
| No limit at all in a test | The old cache file kept the old (empty) limits. Run `rm -f /tmp/hipdevshr.cache`. Also check `LD_AUDIT` is a valid path and the variable name has the device index (`HIP_DEVICE_MEMORY_LIMIT_0`). |
| The limit seems ignored | The value is invalid (for example `1.5G` or `4096MB`). Look for "Ignoring invalid" in the log. Use a whole number with `K`, `M`, `G` or `T`. |
| Need to see what happens | Set `LIBHIP_LOG_LEVEL=3` (info) or `4` (debug). Lines start with `[HAMI-core-hip`. |
| Allocation fails with out of memory below the limit | Another process of the container holds memory in the shared file. Or a dead process left a slot. Try `ACTIVE_OOM_KILLER=true`. |
| Lock timeout warnings | A process died while holding the lock. The library recovers it after one timeout (10 seconds). |
| "No available process slots" | More than 1024 processes use the file. Clear stale files. |
| "allocation tracker full" | More than 16384 live allocations in one process. |
| App fails to start on an old image | The library needs glibc 2.34 or newer in the image. |
| A program escapes the limit | It runs without `LD_AUDIT` (clean `env`, static HIP, musl). This is the known gap. |
| `HSA_CU_MASK` changes have no effect | That is the pin. The pod spec value is served. |
| Library built but `check_glibc_abi.sh` fails | A call pulled in a newer glibc symbol. Check that `glibc_compat.h` is included first and avoid new libc calls. |
| TSan stops with a memory mapping error | ASLR. Use `setarch $(uname -m) -R` or lower `vm.mmap_rnd_bits` (section 10). |
| Build cannot find ROCm | Set `ROCM_HOME`. The build stops when `/opt/rocm` is missing and `ROCM_HOME` is unset. |
| `hipMalloc` hangs or crashes at start-up with the library | Do not use `LD_PRELOAD` for this library. Use `LD_AUDIT`. Also avoid new `pthread` calls in code that runs during binding. |

## 14. Glossary

- **AMD_TASK_PRIORITY**: variable that picks the priority class of a pod's compute queues.
- **ASLR**: address space layout randomization. It places memory at random addresses.
- **audit library**: a library named in `LD_AUDIT` that the linker calls when it loads objects and binds symbols.
- **CU**: compute unit, the compute building block of an AMD GPU.
- **CU mask**: a list of the CUs a process may use. `HSA_CU_MASK=0:0-75` means GPU 0, CUs 0 to 75.
- **cookie**: a number the audit library attaches to each loaded object, used to tell who is calling.
- **dmem cgroup**: a kernel controller that caps device memory per cgroup.
- **DCO**: Developer Certificate of Origin. You agree to it with `git commit -s`.
- **fail open**: let the call through when the check cannot run.
- **gfx1200, gfx1201**: GPU architecture names (RX 9060 XT, RX 9070 XT).
- **glibc baseline**: the oldest glibc the library must run on (2.34).
- **HAMi**: the Kubernetes project for GPU sharing.
- **HIP**: AMD's GPU programming interface, like CUDA.
- **HSA**: the low-level runtime under HIP. It creates the queues.
- **hipMallocManaged**: allocation that the driver can move between VRAM and host RAM.
- **la_symbind64**: the audit function that picks the address for each bound symbol.
- **link-map namespace**: a separate set of loaded libraries inside one process. `LD_AUDIT` libraries live in their own.
- **LD_AUDIT**: environment variable that names an audit library.
- **LD_PRELOAD**: environment variable that loads a library first. Not used here.
- **oversubscription**: giving a pod a memory limit above the real VRAM.
- **PLT**: procedure linkage table, where calls to shared library functions are resolved.
- **ROCm**: AMD's GPU software stack.
- **shared region**: the mmap'd accounting file (`/tmp/hipdevshr.cache`).
- **slice**: the part of a GPU (compute units and memory) given to one pod.
- **slot**: one process's entry in the shared region.
- **TSan, ASan, UBSan**: ThreadSanitizer, AddressSanitizer and UndefinedBehaviorSanitizer.
- **VF**: virtual function, a GPU share exposed to a virtual machine.
- **VRAM**: the memory on the GPU.
- **XNACK**: a GPU feature that lets the GPU handle page faults. Needed to use managed memory beyond VRAM.
- **XGMI**: AMD's link between GPUs. This library does not use it.
