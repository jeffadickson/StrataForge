# Verification & Testing (`tests/`)

All C++20 test binaries compile with `-std=c++20 -fno-exceptions -fno-rtti -Werror -fstack-usage` and are verified across `host-debug`, `host-sanitizers` (ASan + UBSan), and `host-release` (`-O3`).

## Implemented Test Suites (12 CTest Targets)
- `unit/`
  - `test_abi_and_types.cpp` — 8-hypcall ABI, `SharedTimePage`, 32 B `FrameDesc`, 128 B cache-line-isolated `SpscHeader`.
  - `test_core_and_fsm.cpp` — `Expected<T, E>`, compile-time Castagnoli CRC-32C check vector (`0xE3069283`), `QoS`, zero-alloc `Fsm`.
  - `test_ipc_primitives.cpp` — `Blackboard` (`Deadline` + `TransientLocal`), `SpscChannel` (generation checks + coalesced doorbells), `EventWriter` (burst dedup + severity mask), `ParamBank`, `CriticalStore` (mid-write SEU fallback).
  - `test_zero_copy_data_plane.cpp` — Move-only `FrameLease` auto-return, pointer-identity zero-copy 1-to-N `DataTopic`, Class D `DropOldest` non-blocking, lease-timeout reclamation.
  - `test_hw_and_runtime.cpp` — Typed `mmio::Reg` (`RO`/`WO`/`RW`/`RW1C`), 4 KB SCSR `SlotHandle`, `CyclicScheduler`, `CommandRouter`.
  - `test_comms_ccsds_sdls.cpp` — Total-function CCSDS SPP/TC parsers (`CVE-2026-92151` regression), CRC-16-CCITT FECF, SDLS anti-replay/tamper, priority `ComQueue`, idle-filling `ComAggregator`.
  - `test_cdh_services.cpp` — 4-stage PUS `ST[01]` `CmdService` + RBAC, `consteval` `TlmPacketizer` (`CVE-2026-92159` immune), Welford `ST[04]` `StatsCollector`, `ST[14]` `ForwardingControl`, `SeqEngine`, cFS `MD`/`MM`/`CS`.
  - `test_fdir_and_scosa.cpp` — Debounced Watchpoints, Actionpoints, ARINC 653 escalation, Versal XilSEM / Kintex CRAM frame-to-slot attribution, DLR ScOSA spare-slot failover.
  - `test_storage_and_autonomy.cpp` — 64-bit priority `DpCatalog` eviction, partition-jailed `FileService` + DFX staging auth, 64-bit CFDP Class 2 NAK gaps, `AutonomySandbox` budget preemption + opcode allowlist.
- `rule_fuzzer/`
  - `test_rule_fuzzer.cpp` — Deterministic seeded 25,000-step stateful rule fuzzer (F' `STest` parity) proving 0 DDR leaks and 0 crashes under random stalls, crashes, CCSDS garbage, and SEU bit-flips.
- `verilator_hil/` — Reserved for live SystemVerilog `SCSR` + DMA RTL co-simulation.
