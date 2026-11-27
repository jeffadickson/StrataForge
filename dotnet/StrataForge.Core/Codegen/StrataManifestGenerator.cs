using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Xml.Linq;
using StrataForge.Core.Toml;

namespace StrataForge.Core.Codegen;

/// <summary>
/// Represents a single register/parameter parsed from a CCSDS SEDS XML instrument datasheet.
/// </summary>
/// <param name="Name">Symbolic register or parameter name.</param>
/// <param name="Type">Data type identifier (for example, <c>uint32</c>).</param>
/// <param name="ReadOnly"><see langword="true"/> if the register is hardware read-only.</param>
/// <param name="Offset">Byte offset from the slot SCSR base address.</param>
/// <param name="Unit">Engineering unit label (for example, <c>us</c> or <c>K</c>).</param>
/// <param name="CalC0">Constant polynomial calibration offset <c>c0</c>.</param>
/// <param name="CalC1">Linear polynomial calibration slope <c>c1</c>.</param>
public sealed record SedsRegisterInfo(
    string Name,
    string Type,
    bool ReadOnly,
    uint Offset,
    string Unit,
    double CalC0,
    double CalC1);

/// <summary>
/// Represents a parsed CCSDS SEDS XML package and its hardware register definitions.
/// </summary>
/// <param name="Package">The SEDS package identifier.</param>
/// <param name="Registers">Ordered read-only list of hardware registers in the package.</param>
public sealed record SedsPackageDocument(
    string Package,
    IReadOnlyList<SedsRegisterInfo> Registers);

/// <summary>
/// Summarizes the output of a synchronized multi-domain <see cref="StrataManifestGenerator"/> execution.
/// </summary>
/// <param name="MissionName">Declared mission identifier from <c>[mission].name</c>.</param>
/// <param name="TargetSilicon">Target radiation-hardened FPGA family (for example, <c>XQRVC1902</c> or <c>XQRKU060</c>).</param>
/// <param name="ManifestHash">Canonical 64-bit SHA-256 prefix hash synchronized across all emitted domains.</param>
/// <param name="PartitionCount">Number of declared hypervisor partitions.</param>
/// <param name="HwSlotCount">Number of declared FPGA spatial/DFX/AIE hardware slots.</param>
/// <param name="PoolCount">Number of declared XMPU/SMMU zero-copy frame pools.</param>
/// <param name="SedsCount">Number of imported CCSDS SEDS XML datasheets.</param>
/// <param name="EmittedFiles">Sorted list of artifact file paths written when emission was requested.</param>
public sealed record ManifestGenerationResult(
    string MissionName,
    string TargetSilicon,
    ulong ManifestHash,
    int PartitionCount,
    int HwSlotCount,
    int PoolCount,
    int SedsCount,
    IReadOnlyList<string> EmittedFiles);

/// <summary>
/// Single-source-of-truth STRATA-20 manifest validator and parallel multi-domain code generator.
/// </summary>
/// <remarks>
/// <para>
/// Implements requirements <c>STRATA-GEN-001</c> through <c>STRATA-GEN-004</c>:
/// </para>
/// <list type="bullet">
///   <item><description>Validates static CPU schedulability, physical DDR pool alignment &amp; non-overlap, NoC criticality isolation, pipeline DAG acyclicity, and unique command opcodes.</description></item>
///   <item><description>Imports CCSDS SEDS XML datasheets using <see cref="XDocument"/> (<c>System.Xml.Linq</c>).</description></item>
///   <item><description>Computes a canonical 64-bit <c>MANIFEST_HASH</c> (little-endian first 8 bytes of SHA-256 over manifest + SEDS inputs).</description></item>
///   <item><description>Emits C++20 ICD, Hypervisor tables, per-partition GNU ld linker scripts, SystemVerilog SCSR package, Vivado TCL, XTCE 1.2 XML (<see cref="XDocument"/>), and JSON ground dictionary concurrently via <see cref="Task.WhenAll(IEnumerable{Task})"/> and <see cref="Parallel.ForEachAsync{TSource}(IEnumerable{TSource}, CancellationToken, Func{TSource, CancellationToken, ValueTask})"/>.</description></item>
/// </list>
/// </remarks>
public static class StrataManifestGenerator
{
    private static readonly XNamespace XtceNs = "http://www.omg.org/spec/XTCE/20180204";
    private static readonly JsonSerializerOptions PrettyJsonOptions = new()
    {
        WriteIndented = true,
    };

    /// <summary>
    /// Computes the canonical 64-bit <c>MANIFEST_HASH</c> from raw manifest bytes and optional SEDS XML byte payloads.
    /// </summary>
    /// <param name="manifestBytes">Raw UTF-8 bytes of the <c>mission.toml</c> file.</param>
    /// <param name="sedsPayloads">Optional ordered sequence of raw SEDS XML file bytes.</param>
    /// <returns>The little-endian 64-bit prefix of the SHA-256 digest.</returns>
    public static ulong ComputeManifestHash(ReadOnlySpan<byte> manifestBytes, IReadOnlyList<byte[]>? sedsPayloads = null)
    {
        using IncrementalHash sha = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        sha.AppendData(manifestBytes);
        if (sedsPayloads is not null)
        {
            foreach (byte[] payload in sedsPayloads)
            {
                sha.AppendData(payload);
            }
        }

        Span<byte> digest = stackalloc byte[32];
        sha.GetHashAndReset(digest);
        return BinaryPrimitives.ReadUInt64LittleEndian(digest[..8]);
    }

    /// <summary>
    /// Parses a CCSDS SEDS XML instrument datasheet using <see cref="XDocument"/> into typed register metadata.
    /// </summary>
    /// <param name="sedsPath">Filesystem path to the SEDS XML file.</param>
    /// <returns>A parsed <see cref="SedsPackageDocument"/> containing the package name and registers.</returns>
    /// <exception cref="FileNotFoundException">Thrown if <paramref name="sedsPath"/> does not exist.</exception>
    public static SedsPackageDocument ParseSedsXml(string sedsPath)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(sedsPath);
        XDocument doc = XDocument.Load(sedsPath);
        XElement root = doc.Root ?? throw new InvalidDataException($"SEDS XML '{sedsPath}' has no root element.");
        XNamespace ns = root.Name.Namespace;

        XElement? pkgElem = root.Element(ns + "Package");
        string pkgName = pkgElem?.Attribute("name")?.Value ?? Path.GetFileNameWithoutExtension(sedsPath);

        List<SedsRegisterInfo> registers = [];
        foreach (XElement param in root.Descendants(ns + "Parameter"))
        {
            string offsetRaw = param.Attribute("offset")?.Value ?? "0x1000";
            uint offset = ParseUInt32Auto(offsetRaw);
            registers.Add(new SedsRegisterInfo(
                Name: param.Attribute("name")?.Value ?? "UNNAMED",
                Type: param.Attribute("type")?.Value ?? "uint32",
                ReadOnly: string.Equals(param.Attribute("readOnly")?.Value, "true", StringComparison.OrdinalIgnoreCase),
                Offset: offset,
                Unit: param.Attribute("unit")?.Value ?? string.Empty,
                CalC0: double.Parse(param.Attribute("cal_c0")?.Value ?? "0.0", CultureInfo.InvariantCulture),
                CalC1: double.Parse(param.Attribute("cal_c1")?.Value ?? "1.0", CultureInfo.InvariantCulture)));
        }

        return new SedsPackageDocument(pkgName, registers);
    }

    /// <summary>
    /// Performs static schedulability, spatial memory isolation, NoC criticality, DAG acyclicity, and opcode checks.
    /// </summary>
    /// <param name="data">The parsed root <see cref="TomlTable"/> of a <c>mission.toml</c> manifest.</param>
    /// <returns>A list of human-readable validation error strings (empty if valid).</returns>
    public static IReadOnlyList<string> ValidateManifest(TomlTable data)
    {
        ArgumentNullException.ThrowIfNull(data);
        List<string> errors = [];

        TomlTable? mission = data.GetTable("mission");
        if (mission is null)
        {
            return ["Missing [mission] section"];
        }

        long minorUs = mission.GetInt64("minor_frame_us", 0L);
        long majorUs = mission.GetInt64("major_frame_us", 0L);
        if (minorUs <= 0L || majorUs <= 0L || (majorUs % minorUs) != 0L)
        {
            errors.Add($"Invalid frame timing: major_frame_us ({majorUs}) must be a positive multiple of minor_frame_us ({minorUs})");
        }

        Dictionary<string, TomlTable> partById = new(StringComparer.Ordinal);
        Dictionary<string, long> cpuBudgets = new(StringComparer.Ordinal);
        foreach (TomlTable part in data.GetTableArray("partitions"))
        {
            string pid = part.GetString("id");
            if (string.IsNullOrEmpty(pid))
            {
                errors.Add("Partition entry missing 'id'");
                continue;
            }

            if (!partById.TryAdd(pid, part))
            {
                errors.Add($"Duplicate partition id: {pid}");
            }

            string cpu = part.GetString("cpu", "UNKNOWN");
            cpuBudgets[cpu] = cpuBudgets.GetValueOrDefault(cpu, 0L) + part.GetInt64("budget_us", 0L);
        }

        foreach ((string cpu, long totalUs) in cpuBudgets)
        {
            if (totalUs > minorUs)
            {
                errors.Add($"Schedulability violation on {cpu}: sum(budget_us)={totalUs} exceeds minor_frame_us={minorUs}");
            }
        }

        List<(ulong Start, ulong End, string Name)> ranges = [];
        foreach (TomlTable pool in data.GetTableArray("data_pools"))
        {
            string name = pool.GetString("name", "<unnamed>");
            ulong baseAddr = pool.GetUInt64("phys_base", 0UL);
            ulong count = pool.GetUInt64("slot_count", 0UL);
            ulong size = pool.GetUInt64("slot_bytes", 0UL);

            if (count == 0UL || (count & (count - 1UL)) != 0UL)
            {
                errors.Add($"Pool '{name}' slot_count ({count}) must be a power of two");
            }

            if (size == 0UL || (size % 4096UL) != 0UL)
            {
                errors.Add($"Pool '{name}' slot_bytes ({size}) must be 4 KB aligned");
            }

            ulong endAddr = baseAddr + (count * size);
            foreach ((ulong rStart, ulong rEnd, string rName) in ranges)
            {
                if (baseAddr < rEnd && rStart < endAddr)
                {
                    errors.Add($"Physical DDR overlap between pools '{name}' and '{rName}'");
                }
            }

            ranges.Add((baseAddr, endAddr, name));
        }

        HashSet<string> slotNames = new(StringComparer.Ordinal);
        foreach (TomlTable slot in data.GetTableArray("hw_slots"))
        {
            string sname = slot.GetString("name");
            slotNames.Add(sname);
            string owner = slot.GetString("owner_partition");
            if (!partById.TryGetValue(owner, out TomlTable? ownerPart))
            {
                errors.Add($"Slot '{sname}' references unknown owner_partition '{owner}'");
                continue;
            }

            string crit = ownerPart.GetString("criticality");
            string nocGrp = slot.GetString("noc_routing_group");
            if ((crit == "CLASS_C" || crit == "CLASS_D") && nocGrp == "CRITICAL_EXCL")
            {
                errors.Add($"Isolation violation: {crit} slot '{sname}' cannot use CRITICAL_EXCL NoC group");
            }
        }

        foreach (TomlTable pipe in data.GetTableArray("pipelines"))
        {
            string pname = pipe.GetString("name", "<unnamed>");
            HashSet<string> seen = new(StringComparer.Ordinal);
            foreach (string stage in pipe.GetStringArray("stages"))
            {
                if (!seen.Add(stage))
                {
                    errors.Add($"Pipeline '{pname}' contains cycle on stage '{stage}'");
                }

                if (stage.StartsWith("slot:", StringComparison.Ordinal))
                {
                    string target = stage[5..];
                    if (!slotNames.Contains(target))
                    {
                        errors.Add($"Pipeline '{pname}' references unknown hw_slot '{target}'");
                    }
                }
                else if (stage.StartsWith("part:", StringComparison.Ordinal))
                {
                    string target = stage[5..];
                    if (!partById.ContainsKey(target))
                    {
                        errors.Add($"Pipeline '{pname}' references unknown partition '{target}'");
                    }
                }
                else
                {
                    errors.Add($"Pipeline '{pname}' stage '{stage}' must start with 'slot:' or 'part:'");
                }
            }
        }

        HashSet<long> seenOpcodes = [];
        foreach (TomlTable cmd in data.GetTableArray("commands"))
        {
            long op = cmd.GetInt64("opcode", 0L);
            if (!seenOpcodes.Add(op))
            {
                errors.Add($"Duplicate command opcode: 0x{op:X4}");
            }
        }

        return errors;
    }

    /// <summary>
    /// Emits the C++20 mission ICD header (<c>strata_mission_icd.hpp</c>) with ROM command/telemetry tables and SEDS offsets.
    /// </summary>
    /// <param name="data">The validated manifest root table.</param>
    /// <param name="sedsDocs">Imported CCSDS SEDS package documents.</param>
    /// <param name="manifestHash">Canonical 64-bit <c>MANIFEST_HASH</c>.</param>
    /// <returns>Complete UTF-8 C++20 header source text.</returns>
    public static string EmitCppIcd(TomlTable data, IReadOnlyList<SedsPackageDocument> sedsDocs, ulong manifestHash)
    {
        TomlTable mission = data.GetTable("mission")!;
        IReadOnlyList<TomlTable> parts = data.GetTableArray("partitions");
        IReadOnlyList<TomlTable> slots = data.GetTableArray("hw_slots");
        IReadOnlyList<TomlTable> cmds = data.GetTableArray("commands");
        IReadOnlyList<TomlTable> tlms = data.GetTableArray("telemetry");

        Dictionary<string, int> partIdx = new(StringComparer.Ordinal);
        for (int i = 0; i < parts.Count; i++)
        {
            partIdx[parts[i].GetString("id")] = i;
        }

        StringBuilder sb = new(4096);
        sb.AppendLine("#pragma once");
        sb.AppendLine("// AUTO-GENERATED BY StrataForge.Core (strata-tools gen) — DO NOT EDIT");
        sb.AppendLine("#include <strata/core/types.hpp>");
        sb.AppendLine("#include <array>");
        sb.AppendLine("#include <cstdint>");
        sb.AppendLine();
        sb.AppendLine("namespace strata::generated {");
        sb.AppendLine();
        sb.AppendLine(CultureInfo.InvariantCulture, $"inline constexpr const char* kMissionName   = \"{mission.GetString("name")}\";");
        sb.AppendLine(CultureInfo.InvariantCulture, $"inline constexpr const char* kTargetSilicon = \"{mission.GetString("target_silicon")}\";");
        sb.AppendLine(CultureInfo.InvariantCulture, $"inline constexpr uint64_t    kManifestHash  = 0x{manifestHash:X16}ULL;");
        sb.AppendLine(CultureInfo.InvariantCulture, $"inline constexpr uint32_t    kMinorFrameUs  = {mission.GetInt64("minor_frame_us")}U;");
        sb.AppendLine(CultureInfo.InvariantCulture, $"inline constexpr uint32_t    kMajorFrameUs  = {mission.GetInt64("major_frame_us")}U;");
        sb.AppendLine();
        sb.AppendLine("enum class PartitionId : uint8_t {");
        for (int i = 0; i < parts.Count; i++)
        {
            sb.AppendLine(CultureInfo.InvariantCulture, $"    {parts[i].GetString("id")} = {i}U,");
        }
        sb.AppendLine("};");
        sb.AppendLine();

        sb.AppendLine("struct HwSlotDescriptor {");
        sb.AppendLine("    uint16_t slot_id;");
        sb.AppendLine("    uint8_t  owner_partition;");
        sb.AppendLine("    uint64_t scsr_phys_addr;");
        sb.AppendLine("    uint32_t quiesce_timeout_us;");
        sb.AppendLine("};");
        sb.AppendLine(CultureInfo.InvariantCulture, $"inline constexpr std::array<HwSlotDescriptor, {Math.Max(1, slots.Count)}> kHwSlots = {{{{");
        if (slots.Count > 0)
        {
            foreach (TomlTable s in slots)
            {
                int ownerIdx = partIdx.GetValueOrDefault(s.GetString("owner_partition"), 0);
                sb.AppendLine(
                    CultureInfo.InvariantCulture,
                    $"    HwSlotDescriptor{{{s.GetInt64("slot_id")}U, {ownerIdx}U, 0x{s.GetUInt64("scsr_phys_addr", 0UL):X16}ULL, {s.GetInt64("quiesce_timeout_us", 500L)}U}},");
            }
        }
        else
        {
            sb.AppendLine("    HwSlotDescriptor{0U, 0U, 0ULL, 0U},");
        }
        sb.AppendLine("}};");
        sb.AppendLine();

        sb.AppendLine("struct RomCommandEntry {");
        sb.AppendLine("    uint16_t opcode;");
        sb.AppendLine("    uint8_t  owner_partition;");
        sb.AppendLine("    uint16_t payload_bytes;");
        sb.AppendLine("};");
        sb.AppendLine(CultureInfo.InvariantCulture, $"inline constexpr std::array<RomCommandEntry, {Math.Max(1, cmds.Count)}> kRomCommands = {{{{");
        if (cmds.Count > 0)
        {
            foreach (TomlTable c in cmds)
            {
                int ownerIdx = partIdx.GetValueOrDefault(c.GetString("owner_partition"), 0);
                sb.AppendLine(
                    CultureInfo.InvariantCulture,
                    $"    RomCommandEntry{{0x{c.GetInt64("opcode"):X4}U, {ownerIdx}U, {c.GetInt64("payload_bytes", 0L)}U}},");
            }
        }
        else
        {
            sb.AppendLine("    RomCommandEntry{0U, 0U, 0U},");
        }
        sb.AppendLine("}};");
        sb.AppendLine();

        sb.AppendLine("struct RomTelemetryEntry {");
        sb.AppendLine("    uint16_t channel_id;");
        sb.AppendLine("    uint8_t  owner_partition;");
        sb.AppendLine("    double   cal_c0;");
        sb.AppendLine("    double   cal_c1;");
        sb.AppendLine("};");
        sb.AppendLine(CultureInfo.InvariantCulture, $"inline constexpr std::array<RomTelemetryEntry, {Math.Max(1, tlms.Count)}> kRomTelemetry = {{{{");
        if (tlms.Count > 0)
        {
            foreach (TomlTable t in tlms)
            {
                int ownerIdx = partIdx.GetValueOrDefault(t.GetString("owner_partition"), 0);
                string c0 = FormatCppDouble(t.GetDouble("cal_c0", 0.0));
                string c1 = FormatCppDouble(t.GetDouble("cal_c1", 1.0));
                sb.AppendLine(CultureInfo.InvariantCulture, $"    RomTelemetryEntry{{0x{t.GetInt64("channel_id"):X4}U, {ownerIdx}U, {c0}, {c1}}},");
            }
        }
        else
        {
            sb.AppendLine("    RomTelemetryEntry{0U, 0U, 0.0, 1.0},");
        }
        sb.AppendLine("}};");

        if (sedsDocs.Count > 0)
        {
            sb.AppendLine();
            sb.AppendLine("namespace seds {");
            foreach (SedsPackageDocument doc in sedsDocs)
            {
                foreach (SedsRegisterInfo reg in doc.Registers)
                {
                    sb.AppendLine(CultureInfo.InvariantCulture, $"inline constexpr uint32_t kReg_{doc.Package}_{reg.Name} = 0x{reg.Offset:X4}U;");
                }
            }
            sb.AppendLine("} // namespace seds");
        }

        sb.AppendLine();
        sb.AppendLine("} // namespace strata::generated");
        return sb.ToString();
    }

    /// <summary>
    /// Emits the C++20 static hypervisor partition configuration table (<c>strata_hyp_tables.hpp</c>).
    /// </summary>
    /// <param name="data">The validated manifest root table.</param>
    /// <param name="manifestHash">Canonical 64-bit <c>MANIFEST_HASH</c>.</param>
    /// <returns>Complete UTF-8 C++20 header text.</returns>
    public static string EmitHypTables(TomlTable data, ulong manifestHash)
    {
        IReadOnlyList<TomlTable> parts = data.GetTableArray("partitions");
        StringBuilder sb = new(2048);
        sb.AppendLine("#pragma once");
        sb.AppendLine("// AUTO-GENERATED BY StrataForge.Core (strata-tools gen) — DO NOT EDIT");
        sb.AppendLine("#include <strata/hyp/hypervisor.hpp>");
        sb.AppendLine("#include <array>");
        sb.AppendLine();
        sb.AppendLine("namespace strata::generated {");
        sb.AppendLine(CultureInfo.InvariantCulture, $"inline constexpr uint64_t kHypManifestHash = 0x{manifestHash:X16}ULL;");
        sb.AppendLine(CultureInfo.InvariantCulture, $"inline constexpr std::array<strata::hyp::PartitionConfig, {Math.Max(1, parts.Count)}> kPartitionTable = {{{{");

        Dictionary<string, int> cpuIds = new(StringComparer.Ordinal);
        for (int idx = 0; idx < parts.Count; idx++)
        {
            TomlTable p = parts[idx];
            string cpu = p.GetString("cpu", "CPU0");
            if (!cpuIds.TryGetValue(cpu, out int cpuNum))
            {
                cpuNum = cpuIds.Count;
                cpuIds[cpu] = cpuNum;
            }

            string crit = p.GetString("criticality", "CLASS_D");
            bool isB = crit == "CLASS_B";
            bool isFdir = p.GetString("id").Contains("fdir", StringComparison.OrdinalIgnoreCase);
            string priv = (isB && isFdir) ? "true" : "false";
            sb.AppendLine(
                CultureInfo.InvariantCulture,
                $"    strata::hyp::PartitionConfig{{.partition_id = {idx}U, .criticality = {CriticalityToCpp(crit)}, .cpu_id = {cpuNum}U, .privileged_slot_ctl = {priv}, .privileged_part_ctl = {priv}, .budget_us = {p.GetInt64("budget_us", 1000L)}U, .period_minor_frames = 1U, .offset_minor_frames = 0U}},");
        }

        sb.AppendLine("}};");
        sb.AppendLine("} // namespace strata::generated");
        return sb.ToString();
    }

    /// <summary>
    /// Emits a per-partition GNU ld linker script placing <c>.critical_store</c> and <c>.nv_flight_log</c> in NOLOAD memory.
    /// </summary>
    /// <param name="part">The partition TOML table.</param>
    /// <param name="index">Zero-based partition index used for fallback address calculation.</param>
    /// <param name="manifestHash">Canonical 64-bit <c>MANIFEST_HASH</c>.</param>
    /// <returns>Complete GNU ld linker script text.</returns>
    public static string EmitLinkerScript(TomlTable part, int index, ulong manifestHash)
    {
        ulong ramBase = part.GetUInt64("ram_base", 0x1000_0000UL + ((ulong)index * 0x0100_0000UL));
        ulong ramBytes = part.GetUInt64("ram_bytes", 1_048_576UL);
        ulong cdsBytes = part.GetUInt64("cds_bytes", 8_192UL);
        ulong cdsBase = ramBase + ramBytes - cdsBytes;
        ulong mainRam = ramBytes - cdsBytes;

        return $$"""
            /* AUTO-GENERATED STRATA-20 Linker Script for partition '{{part.GetString("id")}}' */
            /* MANIFEST_HASH = 0x{{manifestHash:X16}} */
            MEMORY {
                PART_RAM       (rwx) : ORIGIN = 0x{{ramBase:X8}}, LENGTH = 0x{{mainRam:X8}}
                CRITICAL_STORE (rw)  : ORIGIN = 0x{{cdsBase:X8}}, LENGTH = 0x{{cdsBytes:X8}}
            }

            SECTIONS {
                .text : { *(.text*) *(.rodata*) } > PART_RAM
                .data : { *(.data*) } > PART_RAM
                .bss (NOLOAD) : {
                    __bss_start = .;
                    *(.bss*) *(COMMON)
                    __bss_end = .;
                } > PART_RAM
                .critical_store (NOLOAD) : {
                    __critical_store_start = .;
                    KEEP(*(.critical_store*))
                    __critical_store_end = .;
                } > CRITICAL_STORE
                .nv_flight_log (NOLOAD) : {
                    __nv_flight_log_start = .;
                    KEEP(*(.nv_flight_log*))
                    __nv_flight_log_end = .;
                } > CRITICAL_STORE
            }
            """.Replace("\r\n", "\n", StringComparison.Ordinal) + "\n";
    }

    /// <summary>
    /// Emits the SystemVerilog SCSR register package (<c>scsr_pkg.sv</c>) stamped with <c>MANIFEST_HASH_LO/HI</c>.
    /// </summary>
    /// <param name="data">The validated manifest root table.</param>
    /// <param name="manifestHash">Canonical 64-bit <c>MANIFEST_HASH</c>.</param>
    /// <returns>Complete SystemVerilog package source text.</returns>
    public static string EmitSvScsrPackage(TomlTable data, ulong manifestHash)
    {
        uint lo = (uint)(manifestHash & 0xFFFF_FFFFUL);
        uint hi = (uint)((manifestHash >> 32) & 0xFFFF_FFFFUL);
        IReadOnlyList<TomlTable> slots = data.GetTableArray("hw_slots");

        StringBuilder sb = new(1024);
        sb.AppendLine("// AUTO-GENERATED BY StrataForge.Core (strata-tools gen) — DO NOT EDIT");
        sb.AppendLine("package strata_scsr_pkg;");
        sb.AppendLine(CultureInfo.InvariantCulture, $"  localparam logic [31:0] MANIFEST_HASH_LO = 32'h{lo:X8};");
        sb.AppendLine(CultureInfo.InvariantCulture, $"  localparam logic [31:0] MANIFEST_HASH_HI = 32'h{hi:X8};");
        sb.AppendLine(CultureInfo.InvariantCulture, $"  localparam int unsigned NUM_HW_SLOTS     = {slots.Count};");
        foreach (TomlTable s in slots)
        {
            string sname = s.GetString("name").ToUpperInvariant();
            ulong addr = s.GetUInt64("scsr_phys_addr", 0UL);
            sb.AppendLine(CultureInfo.InvariantCulture, $"  localparam logic [63:0] SLOT_{sname}_BASE = 64'h{addr:X16};");
            sb.AppendLine(CultureInfo.InvariantCulture, $"  localparam int unsigned SLOT_{sname}_ID   = {s.GetInt64("slot_id")};");
        }
        sb.AppendLine("endpackage : strata_scsr_pkg");
        return sb.ToString();
    }

    /// <summary>
    /// Emits the Vivado XMPU/SMMU/NoC QoS isolation TCL configuration script (<c>strata_xmpu_noc.tcl</c>).
    /// </summary>
    /// <param name="data">The validated manifest root table.</param>
    /// <param name="manifestHash">Canonical 64-bit <c>MANIFEST_HASH</c>.</param>
    /// <returns>Complete Vivado TCL script text.</returns>
    public static string EmitVivadoTcl(TomlTable data, ulong manifestHash)
    {
        TomlTable mission = data.GetTable("mission")!;
        StringBuilder sb = new(1024);
        sb.AppendLine("# AUTO-GENERATED STRATA-20 Vivado XMPU/SMMU/NoC QoS Isolation Script");
        sb.AppendLine(CultureInfo.InvariantCulture, $"# Mission: {mission.GetString("name")} | Silicon: {mission.GetString("target_silicon")} | MANIFEST_HASH: 0x{manifestHash:X16}");
        sb.AppendLine(CultureInfo.InvariantCulture, $"set STRATA_MANIFEST_HASH 0x{manifestHash:X16}");
        sb.AppendLine();

        foreach (TomlTable pool in data.GetTableArray("data_pools"))
        {
            ulong baseAddr = pool.GetUInt64("phys_base");
            ulong size = pool.GetUInt64("slot_count") * pool.GetUInt64("slot_bytes");
            string readers = string.Join(", ", pool.GetStringArray("readers"));
            sb.AppendLine(CultureInfo.InvariantCulture, $"# XMPU DDR Pool '{pool.GetString("name")}' writer=slot:{pool.GetInt64("writer_slot")} readers=[{readers}]");
            sb.AppendLine(CultureInfo.InvariantCulture, $"puts \"STRATA XMPU_REGION {pool.GetString("name")} BASE=0x{baseAddr:X16} SIZE=0x{size:X16}\"");
        }

        foreach (TomlTable slot in data.GetTableArray("hw_slots"))
        {
            string grp = slot.GetString("noc_routing_group", "SCIENCE_BEST_EFFORT");
            sb.AppendLine(CultureInfo.InvariantCulture, $"# Slot {slot.GetInt64("slot_id")} ({slot.GetString("name")}): mode={slot.GetString("mode")} NoC={grp}");
            sb.AppendLine(CultureInfo.InvariantCulture, $"puts \"STRATA NOC_QOS {slot.GetString("name")} GROUP={grp} QUIESCE_US={slot.GetInt64("quiesce_timeout_us", 500L)}\"");
        }

        return sb.ToString();
    }

    /// <summary>
    /// Builds a standards-compliant CCSDS XTCE 1.2 XML ground dictionary using <see cref="XDocument"/> (<c>System.Xml.Linq</c>).
    /// </summary>
    /// <param name="data">The validated manifest root table.</param>
    /// <param name="sedsDocs">Imported CCSDS SEDS package documents.</param>
    /// <param name="manifestHash">Canonical 64-bit <c>MANIFEST_HASH</c>.</param>
    /// <returns>An indented <see cref="XDocument"/> conforming to the XTCE 1.2 namespace.</returns>
    public static XDocument BuildXtceDocument(TomlTable data, IReadOnlyList<SedsPackageDocument> sedsDocs, ulong manifestHash)
    {
        TomlTable mission = data.GetTable("mission")!;

        XElement paramTypes = new(XtceNs + "ParameterTypeSet");
        XElement paramSet = new(XtceNs + "ParameterSet");

        foreach (TomlTable t in data.GetTableArray("telemetry"))
        {
            string tname = t.GetString("name");
            string unit = t.GetString("unit");
            XElement ptype = new(
                XtceNs + "IntegerParameterType",
                new XAttribute("name", $"{tname}_Type"),
                new XAttribute("signed", "false"));

            if (!string.IsNullOrEmpty(unit))
            {
                ptype.Add(new XElement(XtceNs + "UnitSet", new XElement(XtceNs + "Unit", unit)));
            }

            string bits = t.GetString("type") == "uint64" ? "64" : "16";
            ptype.Add(
                new XElement(
                    XtceNs + "IntegerDataEncoding",
                    new XAttribute("sizeInBits", bits),
                    new XElement(
                        XtceNs + "DefaultCalibrator",
                        new XElement(
                            XtceNs + "PolynomialCalibrator",
                            new XElement(XtceNs + "Term", new XAttribute("exponent", "0"), new XAttribute("coefficient", t.GetDouble("cal_c0", 0.0).ToString("G", CultureInfo.InvariantCulture))),
                            new XElement(XtceNs + "Term", new XAttribute("exponent", "1"), new XAttribute("coefficient", t.GetDouble("cal_c1", 1.0).ToString("G", CultureInfo.InvariantCulture)))))));

            paramTypes.Add(ptype);
            paramSet.Add(
                new XElement(
                    XtceNs + "Parameter",
                    new XAttribute("name", tname),
                    new XAttribute("parameterTypeRef", $"{tname}_Type"),
                    new XAttribute("shortDescription", $"channel_id=0x{t.GetInt64("channel_id"):X4}")));
        }

        foreach (SedsPackageDocument doc in sedsDocs)
        {
            foreach (SedsRegisterInfo reg in doc.Registers)
            {
                string rname = $"{doc.Package}_{reg.Name}";
                XElement ptype = new(
                    XtceNs + "IntegerParameterType",
                    new XAttribute("name", $"{rname}_Type"),
                    new XAttribute("signed", "false"));

                if (!string.IsNullOrEmpty(reg.Unit))
                {
                    ptype.Add(new XElement(XtceNs + "UnitSet", new XElement(XtceNs + "Unit", reg.Unit)));
                }

                paramTypes.Add(ptype);
                paramSet.Add(
                    new XElement(
                        XtceNs + "Parameter",
                        new XAttribute("name", rname),
                        new XAttribute("parameterTypeRef", $"{rname}_Type"),
                        new XAttribute("shortDescription", $"seds_offset=0x{reg.Offset:X4}")));
            }
        }

        XElement mcSet = new(XtceNs + "MetaCommandSet");
        foreach (TomlTable c in data.GetTableArray("commands"))
        {
            mcSet.Add(
                new XElement(
                    XtceNs + "MetaCommand",
                    new XAttribute("name", c.GetString("name")),
                    new XAttribute(
                        "shortDescription",
                        $"opcode=0x{c.GetInt64("opcode"):X4} role={c.GetString("min_role", "OPERATOR")} bytes={c.GetInt64("payload_bytes", 0L)}")));
        }

        XElement root = new(
            XtceNs + "SpaceSystem",
            new XAttribute("name", mission.GetString("name")),
            new XAttribute("shortDescription", $"STRATA-20 Auto-Generated Dictionary MANIFEST_HASH=0x{manifestHash:X16}"),
            new XElement(XtceNs + "TelemetryMetaData", paramTypes, paramSet),
            new XElement(XtceNs + "CommandMetaData", mcSet));

        return new XDocument(new XDeclaration("1.0", "UTF-8", null), root);
    }

    /// <summary>
    /// Emits the unified JSON ground dictionary (<c>mission_dict.json</c>) consumed by StrataGround and <c>strata-tools ops</c>.
    /// </summary>
    /// <param name="data">The validated manifest root table.</param>
    /// <param name="sedsDocs">Imported CCSDS SEDS package documents.</param>
    /// <param name="manifestHash">Canonical 64-bit <c>MANIFEST_HASH</c>.</param>
    /// <returns>Indented UTF-8 JSON document string.</returns>
    public static string EmitJsonDictionary(TomlTable data, IReadOnlyList<SedsPackageDocument> sedsDocs, ulong manifestHash)
    {
        var payload = new Dictionary<string, object?>(StringComparer.Ordinal)
        {
            ["mission"] = data.GetTable("mission")!.ToPlainDictionary(),
            ["manifest_hash"] = $"0x{manifestHash:X16}",
            ["partitions"] = data.GetTableArray("partitions").Select(p => p.ToPlainDictionary()).ToList(),
            ["hw_slots"] = data.GetTableArray("hw_slots").Select(s => s.ToPlainDictionary()).ToList(),
            ["data_pools"] = data.GetTableArray("data_pools").Select(p => p.ToPlainDictionary()).ToList(),
            ["pipelines"] = data.GetTableArray("pipelines").Select(p => p.ToPlainDictionary()).ToList(),
            ["commands"] = data.GetTableArray("commands").Select(c => c.ToPlainDictionary()).ToList(),
            ["telemetry"] = data.GetTableArray("telemetry").Select(t => t.ToPlainDictionary()).ToList(),
            ["seds_packages"] = sedsDocs.Select(d => new Dictionary<string, object?>(StringComparer.Ordinal)
            {
                ["package"] = d.Package,
                ["registers"] = d.Registers.Select(r => new Dictionary<string, object?>(StringComparer.Ordinal)
                {
                    ["name"] = r.Name,
                    ["type"] = r.Type,
                    ["read_only"] = r.ReadOnly,
                    ["offset"] = r.Offset,
                    ["unit"] = r.Unit,
                    ["cal_c0"] = r.CalC0,
                    ["cal_c1"] = r.CalC1,
                }).ToList(),
            }).ToList(),
        };

        return JsonSerializer.Serialize(payload, PrettyJsonOptions) + "\n";
    }

    /// <summary>
    /// Validates a manifest, parses SEDS XML datasheets concurrently, and emits all multi-domain artifacts in parallel.
    /// </summary>
    /// <param name="manifestPath">Path to the <c>mission.toml</c> manifest file.</param>
    /// <param name="sedsPaths">Optional list of CCSDS SEDS XML file paths.</param>
    /// <param name="emitDirectory">Optional output directory; if <see langword="null"/>, performs validation only.</param>
    /// <param name="cancellationToken">Cancellation token for cooperative async cancellation.</param>
    /// <returns>A tuple containing either a <see cref="ManifestGenerationResult"/> or a non-empty list of validation errors.</returns>
    public static async Task<(ManifestGenerationResult? Result, IReadOnlyList<string> Errors)> GenerateAsync(
        string manifestPath,
        IReadOnlyList<string>? sedsPaths = null,
        string? emitDirectory = null,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(manifestPath);

        byte[] rawBytes = await File.ReadAllBytesAsync(manifestPath, cancellationToken).ConfigureAwait(false);
        TomlTable data = MissionTomlParser.Parse(Encoding.UTF8.GetString(rawBytes));
        IReadOnlyList<string> errors = ValidateManifest(data);
        if (errors.Count > 0)
        {
            return (null, errors);
        }

        List<string> sedsList = sedsPaths is null ? [] : [.. sedsPaths];
        byte[][] sedsBytes = new byte[sedsList.Count][];
        SedsPackageDocument[] sedsDocs = new SedsPackageDocument[sedsList.Count];

        if (sedsList.Count > 0)
        {
            await Parallel.ForEachAsync(
                Enumerable.Range(0, sedsList.Count),
                cancellationToken,
                async (idx, ct) =>
                {
                    sedsBytes[idx] = await File.ReadAllBytesAsync(sedsList[idx], ct).ConfigureAwait(false);
                    sedsDocs[idx] = ParseSedsXml(sedsList[idx]);
                }).ConfigureAwait(false);
        }

        ulong manifestHash = ComputeManifestHash(rawBytes, sedsBytes);
        TomlTable mission = data.GetTable("mission")!;
        IReadOnlyList<TomlTable> parts = data.GetTableArray("partitions");
        ConcurrentBag<string> writtenFiles = [];

        if (!string.IsNullOrWhiteSpace(emitDirectory))
        {
            Directory.CreateDirectory(emitDirectory);

            async Task WriteArtifactAsync(string relativeName, string content)
            {
                string fullPath = Path.Combine(emitDirectory, relativeName);
                await File.WriteAllTextAsync(fullPath, content, new UTF8Encoding(false), cancellationToken).ConfigureAwait(false);
                writtenFiles.Add(fullPath);
            }

            List<Task> tasks =
            [
                Task.Run(() => WriteArtifactAsync("strata_mission_icd.hpp", EmitCppIcd(data, sedsDocs, manifestHash)), cancellationToken),
                Task.Run(() => WriteArtifactAsync("strata_hyp_tables.hpp", EmitHypTables(data, manifestHash)), cancellationToken),
                Task.Run(() => WriteArtifactAsync("scsr_pkg.sv", EmitSvScsrPackage(data, manifestHash)), cancellationToken),
                Task.Run(() => WriteArtifactAsync("strata_xmpu_noc.tcl", EmitVivadoTcl(data, manifestHash)), cancellationToken),
                Task.Run(() =>
                {
                    XDocument xtce = BuildXtceDocument(data, sedsDocs, manifestHash);
                    string xmlText = xtce.Declaration + "\n" + xtce.ToString(SaveOptions.None) + "\n";
                    return WriteArtifactAsync("mission_xtce.xml", xmlText);
                }, cancellationToken),
                Task.Run(() => WriteArtifactAsync("mission_dict.json", EmitJsonDictionary(data, sedsDocs, manifestHash)), cancellationToken),
            ];

            for (int i = 0; i < parts.Count; i++)
            {
                int idxCopy = i;
                TomlTable partCopy = parts[i];
                tasks.Add(Task.Run(
                    () => WriteArtifactAsync($"link_{partCopy.GetString("id")}.ld", EmitLinkerScript(partCopy, idxCopy, manifestHash)),
                    cancellationToken));
            }

            await Task.WhenAll(tasks).ConfigureAwait(false);
        }

        ManifestGenerationResult result = new(
            MissionName: mission.GetString("name"),
            TargetSilicon: mission.GetString("target_silicon"),
            ManifestHash: manifestHash,
            PartitionCount: parts.Count,
            HwSlotCount: data.GetTableArray("hw_slots").Count,
            PoolCount: data.GetTableArray("data_pools").Count,
            SedsCount: sedsDocs.Length,
            EmittedFiles: writtenFiles.OrderBy(p => p, StringComparer.Ordinal).ToList());

        return (result, Array.Empty<string>());
    }

    private static string CriticalityToCpp(string crit) => crit switch
    {
        "CLASS_B" => "strata::Criticality::ClassB",
        "CLASS_C" => "strata::Criticality::ClassC",
        "CLASS_D" => "strata::Criticality::ClassD",
        _ => "strata::Criticality::ClassD",
    };

    private static string FormatCppDouble(double value)
    {
        string text = value.ToString("G17", CultureInfo.InvariantCulture);
        return (!text.Contains('.', StringComparison.Ordinal) && !text.Contains('E', StringComparison.OrdinalIgnoreCase))
            ? text + ".0"
            : text;
    }

    private static uint ParseUInt32Auto(string raw)
    {
        string trimmed = raw.Trim();
        return trimmed.StartsWith("0x", StringComparison.OrdinalIgnoreCase)
            ? uint.Parse(trimmed[2..], NumberStyles.HexNumber, CultureInfo.InvariantCulture)
            : uint.Parse(trimmed, NumberStyles.Integer, CultureInfo.InvariantCulture);
    }
}
