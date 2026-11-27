# StrataForge .NET 11 Multithreaded Utilities (`dotnet/`)

Zero-external-NuGet-dependency **C# / .NET 11 (`net11.0`)** toolchain replacing all host-side code generation, Ground Data System (GDS), I&T automation, StrictDoc traceability, static flight safety auditing, bare-metal cross verification, and live QEMU AArch64 EL2 execution utilities.

## Solution Layout (`StrataForge.slnx`)
- **`StrataForge.Core`** (`StrataForge.Core.xml` XML API docs):
  - `Toml/MissionTomlParser.cs` — Zero-dependency RFC-compliant TOML manifest parser.
  - `Codegen/StrataManifestGenerator.cs` — Static schedulability/DDR/NoC/DAG validator, `XDocument` (`System.Xml.Linq`) CCSDS SEDS XML import & XTCE 1.2 XML generator, and parallel (`Task.WhenAll` / `Parallel.ForEachAsync`) 7-domain code emitter.
  - `Requirements/StrictDocVerifier.cs` — Parallel `.sdoc` parser, traceability graph auditor, and cycle detector.
  - `Audit/FlightSafetyAuditor.cs` — Parallel GCC `.su` stack-usage (`<= 4096 B`) and concurrent `nm -u` zero-heap ELF symbol auditor.
  - `Audit/BareMetalCrossVerifier.cs` — Concurrent Versal AArch64 EL2 & Lockstep Cortex-R5F cross-compilation verifier plus live QEMU Cortex-A72 EL2 execution runner.
- **`StrataForge.Ground`** (`StrataForge.Ground.xml` XML API docs):
  - `MissionDictionary.cs` — Lock-free `FrozenDictionary`-backed single-source-of-truth command/telemetry dictionary.
  - `LimitMonitor.cs` — Thread-safe (`System.Threading.Lock`) Red/Yellow persistence-debounced limit engine.
  - `FlightCodecs.cs` — `ReadOnlySpan<byte>` CCSDS TC/CRC-16, PUS-C TC/TM, `EventRecord`, `NvLogRecord`, `.seq` `SequenceCompiler`, and `ParamBank` `ParamTableCompiler`.
  - `TelemetryBroker.cs` — Bounded `Channel<T>` multithreaded UDP/SIL stream broker, Wireshark `PcapWriter`/`PcapReader`, and async CSV exporter.
  - `GroundTestSession.cs` — Async `PeriodicTimer` I&T automation API (`SendCommand`, `SendPusTc`, `WaitForTelemetryAsync`, `WaitForEventAsync`, `WaitForPusTmAsync`, `AssertLimitsNominal`).
  - `GroundWebConsole.cs` — Concurrent `HttpListener` Mission Control server serving `ground/web/index.html` and JSON REST APIs.
- **`StrataForge.Cli`** (`strata-tools` + `strata-tools.xml` XML API docs):
  - Unified CLI (`gen`, `ops`, `check-sdoc`, `check-safety`, `check-cross`, `check-qemu-el2`, `web-console`, `test`).

## Build & Strict Documentation Enforcement
`dotnet/Directory.Build.props` enforces `<TargetFramework>net11.0</TargetFramework>`, `<TreatWarningsAsErrors>true</TreatWarningsAsErrors>`, `<EnableNETAnalyzers>true</EnableNETAnalyzers>`, and `<GenerateDocumentationFile>true</GenerateDocumentationFile>` (compiling CS1591 missing XML doc warnings as errors across every public symbol).

```bash
dotnet build dotnet/StrataForge.slnx -c Release
```
