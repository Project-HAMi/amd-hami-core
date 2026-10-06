# Security Policy

## Reporting a Vulnerability

Report vulnerabilities privately through the HAMi advisory form, not as a
public issue: https://github.com/Project-HAMi/HAMi/security/advisories/new

### Information to Include
- A clear and concise description of the vulnerability.
- Steps to reproduce the issue.
- Any potential attack scenarios or security impact.
- Suggested mitigations or fixes, if available.

## Is It In Scope?

amd-hami-core's `libamvgpu.so` enforces per-pod AMD GPU memory limits via
an `LD_AUDIT` hook over HIP memory calls, for cooperative multi-tenant
sharing on a trusted cluster. It is not a hard security boundary against a
workload with enough privilege to bypass its own hook, for example by
unsetting `LD_AUDIT`, linking the HIP runtime statically, or calling
`ptrace` on its own process.

- A report that a workload can exceed its own memory quota, without
  affecting another tenant, is not a new vulnerability by itself.
- A report that lets a workload reach another tenant's GPU memory, device,
  or namespace it was not granted is in scope, please report it through
  the process above.

## Response Process

Response times could be affected by weekends, holidays, or time zone
differences. That said, the maintainers will endeavour to reply as soon as
possible, ideally within 5 working days.

## Third-Party Dependencies

amd-hami-core links against the ROCm/HIP runtime. We monitor upstream
ROCm releases and update the supported version range when security fixes
are published there.
