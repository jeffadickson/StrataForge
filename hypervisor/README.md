# Hypervisor (`hypervisor/`)

Custom bare-metal Type-1 hypervisor providing strict time, memory, interrupt, and hardware-slot isolation for StrataForge.

## Layout
- `include/strata/hyp/abi.hpp` — Canonical 8-hypcall ABI + `SharedTimePage` contract.
- `core/` — Time-triggered minor/major frame scheduler, doorbell token-bucket rate limiter, monotonic generation counters, and fault trap router.
- `arch/aarch64/` — Versal Cortex-A72 EL2 Stage-2 MMU, GICv3, and IPI doorbells.
- `arch/arm_r5/` — Versal Lockstep Cortex-R5F MPU and PMC XilSEM / PLM bridge.
- `arch/microblaze/` — Kintex TMR MicroBlaze and external rad-hard supervisor bridge.
- `arch/host_sil/` — Deterministic in-process host SIL hypervisor backend for CI and unit testing.
