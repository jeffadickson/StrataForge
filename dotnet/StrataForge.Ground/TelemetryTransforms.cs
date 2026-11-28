using System.Collections.Concurrent;

namespace StrataForge.Ground;

/// <summary>
/// Describes a sub-word bitfield extraction rule over an unsigned 64-bit telemetry word or status register.
/// </summary>
/// <param name="Name">Symbolic field name (for example, <c>SCSR_SBIT_PASS</c>).</param>
/// <param name="BitOffset">Zero-based least-significant bit index (<c>0..63</c>).</param>
/// <param name="BitWidth">Field width in bits (<c>1..64</c>).</param>
/// <param name="IsSigned"><see langword="true"/> to sign-extend two's-complement fields.</param>
public sealed record BitfieldSpec(string Name, int BitOffset, int BitWidth, bool IsSigned = false);

/// <summary>
/// Zero-allocation bitfield extraction and discrete enumeration decoding utilities for packed flight telemetry words.
/// </summary>
/// <remarks>
/// Implements <c>STRATA-GDS-006</c>.
/// </remarks>
public static class BitfieldCodec
{
    /// <summary>
    /// Extracts an unsigned bitfield slice from a 64-bit packed telemetry word.
    /// </summary>
    /// <param name="word">Packed 64-bit word.</param>
    /// <param name="bitOffset">Least-significant bit position (<c>0..63</c>).</param>
    /// <param name="bitWidth">Number of bits to extract (<c>1..64</c>).</param>
    /// <returns>Right-aligned extracted unsigned value.</returns>
    /// <exception cref="ArgumentOutOfRangeException">Thrown when bit offset or width exceeds 64 bits.</exception>
    public static ulong ExtractUnsigned(ulong word, int bitOffset, int bitWidth)
    {
        if (bitOffset is < 0 or > 63 || bitWidth is < 1 or > 64 || bitOffset + bitWidth > 64)
        {
            throw new ArgumentOutOfRangeException(nameof(bitWidth), $"Invalid bitfield [{bitOffset}..{bitOffset + bitWidth - 1}].");
        }

        ulong mask = bitWidth == 64 ? ulong.MaxValue : ((1UL << bitWidth) - 1UL);
        return (word >> bitOffset) & mask;
    }

    /// <summary>
    /// Extracts a signed two's-complement bitfield slice from a 64-bit packed telemetry word.
    /// </summary>
    /// <param name="word">Packed 64-bit word.</param>
    /// <param name="bitOffset">Least-significant bit position (<c>0..63</c>).</param>
    /// <param name="bitWidth">Number of bits to extract (<c>1..64</c>).</param>
    /// <returns>Sign-extended 64-bit signed integer.</returns>
    public static long ExtractSigned(ulong word, int bitOffset, int bitWidth)
    {
        ulong raw = ExtractUnsigned(word, bitOffset, bitWidth);
        if (bitWidth == 64)
        {
            return unchecked((long)raw);
        }

        ulong signBit = 1UL << (bitWidth - 1);
        return (raw & signBit) != 0UL
            ? unchecked((long)(raw | ~(signBit - 1UL)))
            : (long)raw;
    }

    /// <summary>
    /// Extracts a field defined by <paramref name="spec"/> as a floating-point engineering scalar.
    /// </summary>
    /// <param name="word">Packed 64-bit word.</param>
    /// <param name="spec">Bitfield specification.</param>
    /// <returns>Extracted numeric value.</returns>
    public static double ExtractValue(ulong word, BitfieldSpec spec)
    {
        ArgumentNullException.ThrowIfNull(spec);
        return spec.IsSigned
            ? ExtractSigned(word, spec.BitOffset, spec.BitWidth)
            : ExtractUnsigned(word, spec.BitOffset, spec.BitWidth);
    }
}

/// <summary>
/// Thread-safe discrete raw-value-to-symbolic-label enumeration decoder (for example, <c>0 -&gt; OFF</c>, <c>4 -&gt; SCIENCE</c>).
/// </summary>
public sealed class TelemetryEnumMap
{
    private readonly Dictionary<ulong, string> _labels = [];

    /// <summary>
    /// Gets the symbolic channel name associated with this enumeration mapping.
    /// </summary>
    public string ChannelName { get; }

    /// <summary>
    /// Initializes a new <see cref="TelemetryEnumMap"/> for <paramref name="channelName"/>.
    /// </summary>
    /// <param name="channelName">Symbolic telemetry channel name.</param>
    /// <param name="entries">Initial raw-value to label pairs.</param>
    public TelemetryEnumMap(string channelName, IEnumerable<KeyValuePair<ulong, string>> entries)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(channelName);
        ArgumentNullException.ThrowIfNull(entries);
        ChannelName = channelName;
        foreach (KeyValuePair<ulong, string> kv in entries)
        {
            _labels[kv.Key] = kv.Value;
        }
    }

    /// <summary>
    /// Decodes a raw unsigned value into its symbolic enumeration name or a formatted unknown token.
    /// </summary>
    /// <param name="rawValue">Raw unsigned integer value.</param>
    /// <returns>Symbolic enumeration label.</returns>
    public string Decode(ulong rawValue) =>
        _labels.TryGetValue(rawValue, out string? label) ? label : $"UNKNOWN({rawValue})";
}

/// <summary>
/// Definition of a virtual derived telemetry channel computed dynamically from live peer telemetry samples.
/// </summary>
/// <param name="Name">Symbolic name of the virtual derived channel (for example, <c>HSI_POWER_W</c>).</param>
/// <param name="VirtualChannelId">Synthetic 16-bit channel identifier (typically <c>0xF000..0xFFFF</c>).</param>
/// <param name="Unit">Engineering unit string (for example, <c>W</c> or <c>K</c>).</param>
/// <param name="Compute">Pure computation function receiving the current snapshot of live telemetry samples; returns <see langword="null"/> when inputs are unavailable.</param>
public sealed record DerivedChannelDef(
    string Name,
    ushort VirtualChannelId,
    string Unit,
    Func<IReadOnlyDictionary<string, TelemetrySample>, double?> Compute);

/// <summary>
/// Thread-safe engine managing virtual derived telemetry channels and bitfield/enum decoders.
/// </summary>
/// <remarks>
/// Implements <c>STRATA-GDS-006</c>. Attached to <see cref="TelemetryBroker"/> so derived values are
/// automatically recomputed, limit-checked, and published to time-series/SSE subscribers whenever source telemetry updates.
/// </remarks>
public sealed class DerivedChannelEngine
{
    private readonly ConcurrentDictionary<string, DerivedChannelDef> _derived = new(StringComparer.Ordinal);
    private readonly ConcurrentDictionary<string, TelemetryEnumMap> _enums = new(StringComparer.Ordinal);
    private readonly ConcurrentDictionary<string, List<BitfieldSpec>> _bitfields = new(StringComparer.Ordinal);

    /// <summary>
    /// Registers a virtual derived channel definition.
    /// </summary>
    /// <param name="def">Derived channel computation descriptor.</param>
    public void RegisterDerived(DerivedChannelDef def)
    {
        ArgumentNullException.ThrowIfNull(def);
        _derived[def.Name] = def;
    }

    /// <summary>
    /// Registers a symbolic enumeration map for a discrete telemetry channel.
    /// </summary>
    /// <param name="enumMap">Enumeration mapping.</param>
    public void RegisterEnum(TelemetryEnumMap enumMap)
    {
        ArgumentNullException.ThrowIfNull(enumMap);
        _enums[enumMap.ChannelName] = enumMap;
    }

    /// <summary>
    /// Registers a packed bitfield specification attached to source channel <paramref name="sourceChannel"/>.
    /// </summary>
    /// <param name="sourceChannel">Name of the source packed-integer channel.</param>
    /// <param name="spec">Bitfield slice descriptor.</param>
    public void RegisterBitfield(string sourceChannel, BitfieldSpec spec)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(sourceChannel);
        ArgumentNullException.ThrowIfNull(spec);
        List<BitfieldSpec> list = _bitfields.GetOrAdd(sourceChannel, _ => []);
        lock (list)
        {
            list.Add(spec);
        }
    }

    /// <summary>
    /// Decodes a discrete channel value if an enumeration map is registered for <paramref name="channelName"/>.
    /// </summary>
    /// <param name="channelName">Symbolic channel name.</param>
    /// <param name="rawValue">Raw integer value.</param>
    /// <returns>Symbolic label if registered; otherwise, <see langword="null"/>.</returns>
    public string? TryDecodeEnum(string channelName, ulong rawValue) =>
        _enums.TryGetValue(channelName, out TelemetryEnumMap? map) ? map.Decode(rawValue) : null;

    /// <summary>
    /// Extracts all registered bitfields for <paramref name="sourceChannel"/> from <paramref name="rawWord"/>.
    /// </summary>
    /// <param name="sourceChannel">Packed source channel name.</param>
    /// <param name="rawWord">Raw 64-bit packed integer value.</param>
    /// <returns>Dictionary mapping each bitfield name to its extracted numeric value.</returns>
    public IReadOnlyDictionary<string, double> ExtractBitfields(string sourceChannel, ulong rawWord)
    {
        if (!_bitfields.TryGetValue(sourceChannel, out List<BitfieldSpec>? list))
        {
            return new Dictionary<string, double>();
        }

        BitfieldSpec[] copy;
        lock (list)
        {
            copy = [.. list];
        }

        Dictionary<string, double> result = new(copy.Length, StringComparer.Ordinal);
        foreach (BitfieldSpec spec in copy)
        {
            result[spec.Name] = BitfieldCodec.ExtractValue(rawWord, spec);
        }

        return result;
    }

    /// <summary>
    /// Evaluates all registered virtual derived channels against the latest live telemetry snapshot.
    /// </summary>
    /// <param name="latestSnapshot">Current map of latest telemetry samples keyed by channel name.</param>
    /// <returns>List of newly computed derived channel values ready for limit monitoring and publishing.</returns>
    public IReadOnlyList<(DerivedChannelDef Def, double Value)> EvaluateAll(
        IReadOnlyDictionary<string, TelemetrySample> latestSnapshot)
    {
        ArgumentNullException.ThrowIfNull(latestSnapshot);
        List<(DerivedChannelDef, double)> results = [];
        foreach (DerivedChannelDef def in _derived.Values)
        {
            double? val = def.Compute(latestSnapshot);
            if (val.HasValue && !double.IsNaN(val.Value) && !double.IsInfinity(val.Value))
            {
                results.Add((def, val.Value));
            }
        }

        return results;
    }
}
