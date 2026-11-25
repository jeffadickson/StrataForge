# StrataForge (STRATA-20)

**Shared-FPGA Time-partitioned Real-time Avionics & Telemetry Architecture**

StrataForge is a heap-free, zero-vtable **C++20** bare-metal flight instrument middleware, custom Type-1 hypervisor, hardware SCSR contract, and single-source-of-truth codegen monorepo engineered for sharing AMD/Xilinx **Versal** (`XQRVC1902` / `XQRVE2102`) and **Kintex** (`XQRKU060` / KU+) FPGAs across mixed-criticality **NASA Class B–D** spaceflight instruments—supporting both concurrent spatial partitioning and dynamic temporal sharing (DFX / AIE partial reconfiguration).

## Repository Layout & Assurance Boundaries

```text
StrataForge/
├── cmake/                   # Flight C++20 safety flags & Versal/Kintex cross toolchains
├── docs/architecture/       # 01–07 foundational research, teardowns & flagship specs
├── manifests/               # Single-source-of-truth mission.toml schemas & examples
├── tools/strata-gen/        # Manifest static checker + C++20 / SV / TCL / XTCE generator
├── hypervisor/              # Custom bare-metal Type-1 hypervisor & 8-call ABI
├── middleware/              # Header-only 6-primitive C++20 STRATA-20 SDK
├── services/                # Reusable flight services (cdh, comms, fdir_slot_mgr, storage, autonomy_sandbox)
├── rtl/                     # SystemVerilog 4 KB SCSR contract + Versal/Kintex DFX/XMPU shells
├── examples/                # Reference mission & host SIL targets
└── tests/                   # C++20 unit tests, deterministic rule fuzzer (STest parity), Verilator
```

## Quickstart (Host SIL + Manifest Validation)

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

## Architecture & Research Documentation

All foundational research, middleware teardowns, and flagship specifications live in [`docs/architecture/`](docs/architecture/):

| # | Document | Scope |
| :-: | :--- | :--- |
| **01** | [`01_fpga_os_versal_kintex_research.md`](docs/architecture/01_fpga_os_versal_kintex_research.md) | Technical survey of FPGA OSes, virtualization shells (Coyote v2, TaPaSCo, Corundum), AMD stacks (XRT/AIE, AVED, QDMA, EDF), embedded hypervisors (Xen, Jailhouse, Bao), and security defenses for Versal & Kintex. |
| **02** | [`02_fpga_os_flight_instruments_versal_kintex.md`](docs/architecture/02_fpga_os_flight_instruments_versal_kintex.md) | Aerospace flight-instrument specialization covering spaceflight payloads (`XQRVC1902`, `XQRVE2102`, `XQRKU060`), radiation hardening, and airborne avionics IMA (DO-254, DO-178C, ARINC 653, AMC 20-193). |
| **03** | [`03_nasa_class_bd_shared_fpga_blueprint.md`](docs/architecture/03_nasa_class_bd_shared_fpga_blueprint.md) | Tailored NASA Class B–D shared-FPGA architecture (NPR 7150.2D assurance containment, Versal PMC XilSEM + NoC `EXCLUSIVE_ROUTING_GROUP` + NoC INI DFX + AIE columns, Kintex SpaceCube v3.0 rad-hard monitor/scrubber + selective TMR + Abstract Shell DFX). |
| **04** | [`04_flight_instrument_framework_design.md`](docs/architecture/04_flight_instrument_framework_design.md) | Deep-dive teardown of existing flight & FPGA middlewares (NASA cFS, JPL F', OpenAMP/RPMsg, AMD Vitis Unified/XRT, ESA TASTE, CORDET-C2, CCSDS SOIS/SEDS, OpenCPI, DLR Outpost/ScOSA, Space ROS/DDS) and bare-metal hypervisor framework design. |
| **05** | [`05_strata_cpp20_flagship_middleware.md`](docs/architecture/05_strata_cpp20_flagship_middleware.md) | Flagship C++20 bare-metal middleware architecture (`STRATA-20`) systematically solving all 10 structural weaknesses of JPL F' (`concepts`, `consteval`, move-only `FrameLease`, 1-to-N zero-copy `DataTopic`, Seqlock `Blackboard`, `ParamBank`, typed `RegMap`, MISRA C++:2023 safety profile). |
| **06** | [`06_strata20_complete_fprime_parity_architecture.md`](docs/architecture/06_strata20_complete_fprime_parity_architecture.md) | Exhaustive 19-domain JPL F' v4.x feature-parity & upgrade specification (Framework, OSAL, CmdDispatch, Subtopologies, Sequencing, Wasm Autonomy Sandbox, `TlmPacketizer`, Events, Rate Groups/`ActivePhaser`, Health/WDT, 64-bit File/CFDP, CCSDS/SDLS, Data Products 2.0, `STest` parity, XTCE/GDS). |
| **07** | [`07_strata20_super_flagship_synthesis.md`](docs/architecture/07_strata20_super_flagship_synthesis.md) | Unified super-flagship synthesis merging the signature superpowers of 11 ecosystems (F' v4, cFS `CDS`/`LC`/`MD`/`MM`/`CS`, ESA PUS-C `ST[01]`/`ST[04]`/`ST[12]`/`ST[14]`/`ST[19]`, ARINC 653, DDS static QoS, Hubris, seL4 Microkit, `iceoryx2`, DLR ScOSA, OpenCPI/Coyote v2, CCSDS SEDS/XTCE) into 6 orthogonal primitives inside a ~16–22.5 KSLOC Class B TCB. |

## Core Primitives

1. **`Blackboard<T, QoS>`** — Lock-free Seqlock shared memory (HK telemetry, shared state, `TransientLocal` catch-up, `Deadline` checks)
2. **`SpscChannel<T, QoS>`** — Cache-line-isolated lock-free control rings + hypervisor-rate-limited doorbells + Hubris-style generation counters
3. **`DataTopic<T, Pool, QoS>` & `FrameLease`** — Move-only RAII 32-byte descriptor rings for true zero-copy 1-to-N CPU/PL/AIE data transfer over XMPU/SMMU pools
4. **`ParamBank<T>`** — Double-buffered, CRC-32C-verified hot-swappable binary configuration tables with atomic minor-frame commit
5. **`CriticalStore<T>`** — ECC/CRC-protected persistent RAM checkpoint store surviving warm resets, SEU restarts, and cross-slot failover
6. **`SlotHandle<Spec>` & `RegMap`** — Compile-time-safe MMIO and unified spatial / DFX / AIE hardware slot lifecycle (`SCSR`)
