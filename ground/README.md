# StrataGround (`ground/`)

Zero-external-dependency Python 3.12 Ground Data System (GDS) and Integration & Test (I&T) suite driven directly by `strata-gen`'s `mission_dict.json` and 64-bit `MANIFEST_HASH`.

## Subsystems
- `strata_ground/dictionary.py` — Loads single-source-of-truth commands, telemetry calibration curves, red/yellow limits, partitions, and hardware slots.
- `strata_ground/limits.py` — Real-time Yellow/Red limits engine with configurable persistence debouncing and alert callbacks.
- `strata_ground/cmd_seq_table.py` — CCSDS TC (CRC-16 FECF), ECSS-E-ST-70-41C (PUS-C) TC/TM codecs, relative/absolute `.seq` flight sequence compiler/decompiler (CRC-32C), `ParamBank` binary table compiler/decompiler, and `EventRecord` / `NvLogRecord` decoders.
- `strata_ground/stream_server.py` — Pluggable UDP/SIL `TelemetryBroker` with handler pipeline, standard Wireshark-compatible `.pcap` binary stream capture/replay, and `.csv` telemetry export.
- `strata_ground/test_api.py` — High-level `GroundTestSession` Python automation API (`send_cmd`, `send_pus_tc`, `wait_for_tlm`, `wait_for_evr`, `wait_for_pus_tm`, `assert_limits_nominal`) for FlatSat, TVAC, and Verilator HIL procedures.
- `strata_ground/web_console.py` + `web/index.html` — Live browser-based Mission Control console and JSON REST API.
