# Single-Source-of-Truth Manifest Validator & Multi-Domain Code Generator

Implemented in **.NET 11 / C#** inside [`dotnet/StrataForge.Core/Codegen/StrataManifestGenerator.cs`](../../dotnet/StrataForge.Core/Codegen/StrataManifestGenerator.cs) and exposed via `strata-tools gen`:

- `strata-tools gen <manifest.toml> [--seds <seds.xml>] [--emit <out_dir>]` — Validates frame schedulability, physical DDR pool alignment & non-overlap, NoC criticality isolation (`CRITICAL_EXCL` guard), pipeline DAG acyclicity, and unique opcodes, then computes canonical 64-bit `MANIFEST_HASH`.
- **Parallel Multi-Domain Emission (`Task.WhenAll`)**:
  - C++20 ICD (`strata_mission_icd.hpp`) & Hypervisor partition tables (`strata_hyp_tables.hpp`)
  - Per-partition GNU ld linker scripts (`link_<part_id>.ld`)
  - SystemVerilog `scsr_pkg.sv` & Vivado XMPU/SMMU/NoC QoS TCL (`strata_xmpu_noc.tcl`)
  - CCSDS XTCE 1.2 XML (`mission_xtce.xml` via `System.Xml.Linq.XDocument`) & JSON ground dictionary (`mission_dict.json`)
