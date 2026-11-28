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
├── dotnet/                  # .NET 11 multithreaded C# utilities (strata-tools, StrataGround GDS, XDocument XTCE/SEDS, XML docs)
├── hypervisor/              # Bare-metal Type-1 hypervisor & 8-call ABI
├── middleware/              # Header-only 6-primitive C++20 STRATA-20 SDK + zero-heap SHA-256/AES-256-GCM/Ed25519 crypto
├── services/                # Reusable flight services (cdh, comms, fdir_slot_mgr, storage, autonomy_sandbox)
├── instruments/             # Reusable zero-heap CRTP Mock Flight Instrument Framework & reference payloads
├── rtl/                     # SystemVerilog 4 KB SCSR contract + Versal/Kintex DFX/XMPU shells
├── ground/                  # StrataGround live Mission Control web UI (`web/index.html`, served by .NET 11 StrataForge.Ground)
├── examples/                # Reference mission, host SIL & live UDP flight-to-ground bridge targets
└── tests/                   # C++20 unit tests, deterministic rule fuzzer, Verilator HIL
```

## Quickstart (Host SIL, .NET 11 Tooling, Verilator HIL, QEMU EL2 & Flight Safety Audits)

```bash
cmake --preset host-debug        # Also available: host-sanitizers, host-release, host-clang
cmake --build --preset host-debug
ctest --preset host-debug        # 29 suites: unit, crypto (SHA-256/AES-256-GCM/Ed25519), mock instruments, hypervisor, PUS-C, NvFaultLog, .NET 11 StrataGround + live UDP loop, fuzzer, parallel codegen, StrictDoc, Verilator HIL, QEMU AArch64 EL2, Versal/Kintex sims & stack/zero-heap audit
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

## Formal Requirements, .NET 11 Utilities, Mock Instruments & Flight Safety Gates

- **StrictDoc Specifications** ([`requirements/`](requirements/README.md)): **103 / 103 requirements Implemented** (`0` orphans, `0` cycles), validated automatically in parallel by [`StrictDocVerifier.cs`](dotnet/StrataForge.Core/Requirements/StrictDocVerifier.cs) (`strata-tools check-sdoc`).
- **.NET 11 Multithreaded Tooling & StrataGround** ([`dotnet/`](dotnet/README.md), [`ground/`](ground/README.md)): Zero-external-package C# / `.NET 11` solution (`TreatWarningsAsErrors` + mandatory XML documentation files) delivering parallel 7-domain codegen (`XDocument` SEDS/XTCE), `FrozenDictionary` mission loader, `Lock`-synchronized Red/Yellow limits, `.seq`/`ParamBank` compilers, bounded `Channel<T>` UDP/PCAP/CSV telemetry broker, async `GroundTestSession`, and concurrent Mission Control web console.
- **Mock Flight Instrument Framework** ([`instruments/`](instruments/README.md)): Zero-heap CRTP `MockInstrumentBase`, 1st-order thermal/power physics ODE, SCSR `RegisterTransport` MMIO bank, deterministic `FaultInjector`, zero-copy `FramePool` science frame producer, plus `HyperspectralImagerMock` and `FluxgateMagnetometerMock`.
- **Hardware-Grade Zero-Heap Cryptography**: Freestanding FIPS 180-4 `Sha256`/`Sha512`, RFC 2104 `HmacSha256`, NIST SP 800-38D `Aes256Gcm` (`Aes256GcmSdlsBackend`), and RFC 8032 `Ed25519` (`Ed25519SlotImageAuth`) verified against official NIST/RFC test vectors.
- **CCSDS & ECSS-E-ST-70-41C (PUS-C) Support**: Native CCSDS Space Packet, TC/TM/AOS/USLP, SDLS (AES-256-GCM), CFDP, plus a lean zero-heap ECSS-E-ST-70-41C (PUS-C) adapter (`ST[01/03/05/06/08/09/11/17]`).
- **Bare-Metal Cross-Compilation & Live QEMU AArch64 EL2 Execution** ([`BareMetalCrossVerifier.cs`](dotnet/StrataForge.Core/Audit/BareMetalCrossVerifier.cs)): Concurrently links freestanding W^X Versal Cortex-A72 EL2 ELF and Lockstep Cortex-R5F firmware, and boots the EL2 hypervisor live on `qemu-system-aarch64` Cortex-A72 proving `CurrentEL=2`, Stage-2 MMU Data Abort trapping, EL1 `HVC #0`, GIC SGI wake-up, `NvFaultLog` SECDED warm-reboot survival, and bare-metal `Sha256`.
- **Static Stack-Usage & Zero-Heap Audit** ([`FlightSafetyAuditor.cs`](dotnet/StrataForge.Core/Audit/FlightSafetyAuditor.cs)): Enforces bounded per-function stack frames (`<= 4096 B`, zero unbounded dynamic stack) across GCC `.su` reports in parallel and verifies zero `malloc`/`free`/`operator new`/`__cxa_throw` references across all flight binaries.
- **Continuous Integration** ([`.github/workflows/ci.yml`](.github/workflows/ci.yml)): Verifies GCC Debug, ASan+UBSan, Release, Clang++ C++20, .NET 11 `strata-tools` (with CS1591 XML-doc checks), bare-metal AArch64 EL2 & Cortex-R5F cross builds, live QEMU Cortex-A72 EL2 execution, Verilator `-Wall` RTL lint + closed-loop Versal/Kintex HIL co-simulation, StrataGround GDS + live UDP loop, and StrictDoc traceability on every push.
