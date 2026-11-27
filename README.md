# StrataForge (STRATA-20)

**Shared-FPGA Time-partitioned Real-time Avionics & Telemetry Architecture**

StrataForge is a heap-free, zero-vtable **C++20** bare-metal flight instrument middleware, custom Type-1 hypervisor, hardware SCSR contract, formal requirements suite, and single-source-of-truth codegen monorepo engineered for sharing AMD/Xilinx **Versal** (`XQRVC1902` / `XQRVE2102`) and **Kintex** (`XQRKU060` / KU+) FPGAs across mixed-criticality **NASA Class B–D** spaceflight instruments—supporting both concurrent spatial partitioning and dynamic temporal sharing (DFX / AIE partial reconfiguration).

## Repository Layout

```text
StrataForge/
├── cmake/                   # Flight C++20 safety flags & Versal/Kintex cross toolchains
├── requirements/            # Formal StrictDoc (.sdoc) system, hypervisor, middleware, services & RTL specs
├── docs/architecture/       # Historical research surveys & comparative design studies
├── manifests/               # Single-source-of-truth mission.toml schemas & examples
├── tools/                   # strata-gen static validator/codegen & StrictDoc traceability checker
├── hypervisor/              # Bare-metal Type-1 hypervisor & 8-call ABI
├── middleware/              # Header-only 6-primitive C++20 STRATA-20 SDK
├── services/                # Reusable flight services (cdh, comms, fdir_slot_mgr, storage, autonomy_sandbox)
├── rtl/                     # SystemVerilog 4 KB SCSR contract + Versal/Kintex DFX/XMPU shells
├── examples/                # Reference mission & host SIL targets
└── tests/                   # C++20 unit tests, deterministic rule fuzzer, Verilator HIL
```

## Quickstart (Host SIL, Verilator HIL, Cross-Compile & Flight Safety Audits)

```bash
cmake --preset host-debug        # Also available: host-sanitizers, host-release, host-clang
cmake --build --preset host-debug
ctest --preset host-debug        # 23 suites: unit, hypervisor, fuzzer, codegen, StrictDoc, Verilator HIL, ops CLI, bare-metal cross, Versal/Kintex sims & stack/zero-heap audit
./build/host-debug/examples/versal_mission_flight_sim
./build/host-debug/examples/kintex_mission_flight_sim
```

## Core Middleware Primitives

1. **`Blackboard<T, QoS>`** — Lock-free Seqlock shared memory (HK telemetry, shared state, `TransientLocal` catch-up, `Deadline` freshness checks)
2. **`SpscChannel<T, QoS>`** — Cache-line-isolated lock-free control rings + hypervisor-rate-limited doorbells + monotonic generation counters
3. **`DataTopic<T, Pool, QoS>` & `FrameLease`** — Move-only RAII 32-byte descriptor rings for zero-copy 1-to-N CPU/PL/AIE data transfer over XMPU/SMMU pools with crash-safe lease reclamation
4. **`ParamBank<T>`** — Double-buffered, CRC-32C-verified hot-swappable binary configuration tables with atomic minor-frame commit and rollback
5. **`CriticalStore<T>`** — Ping-pong CRC-32C persistent RAM checkpoint store surviving warm resets, SEU restarts, and spare-slot failover
6. **`SlotHandle<Spec>` & `RegMap`** — Compile-time-safe MMIO (`RO`/`WO`/`RW`/`RW1C`) and unified spatial / DFX / AIE hardware slot lifecycle (`SCSR`)

## Formal Requirements, Ground Ops & Flight Safety Gates

- **StrictDoc Specifications** ([`requirements/`](requirements/README.md)): **84 / 84 requirements Implemented** (`0` orphans, `0` cycles), validated automatically by `tools/check_sdoc.py`.
- **Bare-Metal Cross-Compilation Proof** ([`tools/check_baremetal_cross.py`](tools/check_baremetal_cross.py)): Assembles and links a freestanding W^X Versal Cortex-A72 EL2 ELF (`vbar_el2`, `vttbr_el2`, `eret`, `cntpct_el0`, `.critical_store`, zero libc/PLT) and compiles freestanding ARMv7-R Lockstep Cortex-R5F FDIR firmware.
- **Ground Mission Ops CLI** ([`tools/strata-ops/strata_ops.py`](tools/strata-ops/strata_ops.py)): Consumes `strata-gen`'s `mission_dict.json` for CCSDS TC frame encoding (CRC-16 FECF), polynomial engineering-unit calibration, and binary `EventRecord` decoding.
- **Static Stack-Usage & Zero-Heap Audit** ([`tools/check_flight_safety.py`](tools/check_flight_safety.py)): Enforces bounded per-function stack frames (`<= 4096 B`, zero unbounded dynamic stack) from GCC `.su` reports and verifies zero `malloc`/`free`/`operator new`/`__cxa_throw` references in compiled flight binaries.
- **Continuous Integration** ([`.github/workflows/ci.yml`](.github/workflows/ci.yml)): Verifies GCC Debug, ASan+UBSan, Release, Clang++ C++20, bare-metal AArch64 EL2 & Cortex-R5F cross builds, Verilator `-Wall` RTL lint + closed-loop Versal/Kintex HIL co-simulation, and StrictDoc traceability on every push.
