# StrataForge Formal Requirements (`requirements/`)

All system, hypervisor, middleware, flight service, RTL, codegen, and verification requirements are maintained as Git-native **StrictDoc (`.sdoc`)** specifications with full parent-child traceability and implementation status tracking.

## Specification Tree

| File | Prefix | Scope |
| :--- | :---: | :--- |
| `00_system_requirements.sdoc` | `STRATA-SYS-` | Top-level mission, assurance (NASA Class B–D), isolation, and silicon requirements |
| `01_hypervisor_requirements.sdoc` | `STRATA-HYP-` | Bare-metal Type-1 hypervisor 8-hypcall ABI, time/space partitioning, and fault traps |
| `02_middleware_requirements.sdoc` | `STRATA-MW-` | Header-only C++20 6-primitive SDK, QoS, FSM, hardware `SlotHandle`, and `CyclicScheduler` |
| `03_services_comms_requirements.sdoc` | `STRATA-COM-` | Total-function CCSDS SPP/TC/TM/AOS/USLP, SDLS, `ComQueue`, `ComAggregator`, `InterNodeBridge` |
| `04_services_cdh_requirements.sdoc` | `STRATA-CDH-` | 4-stage `CmdService`, `TlmPacketizer`, online stats, forwarding, `SeqEngine`, memory ops, time |
| `05_services_fdir_storage_autonomy.sdoc` | `STRATA-FDIR-` / `STOR-` / `AUTO-` | Unified `FdirEngine`, `SlotManager`, 64-bit Data Products & File/CFDP, isolated `AutonomySandbox` |
| `06_rtl_tooling_verification.sdoc` | `STRATA-RTL-` / `GEN-` / `VER-` | 4 KB SCSR RTL, Versal/Kintex shells, `strata-gen` codegen, static checks, fuzzing & traceability |
| `07_ground_control_requirements.sdoc` | `STRATA-GDS-` | StrataGround GDS & I&T platform (dictionary, limits, sequence/table compilers, PCAP broker, test API, web console) |

## Validation & Export

- **Built-in CI / CTest Graph & Syntax Check** (zero external dependencies):
  ```bash
  python3 tools/check_sdoc.py requirements/ .
  ```
  Current status: **91/91 requirements Implemented** (`0` orphans, `0` cycles), backed by 25 CTest verification suites covering unit tests, hypervisor SIL, ECSS-E-ST-70-41C (PUS-C), refresh-free TMR+SECDED `NvFaultLog`, StrataGround GDS/I&T, deterministic rule fuzzing, multi-domain `strata-gen` codegen, bare-metal AArch64 EL2 / Cortex-R5F cross proofs, and live Verilator Versal & Kintex RTL co-simulation.
- **StrictDoc HTML / ReqIF / PDF Export** (when `strictdoc` is installed):
  ```bash
  strictdoc export --formats=html,reqif-sdoc --output-dir=build/strictdoc requirements/
  ```
