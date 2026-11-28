using System.Globalization;
using System.Net;
using System.Net.Sockets;
using System.Text.Json;
using StrataForge.Core.Audit;
using StrataForge.Core.Codegen;
using StrataForge.Core.Requirements;
using StrataForge.Ground;

namespace StrataForge.Cli;

/// <summary>
/// Entry point for <c>strata-tools</c>, the unified .NET 11 multithreaded CLI replacing all Python utilities.
/// </summary>
/// <remarks>
/// <para>Supported subcommands:</para>
/// <list type="bullet">
///   <item><description><c>gen &lt;manifest.toml&gt; [--seds &lt;xml&gt;] [--emit &lt;dir&gt;]</c> — Single-source-of-truth validator &amp; parallel multi-domain generator.</description></item>
///   <item><description><c>ops --dict &lt;mission_dict.json&gt; encode-cmd|calibrate|encode-pus-tc ...</c> — Ground command/telemetry codec CLI.</description></item>
///   <item><description><c>check-sdoc &lt;requirements_dir&gt; [repo_root]</c> — Parallel StrictDoc traceability verifier.</description></item>
///   <item><description><c>check-safety &lt;build_dir&gt; [--max-flight-stack-bytes N] [--skip-elf-on-asan] [--elf &lt;bin&gt;]...</c> — Parallel stack-usage &amp; zero-heap ELF auditor.</description></item>
///   <item><description><c>check-cross [repo_root]</c> — Concurrent Versal AArch64 EL2 &amp; Cortex-R5F bare-metal cross proof.</description></item>
///   <item><description><c>check-qemu-el2 [repo_root]</c> — Live bare-metal QEMU AArch64 Cortex-A72 EL2 execution verification.</description></item>
///   <item><description><c>web-console --dict &lt;mission_dict.json&gt; [--web-root &lt;dir&gt;] [--udp-port N] [--http-port N]</c> — Live Mission Control web console.</description></item>
///   <item><description><c>test gen|ops|ground|live-udp &lt;repo_root&gt; [flight_bin]</c> — Automated verification suites.</description></item>
/// </list>
/// </remarks>
public static class Program
{
    /// <summary>
    /// Asynchronous CLI entry point dispatching STRATA-20 tooling subcommands.
    /// </summary>
    /// <param name="args">Command-line arguments beginning with the subcommand name.</param>
    /// <returns>Process exit code (<c>0</c> on success; non-zero on error).</returns>
    public static async Task<int> Main(string[] args)
    {
        if (args.Length == 0 || args[0] is "-h" or "--help")
        {
            PrintUsage();
            return args.Length == 0 ? 2 : 0;
        }

        string cmd = args[0];
        string[] rest = args[1..];

        try
        {
            return cmd switch
            {
                "gen" => await RunGenAsync(rest).ConfigureAwait(false),
                "ops" => RunOps(rest),
                "check-sdoc" => await RunCheckSdocAsync(rest).ConfigureAwait(false),
                "check-safety" => await RunCheckSafetyAsync(rest).ConfigureAwait(false),
                "check-cross" => await RunCheckCrossAsync(rest).ConfigureAwait(false),
                "check-qemu-el2" => await RunCheckQemuEl2Async(rest).ConfigureAwait(false),
                "pcap-slice" => RunPcapSlice(rest),
                "web-console" => await RunWebConsoleAsync(rest).ConfigureAwait(false),
                "test" => await RunTestAsync(rest).ConfigureAwait(false),
                _ => UnknownCommand(cmd),
            };
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"ERROR: {ex.Message}");
            return 1;
        }
    }

    private static async Task<int> RunGenAsync(string[] args)
    {
        string? manifestPath = null;
        string? emitDir = null;
        List<string> sedsPaths = [];

        for (int i = 0; i < args.Length; i++)
        {
            if (args[i] == "--seds" && i + 1 < args.Length)
            {
                sedsPaths.Add(args[++i]);
            }
            else if (args[i] == "--emit" && i + 1 < args.Length)
            {
                emitDir = args[++i];
            }
            else if (!args[i].StartsWith('-'))
            {
                manifestPath = args[i];
            }
        }

        if (string.IsNullOrWhiteSpace(manifestPath))
        {
            Console.Error.WriteLine("Usage: strata-tools gen <manifest.toml> [--seds <seds.xml>] [--emit <out_dir>]");
            return 2;
        }

        var (result, errors) = await StrataManifestGenerator.GenerateAsync(manifestPath, sedsPaths, emitDir).ConfigureAwait(false);
        if (errors.Count > 0 || result is null)
        {
            foreach (string err in errors)
            {
                Console.Error.WriteLine($"ERROR: {err}");
            }

            return 1;
        }

        string emittedMsg = result.EmittedFiles.Count > 0
            ? $" | emitted={result.EmittedFiles.Count} files -> {emitDir}"
            : string.Empty;

        Console.WriteLine(
            $"OK: {result.MissionName} ({result.TargetSilicon}) | partitions={result.PartitionCount} " +
            $"| hw_slots={result.HwSlotCount} | pools={result.PoolCount} | seds={result.SedsCount} " +
            $"| MANIFEST_HASH=0x{result.ManifestHash:X16}{emittedMsg}");
        return 0;
    }

    private static int RunOps(string[] args)
    {
        string? dictPath = null;
        List<string> pos = [];

        for (int i = 0; i < args.Length; i++)
        {
            if (args[i] == "--dict" && i + 1 < args.Length)
            {
                dictPath = args[++i];
            }
            else
            {
                pos.Add(args[i]);
            }
        }

        if (string.IsNullOrWhiteSpace(dictPath) || pos.Count == 0)
        {
            Console.Error.WriteLine("Usage: strata-tools ops --dict <mission_dict.json> encode-cmd|calibrate|encode-pus-tc ...");
            return 2;
        }

        MissionDictionary mdict = MissionDictionary.FromFile(dictPath);
        string sub = pos[0];

        if (sub == "encode-cmd" && pos.Count >= 2)
        {
            string cmdName = pos[1];
            string argHex = string.Empty;
            ushort scid = 0x0123;
            byte vcid = 1;
            byte seq = 0;

            for (int i = 2; i < pos.Count; i++)
            {
                if (pos[i] == "--arg-hex" && i + 1 < pos.Count) argHex = pos[++i];
                else if (pos[i] == "--scid" && i + 1 < pos.Count) scid = (ushort)ParseIntAuto(pos[++i]);
                else if (pos[i] == "--vcid" && i + 1 < pos.Count) vcid = (byte)ParseIntAuto(pos[++i]);
                else if (pos[i] == "--seq" && i + 1 < pos.Count) seq = (byte)ParseIntAuto(pos[++i]);
            }

            byte[] frame = FlightCodecs.EncodeNamedCommand(mdict, cmdName, Convert.FromHexString(argHex), scid, vcid, seq);
            Console.WriteLine(Convert.ToHexString(frame));
            return 0;
        }

        if (sub == "calibrate" && pos.Count >= 3)
        {
            TelemetryDef tdef = mdict.GetTelemetry(pos[1]);
            ulong raw = (ulong)ParseIntAuto(pos[2]);
            double eng = tdef.Calibrate(raw);
            Console.WriteLine(JsonSerializer.Serialize(new
            {
                name = tdef.Name,
                channel_id = tdef.ChannelId,
                raw,
                engineering_value = eng,
                unit = tdef.Unit,
            }));
            return 0;
        }

        if (sub == "encode-pus-tc")
        {
            ushort apid = 0x0105;
            ushort seq = 1;
            byte svc = 0;
            byte subtype = 0;
            string appHex = string.Empty;

            for (int i = 1; i < pos.Count; i++)
            {
                if (pos[i] == "--apid" && i + 1 < pos.Count) apid = (ushort)ParseIntAuto(pos[++i]);
                else if (pos[i] == "--seq" && i + 1 < pos.Count) seq = (ushort)ParseIntAuto(pos[++i]);
                else if (pos[i] == "--svc" && i + 1 < pos.Count) svc = (byte)ParseIntAuto(pos[++i]);
                else if (pos[i] == "--sub" && i + 1 < pos.Count) subtype = (byte)ParseIntAuto(pos[++i]);
                else if (pos[i] == "--app-hex" && i + 1 < pos.Count) appHex = pos[++i];
            }

            byte[] pkt = FlightCodecs.EncodePusCTc(apid, seq, svc, subtype, Convert.FromHexString(appHex));
            Console.WriteLine(Convert.ToHexString(pkt));
            return 0;
        }

        Console.Error.WriteLine($"Unknown ops subcommand: {sub}");
        return 2;
    }

    private static async Task<int> RunCheckSdocAsync(string[] args)
    {
        if (args.Length < 1)
        {
            Console.Error.WriteLine("Usage: strata-tools check-sdoc <requirements_dir> [repo_root]");
            return 2;
        }

        string reqDir = args[0];
        string repoRoot = args.Length >= 2 ? args[1] : Path.GetDirectoryName(Path.GetFullPath(reqDir)) ?? ".";

        StrictDocValidationReport report = await StrictDocVerifier.ValidateDirectoryAsync(reqDir, repoRoot).ConfigureAwait(false);
        if (!report.IsSuccess)
        {
            foreach (string err in report.Errors)
            {
                Console.Error.WriteLine($"ERROR: {err}");
            }

            return 1;
        }

        Console.WriteLine(
            $"OK: validated {report.SpecFileCount} StrictDoc specs ({report.RequirementCount} requirements) | " +
            $"Implemented={report.ImplementedCount} | Partially Implemented={report.PartiallyImplementedCount} | " +
            $"Planned={report.PlannedCount} | 0 orphans | 0 cycles");
        return 0;
    }

    private static async Task<int> RunCheckSafetyAsync(string[] args)
    {
        string? buildDir = null;
        int maxStack = 4096;
        bool skipAsan = false;
        List<string> elfs = [];

        for (int i = 0; i < args.Length; i++)
        {
            if (args[i] == "--max-flight-stack-bytes" && i + 1 < args.Length)
            {
                maxStack = int.Parse(args[++i], CultureInfo.InvariantCulture);
            }
            else if (args[i] == "--skip-elf-on-asan")
            {
                skipAsan = true;
            }
            else if (args[i] == "--elf" && i + 1 < args.Length)
            {
                elfs.Add(args[++i]);
            }
            else if (!args[i].StartsWith('-'))
            {
                buildDir = args[i];
            }
        }

        if (string.IsNullOrWhiteSpace(buildDir))
        {
            Console.Error.WriteLine("Usage: strata-tools check-safety <build_dir> [--max-flight-stack-bytes N] [--skip-elf-on-asan] [--elf <path>]...");
            return 2;
        }

        FlightSafetyAuditReport report = await FlightSafetyAuditor.AuditAsync(buildDir, elfs, maxStack, skipAsan).ConfigureAwait(false);
        if (!report.IsSuccess)
        {
            foreach (string err in report.Errors)
            {
                Console.Error.WriteLine($"AUDIT ERROR: {err}");
            }

            return 1;
        }

        Console.WriteLine(
            $"OK: Flight Safety Audit PASSED | functions_checked={report.FunctionsChecked} " +
            $"| peak_stack={report.PeakFlightStackBytes}B (limit={report.MaxFlightStackBytes}B) " +
            $"| zero_heap_elfs_verified={report.ZeroHeapElfsVerified}");
        return 0;
    }

    private static async Task<int> RunCheckCrossAsync(string[] args)
    {
        string repoRoot = args.Length >= 1 ? args[0] : Directory.GetCurrentDirectory();
        CrossVerificationResult res = await BareMetalCrossVerifier.VerifyBareMetalCrossAsync(repoRoot).ConfigureAwait(false);
        if (!res.Success)
        {
            Console.Error.WriteLine(res.Summary);
            if (!string.IsNullOrEmpty(res.ErrorDetail))
            {
                Console.Error.WriteLine(res.ErrorDetail);
            }

            return 1;
        }

        Console.WriteLine(res.Summary);
        return 0;
    }

    private static async Task<int> RunCheckQemuEl2Async(string[] args)
    {
        string repoRoot = args.Length >= 1 ? args[0] : Directory.GetCurrentDirectory();
        CrossVerificationResult res = await BareMetalCrossVerifier.VerifyQemuAarch64El2Async(repoRoot).ConfigureAwait(false);
        if (!res.Success)
        {
            Console.Error.WriteLine(res.Summary);
            if (!string.IsNullOrEmpty(res.ErrorDetail))
            {
                Console.Error.WriteLine(res.ErrorDetail);
            }

            return 1;
        }

        Console.WriteLine(res.Summary);
        return 0;
    }

    private static int RunPcapSlice(string[] args)
    {
        if (args.Length < 4)
        {
            Console.Error.WriteLine("Usage: strata-tools pcap-slice <input.pcap> <output.pcap> <start_ns> <end_ns>");
            return 2;
        }

        long startNs = long.Parse(args[2], CultureInfo.InvariantCulture);
        long endNs = long.Parse(args[3], CultureInfo.InvariantCulture);
        int copied = PcapSlicer.SliceByTimeWindow(args[0], args[1], startNs, endNs);
        Console.WriteLine($"OK: sliced {copied} packets from {args[0]} -> {args[1]}");
        return 0;
    }

    private static async Task<int> RunWebConsoleAsync(string[] args)
    {
        string? dictPath = null;
        string webRoot = Path.Combine(Directory.GetCurrentDirectory(), "ground/web");
        int udpPort = 9100;
        int txPort = 9101;
        int httpPort = 8088;

        for (int i = 0; i < args.Length; i++)
        {
            if (args[i] == "--dict" && i + 1 < args.Length) dictPath = args[++i];
            else if (args[i] == "--web-root" && i + 1 < args.Length) webRoot = args[++i];
            else if (args[i] == "--udp-port" && i + 1 < args.Length) udpPort = int.Parse(args[++i], CultureInfo.InvariantCulture);
            else if (args[i] == "--tx-port" && i + 1 < args.Length) txPort = int.Parse(args[++i], CultureInfo.InvariantCulture);
            else if (args[i] == "--http-port" && i + 1 < args.Length) httpPort = int.Parse(args[++i], CultureInfo.InvariantCulture);
        }

        if (string.IsNullOrWhiteSpace(dictPath))
        {
            Console.Error.WriteLine("Usage: strata-tools web-console --dict <mission_dict.json> [--web-root <dir>] [--udp-port 9100] [--tx-port 9101] [--http-port 8088]");
            return 2;
        }

        MissionDictionary mdict = MissionDictionary.FromFile(dictPath);
        await using TelemetryBroker broker = new(mdict);
        int boundUdp = broker.StartUdpListener("127.0.0.1", udpPort, workerCount: 4);

        using UdpClient txUdp = new();
        IPEndPoint txEp = new(IPAddress.Loopback, txPort);
        GroundTestSession session = new(mdict, broker, wire => txUdp.Send(wire, wire.Length, txEp));

        await using GroundWebConsole console = new(mdict, broker, session, webRoot, "127.0.0.1", httpPort);
        int boundHttp = console.Start();

        Console.WriteLine($"StrataGround (.NET 11) running | HTTP=http://127.0.0.1:{boundHttp} | UDP_RX={boundUdp} | UDP_TX={txPort}");
        using CancellationTokenSource cts = new();
        Console.CancelKeyPress += (_, e) =>
        {
            e.Cancel = true;
            cts.Cancel();
        };

        try
        {
            await Task.Delay(Timeout.Infinite, cts.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            // Graceful Ctrl-C shutdown
        }

        return 0;
    }

    private static async Task<int> RunTestAsync(string[] args)
    {
        if (args.Length < 2)
        {
            Console.Error.WriteLine("Usage: strata-tools test gen|ops|ground|live-udp <repo_root> [flight_bin]");
            return 2;
        }

        string suite = args[0];
        string repoRoot = args[1];
        return suite switch
        {
            "gen" => await VerificationSuites.RunCodegenSuiteAsync(repoRoot).ConfigureAwait(false),
            "ops" => await VerificationSuites.RunOpsSuiteAsync(repoRoot).ConfigureAwait(false),
            "ground" => await VerificationSuites.RunGroundEndToEndSuiteAsync(repoRoot).ConfigureAwait(false),
            "live-udp" when args.Length >= 3 => await VerificationSuites.RunLiveUdpFlightLoopAsync(repoRoot, args[2]).ConfigureAwait(false),
            _ => UnknownCommand($"test {suite}"),
        };
    }

    private static long ParseIntAuto(string raw) =>
        raw.StartsWith("0x", StringComparison.OrdinalIgnoreCase)
            ? long.Parse(raw[2..], NumberStyles.HexNumber, CultureInfo.InvariantCulture)
            : long.Parse(raw, NumberStyles.Integer, CultureInfo.InvariantCulture);

    private static int UnknownCommand(string cmd)
    {
        Console.Error.WriteLine($"Unknown command: '{cmd}'");
        PrintUsage();
        return 2;
    }

    private static void PrintUsage()
    {
        Console.WriteLine("strata-tools (.NET 11) — STRATA-20 Flight Codegen, Ground Control & Verification Suite");
        Console.WriteLine("Commands: gen | ops | check-sdoc | check-safety | check-cross | check-qemu-el2 | web-console | test");
    }
}
