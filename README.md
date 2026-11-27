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
├── ground/                  # StrataGround GDS & I&T suite (limits, seq/table compilers, PCAP broker, test API, web console)
├── examples/                # Reference mission & host SIL targets
└── tests/                   # C++20 unit tests, deterministic rule fuzzer, Verilator HIL
```

## Quickstart (Host SIL, Verilator HIL, StrataGround & Flight Safety Audits)

```bash
cmake --preset host-debug        # Also available: host-sanitizers, host-release, host-clang
cmake --build --preset host-debug
ctest --preset host-debug        # 25 suites: unit, hypervisor, PUS-C, SECDED/TMR NvFaultLog, StrataGround GDS/I&T, fuzzer, codegen, StrictDoc, Verilator HIL, ops CLI, bare-metal cross, Versal/Kintex sims & stack/zero-heap audit
./build/host-debug/examples/versal_mission_flight_sim
./build/host-debug/examples/kintex_mission_flight_sim
```

## Core Middleware Primitives

1. **`Blackboard<T, QoS>`** — Lock-free Seqlock shared memory (HK telemetry, shared state, `TransientLocal` catch-up, `Deadline` freshness checks)
2. **`SpscChannel<T, QoS>`** — Cache-line-isolated lock-free control rings + hypervisor-rate-limited doorbells + monotonic generation counters
3. **`DataTopic<T, Pool, QoS>` & `FrameLease`** — Move-only RAII 32-byte descriptor rings for zero-copy 1-to-N CPU/PL/AIE data transfer over XMPU/SMMU pools with crash-safe lease reclamation
4. **`ParamBank<T>`** — Double-buffered, CRC-32C-verified hot-swappable binary configuration tables with atomic minor-frame commit and rollback
5. **`CriticalStore<T>` & `NvFaultLog<N>`** — Ping-pong CRC-32C warm-restart checkpoint store plus a refresh-free radiation-hardened MRAM/FRAM reboot-surviving flight log (2-of-3 TMR header + Hamming(72,64) SECDED ECC + torn-write fences + background scrubbing in `.nv_flight_log`)
6. **`SlotHandle<Spec>` & `RegMap`** — Compile-time-safe MMIO (`RO`/`WO`/`RW`/`RW1C`) and unified spatial / DFX / AIE hardware slot lifecycle (`SCSR`)

## Formal Requirements, Ground Control & Flight Safety Gates

- **StrictDoc Specifications** ([`requirements/`](requirements/README.md)): **91 / 91 requirements Implemented** (`0` orphans, `0` cycles), validated automatically by `tools/check_sdoc.py`.
- **StrataGround GDS & I&T Platform** ([`ground/`](ground/README.md)): Manifest-driven dictionary loader, Red/Yellow persistence limits, `.seq` & `ParamBank` binary compilers, UDP/PCAP/CSV telemetry broker, Python `GroundTestSession` automation API, and zero-dependency live Mission Control web console.
- **CCSDS & ECSS-E-ST-70-41C (PUS-C) Support**: Native CCSDS Space Packet, TC/TM/AOS/USLP, SDLS, CFDP, plus a lean zero-heap ECSS-E-ST-70-41C (PUS-C) adapter (`ST[01/03/05/06/08/09/11/17]`).
- **Bare-Metal Cross-Compilation Proof** ([`tools/check_baremetal_cross.py`](tools/check_baremetal_cross.py)): Assembles and links a freestanding W^X Versal Cortex-A72 EL2 ELF (`vbar_el2`, `vttbr_el2`, `eret`, `cntpct_el0`, `.critical_store`, `.nv_flight_log`, zero libc/PLT) and compiles freestanding ARMv7-R Lockstep Cortex-R5F FDIR firmware.
- **Static Stack-Usage & Zero-Heap Audit** ([`tools/check_flight_safety.py`](tools/check_flight_safety.py)): Enforces bounded per-function stack frames (`<= 4096 B`, zero unbounded dynamic stack) from GCC `.su` reports and verifies zero `malloc`/`free`/`operator new`/`__cxa_throw` references in compiled flight binaries.
- **Continuous Integration** ([`.github/workflows/ci.yml`](.github/workflows/ci.yml)): Verifies GCC Debug, ASan+UBSan, Release, Clang++ C++20, bare-metal AArch64 EL2 & Cortex-R5F cross builds, Verilator `-Wall` RTL lint + closed-loop Versal/Kintex HIL co-simulation, StrataGround GDS/I&T, and StrictDoc traceability on every push.
