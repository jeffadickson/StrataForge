# Middleware SDK (`middleware/`) — Header-Only C++20 STRATA-20 Core

Zero-heap, zero-vtable, zero-exception C++20 primitives shared across Class B, C, and D partitions.

## Planned Modules (`include/strata/`)
- `core/` — `types.hpp`, `Expected<T, E>`, C++20 `concepts`, static `QoS<...>`, `Fsm<...>`.
- `ipc/` — `Blackboard<T, QoS>`, `SpscChannel<T, QoS>`, `EventRing<E, N>`, `ParamBank<T>`, `CriticalStore<T>`.
- `data/` — `FrameDesc` (32 B), move-only `FrameLease<T, Pool>`, 1-to-N zero-copy `DataTopic<T, Pool, QoS>`.
- `hw/` — Compile-time `mmio::Reg<Offset, Access, T>`, standard 4 KB `SCSR`, unified `SlotHandle<Spec>`.
- `runtime/` — Zero-thread-explosion `PartitionRuntime`, unrolled `RateGroup<Hz>`, `PhasedSchedule`, static `CommandRoutes`.
