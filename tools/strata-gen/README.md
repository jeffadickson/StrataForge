# `strata-gen` (`tools/strata-gen/`)

Single-source-of-truth manifest validator and code generator driven by `mission.toml` (plus optional CCSDS SEDS device datasheets).

## Current Capabilities
- `strata_gen.py <manifest.toml>` — Static validation of frame schedulability, physical DDR pool alignment & non-overlap, NoC criticality isolation (`CRITICAL_EXCL` guard), and canonical 64-bit `MANIFEST_HASH` generation.

## Planned Emitters
- `emit_cpp.py` — C++20 ICD headers, `mmio::Reg` banks, `consteval` ROM routing tables, linker scripts.
- `emit_rtl.py` — SystemVerilog `scsr_pkg.sv` + Vivado XMPU/NoC QoS TCL.
- `emit_ground.py` — Standard CCSDS XTCE and JSON ground telemetry/command dictionaries.
