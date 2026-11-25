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

## Quickstart (Host SIL + Requirements & Manifest Validation)

```bash
cmake --preset host-debug
cmake --build --preset host-debug
ctest --preset host-debug
./build/host-debug/examples/dual_instrument_sil
```

For AddressSanitizer + UndefinedBehaviorSanitizer checks:

```bash
cmake --preset host-sanitizers
cmake --build --preset host-sanitizers
ctest --preset host-sanitizers
```

## Core Middleware Primitives

1. **`Blackboard<T, QoS>`** — Lock-free Seqlock shared memory (HK telemetry, shared state, `TransientLocal` catch-up, `Deadline` freshness checks)
2. **`SpscChannel<T, QoS>`** — Cache-line-isolated lock-free control rings + hypervisor-rate-limited doorbells + monotonic generation counters
3. **`DataTopic<T, Pool, QoS>` & `FrameLease`** — Move-only RAII 32-byte descriptor rings for zero-copy 1-to-N CPU/PL/AIE data transfer over XMPU/SMMU pools with crash-safe lease reclamation
4. **`ParamBank<T>`** — Double-buffered, CRC-32C-verified hot-swappable binary configuration tables with atomic minor-frame commit and rollback
5. **`CriticalStore<T>`** — Ping-pong CRC-32C persistent RAM checkpoint store surviving warm resets, SEU restarts, and spare-slot failover
6. **`SlotHandle<Spec>` & `RegMap`** — Compile-time-safe MMIO (`RO`/`WO`/`RW`/`RW1C`) and unified spatial / DFX / AIE hardware slot lifecycle (`SCSR`)

## Formal Requirements (`requirements/`)

All system, hypervisor, middleware, service, RTL, codegen, and verification requirements are authored in **StrictDoc** (`.sdoc`) under [`requirements/`](requirements/README.md) with full parent-child traceability verified automatically by `ctest`.
