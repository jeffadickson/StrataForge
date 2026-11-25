# Flight Services (`services/`) — Strict Class B vs. Class C Separation

Reusable mission services achieving 100% JPL F' v4.x parity + cFS + ECSS PUS-C + DLR ScOSA operational capabilities.

## Class B TCB (`services/class_b/`)
- `cdh/` — `CmdRouter` (4-stage PUS `ST[01]`), `consteval` `TlmPacketizer` (`ST[04]` stats, `ST[14]` forwarding), `EventMgr`, `SeqEngine`, `MemDwell` (`MD`), `MemPatch` (`MM`), `SwChecksum` (`CS`), `SysMonitor`, `BuildManifest`.
- `comms/` — Total-function CCSDS SPP / TC / TM / AOS / USLP parsers & framers (`Expected<View, WireErr>`), `SdlsEngine` (AES-GCM w/ HW offload hook), zero-copy `ComPipeline`.
- `fdir_slot_mgr/` — Unified `FdirEngine` (cFS `LC` + PUS `ST[12]`/`ST[19]` + ARINC 653 HM + ScOSA failover), XilSEM/Scrubber upset correlation, authenticated DFX/AIE `SlotManager`.

## Class C Isolated Services (`services/class_c/`)
- `storage/` — Zero-copy `DpEngine` + `DpCatalog` (optional PL compression offload), 64-bit `FileService`, `CfdpEngine` (Class 1 & 2).
- `autonomy_sandbox/` — Hardware-sandboxed Wasm-AOT / bounded script runner replacing in-process `WasmSequencer`.
