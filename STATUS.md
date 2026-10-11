# Status

What was checked, what was found and what is left, in plain words. Read [ONBOARDING.md](ONBOARDING.md) to learn how the library works.

## In one minute

- We re-checked **every merged pull request** of this repository (19) on a real AMD GPU, together with the [amd-device-plugin](https://github.com/Project-HAMi/amd-device-plugin) pull requests (131) that load this library.
- Each pull request has one line below: the result and the one thing still not verified.
- Found here: no issue or pull request templates (added in #26) and README and CONTRIBUTING that lagged behind the tests (updated in #27). The plugin side found six bugs, listed in its [STATUS.md](https://github.com/Project-HAMi/amd-device-plugin/blob/main/STATUS.md).
- What is left: a few checks that need hardware we did not have.

## How we tested

- A real AMD Instinct MI325X (one GPU, shown to the machine as a virtual function) with k3s, the HAMi scheduler and this plugin built from `main`.
- First kernel 6.8, then kernel 7.0 (the kernel dmem memory cap needs 6.14 or newer).
- Tests that need several GPUs, a real compute-partition flip, RDNA cards or managed-memory spill could not run on this machine (see "What we could not check").

## What the library was measured to do

- In a 25 percent slice, a compute kernel ran at 215 kernels/s. Setting `HSA_CU_MASK` from inside the process did not widen it with the library (215), and did without it (1015).
- Two processes on one slice got 144.7 and 72.3 kernels/s with `AMD_TASK_PRIORITY` 0 and 3, against about 108.6 each with no priority.
- With a 1 GiB limit, `hipMalloc3D`, arrays, pools and `hipMemCreate` all returned out of memory, and `hipMemGetInfo` reported 1 GiB.
- Three processes each asking for 1.5 GiB under a 4 GiB limit: exactly two succeeded, in each of three rounds.
- The C tests pass under AddressSanitizer, UBSan and ThreadSanitizer, and the glibc 2.34 check passes.

## What we could not check

- A `hipMalloc` larger than the VRAM, served from host memory. It needs XNACK, which is off on the virtual function.
- Several GPUs at once and RDNA cards.

## Every pull request of this repository

| PR | Change | Result | Still open |
|---|---|---|---|
| #2 | docs: add AMD HAMi core contribution templates | Closed, not merged. Templates later added in #26. | Nothing |
| #7 | docs: clarify libamvgpu.so does memory limiting, not CU masking | README checked on main. A 25 percent slice ran at the same speed with and without the hook. | Nothing |
| #8 | fix: flag src/multiprocess/*.c NVIDIA leftovers as unbuilt, not wired in | Superseded. Files were removed in #20. Main builds without them. | Compute limiting on AMD was never ported or tested. |
| #9 | fix: restore glibc 2.34 ABI baseline, add a regression check | ABI check passes on the shipped library. Hook loaded on glibc 2.34 images. Musl does not load it. | Other build hosts and the musl case were not tested. |
| #11 | docs: add LICENSE and CONTRIBUTING.md | Both files exist on main. | Build commands in CONTRIBUTING.md were not run as written. |
| #12 | fix: restore_container_env was a no-op across the LD_AUDIT namespace boundary | In a sliced Pod the runtime sees the pod mask and device list, also in child processes. | A real vLLM or SGLang engine was not run. |
| #13 | fix: pin HSA_CU_MASK to the pod spec value | Escape test on a 25 percent slice: setenv escape was ignored with the hook, and gave the whole GPU without it. | Child processes without LD_AUDIT still escape the slice. |
| #14 | fix: make memory accounting exact under concurrency | All C tests pass under ASan, UBSan and TSan. Three processes under a 4096 MiB limit: two succeeded, one got out of memory. | Real frameworks and shared regions across many containers were not run. |
| #15 | docs: add security-insights.yml and SECURITY.md | Both files exist on main. | YAML was not checked against the schema and links were not tested. |
| #16 | ci: add CI, CodeQL and Scorecard workflows; add CODE_OF_CONDUCT.md | Workflow files and code of conduct exist. Last three runs of each workflow succeeded. | CI never runs on a GPU. |
| #17 | ci: pin ROCm base images by digest, add Dependabot config | Base images are pinned by digest and all actions use commit SHAs. Dependabot config exists. | Dependabot has not opened a bump PR yet. |
| #18 | fix: correct CMake build comment, use real glibc baseline check in Docker, sync security-insights.yml tools | Docker build of main succeeds. The glibc baseline check passes on the built library. | The log level fallback and fork behavior have no test. |
| #19 | chore: ignore all five test binaries, match CU mask example to README | .gitignore lists test binaries. README mask example matches the format the plugin sets. | Nothing |
| #20 | fix: publish the shared region with acquire/release, drop dead sources | test_init passes under ASan, UBSan and TSan. Dead sources are gone and the Docker build works. | Arm64 and the environ load path have no test. |
| #21 | fix: read the limit retry environment under the lock | test_retry passes under ASan, UBSan and TSan. | The case where the limit appears late in a child is not covered. |
| #22 | ci: keep the pinned ROCm images where Dependabot can update them | Pinned images live in .github/rocm files and ci.yml reads them. CI on main succeeds. | No Dependabot bump PR has been seen yet. |
| #23 | feat: set the HSA queue priority from AMD_TASK_PRIORITY | Two processes on one 25 percent slice: high priority 144.7 kernels/s, low 72.3. Without priority both about 108. | Queue wrapper and dlmopen lookup have no unit test. |
| #24 | feat: cap hipMalloc3D, arrays, pools and VMM, and report the limit as the total memory | With a 1024 MiB limit the reported total is 1024 MiB. Five allocator types return out of memory at 2 GiB. | Hard coded property offsets are untested across ROCm versions. |
| #25 | feat: serve hipMalloc from managed memory when HIP_OVERSUBSCRIBE is set | Unit test passes under ASan and UBSan. A 100 GB hipMalloc with memset worked. | Spill to host memory was not verified because XNACK is off. |
