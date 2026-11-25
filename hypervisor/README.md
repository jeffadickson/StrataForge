# Hypervisor (`hypervisor/`) — NASA Class B TCB

Custom bare-metal Type-1 hypervisor providing strict time, memory, interrupt, and hardware-slot isolation for STRATA-20.

## Layout
- `include/strata/hyp/abi.hpp` — Canonical 8-hypcall ABI + `SharedTimePage` contract.
- `core/` — Time-triggered minor/major frame scheduler, doorbell token-bucket rate limiter, Hubris-style generation counters, fault trap router.
- `arch/aarch64/` — Versal Cortex-A72 EL2 Stage-2 MMU, GICv3, IPI doorbells.
- `arch/arm_r5/` — Versal Lockstep Cortex-R5F MPU + PMC XilSEM / PLM bridge.
- `arch/microblaze/` — Kintex TMR MicroBlaze + external rad-hard supervisor bridge.
- `arch/host_sil/` — Deterministic in-process host SIL hypervisor mock for CI/testing.
