# Flight Services (`services/`)

Reusable zero-heap C++20 mission services achieving 100% JPL F' v4.x parity plus cFS, ECSS PUS-C, ARINC 653, and DLR ScOSA operational capabilities. Partition criticality (NASA Class B, C, or D) is configured per mission in `mission.toml` rather than hardcoded into the directory layout.

## Modules
- `cdh/` — `CmdRouter` (4-stage PUS `ST[01]`), `consteval` `TlmPacketizer` (`ST[04]` stats, `ST[14]` forwarding), `EventMgr`, `SeqEngine`, `MemDwell` (`MD`), `MemPatch` (`MM`), `SwChecksum` (`CS`), `SysMonitor`, `BuildManifest`.
- `comms/` — Total-function CCSDS SPP / TC / TM / AOS / USLP parsers & framers (`Expected<View, WireErr>`), `SdlsEngine` (AES-GCM w/ HW offload hook), zero-copy `ComPipeline`.
- `fdir_slot_mgr/` — Unified `FdirEngine` (cFS `LC` + PUS `ST[12]`/`ST[19]` + ARINC 653 HM + ScOSA failover), XilSEM/Scrubber upset correlation, authenticated DFX/AIE `SlotManager`.
- `storage/` — Zero-copy `DpEngine` + `DpCatalog` (optional PL compression offload), 64-bit `FileService`, `CfdpEngine` (Class 1 & 2).
- `autonomy_sandbox/` — Hardware-sandboxed Wasm-AOT / bounded script runner replacing in-process `WasmSequencer`.
