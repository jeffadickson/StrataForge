using System.Buffers.Binary;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using System.Xml.Linq;
using StrataForge.Core.Codegen;
using StrataForge.Ground;

namespace StrataForge.Cli;

/// <summary>
/// Automated end-to-end verification suites replacing the legacy Python test harnesses with native
/// multithreaded .NET 11 verification routines.
/// </summary>
public static class VerificationSuites
{
    /// <summary>
    /// Verifies <c>STRATA-GEN-001</c> through <c>STRATA-GEN-004</c>: parallel multi-domain emission,
    /// CCSDS SEDS XML import, <see cref="XDocument"/> XTCE 1.2 validation, synchronized <c>MANIFEST_HASH</c>,
    /// negative NoC isolation checks, and live <c>g++ -std=c++20 -fsyntax-only</c> compilation.
    /// </summary>
    /// <param name="repoRoot">Absolute path to the StrataForge repository root.</param>
    /// <param name="cancellationToken">Cancellation token.</param>
    /// <returns>Zero on success; non-zero on failure.</returns>
    public static async Task<int> RunCodegenSuiteAsync(string repoRoot, CancellationToken cancellationToken = default)
    {
        string tempDir = Directory.CreateTempSubdirectory("strata_gen_test_").FullName;
        try
        {
            string versalToml = Path.Combine(repoRoot, "manifests/examples/versal_dual_instrument.toml");
            string sedsXml = Path.Combine(repoRoot, "manifests/examples/hyperspec_sensor_seds.xml");
            string outDir = Path.Combine(tempDir, "gen_out");

            var (res, errs) = await StrataManifestGenerator.GenerateAsync(versalToml, [sedsXml], outDir, cancellationToken).ConfigureAwait(false);
            AssertTrue(errs.Count == 0 && res is not null, $"strata-tools gen failed: {string.Join("; ", errs)}");

            string hashHex = $"{res!.ManifestHash:X16}";
            string hashLo = hashHex[8..16];
            string hashHi = hashHex[0..8];

            string[] expectedFiles =
            [
                "strata_mission_icd.hpp",
                "strata_hyp_tables.hpp",
                "link_cdh_b.ld",
                "link_fdir_slot_mgr_b.ld",
                "link_storage_c.ld",
                "link_hyperspec_d.ld",
                "link_sar_dfx_d.ld",
                "scsr_pkg.sv",
                "strata_xmpu_noc.tcl",
                "mission_xtce.xml",
                "mission_dict.json",
            ];

            foreach (string fname in expectedFiles)
            {
                AssertTrue(File.Exists(Path.Combine(outDir, fname)), $"Missing generated artifact: {fname}");
            }

            string icdText = await File.ReadAllTextAsync(Path.Combine(outDir, "strata_mission_icd.hpp"), cancellationToken).ConfigureAwait(false);
            string hypText = await File.ReadAllTextAsync(Path.Combine(outDir, "strata_hyp_tables.hpp"), cancellationToken).ConfigureAwait(false);
            string ldText = await File.ReadAllTextAsync(Path.Combine(outDir, "link_cdh_b.ld"), cancellationToken).ConfigureAwait(false);
            string svText = await File.ReadAllTextAsync(Path.Combine(outDir, "scsr_pkg.sv"), cancellationToken).ConfigureAwait(false);
            string tclText = await File.ReadAllTextAsync(Path.Combine(outDir, "strata_xmpu_noc.tcl"), cancellationToken).ConfigureAwait(false);

            AssertTrue(icdText.Contains($"0x{hashHex}ULL", StringComparison.Ordinal), "ICD missing MANIFEST_HASH");
            AssertTrue(hypText.Contains($"0x{hashHex}ULL", StringComparison.Ordinal), "Hyp tables missing MANIFEST_HASH");
            AssertTrue(ldText.Contains($"0x{hashHex}", StringComparison.Ordinal), "Linker script missing MANIFEST_HASH");
            AssertTrue(svText.Contains($"32'h{hashLo}", StringComparison.Ordinal) && svText.Contains($"32'h{hashHi}", StringComparison.Ordinal), "SV pkg missing MANIFEST_HASH");
            AssertTrue(tclText.Contains($"0x{hashHex}", StringComparison.Ordinal), "TCL script missing MANIFEST_HASH");
            AssertTrue(icdText.Contains("kReg_HyperspecImagerSensor_INTEGRATION_US = 0x1000U", StringComparison.Ordinal), "Missing SEDS register in ICD");

            XDocument xtce = XDocument.Load(Path.Combine(outDir, "mission_xtce.xml"));
            string shortDesc = xtce.Root?.Attribute("shortDescription")?.Value ?? string.Empty;
            AssertTrue(shortDesc.Contains(hashHex, StringComparison.Ordinal), "XTCE XML missing MANIFEST_HASH");

            MissionDictionary mdict = MissionDictionary.FromFile(Path.Combine(outDir, "mission_dict.json"));
            AssertTrue(mdict.ManifestHash == res.ManifestHash, "mission_dict.json hash mismatch");
            AssertTrue(mdict.CommandsByName.Count >= 3 && mdict.TelemetryByName.Count >= 2, "Missing commands/telemetry in JSON dict");

            // Negative test: Class D slot on CRITICAL_EXCL NoC group must be rejected
            string badToml = Path.Combine(tempDir, "bad_isolation.toml");
            await File.WriteAllTextAsync(
                badToml,
                """
                [mission]
                name = "bad_demo"
                target_silicon = "XQRVC1902"
                minor_frame_us = 10000
                major_frame_us = 100000

                [[partitions]]
                id = "payload_d"
                criticality = "CLASS_D"
                cpu = "APU_CORE1"
                budget_us = 5000

                [[hw_slots]]
                slot_id = 0
                name = "bad_slot"
                mode = "PL_DFX"
                owner_partition = "payload_d"
                noc_routing_group = "CRITICAL_EXCL"
                """,
                cancellationToken).ConfigureAwait(false);

            var (_, badErrs) = await StrataManifestGenerator.GenerateAsync(badToml, null, null, cancellationToken).ConfigureAwait(false);
            AssertTrue(badErrs.Any(e => e.Contains("Isolation violation", StringComparison.Ordinal)), "Expected NoC isolation violation");

            // Syntax-check generated C++20 headers using g++ -std=c++20 -fsyntax-only
            string cppCheck = Path.Combine(tempDir, "check_gen.cpp");
            await File.WriteAllTextAsync(
                cppCheck,
                """
                #include "strata_mission_icd.hpp"
                #include "strata_hyp_tables.hpp"
                static_assert(strata::generated::kManifestHash == strata::generated::kHypManifestHash);
                int main() { return 0; }
                """,
                cancellationToken).ConfigureAwait(false);

            ProcessStartInfo psi = new("g++")
            {
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false,
            };
            foreach (string arg in new[]
                     {
                         "-std=c++20", "-fno-exceptions", "-fno-rtti", "-Wall", "-Wextra", "-Werror",
                         "-fsyntax-only",
                         $"-I{outDir}",
                         $"-I{Path.Combine(repoRoot, "middleware/include")}",
                         $"-I{Path.Combine(repoRoot, "hypervisor/include")}",
                         cppCheck,
                     })
            {
                psi.ArgumentList.Add(arg);
            }

            using Process proc = Process.Start(psi)!;
            string stderr = await proc.StandardError.ReadToEndAsync(cancellationToken).ConfigureAwait(false);
            await proc.WaitForExitAsync(cancellationToken).ConfigureAwait(false);
            AssertTrue(proc.ExitCode == 0, $"Generated C++20 headers failed g++ syntax check: {stderr}");

            Console.WriteLine($"OK: strata-tools gen multi-domain emission + SEDS + XDocument XTCE/JSON + C++20 syntax check PASSED (HASH=0x{hashHex})");
            return 0;
        }
        finally
        {
            TryDelete(tempDir);
        }
    }

    /// <summary>
    /// Verifies ground CLI operations: CCSDS TC framing + CRC-16 FECF, telemetry polynomial calibration,
    /// 32-byte <c>EventRecord</c> decoding, and PUS-C TC/TM wire codecs.
    /// </summary>
    /// <param name="repoRoot">Absolute path to the StrataForge repository root.</param>
    /// <param name="cancellationToken">Cancellation token.</param>
    /// <returns>Zero on success; non-zero on failure.</returns>
    public static async Task<int> RunOpsSuiteAsync(string repoRoot, CancellationToken cancellationToken = default)
    {
        string tempDir = Directory.CreateTempSubdirectory("strata_ops_test_").FullName;
        try
        {
            string versalToml = Path.Combine(repoRoot, "manifests/examples/versal_dual_instrument.toml");
            string sedsXml = Path.Combine(repoRoot, "manifests/examples/hyperspec_sensor_seds.xml");
            string genDir = Path.Combine(tempDir, "gen");

            var (_, errs) = await StrataManifestGenerator.GenerateAsync(versalToml, [sedsXml], genDir, cancellationToken).ConfigureAwait(false);
            AssertTrue(errs.Count == 0, "Failed to generate mission_dict.json");

            MissionDictionary mdict = MissionDictionary.FromFile(Path.Combine(genDir, "mission_dict.json"));

            // 1. Encode HSI_SET_INTEGRATION (opcode 0x0201, 4-byte arg = 4500 us)
            byte[] rawTc = FlightCodecs.EncodeNamedCommand(mdict, "HSI_SET_INTEGRATION", Convert.FromHexString("00001194"), frameSeq: 7);
            AssertTrue(rawTc.Length == 13, $"Expected 13B TC frame, got {rawTc.Length}");
            ushort fecf = BinaryPrimitives.ReadUInt16BigEndian(rawTc.AsSpan(11, 2));
            AssertTrue(FlightCodecs.Crc16Ccitt(rawTc.AsSpan(0, 11)) == fecf, "TC FECF mismatch");
            AssertTrue(rawTc[4] == 7, "TC seq mismatch");
            AssertTrue(BinaryPrimitives.ReadUInt16BigEndian(rawTc.AsSpan(5, 2)) == 0x0201, "Opcode mismatch");
            AssertTrue(BinaryPrimitives.ReadUInt32BigEndian(rawTc.AsSpan(7, 4)) == 4500U, "Arg mismatch");

            // 2. Calibrate HSI_DETECTOR_TEMP_K: raw=2000 -> 180.0 + 0.05 * 2000 = 280.0 K
            TelemetryDef tdef = mdict.GetTelemetry("HSI_DETECTOR_TEMP_K");
            double eng = tdef.Calibrate(2000UL);
            AssertTrue(Math.Abs(eng - 280.0) < 1e-6 && tdef.Unit == "K", $"Calibration mismatch: {eng}");

            // 3. Decode 32-byte EventRecord
            byte[] rawEv = new byte[32];
            BinaryPrimitives.WriteUInt64LittleEndian(rawEv.AsSpan(0, 8), 1000UL);
            BinaryPrimitives.WriteUInt64LittleEndian(rawEv.AsSpan(8, 8), 2000UL);
            BinaryPrimitives.WriteUInt32LittleEndian(rawEv.AsSpan(16, 4), 0x0042U);
            BinaryPrimitives.WriteUInt32LittleEndian(rawEv.AsSpan(20, 4), 0x11U);
            BinaryPrimitives.WriteUInt32LittleEndian(rawEv.AsSpan(24, 4), 0x22U);
            BinaryPrimitives.WriteUInt16LittleEndian(rawEv.AsSpan(28, 2), 3);
            rawEv[30] = 6; // FATAL
            rawEv[31] = 1;
            DecodedEventRecord ev = FlightCodecs.DecodeEventRecord(rawEv);
            AssertTrue(ev.EventIdHex == "0x0042" && ev.Severity == "FATAL" && ev.RepeatCount == 3, "EventRecord decode mismatch");

            // 4. PUS-C TC(17,1) & TM(17,2) round-trip
            byte[] pusTc = FlightCodecs.EncodePusCTc(0x0105, 1, 17, 1);
            AssertTrue(pusTc.Length == 11 && pusTc[6] == 0x29 && pusTc[7] == 17 && pusTc[8] == 1, "PUS-C TC encode mismatch");

            byte[] tmSp = BuildPusCTmPacket(0x0105, 1, 17, 2, 1, 1, 999_999UL);
            DecodedPusCTm decTm = FlightCodecs.DecodePusCTm(tmSp);
            AssertTrue(decTm.ServiceType == 17 && decTm.ServiceSubtype == 2 && decTm.CucTaiNs == 999_999UL, "PUS-C TM decode mismatch");

            Console.WriteLine("OK: strata-tools ops ground CLI roundtrip (CCSDS TC FECF + PUS-C + calibration + EventRecord) PASSED");
            return 0;
        }
        finally
        {
            TryDelete(tempDir);
        }
    }

    /// <summary>
    /// Verifies <c>STRATA-GDS-001</c> through <c>STRATA-GDS-005</c>: dictionary loading, persistence-debounced
    /// Red/Yellow limits, <c>.seq</c> &amp; <c>ParamBank</c> compilers, 56-byte <c>NvLogRecord</c> decoding,
    /// multithreaded <see cref="TelemetryBroker"/> UDP/Channel ingress, Wireshark PCAP capture/replay,
    /// CSV export, <see cref="GroundTestSession"/>, and live concurrent <see cref="GroundWebConsole"/> REST APIs.
    /// </summary>
    /// <param name="repoRoot">Absolute path to the StrataForge repository root.</param>
    /// <param name="cancellationToken">Cancellation token.</param>
    /// <returns>Zero on success; non-zero on failure.</returns>
    public static async Task<int> RunGroundEndToEndSuiteAsync(string repoRoot, CancellationToken cancellationToken = default)
    {
        string tempDir = Directory.CreateTempSubdirectory("strata_ground_test_").FullName;
        try
        {
            string versalToml = Path.Combine(repoRoot, "manifests/examples/versal_dual_instrument.toml");
            string sedsXml = Path.Combine(repoRoot, "manifests/examples/hyperspec_sensor_seds.xml");
            string genDir = Path.Combine(tempDir, "gen");

            var (_, errs) = await StrataManifestGenerator.GenerateAsync(versalToml, [sedsXml], genDir, cancellationToken).ConfigureAwait(false);
            AssertTrue(errs.Count == 0, "strata-tools gen failed");

            MissionDictionary mdict = MissionDictionary.FromFile(Path.Combine(genDir, "mission_dict.json"));
            AssertTrue(mdict.MissionName == "versal_dual_instrument_demo", "Mission name mismatch");
            AssertTrue(mdict.CommandsByName.ContainsKey("HSI_SET_INTEGRATION"), "Missing HSI_SET_INTEGRATION");

            // 1. Red/Yellow LimitMonitor with persistence=2 debouncing
            LimitMonitor limits = new(mdict);
            limits.Register(new LimitSpec("HSI_DETECTOR_TEMP_K", 200.0, 220.0, 295.0, 310.0, Persistence: 2), "K");
            AssertTrue(limits.Evaluate("HSI_DETECTOR_TEMP_K", 280.0, 1) == LimitState.Nominal, "Expected Nominal");
            AssertTrue(limits.Evaluate("HSI_DETECTOR_TEMP_K", 300.0, 2) == LimitState.Nominal, "1st hit should be debounced");
            AssertTrue(limits.Evaluate("HSI_DETECTOR_TEMP_K", 301.0, 3) == LimitState.WarnHi, "2nd hit should trip WarnHi");
            AssertTrue(limits.Evaluate("HSI_DETECTOR_TEMP_K", 315.0, 4) == LimitState.WarnHi, "1st Red hit debounced");
            AssertTrue(limits.Evaluate("HSI_DETECTOR_TEMP_K", 316.0, 5) == LimitState.ErrorHi, "2nd Red hit should trip ErrorHi");
            AssertTrue(limits.GetAlertHistory().Count == 2, "Expected 2 alerts");
            limits.Evaluate("HSI_DETECTOR_TEMP_K", 275.0, 6);
            AssertTrue(limits.Evaluate("HSI_DETECTOR_TEMP_K", 275.0, 7) == LimitState.Nominal, "Expected return to Nominal");

            // 2. SequenceCompiler & ParamTableCompiler round-trips
            SequenceCompiler seqC = new(mdict);
            byte[] seqBin = seqC.CompileText(
                """
                # Automated science commissioning sequence
                0.000 NOOP
                +1.500 HSI_SET_INTEGRATION 4500
                +2.000 SLOT_RELOAD_DFX 1
                """);
            IReadOnlyList<SequenceStep> steps = seqC.Decompile(seqBin);
            AssertTrue(steps.Count == 3, "Expected 3 sequence steps");
            AssertTrue(steps[0].CommandName == "NOOP" && !steps[0].IsRelative, "Step 0 mismatch");
            AssertTrue(steps[1].CommandName == "HSI_SET_INTEGRATION" && steps[1].DelayMs == 1500 && steps[1].ArgU64 == 4500, "Step 1 mismatch");
            AssertTrue(steps[2].CommandName == "SLOT_RELOAD_DFX" && steps[2].DelayMs == 2000, "Step 2 mismatch");

            ParamTableCompiler tblC = new(mdict.ManifestHash);
            byte[] tblBin = tblC.Compile(4U, [(1U, 4500UL), (2U, 16UL)]);
            var (ver, ents) = tblC.Decompile(tblBin);
            AssertTrue(ver == 4U && ents.Count == 2 && ents[0] == (1U, 4500UL) && ents[1] == (2U, 16UL), "ParamBank round-trip mismatch");

            // 3. 56-byte NvLogRecord decoder + Castagnoli CRC-32C check
            byte[] rawNv = new byte[56];
            BinaryPrimitives.WriteUInt32LittleEndian(rawNv.AsSpan(0, 4), 2U);
            BinaryPrimitives.WriteUInt32LittleEndian(rawNv.AsSpan(4, 4), 7U);
            BinaryPrimitives.WriteUInt64LittleEndian(rawNv.AsSpan(8, 8), 50_000UL);
            BinaryPrimitives.WriteUInt16LittleEndian(rawNv.AsSpan(16, 2), 2); // CrashTrap
            BinaryPrimitives.WriteUInt16LittleEndian(rawNv.AsSpan(18, 2), 4);
            BinaryPrimitives.WriteUInt32LittleEndian(rawNv.AsSpan(20, 4), 0x9200_0047U);
            BinaryPrimitives.WriteUInt64LittleEndian(rawNv.AsSpan(24, 8), 0x1400_2210UL);
            BinaryPrimitives.WriteUInt64LittleEndian(rawNv.AsSpan(32, 8), 0x1000_0000UL);
            uint nvCrc = FlightCodecs.Crc32c(rawNv);
            BinaryPrimitives.WriteUInt32LittleEndian(rawNv.AsSpan(48, 4), nvCrc);
            DecodedNvLogRecord nvDec = FlightCodecs.DecodeNvLogRecord(rawNv);
            AssertTrue(nvDec.CrcValid && nvDec.BootEpoch == 2U && nvDec.Kind == "CrashTrap", "NvLogRecord decode mismatch");

            // 4. Multithreaded UDP TelemetryBroker + PCAP + CSV + GroundTestSession + GroundWebConsole
            await using TelemetryBroker broker = new(mdict, limits);
            string pcapPath = Path.Combine(tempDir, "flight_capture.pcap");
            string csvPath = Path.Combine(tempDir, "tlm_export.csv");
            broker.StartPcapCapture(pcapPath);
            int udpPort = broker.StartUdpListener("127.0.0.1", 0, workerCount: 4);

            List<byte[]> sentFrames = [];
            using UdpClient txUdp = new();
            IPEndPoint rxEp = new(IPAddress.Loopback, udpPort);

            void SendUplink(byte[] wire)
            {
                lock (sentFrames)
                {
                    sentFrames.Add(wire);
                }

                // Loopback echo for PUS-C ST[17,1] -> TM(17,2)
                if (wire.Length >= 11 && wire[7] == 17 && wire[8] == 1)
                {
                    byte[] reply = BuildPusCTmPacket(0x0105, 1, 17, 2, 1, 1, 123_456UL);
                    txUdp.Send(reply, reply.Length, rxEp);
                }
            }

            GroundTestSession session = new(mdict, broker, SendUplink);
            session.SendCommand("HSI_SET_INTEGRATION", Convert.FromHexString("00001194"));
            session.SendPusTc(17, 1);

            DecodedPusCTm tm17 = await session.WaitForPusTmAsync(17, 2, TimeSpan.FromSeconds(2), cancellationToken).ConfigureAwait(false);
            AssertTrue(tm17.ServiceType == 17 && tm17.ServiceSubtype == 2, "Missing PUS-C TM(17,2)");

            // Burst 40 concurrent UDP telemetry & EVR packets across parallel tasks to stress thread safety
            await Parallel.ForEachAsync(
                Enumerable.Range(0, 20),
                cancellationToken,
                async (idx, ct) =>
                {
                    byte[] sttm = new byte[14];
                    sttm[0] = (byte)'S'; sttm[1] = (byte)'T'; sttm[2] = (byte)'T'; sttm[3] = (byte)'M';
                    BinaryPrimitives.WriteUInt16BigEndian(sttm.AsSpan(4, 2), 0x0010);
                    BinaryPrimitives.WriteUInt64BigEndian(sttm.AsSpan(6, 8), (ulong)(1800 + idx));
                    await txUdp.SendAsync(sttm, rxEp, ct).ConfigureAwait(false);
                }).ConfigureAwait(false);

            byte[] stev = new byte[36];
            stev[0] = (byte)'S'; stev[1] = (byte)'T'; stev[2] = (byte)'E'; stev[3] = (byte)'V';
            BinaryPrimitives.WriteUInt64LittleEndian(stev.AsSpan(4, 8), 100UL);
            BinaryPrimitives.WriteUInt64LittleEndian(stev.AsSpan(12, 8), 100UL);
            BinaryPrimitives.WriteUInt32LittleEndian(stev.AsSpan(20, 4), 0x0101U);
            BinaryPrimitives.WriteUInt16LittleEndian(stev.AsSpan(32, 2), 1);
            stev[34] = 3; // ACT_HI
            stev[35] = 0;
            await txUdp.SendAsync(stev, rxEp, cancellationToken).ConfigureAwait(false);

            TelemetrySample tlmSample = await session.WaitForTelemetryAsync("HSI_DETECTOR_TEMP_K", s => s.EngValue >= 270.0, TimeSpan.FromSeconds(2), cancellationToken).ConfigureAwait(false);
            AssertTrue(tlmSample.LimitState == LimitState.Nominal, "Expected nominal detector temp");
            DecodedEventRecord evr = await session.WaitForEventAsync(0x0101U, TimeSpan.FromSeconds(2), cancellationToken).ConfigureAwait(false);
            AssertTrue(evr.Severity == "ACT_HI", "Expected ACT_HI EVR");
            session.AssertLimitsNominal();

            // 5. Concurrent Web Mission Control REST API verification
            string webRoot = Path.Combine(repoRoot, "ground/web");
            await using GroundWebConsole console = new(mdict, broker, session, webRoot, "127.0.0.1", 0);
            int httpPort = console.Start();

            using HttpClient http = new() { BaseAddress = new Uri($"http://127.0.0.1:{httpPort}") };
            Task<string> htmlTask = http.GetStringAsync("/", cancellationToken);
            Task<string> dictTask = http.GetStringAsync("/api/dictionary", cancellationToken);
            Task<string> tlmTask = http.GetStringAsync("/api/telemetry", cancellationToken);
            Task<string> limTask = http.GetStringAsync("/api/limits", cancellationToken);
            await Task.WhenAll(htmlTask, dictTask, tlmTask, limTask).ConfigureAwait(false);

            AssertTrue(htmlTask.Result.Contains("StrataGround", StringComparison.Ordinal), "Missing HTML title");
            AssertTrue(dictTask.Result.Contains("versal_dual_instrument_demo", StringComparison.Ordinal), "Missing dict JSON");
            AssertTrue(tlmTask.Result.Contains("HSI_DETECTOR_TEMP_K", StringComparison.Ordinal), "Missing telemetry JSON");
            AssertTrue(limTask.Result.Contains("\"all_nominal\":true", StringComparison.Ordinal), "Expected all_nominal=true");

            using HttpResponseMessage seqResp = await http.PostAsync(
                "/api/sequence/compile",
                new StringContent("{\"text\":\"0.000 NOOP\\n+1.000 HSI_SET_INTEGRATION 4000\"}", Encoding.UTF8, "application/json"),
                cancellationToken).ConfigureAwait(false);
            seqResp.EnsureSuccessStatusCode();
            string seqJson = await seqResp.Content.ReadAsStringAsync(cancellationToken).ConfigureAwait(false);
            AssertTrue(seqJson.Contains("\"command\":\"HSI_SET_INTEGRATION\"", StringComparison.Ordinal), "Sequence compile API failed");

            // 6. Verify advanced mission-ops subsystems (STRATA-GDS-006..008):
            //    (a) Conditional mode-gated limits & reverse DN comparator threshold solver
            TelemetryDef tempDef = mdict.GetTelemetry("HSI_DETECTOR_TEMP_K");
            RawDnLimitThresholds dnLimits = limits.SolveRawDnThresholds(tempDef);
            // 295.0 K -> (295 - 180) / 0.05 = 2300 DN; 310.0 K -> 2600 DN
            AssertTrue(dnLimits.HiWarnDn == 2300UL && dnLimits.HiErrDn == 2600UL, "Reverse DN limit solver mismatch");

            LimitMonitor condLimits = new();
            condLimits.Register(new LimitSpec(
                "HSI_DETECTOR_TEMP_K",
                HiWarn: 295.0,
                HiErr: 310.0,
                Persistence: 1,
                Condition: new LimitCondition("HSI_MODE", m => Math.Abs(m - 4.0) < 0.1)));
            // Mode=2 (Standby) -> thermal limit suppressed even at 315 K
            var peersStandby = new Dictionary<string, double> { ["HSI_MODE"] = 2.0 };
            AssertTrue(condLimits.Evaluate("HSI_DETECTOR_TEMP_K", 315.0, 10, peersStandby) == LimitState.Nominal, "Conditional limit should suppress in Standby");
            // Mode=4 (Science) -> thermal limit trips ErrorHi immediately
            var peersScience = new Dictionary<string, double> { ["HSI_MODE"] = 4.0 };
            AssertTrue(condLimits.Evaluate("HSI_DETECTOR_TEMP_K", 315.0, 11, peersScience) == LimitState.ErrorHi, "Conditional limit should trip in Science");

            //    (b) Spacecraft Clock (SCLK <-> TAI <-> UTC) correlation with leap seconds & drift
            DateTimeOffset epochUtc = new(2026, 6, 1, 0, 0, 0, TimeSpan.Zero);
            SpacecraftTimeCorrelator corr = new(new SclkCorrelationConfig(SclkEpochSeconds: 1000.0, TaiEpochNs: 0L, DriftRatePpm: 50.0));
            long epochTaiNs = corr.UtcToTaiNs(epochUtc);
            AssertTrue(corr.GetTaiMinusUtcSeconds(epochUtc) == 37, "Expected 37s TAI-UTC leap second offset");
            corr.UpdateCorrelation(new SclkCorrelationConfig(1000.0, epochTaiNs, DriftRatePpm: 50.0));
            SclkTime sclkT = new(1100U, 32768U); // +100.5 s of spacecraft clock
            long taiT = corr.SclkToTaiNs(sclkT);
            SclkTime roundTripSclk = corr.TaiNsToSclk(taiT);
            AssertTrue(roundTripSclk.CoarseSeconds == 1100U && Math.Abs((int)roundTripSclk.FineTicks - 32768) <= 2, "SCLK round-trip mismatch");
            AssertTrue(corr.TaiNsToUtc(epochTaiNs) == epochUtc, "TAI <-> UTC round-trip mismatch");

            //    (c) Bitfield extraction, discrete enum mapping, and live derived virtual channels
            ulong packedWord = 0x0000_0000_0000_F4A5UL; // bits[3:0]=5, bits[7:4]=0xA (-6 signed 4-bit), bits[11:8]=4 (Science)
            AssertTrue(BitfieldCodec.ExtractUnsigned(packedWord, 0, 4) == 5UL, "Unsigned bitfield mismatch");
            AssertTrue(BitfieldCodec.ExtractSigned(packedWord, 4, 4) == -6L, "Signed bitfield mismatch");
            TelemetryEnumMap modeEnum = new("HSI_MODE", [new(0UL, "OFF"), new(4UL, "SCIENCE"), new(6UL, "FAULT")]);
            broker.DerivedChannels.RegisterEnum(modeEnum);
            AssertTrue(broker.DerivedChannels.TryDecodeEnum("HSI_MODE", 4UL) == "SCIENCE", "Enum decode mismatch");
            broker.DerivedChannels.RegisterDerived(new DerivedChannelDef(
                "HSI_TEMP_DELTA_C",
                0xF001,
                "C",
                snap => snap.TryGetValue("HSI_DETECTOR_TEMP_K", out TelemetrySample? t) ? t.EngValue - 273.15 : null));

            //    (d) Async AlertNotificationHub + live SSE /api/stream + /api/timeseries query
            await using AlertNotificationHub alertHub = new(cooldown: TimeSpan.FromMilliseconds(500));
            int hubAlerts = 0;
            alertHub.AddSink((_, _) => { Interlocked.Increment(ref hubAlerts); return ValueTask.CompletedTask; });
            broker.AttachAlertHub(alertHub);
            alertHub.Publish(new LimitAlert("HSI_DETECTOR_TEMP_K", LimitState.WarnHi, LimitState.Nominal, 300.0, "K", 1_000_000_000L));
            alertHub.Publish(new LimitAlert("HSI_DETECTOR_TEMP_K", LimitState.ErrorHi, LimitState.WarnHi, 315.0, "K", 1_050_000_000L)); // Within 500ms cooldown -> suppressed

            Task<string> sseTask = http.GetStringAsync("/api/stream?max=2", cancellationToken);
            await Task.Delay(40, cancellationToken).ConfigureAwait(false);
            broker.IngestTelemetryChannel(0x0010, 2000UL, 1_000_000_000L); // emits HSI_DETECTOR_TEMP_K + derived HSI_TEMP_DELTA_C!
            string sseBody = await sseTask.WaitAsync(TimeSpan.FromSeconds(2), cancellationToken).ConfigureAwait(false);
            AssertTrue(sseBody.Contains("event: telemetry", StringComparison.Ordinal) && sseBody.Contains("HSI_TEMP_DELTA_C", StringComparison.Ordinal), "SSE live push failed");
            AssertTrue(hubAlerts == 1 && alertHub.SuppressedCount >= 1, $"AlertNotificationHub cooldown mismatch: delivered={hubAlerts}, suppressed={alertHub.SuppressedCount}");

            string tsJson = await http.GetStringAsync("/api/timeseries?channel=HSI_DETECTOR_TEMP_K&limit=50", cancellationToken).ConfigureAwait(false);
            AssertTrue(tsJson.Contains("\"stats\"", StringComparison.Ordinal) && tsJson.Contains("\"points\"", StringComparison.Ordinal), "Timeseries REST endpoint failed");
            AssertTrue(broker.TryGetLatestTelemetry("HSI_TEMP_DELTA_C", out TelemetrySample? derivedSample) && Math.Abs(derivedSample!.EngValue - 6.85) < 0.01, "Derived channel value mismatch");

            //    (e) Rotating PCAP archiver rollover + time-window PCAP slicing
            string rotDir = Path.Combine(tempDir, "rot_pcap");
            using (RotatingPcapArchiver rot = new(rotDir, "tvac", maxBytesPerFile: 90L))
            {
                for (int i = 0; i < 6; i++)
                {
                    rot.WritePacket(new byte[24], 1_000_000_000L + (i * 1_000_000L));
                }

                AssertTrue(rot.CreatedFiles.Count >= 3, $"Expected >=3 rotated PCAP segments, got {rot.CreatedFiles.Count}");
            }

            await console.StopAsync().ConfigureAwait(false);
            await broker.StopUdpListenerAsync().ConfigureAwait(false);
            broker.StopPcapCapture();

            IReadOnlyList<(long, byte[])> pcapPkts = PcapReader.ReadAll(pcapPath);
            AssertTrue(pcapPkts.Count >= 10, $"Expected >=10 PCAP packets, got {pcapPkts.Count}");
            string slicedPcap = Path.Combine(tempDir, "sliced.pcap");
            long sliceStart = pcapPkts[2].Item1;
            long sliceEnd = pcapPkts[5].Item1;
            int slicedCount = PcapSlicer.SliceByTimeWindow(pcapPath, slicedPcap, sliceStart, sliceEnd);
            AssertTrue(slicedCount >= 4 && PcapReader.ReadAll(slicedPcap).Count == slicedCount, "PCAP time-window slicing mismatch");

            await TelemetryBroker.ExportTelemetryCsvAsync(broker.GetTelemetryHistory(), csvPath, cancellationToken).ConfigureAwait(false);
            AssertTrue(new FileInfo(csvPath).Length > 100, "CSV export empty");

            Console.WriteLine(
                $"OK: StrataGround (.NET 11 multithreaded) GDS & I&T suite PASSED " +
                $"| pcap_packets={pcapPkts.Count} | sliced={slicedCount} | tlm_samples={broker.GetTelemetryHistory().Count} " +
                $"| limit_alerts={limits.GetAlertHistory().Count} | sse_push=OK | web_api=OK");
            return 0;
        }
        finally
        {
            TryDelete(tempDir);
        }
    }

    /// <summary>
    /// Executes the live closed-loop Flight-to-Ground UDP integration test pairing the compiled C++20
    /// <c>live_ground_sil</c> binary with the multithreaded .NET 11 <see cref="TelemetryBroker"/> and <see cref="GroundTestSession"/>.
    /// </summary>
    /// <param name="repoRoot">Absolute path to the StrataForge repository root.</param>
    /// <param name="flightBinaryPath">Path to the compiled <c>live_ground_sil</c> executable.</param>
    /// <param name="cancellationToken">Cancellation token.</param>
    /// <returns>Zero on success; non-zero on failure.</returns>
    public static async Task<int> RunLiveUdpFlightLoopAsync(
        string repoRoot,
        string flightBinaryPath,
        CancellationToken cancellationToken = default)
    {
        string tempDir = Directory.CreateTempSubdirectory("strata_live_loop_").FullName;
        try
        {
            string versalToml = Path.Combine(repoRoot, "manifests/examples/versal_dual_instrument.toml");
            string sedsXml = Path.Combine(repoRoot, "manifests/examples/hyperspec_sensor_seds.xml");
            string genDir = Path.Combine(tempDir, "gen");

            var (_, errs) = await StrataManifestGenerator.GenerateAsync(versalToml, [sedsXml], genDir, cancellationToken).ConfigureAwait(false);
            AssertTrue(errs.Count == 0, "strata-tools gen failed");

            MissionDictionary mdict = MissionDictionary.FromFile(Path.Combine(genDir, "mission_dict.json"));
            LimitMonitor limits = new(mdict);
            await using TelemetryBroker broker = new(mdict, limits);

            string pcapPath = Path.Combine(tempDir, "live_pass.pcap");
            string csvPath = Path.Combine(tempDir, "live_tlm.csv");
            broker.StartPcapCapture(pcapPath);

            int groundRxPort = broker.StartUdpListener("127.0.0.1", 0, workerCount: 2);
            int flightRxPort = PickFreeUdpPort();

            using UdpClient txUdp = new();
            IPEndPoint flightEp = new(IPAddress.Loopback, flightRxPort);
            GroundTestSession session = new(mdict, broker, wire => txUdp.Send(wire, wire.Length, flightEp));

            ProcessStartInfo psi = new(flightBinaryPath)
            {
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false,
            };
            foreach (string arg in new[]
                     {
                         "--rx-port", flightRxPort.ToString(System.Globalization.CultureInfo.InvariantCulture),
                         "--tx-port", groundRxPort.ToString(System.Globalization.CultureInfo.InvariantCulture),
                         "--ticks", "800",
                         "--tick-ms", "10",
                     })
            {
                psi.ArgumentList.Add(arg);
            }

            using Process proc = Process.Start(psi)
                ?? throw new InvalidOperationException($"Failed to start '{flightBinaryPath}'.");

            Task<string> outTask = proc.StandardOutput.ReadToEndAsync(cancellationToken);
            Task<string> errTask = proc.StandardError.ReadToEndAsync(cancellationToken);

            try
            {
                // 1. Wait for live nominal detector telemetry over UDP
                TelemetrySample s0 = await session.WaitForTelemetryAsync(
                    "HSI_DETECTOR_TEMP_K",
                    s => s.EngValue is >= 240.0 and <= 290.0,
                    TimeSpan.FromSeconds(3),
                    cancellationToken).ConfigureAwait(false);
                AssertTrue(s0.LimitState == LimitState.Nominal, "Expected initial nominal temp");

                // 2. Send CCSDS TC HSI_SET_INTEGRATION = 7500 us -> wait for EVR 0x0101
                byte[] intArg = new byte[4];
                BinaryPrimitives.WriteUInt32BigEndian(intArg, 7500U);
                session.SendCommand("HSI_SET_INTEGRATION", intArg);
                DecodedEventRecord evCmd = await session.WaitForEventAsync(0x0101U, TimeSpan.FromSeconds(2), cancellationToken).ConfigureAwait(false);
                AssertTrue(evCmd.Arg1 == 7500U, $"Expected integration 7500, got {evCmd.Arg1}");

                // 3. Send PUS-C ST[17,1] Are-You-Alive -> wait for TM(17,2) & ST[01,7]
                session.SendPusTc(17, 1);
                DecodedPusCTm tm17 = await session.WaitForPusTmAsync(17, 2, TimeSpan.FromSeconds(2), cancellationToken).ConfigureAwait(false);
                DecodedPusCTm tm01 = await session.WaitForPusTmAsync(1, 7, TimeSpan.FromSeconds(2), cancellationToken).ConfigureAwait(false);
                AssertTrue(tm17.ServiceType == 17 && tm01.ServiceType == 1, "Missing PUS-C replies");

                // 4. Inject live ThermalRunaway (+260 W) via PUS-C ST[08,1] (func_id = 0x8001)
                byte[] injData = new byte[6];
                BinaryPrimitives.WriteUInt16BigEndian(injData.AsSpan(0, 2), 0x8001);
                BinaryPrimitives.WriteUInt32BigEndian(injData.AsSpan(2, 4), 260U);
                session.SendPusTc(8, 1, injData);

                // 5. Observe real physics push detector temperature >= 310 K -> LimitMonitor & FDIR trip!
                TelemetrySample hot = await session.WaitForTelemetryAsync(
                    "HSI_DETECTOR_TEMP_K",
                    s => s.EngValue >= 310.0,
                    TimeSpan.FromSeconds(4),
                    cancellationToken).ConfigureAwait(false);
                AssertTrue(hot.LimitState is LimitState.WarnHi or LimitState.ErrorHi, $"Expected hot limit state, got {hot.LimitState}");

                DecodedEventRecord evFdir = await session.WaitForEventAsync(0x0201U, TimeSpan.FromSeconds(3), cancellationToken).ConfigureAwait(false);
                DecodedPusCTm tm05 = await session.WaitForPusTmAsync(5, 3, TimeSpan.FromSeconds(2), cancellationToken).ConfigureAwait(false);
                AssertTrue(evFdir.EventId == 0x0201U && tm05.ServiceType == 5, "Missing FDIR safing telemetry");
                AssertTrue(limits.GetAlertHistory().Count >= 1, "Expected LimitMonitor alert history");

                // 6. Request graceful shutdown via PUS-C ST[08,1] func_id = 0x80FF
                byte[] stopData = new byte[6];
                BinaryPrimitives.WriteUInt16BigEndian(stopData.AsSpan(0, 2), 0x80FF);
                BinaryPrimitives.WriteUInt32BigEndian(stopData.AsSpan(2, 4), 0U);
                session.SendPusTc(8, 1, stopData);

                using CancellationTokenSource exitCts = new(TimeSpan.FromSeconds(4));
                await proc.WaitForExitAsync(exitCts.Token).ConfigureAwait(false);

                string stdout = await outTask.ConfigureAwait(false);
                string stderr = await errTask.ConfigureAwait(false);
                AssertTrue(proc.ExitCode == 0, $"live_ground_sil failed (rc={proc.ExitCode}): {stderr}");
                AssertTrue(
                    stdout.Contains("nv_log_records=2", StringComparison.Ordinal) ||
                    stdout.Contains("nv_log_records=1", StringComparison.Ordinal),
                    $"Expected NvFaultLog record in stdout: {stdout}");
            }
            finally
            {
                if (!proc.HasExited)
                {
                    proc.Kill(entireProcessTree: true);
                    await proc.WaitForExitAsync(CancellationToken.None).ConfigureAwait(false);
                }

                await broker.StopUdpListenerAsync().ConfigureAwait(false);
                broker.StopPcapCapture();
            }

            IReadOnlyList<(long, byte[])> pcapPkts = PcapReader.ReadAll(pcapPath);
            AssertTrue(pcapPkts.Count >= 20, $"Expected >=20 PCAP packets, got {pcapPkts.Count}");
            await TelemetryBroker.ExportTelemetryCsvAsync(broker.GetTelemetryHistory(), csvPath, cancellationToken).ConfigureAwait(false);
            AssertTrue(new FileInfo(csvPath).Length > 200, "CSV output too small");

            Console.WriteLine(
                $"OK: Live Flight-to-Ground UDP Loop (.NET 11) PASSED | pcap_packets={pcapPkts.Count} " +
                $"| tlm_samples={broker.GetTelemetryHistory().Count} | limit_alerts={limits.GetAlertHistory().Count} " +
                $"| pus_tm={broker.GetPusTmHistory().Count} | evrs={broker.GetEventHistory().Count}");
            return 0;
        }
        finally
        {
            TryDelete(tempDir);
        }
    }

    private static byte[] BuildPusCTmPacket(
        ushort apid,
        ushort seq,
        byte svc,
        byte sub,
        ushort counter,
        ushort destId,
        ulong taiNs)
    {
        byte[] sp = new byte[6 + 15];
        BinaryPrimitives.WriteUInt16BigEndian(sp.AsSpan(0, 2), (ushort)(0x0800 | (apid & 0x07FF)));
        BinaryPrimitives.WriteUInt16BigEndian(sp.AsSpan(2, 2), (ushort)(0xC000 | (seq & 0x3FFF)));
        BinaryPrimitives.WriteUInt16BigEndian(sp.AsSpan(4, 2), 14);
        sp[6] = 0x20;
        sp[7] = svc;
        sp[8] = sub;
        BinaryPrimitives.WriteUInt16BigEndian(sp.AsSpan(9, 2), counter);
        BinaryPrimitives.WriteUInt16BigEndian(sp.AsSpan(11, 2), destId);
        BinaryPrimitives.WriteUInt64BigEndian(sp.AsSpan(13, 8), taiNs);
        return sp;
    }

    private static int PickFreeUdpPort()
    {
        using UdpClient probe = new(new IPEndPoint(IPAddress.Loopback, 0));
        return ((IPEndPoint)probe.Client.LocalEndPoint!).Port;
    }

    private static void AssertTrue(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException($"ASSERTION FAILED: {message}");
        }
    }

    private static void TryDelete(string path)
    {
        try
        {
            if (Directory.Exists(path))
            {
                Directory.Delete(path, recursive: true);
            }
        }
        catch
        {
            // Ignore temp cleanup races
        }
    }
}
