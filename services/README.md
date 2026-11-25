# Flight Services (`services/`)

Zero-heap, zero-vtable, zero-exception C++20 mission services built on the STRATA-20 middleware SDK. Partition criticality (NASA Class B, C, or D) is configured per mission in `mission.toml`.

## Implemented Modules
- **`comms/`** (`include/strata/services/comms/`)
  - `ccsds.hpp` — Total-function CCSDS Space Packet & TC Transfer Frame parsers/framers (zero asserts on wire input), CRC-16-CCITT FECF, pluggable `SdlsEngine` with 64-frame sliding anti-replay window.
  - `com_pipeline.hpp` — Priority-tiered `ComQueue` and idle-packet-filling `ComAggregator`.
- **`cdh/`** (`include/strata/services/cdh/`)
  - `cmd_service.hpp` — 4-stage command verification `CmdService` with partition RBAC & SDLS enforcement.
  - `telemetry.hpp` — Compile-time-safe `TlmPacketizer`, Welford online `StatsCollector`, and dynamic `ForwardingControl`.
  - `sequencer.hpp` — Multi-slot CRC-32C `SeqEngine` (absolute/relative TAI steps + safe-mode abort).
  - `flight_ops.hpp` — Allowlisted Memory Dwell, two-step authenticated Memory Patch, background `SwChecksum` scrubbing, and `BuildManifestInfo`.
- **`fdir_slot_mgr/`** (`include/strata/services/fdir/`)
  - `fdir_engine.hpp` — Unified `FdirEngine`: debounced Watchpoints, Actionpoints, multi-level recovery escalation, Versal XilSEM / Kintex Scrubber CRAM frame-to-slot attribution, and deterministic spare-slot failover.
- **`storage/`** (`include/strata/services/storage/`)
  - `data_products.hpp` — Zero-copy 64 B `DpHeader` and 64-bit priority `DpCatalog` with high-water lower-priority eviction and resume offsets.
  - `file_and_cfdp.hpp` — 64-bit partition-jailed `FileService`, authenticated DFX/PDI staging, and 64-bit CCSDS CFDP Class 1/2 missing-extent NAK tracker.
- **`autonomy_sandbox/`** (`include/strata/services/autonomy/`)
  - `sandbox_runner.hpp` — Isolated `AutonomySandbox` with hard step-budget preemption, read-only telemetry inputs, and strict command opcode allowlist gating.
