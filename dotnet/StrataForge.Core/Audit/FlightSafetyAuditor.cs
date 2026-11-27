using System.Collections.Concurrent;
using System.Diagnostics;
using System.Globalization;

namespace StrataForge.Core.Audit;

/// <summary>
/// Result of running the STRATA-20 static stack-usage and zero-heap/zero-exception ELF flight safety audit.
/// </summary>
/// <param name="FunctionsChecked">Total number of compiled functions inspected across GCC <c>.su</c> reports.</param>
/// <param name="PeakFlightStackBytes">Peak static stack frame size observed across flight example binaries.</param>
/// <param name="MaxFlightStackBytes">Configured maximum static stack ceiling in bytes.</param>
/// <param name="ZeroHeapElfsVerified">Number of flight ELF executables verified free of heap/exception symbols.</param>
/// <param name="Errors">All safety violations discovered (empty when compliant).</param>
public sealed record FlightSafetyAuditReport(
    int FunctionsChecked,
    int PeakFlightStackBytes,
    int MaxFlightStackBytes,
    int ZeroHeapElfsVerified,
    IReadOnlyList<string> Errors)
{
    /// <summary>
    /// Gets a value indicating whether all flight safety checks passed without violations.
    /// </summary>
    /// <value><see langword="true"/> if <see cref="Errors"/> is empty; otherwise, <see langword="false"/>.</value>
    public bool IsSuccess => Errors.Count == 0;
}

/// <summary>
/// Multithreaded static stack-usage (<c>-fstack-usage</c>) and zero-heap/zero-exception ELF symbol auditor.
/// </summary>
/// <remarks>
/// <para>
/// Enforces two NASA NPR 7150.2D Class B flight constraints concurrently in CI:
/// </para>
/// <list type="number">
///   <item><description>Parses every GCC <c>.su</c> report in parallel, forbidding unbounded <c>dynamic</c> stack frames (VLA/<c>alloca</c>) and verifying flight binary functions remain within <c>maxFlightStackBytes</c> (default 4096 B).</description></item>
///   <item><description>Invokes <c>nm -u</c> concurrently across all flight executables to prove zero undefined references to <c>malloc</c>/<c>free</c>/<c>operator new</c>/<c>__cxa_throw</c>.</description></item>
/// </list>
/// </remarks>
public static class FlightSafetyAuditor
{
    private static readonly HashSet<string> ForbiddenSymbols = new(StringComparer.Ordinal)
    {
        "malloc",
        "calloc",
        "realloc",
        "free",
        "_Znwm",
        "_Znam",
        "_ZdlPv",
        "_ZdaPv",
        "__cxa_throw",
        "__cxa_begin_catch",
        "__cxa_allocate_exception",
    };

    /// <summary>
    /// Executes the parallel stack-usage and zero-heap ELF audit over a CMake build directory.
    /// </summary>
    /// <param name="buildDirectory">Path to the CMake build tree containing <c>.su</c> reports.</param>
    /// <param name="elfPaths">List of compiled flight executable paths to audit with <c>nm -u</c>.</param>
    /// <param name="maxFlightStackBytes">Maximum permitted static stack frame size in bytes (default 4096).</param>
    /// <param name="skipElfOnAsan">When <see langword="true"/>, relaxes sanitizer-instrumented ELF and stack redzone checks on ASan builds.</param>
    /// <param name="cancellationToken">Cancellation token for cooperative async cancellation.</param>
    /// <returns>A <see cref="FlightSafetyAuditReport"/> detailing functions checked, peak stack, and any violations.</returns>
    public static async Task<FlightSafetyAuditReport> AuditAsync(
        string buildDirectory,
        IReadOnlyList<string> elfPaths,
        int maxFlightStackBytes = 4096,
        bool skipElfOnAsan = false,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(buildDirectory);
        ArgumentNullException.ThrowIfNull(elfPaths);

        string fullBuildDir = Path.GetFullPath(buildDirectory);
        bool isAsan = skipElfOnAsan && fullBuildDir.Contains("sanitizer", StringComparison.OrdinalIgnoreCase);

        ConcurrentBag<string> errors = [];
        int fnCount = 0;
        int peakBytes = 0;

        string[] suFiles = Directory.Exists(fullBuildDir)
            ? Directory.GetFiles(fullBuildDir, "*.su", SearchOption.AllDirectories)
            : Array.Empty<string>();

        Task stackTask = Parallel.ForEachAsync(
            suFiles,
            cancellationToken,
            async (suPath, ct) =>
            {
                string rel = Path.GetRelativePath(fullBuildDir, suPath).Replace('\\', '/');
                if (rel.Contains("verilated", StringComparison.Ordinal) ||
                    rel.Contains("Vversal_", StringComparison.Ordinal) ||
                    rel.Contains("Vkintex_", StringComparison.Ordinal))
                {
                    return;
                }

                string[] lines = await File.ReadAllLinesAsync(suPath, ct).ConfigureAwait(false);
                bool inExamples = rel.Contains("examples/", StringComparison.Ordinal);

                foreach (string rawLine in lines)
                {
                    string[] parts = rawLine.Trim().Split('\t');
                    if (parts.Length < 3)
                    {
                        continue;
                    }

                    string fnLoc = parts[0];
                    string kind = parts[2];
                    Interlocked.Increment(ref fnCount);

                    if (!int.TryParse(parts[1], NumberStyles.Integer, CultureInfo.InvariantCulture, out int sizeBytes))
                    {
                        continue;
                    }

                    if (inExamples)
                    {
                        UpdateMax(ref peakBytes, sizeBytes);
                    }

                    if (kind == "dynamic")
                    {
                        errors.Add($"Unbounded dynamic stack frame forbidden in flight build: {fnLoc} ({sizeBytes} B {kind})");
                    }

                    if (!isAsan && inExamples && sizeBytes > maxFlightStackBytes)
                    {
                        errors.Add($"Flight stack budget exceeded ({sizeBytes} B > {maxFlightStackBytes} B): {fnLoc}");
                    }
                }
            });

        int auditedElfs = 0;
        Task elfTask = isAsan
            ? Task.CompletedTask
            : Parallel.ForEachAsync(
                elfPaths,
                cancellationToken,
                async (elfPath, ct) =>
                {
                    IReadOnlyList<string> elfErrors = await AuditZeroHeapElfAsync(elfPath, ct).ConfigureAwait(false);
                    foreach (string err in elfErrors)
                    {
                        errors.Add(err);
                    }

                    Interlocked.Increment(ref auditedElfs);
                });

        await Task.WhenAll(stackTask, elfTask).ConfigureAwait(false);

        return new FlightSafetyAuditReport(
            FunctionsChecked: fnCount,
            PeakFlightStackBytes: peakBytes,
            MaxFlightStackBytes: maxFlightStackBytes,
            ZeroHeapElfsVerified: auditedElfs,
            Errors: errors.OrderBy(e => e, StringComparer.Ordinal).ToList());
    }

    private static async Task<IReadOnlyList<string>> AuditZeroHeapElfAsync(string elfPath, CancellationToken cancellationToken)
    {
        if (!File.Exists(elfPath))
        {
            return [$"Flight ELF not found: {elfPath}"];
        }

        ProcessStartInfo psi = new("nm")
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
        };
        psi.ArgumentList.Add("-u");
        psi.ArgumentList.Add(elfPath);

        using Process proc = Process.Start(psi)
            ?? throw new InvalidOperationException("Failed to launch 'nm'.");

        Task<string> stdoutTask = proc.StandardOutput.ReadToEndAsync(cancellationToken);
        Task<string> stderrTask = proc.StandardError.ReadToEndAsync(cancellationToken);
        await proc.WaitForExitAsync(cancellationToken).ConfigureAwait(false);

        string stdout = await stdoutTask.ConfigureAwait(false);
        string stderr = await stderrTask.ConfigureAwait(false);
        if (proc.ExitCode != 0)
        {
            return [$"nm failed on {elfPath}: {stderr.Trim()}"];
        }

        List<string> errors = [];
        string elfName = Path.GetFileName(elfPath);
        foreach (string line in stdout.Split('\n'))
        {
            string[] tokens = line.Trim().Split(' ', StringSplitOptions.RemoveEmptyEntries);
            if (tokens.Length == 0)
            {
                continue;
            }

            string sym = tokens[^1].Split('@')[0];
            if (ForbiddenSymbols.Contains(sym))
            {
                errors.Add($"Forbidden heap/exception symbol '{sym}' referenced by {elfName}");
            }
        }

        return errors;
    }

    private static void UpdateMax(ref int target, int candidate)
    {
        int current = Volatile.Read(ref target);
        while (candidate > current)
        {
            int prior = Interlocked.CompareExchange(ref target, candidate, current);
            if (prior == current)
            {
                break;
            }

            current = prior;
        }
    }
}
