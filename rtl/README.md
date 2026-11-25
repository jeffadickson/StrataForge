# Hardware RTL & Constraints (`rtl/`)

Standardized SystemVerilog hardware contracts and Vivado isolation/DFX constraints matching STRATA-20's C++20 software ABI.

## Layout
- `common/scsr_defs.svh` — Canonical 4 KB AXI4-Lite SCSR register map and `SlotState` encodings.
- `versal/` — Versal NoC INI DFX wrapper, XMPU/SMMU & NoC `EXCLUSIVE_ROUTING_GROUP` TCL templates, AIE GMIO/PLIO interface shells.
- `kintex/` — Kintex Abstract Shell DFX wrapper, `DFX AXI Shutdown Manager` + `DFX Decoupler`, selective-TMR static shell templates.
