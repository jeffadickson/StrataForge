# Middleware SDK (`middleware/`) — Header-Only C++20 STRATA-20 Core

Zero-heap, zero-vtable, zero-exception C++20 primitives (`-std=c++20 -fno-exceptions -fno-rtti -Werror -fstack-usage`) shared across all partitions.

## Implemented Headers (`include/strata/`)
- `middleware.hpp` — Single umbrella include for the entire STRATA-20 SDK.
- `core/`
  - `types.hpp` — `TaiTimestamp`, `DurationNs` + `_ns`/`_us`/`_ms`/`_s` literals, `GenId`, `SlotState`, `SlotCmd`, `EventSeverity`, `CmdStage`, `CmdResult`, `Status`, 32 B `FrameDesc`, 128 B `SpscHeader`.
  - `expected.hpp` — Zero-exception, zero-heap `Expected<T, E>` (including `Expected<void, E>`).
  - `crc32c.hpp` — Compile-time + runtime Castagnoli CRC-32C (`0x82F63B78`).
  - `qos.hpp` — Compile-time static `QoSConfig` (`Durability`, `Reliability`, `deadline`, `liveliness`, `lease_timeout`, `history_depth`).
  - `concepts.hpp` — `WirePod`, `PowerOfTwo`, `DoorbellNotifier`, `TimeSource`, `RegisterTransport`, `InstrumentComponent`.
  - `fsm.hpp` — Zero-allocation table-driven `Fsm<Context, StateEnum, EventEnum, N>`.
- `ipc/`
  - `blackboard.hpp` — Lock-free Seqlock `BlackboardStorage<T>`, `BlackboardWriter`, `BlackboardReader` (with `TransientLocal` & `Deadline` checks).
  - `spsc_channel.hpp` — Cache-line-isolated lock-free `SpscRingStorage`, `SpscProducer`, `SpscConsumer` (generation counters, coalesced doorbells, bounded `drain_up_to`).
  - `event_ring.hpp` — Bounded `EventWriter` with severity filtering, burst deduplication, and cooldown rate-capping.
  - `param_bank.hpp` — Double-buffered CRC-32C `ParamBank<ParamT>` with staged validation, atomic commit, rollback, and SEU verification.
  - `critical_store.hpp` — Ping-pong crash-atomic CRC-32C `CriticalStore<StateT>` surviving warm restarts and spare-slot failover.
- `data/`
  - `frame_lease.hpp` — Atomic subscriber-bitmask `FramePool`, move-only RAII `FrameLease`, and move-only `ReadOnlyFrameView` with crash-safe lease reclamation.
  - `data_topic.hpp` — True zero-copy 1-to-N `DataTopic` with mixed-criticality `Guaranteed` vs. `BestEffortDropOldest` QoS.
- `hw/`
  - `reg_map.hpp` — Compile-time `RO`/`WO`/`RW`/`RW1C` `mmio::Reg`, `DirectMmioTransport`, and `MockRegisterBank`.
  - `slot_handle.hpp` — Standard 4 KB `SCSR` contract and unified spatial/DFX/AIE `SlotHandle<Transport>`.
- `runtime/`
  - `partition_runtime.hpp` — Zero-thread `CyclicScheduler` (periodic & phased windows) and compile-time unique-opcode `CommandRouter`.
