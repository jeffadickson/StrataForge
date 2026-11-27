# StrataGround (`ground/` + `dotnet/StrataForge.Ground/`)

Zero-external-dependency **.NET 11 multithreaded** Ground Data System (GDS) and Integration & Test (I&T) suite driven directly by `strata-tools gen`'s `mission_dict.json` and 64-bit `MANIFEST_HASH`.

## Subsystems
- `dotnet/StrataForge.Ground/MissionDictionary.cs` — Lock-free `FrozenDictionary` loader for commands, telemetry calibration curves, red/yellow limits, partitions, and hardware slots.
- `dotnet/StrataForge.Ground/LimitMonitor.cs` — Thread-safe (`System.Threading.Lock`) real-time Yellow/Red limits engine with configurable persistence debouncing and alert callbacks.
- `dotnet/StrataForge.Ground/FlightCodecs.cs` — `ReadOnlySpan<byte>` CCSDS TC (CRC-16 FECF), ECSS-E-ST-70-41C (PUS-C) TC/TM codecs, relative/absolute `.seq` flight sequence compiler/decompiler (CRC-32C), `ParamBank` binary table compiler/decompiler, and `EventRecord` / `NvLogRecord` decoders.
- `dotnet/StrataForge.Ground/TelemetryBroker.cs` — Bounded `Channel<T>` multithreaded UDP/SIL stream broker with parallel workers, standard Wireshark-compatible `.pcap` capture/replay, and async `.csv` telemetry export.
- `dotnet/StrataForge.Ground/GroundTestSession.cs` — Async `PeriodicTimer` I&T automation API (`SendCommand`, `SendPusTc`, `WaitForTelemetryAsync`, `WaitForEventAsync`, `WaitForPusTmAsync`, `AssertLimitsNominal`).
- `dotnet/StrataForge.Ground/GroundWebConsole.cs` + `ground/web/index.html` — Concurrent browser-based Mission Control console and JSON REST API.
