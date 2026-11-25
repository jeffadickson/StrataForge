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

## Quickstart (Host SIL, Verilator Co-Sim & Flight Safety Audits)

```bash
cmake --preset host-debug        # Also available: host-sanitizers, host-release, host-clang
cmake --build --preset host-debug
ctest --preset host-debug        # 19 suites: unit, hypervisor, fuzzer, codegen, StrictDoc, Verilator HIL, mission sim & stack/zero-heap audit
./build/host-debug/examples/versal_mission_flight_sim
```

## Core Middleware Primitives

1. **`Blackboard<T, QoS>`** — Lock-free Seqlock shared memory (HK telemetry, shared state, `TransientLocal` catch-up, `Deadline` freshness checks)
2. **`SpscChannel<T, QoS>`** — Cache-line-isolated lock-free control rings + hypervisor-rate-limited doorbells + monotonic generation counters
3. **`DataTopic<T, Pool, QoS>` & `FrameLease`** — Move-only RAII 32-byte descriptor rings for zero-copy 1-to-N CPU/PL/AIE data transfer over XMPU/SMMU pools with crash-safe lease reclamation
4. **`ParamBank<T>`** — Double-buffered, CRC-32C-verified hot-swappable binary configuration tables with atomic minor-frame commit and rollback
5. **`CriticalStore<T>`** — Ping-pong CRC-32C persistent RAM checkpoint store surviving warm resets, SEU restarts, and spare-slot failover
6. **`SlotHandle<Spec>` & `RegMap`** — Compile-time-safe MMIO (`RO`/`WO`/`RW`/`RW1C`) and unified spatial / DFX / AIE hardware slot lifecycle (`SCSR`)

## Formal Requirements & Flight Safety Gates

- **StrictDoc Specifications** ([`requirements/`](requirements/README.md)): **84 / 84 requirements Implemented** (`0` orphans, `0` cycles), validated automatically by `tools/check_sdoc.py`.
- **Static Stack-Usage & Zero-Heap Audit** ([`tools/check_flight_safety.py`](tools/check_flight_safety.py)): Enforces bounded per-function stack frames (`<= 4096 B`, zero unbounded dynamic stack) from GCC `.su` reports and verifies zero `malloc`/`free`/`operator new`/`__cxa_throw` references in compiled flight binaries.
- **Continuous Integration** ([`.github/workflows/ci.yml`](.github/workflows/ci.yml)): Verifies GCC Debug, ASan+UBSan, Release, Clang++ C++20, Verilator `-Wall` RTL lint + Versal/Kintex co-simulation, and requirements traceability on every push.
