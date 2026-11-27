using System.Diagnostics;

namespace StrataForge.Core.Audit;

/// <summary>
/// Result of a bare-metal cross-compilation or QEMU hardware execution verification run.
/// </summary>
/// <param name="Success"><see langword="true"/> if verification succeeded or cleanly skipped when cross tools were absent.</param>
/// <param name="Summary">Human-readable verification summary line.</param>
/// <param name="ErrorDetail">Detailed compiler or emulator stderr/stdout on failure.</param>
public sealed record CrossVerificationResult(
    bool Success,
    string Summary,
    string? ErrorDetail = null);

/// <summary>
/// Concurrent bare-metal cross-compilation verifier for Versal AArch64 EL2, Lockstep Cortex-R5F,
/// and live QEMU AArch64 Cortex-A72 EL2 hypervisor execution.
/// </summary>
/// <remarks>
/// <para>
/// Implements <c>STRATA-HYP-009</c> and <c>STRATA-VER-006</c>:
/// </para>
/// <list type="bullet">
///   <item><description><see cref="VerifyBareMetalCrossAsync"/> compiles Versal Cortex-A72 EL2 ELF and ARMv7-R Lockstep Cortex-R5F firmware concurrently via <see cref="Task.WhenAll(IEnumerable{Task})"/>.</description></item>
///   <item><description><see cref="VerifyQemuAarch64El2Async"/> compiles, links, and executes the bare-metal AArch64 EL2 hypervisor live under <c>qemu-system-aarch64 -machine virt,virtualization=on -cpu cortex-a72</c>.</description></item>
/// </list>
/// </remarks>
public static class BareMetalCrossVerifier
{
    /// <summary>
    /// Concurrently cross-compiles and audits the freestanding Versal AArch64 EL2 ELF and Lockstep Cortex-R5F FDIR object.
    /// </summary>
    /// <param name="repoRoot">Path to the StrataForge repository root.</param>
    /// <param name="cancellationToken">Cancellation token for cooperative async cancellation.</param>
    /// <returns>A <see cref="CrossVerificationResult"/> summarizing both target proofs.</returns>
    public static async Task<CrossVerificationResult> VerifyBareMetalCrossAsync(
        string repoRoot,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(repoRoot);
        string fullRoot = Path.GetFullPath(repoRoot);
        string tempDir = Directory.CreateTempSubdirectory("strata_cross_").FullName;

        try
        {
            Task<(bool Ok, string Msg, string? Err)> a64Task = VerifyAarch64El2Async(fullRoot, tempDir, cancellationToken);
            Task<(bool Ok, string Msg, string? Err)> r5Task = VerifyArmR5Async(fullRoot, tempDir, cancellationToken);

            await Task.WhenAll(a64Task, r5Task).ConfigureAwait(false);
            var a64 = a64Task.Result;
            var r5 = r5Task.Result;

            if (!a64.Ok)
            {
                return new CrossVerificationResult(false, "FAIL: Versal AArch64 EL2 cross build failed", a64.Err);
            }

            if (!r5.Ok)
            {
                return new CrossVerificationResult(false, "FAIL: Versal Cortex-R5F cross build failed", r5.Err);
            }

            return new CrossVerificationResult(
                true,
                $"OK: Bare-Metal Cross Audit | Versal-A72-EL2={a64.Msg} | Versal-R5F={r5.Msg}");
        }
        finally
        {
            TryDeleteDirectory(tempDir);
        }
    }

    /// <summary>
    /// Compiles, links, and executes the bare-metal AArch64 EL2 hypervisor self-test live on QEMU Cortex-A72.
    /// </summary>
    /// <param name="repoRoot">Path to the StrataForge repository root.</param>
    /// <param name="cancellationToken">Cancellation token for cooperative async cancellation.</param>
    /// <returns>A <see cref="CrossVerificationResult"/> containing the live PL011 UART verification transcript.</returns>
    public static async Task<CrossVerificationResult> VerifyQemuAarch64El2Async(
        string repoRoot,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(repoRoot);
        string? cxx = FindExecutable("aarch64-linux-gnu-g++");
        string? qemu = FindExecutable("qemu-system-aarch64");
        if (cxx is null || qemu is null)
        {
            return new CrossVerificationResult(true, "SKIP: aarch64-linux-gnu-g++ or qemu-system-aarch64 not installed");
        }

        string fullRoot = Path.GetFullPath(repoRoot);
        string tempDir = Directory.CreateTempSubdirectory("strata_qemu_el2_").FullName;

        try
        {
            string bootO = Path.Combine(tempDir, "qemu_el2_boot.o");
            string mainO = Path.Combine(tempDir, "qemu_el2_selftest.o");
            string elf = Path.Combine(tempDir, "strata_qemu_el2.elf");

            Task<ProcessRunResult> asmTask = RunProcessAsync(
                cxx,
                ["-march=armv8-a", "-c", Path.Combine(fullRoot, "hypervisor/arch/aarch64/qemu_el2_boot.S"), "-o", bootO],
                TimeSpan.FromSeconds(20),
                cancellationToken);

            Task<ProcessRunResult> compileTask = RunProcessAsync(
                cxx,
                [
                    "-std=c++20", "-march=armv8-a", "-mstrict-align", "-ffreestanding",
                    "-fno-exceptions", "-fno-rtti", "-fno-threadsafe-statics", "-O2",
                    "-Wall", "-Wextra", "-Werror",
                    $"-I{Path.Combine(fullRoot, "hypervisor/include")}",
                    $"-I{Path.Combine(fullRoot, "hypervisor/arch")}",
                    $"-I{Path.Combine(fullRoot, "middleware/include")}",
                    "-c", Path.Combine(fullRoot, "hypervisor/core/qemu_el2_selftest.cpp"),
                    "-o", mainO,
                ],
                TimeSpan.FromSeconds(20),
                cancellationToken);

            await Task.WhenAll(asmTask, compileTask).ConfigureAwait(false);
            if (asmTask.Result.ExitCode != 0)
            {
                return new CrossVerificationResult(false, "FAIL: qemu_el2_boot.S assembly failed", asmTask.Result.Stderr);
            }

            if (compileTask.Result.ExitCode != 0)
            {
                return new CrossVerificationResult(false, "FAIL: qemu_el2_selftest.cpp compile failed", compileTask.Result.Stderr);
            }

            ProcessRunResult linkRes = await RunProcessAsync(
                cxx,
                [
                    "-nostdlib", "-static",
                    $"-Wl,-T,{Path.Combine(fullRoot, "hypervisor/arch/aarch64/qemu_el2_virt.ld")}",
                    bootO, mainO, "-o", elf,
                ],
                TimeSpan.FromSeconds(20),
                cancellationToken).ConfigureAwait(false);

            if (linkRes.ExitCode != 0)
            {
                return new CrossVerificationResult(false, "FAIL: QEMU EL2 ELF link failed", linkRes.Stderr);
            }

            ProcessRunResult qemuRes = await RunProcessAsync(
                qemu,
                [
                    "-machine", "virt,virtualization=on",
                    "-cpu", "cortex-a72",
                    "-m", "128M",
                    "-nographic",
                    "-semihosting-config", "enable=on,target=native",
                    "-kernel", elf,
                ],
                TimeSpan.FromSeconds(15),
                cancellationToken).ConfigureAwait(false);

            if (qemuRes.ExitCode != 0 ||
                !qemuRes.Stdout.Contains("CurrentEL=2", StringComparison.Ordinal) ||
                !qemuRes.Stdout.Contains("FAR_EL2=0x0000000080000000", StringComparison.Ordinal) ||
                !qemuRes.Stdout.Contains("PASS: QEMU AArch64 EL2 Execution verified", StringComparison.Ordinal))
            {
                return new CrossVerificationResult(
                    false,
                    $"FAIL: QEMU AArch64 EL2 execution failed (exit={qemuRes.ExitCode})",
                    $"{qemuRes.Stdout}\n{qemuRes.Stderr}");
            }

            return new CrossVerificationResult(true, qemuRes.Stdout.Trim());
        }
        finally
        {
            TryDeleteDirectory(tempDir);
        }
    }

    private static async Task<(bool Ok, string Msg, string? Err)> VerifyAarch64El2Async(
        string fullRoot,
        string tempDir,
        CancellationToken cancellationToken)
    {
        string? cxx = FindExecutable("aarch64-linux-gnu-g++");
        string? objdump = FindExecutable("aarch64-linux-gnu-objdump");
        string? readelf = FindExecutable("aarch64-linux-gnu-readelf");
        if (cxx is null || objdump is null || readelf is null)
        {
            return (true, "SKIP (aarch64-linux-gnu-g++ not installed)", null);
        }

        string vecO = Path.Combine(tempDir, "el2_vectors.o");
        string mainO = Path.Combine(tempDir, "el2_main.o");
        string elf = Path.Combine(tempDir, "strata_versal_el2.elf");

        Task<ProcessRunResult> vecTask = RunProcessAsync(
            cxx,
            ["-march=armv8-a", "-c", Path.Combine(fullRoot, "hypervisor/arch/aarch64/el2_vectors.S"), "-o", vecO],
            TimeSpan.FromSeconds(20),
            cancellationToken);

        Task<ProcessRunResult> mainTask = RunProcessAsync(
            cxx,
            [
                "-std=c++20", "-march=armv8-a", "-mstrict-align", "-ffreestanding",
                "-fno-exceptions", "-fno-rtti", "-fno-threadsafe-statics", "-O2",
                "-Wall", "-Wextra", "-Werror",
                $"-I{Path.Combine(fullRoot, "hypervisor/include")}",
                $"-I{Path.Combine(fullRoot, "hypervisor/arch")}",
                $"-I{Path.Combine(fullRoot, "middleware/include")}",
                $"-I{Path.Combine(fullRoot, "services/fdir_slot_mgr/include")}",
                "-c", Path.Combine(fullRoot, "hypervisor/core/el2_hypervisor_main.cpp"),
                "-o", mainO,
            ],
            TimeSpan.FromSeconds(20),
            cancellationToken);

        await Task.WhenAll(vecTask, mainTask).ConfigureAwait(false);
        if (vecTask.Result.ExitCode != 0)
        {
            return (false, string.Empty, vecTask.Result.Stderr);
        }

        if (mainTask.Result.ExitCode != 0)
        {
            return (false, string.Empty, mainTask.Result.Stderr);
        }

        ProcessRunResult linkRes = await RunProcessAsync(
            cxx,
            [
                "-nostdlib", "-static",
                $"-Wl,-T,{Path.Combine(fullRoot, "hypervisor/arch/aarch64/el2_hyp.ld")}",
                vecO, mainO, "-o", elf,
            ],
            TimeSpan.FromSeconds(20),
            cancellationToken).ConfigureAwait(false);

        if (linkRes.ExitCode != 0)
        {
            return (false, string.Empty, linkRes.Stderr);
        }

        Task<ProcessRunResult> disTask = RunProcessAsync(objdump, ["-d", elf], TimeSpan.FromSeconds(20), cancellationToken);
        Task<ProcessRunResult> secTask = RunProcessAsync(readelf, ["-S", elf], TimeSpan.FromSeconds(20), cancellationToken);
        await Task.WhenAll(disTask, secTask).ConfigureAwait(false);

        string dis = disTask.Result.Stdout.ToLowerInvariant();
        foreach (string instr in new[] { "vbar_el2", "vttbr_el2", "eret", "cntpct_el0" })
        {
            if (!dis.Contains(instr, StringComparison.Ordinal))
            {
                return (false, string.Empty, $"Missing expected AArch64 EL2 instruction '{instr}' in {elf}");
            }
        }

        string secs = secTask.Result.Stdout;
        if (!secs.Contains(".critical_store", StringComparison.Ordinal) ||
            !secs.Contains(".nv_flight_log", StringComparison.Ordinal) ||
            secs.Contains(".plt", StringComparison.Ordinal))
        {
            return (false, string.Empty, $"Section layout check failed for {elf}");
        }

        long sizeBytes = new FileInfo(elf).Length;
        return (true, $"PASS (AArch64 EL2 ELF {sizeBytes}B: vbar_el2/vttbr_el2/eret/cntpct_el0 + .nv_flight_log verified, zero libc/PLT)", null);
    }

    private static async Task<(bool Ok, string Msg, string? Err)> VerifyArmR5Async(
        string fullRoot,
        string tempDir,
        CancellationToken cancellationToken)
    {
        string? cxx = FindExecutable("arm-none-eabi-g++");
        if (cxx is null)
        {
            return (true, "SKIP (arm-none-eabi-g++ not installed)", null);
        }

        string r5O = Path.Combine(tempDir, "r5_fdir.o");
        ProcessRunResult res = await RunProcessAsync(
            cxx,
            [
                "-std=c++20", "-mcpu=cortex-r5", "-marm", "-ffreestanding",
                "-fno-exceptions", "-fno-rtti", "-fno-threadsafe-statics", "-O2",
                "-Wall", "-Wextra", "-Werror",
                $"-I{Path.Combine(fullRoot, "hypervisor/include")}",
                $"-I{Path.Combine(fullRoot, "hypervisor/arch")}",
                $"-I{Path.Combine(fullRoot, "middleware/include")}",
                $"-I{Path.Combine(fullRoot, "services/fdir_slot_mgr/include")}",
                "-c", Path.Combine(fullRoot, "hypervisor/core/r5_fdir_main.cpp"),
                "-o", r5O,
            ],
            TimeSpan.FromSeconds(20),
            cancellationToken).ConfigureAwait(false);

        if (res.ExitCode != 0)
        {
            return (false, string.Empty, res.Stderr);
        }

        long sizeBytes = new FileInfo(r5O).Length;
        return (true, $"PASS (ARMv7-R Lockstep Cortex-R5F obj {sizeBytes}B verified, -Werror clean)", null);
    }

    /// <summary>
    /// Searches the current process <c>PATH</c> for an executable file name.
    /// </summary>
    /// <param name="executableName">Binary name (for example, <c>qemu-system-aarch64</c>).</param>
    /// <returns>The resolved full path if found; otherwise, <see langword="null"/>.</returns>
    public static string? FindExecutable(string executableName)
    {
        string pathEnv = Environment.GetEnvironmentVariable("PATH") ?? string.Empty;
        foreach (string dir in pathEnv.Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries))
        {
            string candidate = Path.Combine(dir, executableName);
            if (File.Exists(candidate))
            {
                return candidate;
            }
        }

        return null;
    }

    private sealed record ProcessRunResult(int ExitCode, string Stdout, string Stderr);

    private static async Task<ProcessRunResult> RunProcessAsync(
        string fileName,
        IReadOnlyList<string> args,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        using CancellationTokenSource linked = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        linked.CancelAfter(timeout);

        ProcessStartInfo psi = new(fileName)
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
        };
        foreach (string a in args)
        {
            psi.ArgumentList.Add(a);
        }

        using Process proc = Process.Start(psi)
            ?? throw new InvalidOperationException($"Failed to start '{fileName}'.");

        Task<string> outTask = proc.StandardOutput.ReadToEndAsync(linked.Token);
        Task<string> errTask = proc.StandardError.ReadToEndAsync(linked.Token);

        try
        {
            await proc.WaitForExitAsync(linked.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            try
            {
                proc.Kill(entireProcessTree: true);
            }
            catch
            {
                // Best-effort cleanup on timeout
            }

            throw;
        }

        return new ProcessRunResult(proc.ExitCode, await outTask.ConfigureAwait(false), await errTask.ConfigureAwait(false));
    }

    private static void TryDeleteDirectory(string path)
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
