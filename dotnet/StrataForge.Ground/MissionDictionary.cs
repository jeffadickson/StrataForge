using System.Collections.Frozen;
using System.Globalization;
using System.Text.Json;

namespace StrataForge.Ground;

/// <summary>
/// Immutable definition of a spacecraft command declared in <c>mission_dict.json</c>.
/// </summary>
/// <param name="Opcode">16-bit unique command opcode.</param>
/// <param name="Name">Symbolic command mnemonic (for example, <c>HSI_SET_INTEGRATION</c>).</param>
/// <param name="OwnerPartition">Target flight partition owning command execution.</param>
/// <param name="MinRole">Minimum operator privilege role required (for example, <c>OPERATOR</c> or <c>FLIGHT_DIRECTOR</c>).</param>
/// <param name="PayloadBytes">Exact expected argument payload size in bytes.</param>
public sealed record CommandDef(
    ushort Opcode,
    string Name,
    string OwnerPartition,
    string MinRole = "OPERATOR",
    int PayloadBytes = 0);

/// <summary>
/// Immutable definition of a spacecraft telemetry channel with polynomial calibration and Red/Yellow limit bounds.
/// </summary>
/// <param name="ChannelId">16-bit unique telemetry channel identifier.</param>
/// <param name="Name">Symbolic channel mnemonic (for example, <c>HSI_DETECTOR_TEMP_K</c>).</param>
/// <param name="OwnerPartition">Flight partition producing this channel.</param>
/// <param name="DataType">Wire scalar type (for example, <c>uint16</c> or <c>uint64</c>).</param>
/// <param name="Unit">Engineering unit string (for example, <c>K</c> or <c>ms</c>).</param>
/// <param name="CalC0">Constant offset term <c>c0</c> in <c>eng = c0 + c1 * raw</c>.</param>
/// <param name="CalC1">Linear slope term <c>c1</c> in <c>eng = c0 + c1 * raw</c>.</param>
/// <param name="LimitLoErr">Optional lower Red error bound.</param>
/// <param name="LimitLoWarn">Optional lower Yellow warning bound.</param>
/// <param name="LimitHiWarn">Optional upper Yellow warning bound.</param>
/// <param name="LimitHiErr">Optional upper Red error bound.</param>
public sealed record TelemetryDef(
    ushort ChannelId,
    string Name,
    string OwnerPartition,
    string DataType = "uint32",
    string Unit = "",
    double CalC0 = 0.0,
    double CalC1 = 1.0,
    double? LimitLoErr = null,
    double? LimitLoWarn = null,
    double? LimitHiWarn = null,
    double? LimitHiErr = null)
{
    /// <summary>
    /// Converts a raw integer telemetry value into calibrated engineering units using <c>c0 + c1 * raw</c>.
    /// </summary>
    /// <param name="rawValue">Raw unsigned integer sample from the spacecraft wire frame.</param>
    /// <returns>The calibrated engineering-unit value.</returns>
    public double Calibrate(ulong rawValue) => CalC0 + (CalC1 * rawValue);

    /// <summary>
    /// Solves the inverse linear calibration (<c>raw = round((eng - c0) / c1)</c>) to compute a raw Data Number (DN)
    /// threshold suitable for programming hardware flight comparators.
    /// </summary>
    /// <param name="engineeringValue">Target calibrated engineering-unit threshold.</param>
    /// <returns>Nearest non-negative raw unsigned 64-bit Data Number.</returns>
    /// <exception cref="InvalidOperationException">Thrown when <see cref="CalC1"/> is zero.</exception>
    public ulong InvertToRawDn(double engineeringValue)
    {
        if (Math.Abs(CalC1) < 1e-15)
        {
            throw new InvalidOperationException($"Cannot invert calibration for '{Name}' because CalC1 is zero.");
        }

        double raw = Math.Round((engineeringValue - CalC0) / CalC1);
        return raw <= 0.0 ? 0UL : (ulong)raw;
    }
}

/// <summary>
/// Thread-safe, lock-free single-source-of-truth ground mission dictionary loaded from <c>mission_dict.json</c>.
/// </summary>
/// <remarks>
/// Implements <c>STRATA-GDS-001</c>. Uses <see cref="FrozenDictionary{TKey, TValue}"/> internally for
/// zero-contention concurrent lookups across telemetry ingress workers, limit monitors, and web API threads.
/// </remarks>
[System.Diagnostics.CodeAnalysis.SuppressMessage(
    "Naming",
    "CA1711:Identifiers should not have incorrect suffix",
    Justification = "MissionDictionary is standard spacecraft ground-data-system terminology.")]
public sealed class MissionDictionary
{
    /// <summary>
    /// Gets the raw UTF-8 JSON document string as emitted by <c>strata-tools gen</c>.
    /// </summary>
    public string RawJson { get; }

    /// <summary>
    /// Gets the declared mission name.
    /// </summary>
    public string MissionName { get; }

    /// <summary>
    /// Gets the target FPGA silicon identifier (for example, <c>XQRVC1902</c>).
    /// </summary>
    public string TargetSilicon { get; }

    /// <summary>
    /// Gets the hexadecimal 64-bit <c>MANIFEST_HASH</c> string (for example, <c>0xEE35F9CFC3036975</c>).
    /// </summary>
    public string ManifestHashHex { get; }

    /// <summary>
    /// Gets the parsed 64-bit <c>MANIFEST_HASH</c> value synchronized with flight binaries and RTL.
    /// </summary>
    public ulong ManifestHash { get; }

    /// <summary>
    /// Gets a read-only dictionary of command definitions keyed by symbolic command name.
    /// </summary>
    public FrozenDictionary<string, CommandDef> CommandsByName { get; }

    /// <summary>
    /// Gets a read-only dictionary of command definitions keyed by 16-bit opcode.
    /// </summary>
    public FrozenDictionary<ushort, CommandDef> CommandsByOpcode { get; }

    /// <summary>
    /// Gets a read-only dictionary of telemetry definitions keyed by symbolic channel name.
    /// </summary>
    public FrozenDictionary<string, TelemetryDef> TelemetryByName { get; }

    /// <summary>
    /// Gets a read-only dictionary of telemetry definitions keyed by 16-bit channel ID.
    /// </summary>
    public FrozenDictionary<ushort, TelemetryDef> TelemetryById { get; }

    private MissionDictionary(
        string rawJson,
        string missionName,
        string targetSilicon,
        string manifestHashHex,
        ulong manifestHash,
        FrozenDictionary<string, CommandDef> cmdsByName,
        FrozenDictionary<ushort, CommandDef> cmdsByOp,
        FrozenDictionary<string, TelemetryDef> tlmByName,
        FrozenDictionary<ushort, TelemetryDef> tlmById)
    {
        RawJson = rawJson;
        MissionName = missionName;
        TargetSilicon = targetSilicon;
        ManifestHashHex = manifestHashHex;
        ManifestHash = manifestHash;
        CommandsByName = cmdsByName;
        CommandsByOpcode = cmdsByOp;
        TelemetryByName = tlmByName;
        TelemetryById = tlmById;
    }

    /// <summary>
    /// Loads and parses a <see cref="MissionDictionary"/> from a <c>mission_dict.json</c> file on disk.
    /// </summary>
    /// <param name="path">Filesystem path to <c>mission_dict.json</c>.</param>
    /// <returns>A thread-safe immutable <see cref="MissionDictionary"/> instance.</returns>
    public static MissionDictionary FromFile(string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        return FromJson(File.ReadAllText(path));
    }

    /// <summary>
    /// Parses a <see cref="MissionDictionary"/> directly from raw UTF-8 JSON text.
    /// </summary>
    /// <param name="jsonText">JSON content generated by <c>strata-tools gen</c>.</param>
    /// <returns>A thread-safe immutable <see cref="MissionDictionary"/> instance.</returns>
    public static MissionDictionary FromJson(string jsonText)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(jsonText);
        using JsonDocument doc = JsonDocument.Parse(jsonText);
        JsonElement root = doc.RootElement;

        JsonElement mission = root.GetProperty("mission");
        string missionName = mission.GetProperty("name").GetString() ?? string.Empty;
        string silicon = mission.GetProperty("target_silicon").GetString() ?? string.Empty;
        string hashHex = root.GetProperty("manifest_hash").GetString() ?? "0x0";
        ulong hash = ParseHexUInt64(hashHex);

        Dictionary<string, CommandDef> cmdsByName = new(StringComparer.Ordinal);
        Dictionary<ushort, CommandDef> cmdsByOp = [];
        if (root.TryGetProperty("commands", out JsonElement cmdsElem))
        {
            foreach (JsonElement c in cmdsElem.EnumerateArray())
            {
                CommandDef def = new(
                    Opcode: (ushort)c.GetProperty("opcode").GetInt32(),
                    Name: c.GetProperty("name").GetString() ?? string.Empty,
                    OwnerPartition: c.GetProperty("owner_partition").GetString() ?? string.Empty,
                    MinRole: c.TryGetProperty("min_role", out JsonElement r) ? r.GetString() ?? "OPERATOR" : "OPERATOR",
                    PayloadBytes: c.TryGetProperty("payload_bytes", out JsonElement pb) ? pb.GetInt32() : 0);
                cmdsByName[def.Name] = def;
                cmdsByOp[def.Opcode] = def;
            }
        }

        Dictionary<string, TelemetryDef> tlmByName = new(StringComparer.Ordinal);
        Dictionary<ushort, TelemetryDef> tlmById = [];
        if (root.TryGetProperty("telemetry", out JsonElement tlmElem))
        {
            foreach (JsonElement t in tlmElem.EnumerateArray())
            {
                TelemetryDef def = new(
                    ChannelId: (ushort)t.GetProperty("channel_id").GetInt32(),
                    Name: t.GetProperty("name").GetString() ?? string.Empty,
                    OwnerPartition: t.GetProperty("owner_partition").GetString() ?? string.Empty,
                    DataType: t.TryGetProperty("type", out JsonElement dt) ? dt.GetString() ?? "uint32" : "uint32",
                    Unit: t.TryGetProperty("unit", out JsonElement u) ? u.GetString() ?? string.Empty : string.Empty,
                    CalC0: t.TryGetProperty("cal_c0", out JsonElement c0) ? c0.GetDouble() : 0.0,
                    CalC1: t.TryGetProperty("cal_c1", out JsonElement c1) ? c1.GetDouble() : 1.0,
                    LimitLoErr: TryOptDouble(t, "limit_lo_err"),
                    LimitLoWarn: TryOptDouble(t, "limit_lo_warn"),
                    LimitHiWarn: TryOptDouble(t, "limit_hi_warn"),
                    LimitHiErr: TryOptDouble(t, "limit_hi_err"));
                tlmByName[def.Name] = def;
                tlmById[def.ChannelId] = def;
            }
        }

        return new MissionDictionary(
            jsonText,
            missionName,
            silicon,
            hashHex,
            hash,
            cmdsByName.ToFrozenDictionary(StringComparer.Ordinal),
            cmdsByOp.ToFrozenDictionary(),
            tlmByName.ToFrozenDictionary(StringComparer.Ordinal),
            tlmById.ToFrozenDictionary());
    }

    /// <summary>
    /// Retrieves a command definition by symbolic name.
    /// </summary>
    /// <param name="name">The symbolic command mnemonic.</param>
    /// <returns>The matching <see cref="CommandDef"/>.</returns>
    /// <exception cref="KeyNotFoundException">Thrown if no command with <paramref name="name"/> exists.</exception>
    public CommandDef GetCommand(string name) =>
        CommandsByName.TryGetValue(name, out CommandDef? def)
            ? def
            : throw new KeyNotFoundException($"Unknown command '{name}'.");

    /// <summary>
    /// Retrieves a command definition by 16-bit opcode.
    /// </summary>
    /// <param name="opcode">The 16-bit command opcode.</param>
    /// <returns>The matching <see cref="CommandDef"/>.</returns>
    /// <exception cref="KeyNotFoundException">Thrown if no command with <paramref name="opcode"/> exists.</exception>
    public CommandDef GetCommand(ushort opcode) =>
        CommandsByOpcode.TryGetValue(opcode, out CommandDef? def)
            ? def
            : throw new KeyNotFoundException($"Unknown command opcode 0x{opcode:X4}.");

    /// <summary>
    /// Retrieves a telemetry channel definition by symbolic name.
    /// </summary>
    /// <param name="name">The symbolic channel name.</param>
    /// <returns>The matching <see cref="TelemetryDef"/>.</returns>
    /// <exception cref="KeyNotFoundException">Thrown if no channel with <paramref name="name"/> exists.</exception>
    public TelemetryDef GetTelemetry(string name) =>
        TelemetryByName.TryGetValue(name, out TelemetryDef? def)
            ? def
            : throw new KeyNotFoundException($"Unknown telemetry channel '{name}'.");

    /// <summary>
    /// Retrieves a telemetry channel definition by 16-bit channel ID.
    /// </summary>
    /// <param name="channelId">The 16-bit channel identifier.</param>
    /// <returns>The matching <see cref="TelemetryDef"/>.</returns>
    /// <exception cref="KeyNotFoundException">Thrown if no channel with <paramref name="channelId"/> exists.</exception>
    public TelemetryDef GetTelemetry(ushort channelId) =>
        TelemetryById.TryGetValue(channelId, out TelemetryDef? def)
            ? def
            : throw new KeyNotFoundException($"Unknown telemetry channel ID 0x{channelId:X4}.");

    private static double? TryOptDouble(JsonElement elem, string prop) =>
        elem.TryGetProperty(prop, out JsonElement val) && val.ValueKind == JsonValueKind.Number
            ? val.GetDouble()
            : null;

    private static ulong ParseHexUInt64(string hex)
    {
        string s = hex.StartsWith("0x", StringComparison.OrdinalIgnoreCase) ? hex[2..] : hex;
        return ulong.Parse(s, NumberStyles.HexNumber, CultureInfo.InvariantCulture);
    }
}
