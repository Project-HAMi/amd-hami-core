# amd-hami-core

[![CI](https://github.com/Project-HAMi/amd-hami-core/actions/workflows/ci.yml/badge.svg)](https://github.com/Project-HAMi/amd-hami-core/actions/workflows/ci.yml)
[![OpenSSF Scorecard](https://api.securityscorecards.dev/projects/github.com/Project-HAMi/amd-hami-core/badge)](https://securityscorecards.dev/viewer/?uri=github.com/Project-HAMi/amd-hami-core)
[![License](https://img.shields.io/github/license/Project-HAMi/amd-hami-core)](LICENSE)

The `LD_AUDIT` library (`libamvgpu.so`) that [amd-device-plugin](https://github.com/Project-HAMi/amd-device-plugin) uses to limit the memory of AMD GPU pods under [HAMi](https://github.com/Project-HAMi/HAMi).

The status and the guide for this library live in the amd-device-plugin repository, in one place for both projects:

- [STATUS.md](https://github.com/Project-HAMi/amd-device-plugin/blob/main/STATUS.md): what was checked on a real GPU, the problems found and fixed, and what is left.
- [ONBOARDING.md](https://github.com/Project-HAMi/amd-device-plugin/blob/main/ONBOARDING.md): how the plugin and this library work, how to build and test, for someone new (this library is section 5).
