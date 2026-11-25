# Reference Mission Examples (`examples/`)

- `dual_instrument_sil/` — Focused host Software-in-the-Loop (SIL) reference target exercising the STRATA-20 middleware primitives (`SlotHandle`, `DataTopic`, `Blackboard`, `ParamBank`, `CriticalStore`, `CyclicScheduler`, `CommandRouter`).
- `versal_mission_flight_sim/` — Full 5-partition Versal mission integration simulator consuming build-time `strata-gen` generated headers (`strata_mission_icd.hpp`, `strata_hyp_tables.hpp`, SEDS register offsets) across the Type-1 hypervisor, CCSDS/SDLS uplink, zero-copy PL compression + CFDP downlink, Class D trap recovery, CRAM SEU DFX reload, and signed AOT autonomy sandbox. Both flight binaries are continuously audited for `< 4 KB` peak stack usage and zero heap/exception symbols.
