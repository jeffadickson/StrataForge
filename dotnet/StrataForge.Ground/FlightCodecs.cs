using System.Buffers.Binary;
using System.Globalization;

namespace StrataForge.Ground;

/// <summary>
/// Decoded ECSS-E-ST-70-41C (PUS-C) Telemetry Space Packet.
/// </summary>
/// <param name="Apid">11-bit CCSDS Application Process Identifier.</param>
/// <param name="SeqCount">14-bit CCSDS packet sequence count.</param>
/// <param name="ServiceType">PUS-C Service Type ID (for example, 1, 3, 5, 17).</param>
/// <param name="ServiceSubtype">PUS-C Service Subtype ID.</param>
/// <param name="MsgTypeCounter">16-bit per-service message type counter.</param>
/// <param name="DestinationId">16-bit ground destination identifier.</param>
/// <param name="CucTaiNs">64-bit nanosecond CUC TAI timestamp.</param>
/// <param name="AppData">Raw application data payload following the 15-byte PUS-C TM secondary header.</param>
public sealed record DecodedPusCTm(
    ushort Apid,
    ushort SeqCount,
    byte ServiceType,
    byte ServiceSubtype,
    ushort MsgTypeCounter,
    ushort DestinationId,
    ulong CucTaiNs,
    byte[] AppData)
{
    /// <summary>
    /// Gets the uppercase hexadecimal string encoding of <see cref="AppData"/>.
    /// </summary>
    public string AppDataHex => Convert.ToHexString(AppData);
}

/// <summary>
/// Decoded 32-byte binary <c>strata::EventRecord</c> emitted by flight software.
/// </summary>
/// <param name="FirstTaiNs">First occurrence nanosecond TAI timestamp.</param>
/// <param name="LastTaiNs">Most recent deduplicated occurrence nanosecond TAI timestamp.</param>
/// <param name="EventId">32-bit event identifier.</param>
/// <param name="Arg0">First 32-bit event diagnostic argument.</param>
/// <param name="Arg1">Second 32-bit event diagnostic argument.</param>
/// <param name="RepeatCount">Number of deduplicated occurrences collapsed into this record.</param>
/// <param name="Severity">Human-readable severity string (<c>DIAG</c>.. <c>FATAL</c>).</param>
/// <param name="PartitionId">Originating hypervisor partition identifier.</param>
public sealed record DecodedEventRecord(
    ulong FirstTaiNs,
    ulong LastTaiNs,
    uint EventId,
    uint Arg0,
    uint Arg1,
    ushort RepeatCount,
    string Severity,
    byte PartitionId)
{
    /// <summary>
    /// Gets the formatted hexadecimal event identifier (for example, <c>0x0201</c>).
    /// </summary>
    public string EventIdHex => $"0x{EventId:X4}";
}

/// <summary>
/// Decoded 56-byte logical <c>strata::NvLogRecord</c> extracted from refresh-free MRAM/FRAM flight logs.
/// </summary>
/// <param name="BootEpoch">Monotonic power-cycle / warm-reboot epoch number.</param>
/// <param name="SeqNum">Monotonic sequence number within the non-volatile log.</param>
/// <param name="TaiNs">Nanosecond TAI timestamp.</param>
/// <param name="Kind">Decoded record kind (<c>RebootMarker</c>, <c>CrashTrap</c>, <c>FdirAction</c>, etc.).</param>
/// <param name="SourceId">Originating partition or subsystem identifier.</param>
/// <param name="Code">Record-specific event, fault, or ESR syndrome code.</param>
/// <param name="Aux0">First 64-bit auxiliary diagnostic register (for example, <c>ELR_EL2</c>).</param>
/// <param name="Aux1">Second 64-bit auxiliary diagnostic register (for example, <c>FAR_EL2</c>).</param>
/// <param name="Aux2">Third 64-bit auxiliary diagnostic register.</param>
/// <param name="CrcValid"><see langword="true"/> when the Castagnoli CRC-32C matches the stored checksum.</param>
public sealed record DecodedNvLogRecord(
    uint BootEpoch,
    uint SeqNum,
    ulong TaiNs,
    string Kind,
    ushort SourceId,
    uint Code,
    ulong Aux0,
    ulong Aux1,
    ulong Aux2,
    bool CrcValid);

/// <summary>
/// Represents a single timed command step inside a compiled flight sequence (<c>.seq</c>).
/// </summary>
/// <param name="DelayMs">Delay or absolute execution time in milliseconds.</param>
/// <param name="IsRelative"><see langword="true"/> for relative (<c>+1.500</c>) timing; <see langword="false"/> for absolute.</param>
/// <param name="Opcode">16-bit target command opcode.</param>
/// <param name="CommandName">Symbolic command name resolved from <see cref="MissionDictionary"/>.</param>
/// <param name="ArgU64">Optional 64-bit integer command argument.</param>
public sealed record SequenceStep(
    uint DelayMs,
    bool IsRelative,
    ushort Opcode,
    string CommandName,
    ulong ArgU64 = 0UL);

/// <summary>
/// Zero-dependency CCSDS, ECSS-E-ST-70-41C (PUS-C), <c>EventRecord</c>, and <c>NvLogRecord</c> wire codecs.
/// </summary>
/// <remarks>
/// All checksums and binary struct layouts match the C++20 flight software bit-for-bit across host SIL,
/// Verilator HIL, and bare-metal AArch64/Cortex-R5F targets.
/// </remarks>
public static class FlightCodecs
{
    private static readonly string[] SeverityNames =
        ["DIAG", "CMD", "ACT_LO", "ACT_HI", "WARN_LO", "WARN_HI", "FATAL"];

    /// <summary>
    /// Computes the CCSDS CRC-16-CCITT checksum (polynomial <c>0x1021</c>, initial <c>0xFFFF</c>) matching C++20 <c>crc16_ccitt</c>.
    /// </summary>
    /// <param name="data">Input byte span to checksum.</param>
    /// <returns>The 16-bit CRC-16-CCITT value.</returns>
    public static ushort Crc16Ccitt(ReadOnlySpan<byte> data)
    {
        ushort crc = 0xFFFF;
        foreach (byte b in data)
        {
            crc ^= (ushort)(b << 8);
            for (int bit = 0; bit < 8; bit++)
            {
                crc = (crc & 0x8000) != 0
                    ? (ushort)(((crc << 1) ^ 0x1021) & 0xFFFF)
                    : (ushort)((crc << 1) & 0xFFFF);
            }
        }

        return crc;
    }

    /// <summary>
    /// Computes the Castagnoli CRC-32C checksum (reflected polynomial <c>0x82F63B78</c>) matching C++20 <c>strata::Crc32c</c>.
    /// </summary>
    /// <param name="data">Input byte span to checksum.</param>
    /// <param name="initial">Optional running CRC seed (default 0).</param>
    /// <returns>The 32-bit Castagnoli CRC-32C value.</returns>
    public static uint Crc32c(ReadOnlySpan<byte> data, uint initial = 0U)
    {
        uint crc = ~initial;
        foreach (byte b in data)
        {
            crc ^= b;
            for (int bit = 0; bit < 8; bit++)
            {
                crc = (crc & 1U) != 0U ? ((crc >> 1) ^ 0x82F6_3B78U) : (crc >> 1);
            }
        }

        return ~crc;
    }

    /// <summary>
    /// Encodes a CCSDS 232.0-B-4 TC Transfer Frame with trailing 16-bit CRC-16-CCITT FECF.
    /// </summary>
    /// <param name="scid">10-bit Spacecraft Identifier.</param>
    /// <param name="vcid">6-bit Virtual Channel Identifier.</param>
    /// <param name="frameSeq">8-bit frame sequence number.</param>
    /// <param name="dataField">Transfer frame data field payload.</param>
    /// <returns>Complete framed byte array including the 2-byte FECF trailer.</returns>
    /// <exception cref="ArgumentOutOfRangeException">Thrown when total frame length exceeds 1024 bytes.</exception>
    public static byte[] EncodeTcFrame(ushort scid, byte vcid, byte frameSeq, ReadOnlySpan<byte> dataField)
    {
        int total = 5 + dataField.Length + 2;
        if (total > 1024)
        {
            throw new ArgumentOutOfRangeException(nameof(dataField), $"TC frame length {total} exceeds 1024-byte CCSDS maximum.");
        }

        byte[] frame = new byte[total];
        ushort w0 = (ushort)(scid & 0x03FF);
        ushort w1 = (ushort)(((vcid & 0x3F) << 10) | ((total - 1) & 0x03FF));
        BinaryPrimitives.WriteUInt16BigEndian(frame.AsSpan(0, 2), w0);
        BinaryPrimitives.WriteUInt16BigEndian(frame.AsSpan(2, 2), w1);
        frame[4] = frameSeq;
        dataField.CopyTo(frame.AsSpan(5));

        ushort fecf = Crc16Ccitt(frame.AsSpan(0, total - 2));
        BinaryPrimitives.WriteUInt16BigEndian(frame.AsSpan(total - 2, 2), fecf);
        return frame;
    }

    /// <summary>
    /// Validates a named command against <paramref name="dictionary"/> and encodes it into a CCSDS TC Transfer Frame.
    /// </summary>
    /// <param name="dictionary">Mission dictionary supplying opcode and payload-length rules.</param>
    /// <param name="commandName">Symbolic command mnemonic.</param>
    /// <param name="argBytes">Raw argument payload bytes.</param>
    /// <param name="scid">10-bit Spacecraft ID (default <c>0x123</c>).</param>
    /// <param name="vcid">6-bit Virtual Channel ID (default 1).</param>
    /// <param name="frameSeq">8-bit TC sequence counter.</param>
    /// <returns>Complete CCSDS TC Transfer Frame bytes.</returns>
    public static byte[] EncodeNamedCommand(
        MissionDictionary dictionary,
        string commandName,
        ReadOnlySpan<byte> argBytes,
        ushort scid = 0x0123,
        byte vcid = 1,
        byte frameSeq = 0)
    {
        ArgumentNullException.ThrowIfNull(dictionary);
        CommandDef cmd = dictionary.GetCommand(commandName);
        if (argBytes.Length != cmd.PayloadBytes)
        {
            throw new ArgumentException($"Command '{commandName}' expects {cmd.PayloadBytes} payload bytes, got {argBytes.Length}.");
        }

        byte[] payload = new byte[2 + argBytes.Length];
        BinaryPrimitives.WriteUInt16BigEndian(payload.AsSpan(0, 2), cmd.Opcode);
        argBytes.CopyTo(payload.AsSpan(2));
        return EncodeTcFrame(scid, vcid, frameSeq, payload);
    }

    /// <summary>
    /// Encodes a CCSDS Space Packet carrying an ECSS-E-ST-70-41C (PUS-C) Telecommand secondary header.
    /// </summary>
    /// <param name="apid">11-bit destination APID.</param>
    /// <param name="seq">14-bit Space Packet sequence count.</param>
    /// <param name="serviceType">PUS-C Service Type ID.</param>
    /// <param name="serviceSubtype">PUS-C Service Subtype ID.</param>
    /// <param name="appData">Optional application data following the 5-byte PUS-C TC secondary header.</param>
    /// <param name="ackFlags">4-bit ST[01] verification request mask (default <c>0x09</c>: acceptance + completion).</param>
    /// <param name="sourceId">16-bit ground source identifier.</param>
    /// <returns>Complete CCSDS Space Packet bytes.</returns>
    public static byte[] EncodePusCTc(
        ushort apid,
        ushort seq,
        byte serviceType,
        byte serviceSubtype,
        ReadOnlySpan<byte> appData = default,
        byte ackFlags = 0x09,
        ushort sourceId = 1)
    {
        int payloadLen = 5 + appData.Length;
        byte[] pkt = new byte[6 + payloadLen];

        ushort w0 = (ushort)(0x1800 | (apid & 0x07FF));
        ushort w1 = (ushort)(0xC000 | (seq & 0x3FFF));
        ushort w2 = (ushort)(payloadLen - 1);
        BinaryPrimitives.WriteUInt16BigEndian(pkt.AsSpan(0, 2), w0);
        BinaryPrimitives.WriteUInt16BigEndian(pkt.AsSpan(2, 2), w1);
        BinaryPrimitives.WriteUInt16BigEndian(pkt.AsSpan(4, 2), w2);

        pkt[6] = (byte)((2 << 4) | (ackFlags & 0x0F));
        pkt[7] = serviceType;
        pkt[8] = serviceSubtype;
        BinaryPrimitives.WriteUInt16BigEndian(pkt.AsSpan(9, 2), sourceId);
        appData.CopyTo(pkt.AsSpan(11));
        return pkt;
    }

    /// <summary>
    /// Decodes a CCSDS Space Packet carrying an ECSS-E-ST-70-41C (PUS-C) Telemetry secondary header.
    /// </summary>
    /// <param name="rawSpacePacket">Raw wire bytes beginning with the 6-byte CCSDS primary header.</param>
    /// <returns>A parsed <see cref="DecodedPusCTm"/> record.</returns>
    /// <exception cref="InvalidDataException">Thrown if the packet is truncated or has a non-PUS-C version field.</exception>
    public static DecodedPusCTm DecodePusCTm(ReadOnlySpan<byte> rawSpacePacket)
    {
        if (rawSpacePacket.Length < 6 + 15)
        {
            throw new InvalidDataException("Truncated PUS-C TM Space Packet.");
        }

        ushort w0 = BinaryPrimitives.ReadUInt16BigEndian(rawSpacePacket[0..2]);
        ushort w1 = BinaryPrimitives.ReadUInt16BigEndian(rawSpacePacket[2..4]);
        ushort w2 = BinaryPrimitives.ReadUInt16BigEndian(rawSpacePacket[4..6]);
        int payloadLen = w2 + 1;
        if (rawSpacePacket.Length < 6 + payloadLen || payloadLen < 15)
        {
            throw new InvalidDataException("PUS-C TM Space Packet payload length mismatch.");
        }

        ReadOnlySpan<byte> payload = rawSpacePacket.Slice(6, payloadLen);
        int version = payload[0] >> 4;
        if (version != 2)
        {
            throw new InvalidDataException($"Invalid PUS-C version: {version} (expected 2).");
        }

        return new DecodedPusCTm(
            Apid: (ushort)(w0 & 0x07FF),
            SeqCount: (ushort)(w1 & 0x3FFF),
            ServiceType: payload[1],
            ServiceSubtype: payload[2],
            MsgTypeCounter: BinaryPrimitives.ReadUInt16BigEndian(payload[3..5]),
            DestinationId: BinaryPrimitives.ReadUInt16BigEndian(payload[5..7]),
            CucTaiNs: BinaryPrimitives.ReadUInt64BigEndian(payload[7..15]),
            AppData: payload[15..].ToArray());
    }

    /// <summary>
    /// Decodes a 32-byte little-endian binary <c>strata::EventRecord</c>.
    /// </summary>
    /// <param name="raw32">Exactly 32 bytes matching <c>strata::EventRecord</c>.</param>
    /// <returns>A parsed <see cref="DecodedEventRecord"/>.</returns>
    /// <exception cref="ArgumentException">Thrown when <paramref name="raw32"/> is not 32 bytes.</exception>
    public static DecodedEventRecord DecodeEventRecord(ReadOnlySpan<byte> raw32)
    {
        if (raw32.Length != 32)
        {
            throw new ArgumentException($"EventRecord must be exactly 32 bytes, got {raw32.Length}.", nameof(raw32));
        }

        ulong firstTai = BinaryPrimitives.ReadUInt64LittleEndian(raw32[0..8]);
        ulong lastTai = BinaryPrimitives.ReadUInt64LittleEndian(raw32[8..16]);
        uint eventId = BinaryPrimitives.ReadUInt32LittleEndian(raw32[16..20]);
        uint arg0 = BinaryPrimitives.ReadUInt32LittleEndian(raw32[20..24]);
        uint arg1 = BinaryPrimitives.ReadUInt32LittleEndian(raw32[24..28]);
        ushort repeatCnt = BinaryPrimitives.ReadUInt16LittleEndian(raw32[28..30]);
        byte sev = raw32[30];
        byte partId = raw32[31];

        string sevName = sev < SeverityNames.Length ? SeverityNames[sev] : sev.ToString(CultureInfo.InvariantCulture);
        return new DecodedEventRecord(firstTai, lastTai, eventId, arg0, arg1, repeatCnt, sevName, partId);
    }

    /// <summary>
    /// Decodes a 56-byte logical <c>strata::NvLogRecord</c> and validates its Castagnoli CRC-32C checksum.
    /// </summary>
    /// <param name="raw56">Exactly 56 bytes matching <c>strata::NvLogRecord</c>.</param>
    /// <returns>A parsed <see cref="DecodedNvLogRecord"/> with <see cref="DecodedNvLogRecord.CrcValid"/> populated.</returns>
    /// <exception cref="ArgumentException">Thrown when <paramref name="raw56"/> is not 56 bytes.</exception>
    public static DecodedNvLogRecord DecodeNvLogRecord(ReadOnlySpan<byte> raw56)
    {
        if (raw56.Length != 56)
        {
            throw new ArgumentException($"NvLogRecord must be exactly 56 bytes, got {raw56.Length}.", nameof(raw56));
        }

        uint epoch = BinaryPrimitives.ReadUInt32LittleEndian(raw56[0..4]);
        uint seq = BinaryPrimitives.ReadUInt32LittleEndian(raw56[4..8]);
        ulong taiNs = BinaryPrimitives.ReadUInt64LittleEndian(raw56[8..16]);
        ushort kind = BinaryPrimitives.ReadUInt16LittleEndian(raw56[16..18]);
        ushort srcId = BinaryPrimitives.ReadUInt16LittleEndian(raw56[18..20]);
        uint code = BinaryPrimitives.ReadUInt32LittleEndian(raw56[20..24]);
        ulong aux0 = BinaryPrimitives.ReadUInt64LittleEndian(raw56[24..32]);
        ulong aux1 = BinaryPrimitives.ReadUInt64LittleEndian(raw56[32..40]);
        ulong aux2 = BinaryPrimitives.ReadUInt64LittleEndian(raw56[40..48]);
        uint storedCrc = BinaryPrimitives.ReadUInt32LittleEndian(raw56[48..52]);

        Span<byte> zeroed = stackalloc byte[56];
        raw56.CopyTo(zeroed);
        zeroed[48..52].Clear();
        bool crcValid = Crc32c(zeroed) == storedCrc;

        string kindName = kind switch
        {
            1 => "RebootMarker",
            2 => "CrashTrap",
            3 => "CriticalEvent",
            4 => "FdirAction",
            5 => "DfxTransition",
            6 => "SeuAlert",
            _ => kind.ToString(CultureInfo.InvariantCulture),
        };

        return new DecodedNvLogRecord(epoch, seq, taiNs, kindName, srcId, code, aux0, aux1, aux2, crcValid);
    }
}

/// <summary>
/// Compiles human-readable relative/absolute flight command sequences (<c>.seq</c>) into
/// <c>MANIFEST_HASH</c>-stamped, CRC-32C-verified binary flight images and decompiles them back.
/// </summary>
/// <remarks>
/// Implements <c>STRATA-GDS-003</c> targeting <c>strata::services::cdh::SeqEngine</c>.
/// </remarks>
public sealed class SequenceCompiler
{
    private const uint Magic = 0x5354_5351U; // "STSQ"
    private const int HeaderSize = 16;
    private const int StepSize = 16;

    private readonly MissionDictionary _dictionary;

    /// <summary>
    /// Initializes a new <see cref="SequenceCompiler"/> bound to <paramref name="dictionary"/>.
    /// </summary>
    /// <param name="dictionary">Mission dictionary defining valid opcodes and <c>MANIFEST_HASH</c>.</param>
    public SequenceCompiler(MissionDictionary dictionary)
    {
        ArgumentNullException.ThrowIfNull(dictionary);
        _dictionary = dictionary;
    }

    /// <summary>
    /// Compiles a human-readable <c>.seq</c> text script into a CRC-32C-verified binary flight image.
    /// </summary>
    /// <param name="sequenceText">Multi-line sequence text (<c>&lt;time_s&gt; &lt;CMD_NAME&gt; [arg_int]</c>, <c>#</c> comments supported).</param>
    /// <returns>Compiled binary image ending in a 4-byte little-endian CRC-32C trailer.</returns>
    public byte[] CompileText(string sequenceText)
    {
        ArgumentNullException.ThrowIfNull(sequenceText);
        List<SequenceStep> steps = [];
        string[] lines = sequenceText.Replace("\r\n", "\n", StringComparison.Ordinal).Split('\n');

        for (int lineIdx = 0; lineIdx < lines.Length; lineIdx++)
        {
            string line = lines[lineIdx];
            int hashIdx = line.IndexOf('#');
            if (hashIdx >= 0)
            {
                line = line[..hashIdx];
            }

            line = line.Trim();
            if (line.Length == 0)
            {
                continue;
            }

            string[] parts = line.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length < 2)
            {
                throw new FormatException($"Line {lineIdx + 1}: expected '<time_s> <CMD_NAME> [arg_int]'.");
            }

            string timeTok = parts[0];
            bool isRel = timeTok.StartsWith('+');
            string numStr = isRel ? timeTok[1..] : timeTok;
            double secs = double.Parse(numStr, NumberStyles.Float, CultureInfo.InvariantCulture);
            if (secs < 0.0)
            {
                throw new FormatException($"Line {lineIdx + 1}: negative delay is not permitted.");
            }

            uint delayMs = (uint)Math.Round(secs * 1000.0);
            CommandDef cmd = _dictionary.GetCommand(parts[1]);
            ulong argVal = parts.Length >= 3 ? ParseUInt64Auto(parts[2]) : 0UL;
            steps.Add(new SequenceStep(delayMs, isRel, cmd.Opcode, cmd.Name, argVal));
        }

        int bodyLen = HeaderSize + (steps.Count * StepSize);
        byte[] blob = new byte[bodyLen + 4];
        BinaryPrimitives.WriteUInt32LittleEndian(blob.AsSpan(0, 4), Magic);
        BinaryPrimitives.WriteUInt64LittleEndian(blob.AsSpan(4, 8), _dictionary.ManifestHash);
        BinaryPrimitives.WriteUInt16LittleEndian(blob.AsSpan(12, 2), (ushort)steps.Count);
        BinaryPrimitives.WriteUInt16LittleEndian(blob.AsSpan(14, 2), 0);

        for (int i = 0; i < steps.Count; i++)
        {
            int off = HeaderSize + (i * StepSize);
            SequenceStep st = steps[i];
            BinaryPrimitives.WriteUInt32LittleEndian(blob.AsSpan(off, 4), st.DelayMs);
            blob[off + 4] = st.IsRelative ? (byte)1 : (byte)0;
            blob[off + 5] = 0;
            BinaryPrimitives.WriteUInt16LittleEndian(blob.AsSpan(off + 6, 2), st.Opcode);
            BinaryPrimitives.WriteUInt64LittleEndian(blob.AsSpan(off + 8, 8), st.ArgU64);
        }

        uint crc = FlightCodecs.Crc32c(blob.AsSpan(0, bodyLen));
        BinaryPrimitives.WriteUInt32LittleEndian(blob.AsSpan(bodyLen, 4), crc);
        return blob;
    }

    /// <summary>
    /// Verifies CRC-32C, magic, and <c>MANIFEST_HASH</c> of a compiled sequence binary and decompiles all steps.
    /// </summary>
    /// <param name="blob">Compiled binary sequence image.</param>
    /// <returns>Ordered list of decoded <see cref="SequenceStep"/> entries.</returns>
    public IReadOnlyList<SequenceStep> Decompile(ReadOnlySpan<byte> blob)
    {
        if (blob.Length < HeaderSize + 4)
        {
            throw new InvalidDataException("Sequence binary is too short.");
        }

        int bodyLen = blob.Length - 4;
        uint rxCrc = BinaryPrimitives.ReadUInt32LittleEndian(blob[bodyLen..]);
        if (FlightCodecs.Crc32c(blob[..bodyLen]) != rxCrc)
        {
            throw new InvalidDataException("Sequence CRC-32C mismatch.");
        }

        uint magic = BinaryPrimitives.ReadUInt32LittleEndian(blob[0..4]);
        ulong mhash = BinaryPrimitives.ReadUInt64LittleEndian(blob[4..12]);
        ushort count = BinaryPrimitives.ReadUInt16LittleEndian(blob[12..14]);

        if (magic != Magic)
        {
            throw new InvalidDataException($"Invalid sequence magic: 0x{magic:X8}.");
        }

        if (mhash != _dictionary.ManifestHash)
        {
            throw new InvalidDataException($"Sequence MANIFEST_HASH mismatch: 0x{mhash:X16} != {_dictionary.ManifestHashHex}.");
        }

        if (bodyLen != HeaderSize + (count * StepSize))
        {
            throw new InvalidDataException("Sequence binary length does not match step_count.");
        }

        List<SequenceStep> steps = new(count);
        for (int i = 0; i < count; i++)
        {
            int off = HeaderSize + (i * StepSize);
            uint delayMs = BinaryPrimitives.ReadUInt32LittleEndian(blob.Slice(off, 4));
            bool isRel = blob[off + 4] != 0;
            ushort opcode = BinaryPrimitives.ReadUInt16LittleEndian(blob.Slice(off + 6, 2));
            ulong arg = BinaryPrimitives.ReadUInt64LittleEndian(blob.Slice(off + 8, 8));
            string name = _dictionary.CommandsByOpcode.TryGetValue(opcode, out CommandDef? def)
                ? def.Name
                : $"0x{opcode:X4}";
            steps.Add(new SequenceStep(delayMs, isRel, opcode, name, arg));
        }

        return steps;
    }

    private static ulong ParseUInt64Auto(string raw) =>
        raw.StartsWith("0x", StringComparison.OrdinalIgnoreCase)
            ? ulong.Parse(raw[2..], NumberStyles.HexNumber, CultureInfo.InvariantCulture)
            : ulong.Parse(raw, NumberStyles.Integer, CultureInfo.InvariantCulture);
}

/// <summary>
/// Compiles and decompiles key-value flight parameter tables (<c>ParamBank</c>) with CRC-32C integrity trailers.
/// </summary>
/// <remarks>
/// Implements <c>STRATA-GDS-003</c> targeting <c>strata::ParamBank</c>.
/// </remarks>
public sealed class ParamTableCompiler
{
    private const uint Magic = 0x5354_5042U; // "STPB"
    private const int HeaderSize = 20;
    private const int EntrySize = 12;

    private readonly ulong _manifestHash;

    /// <summary>
    /// Initializes a new <see cref="ParamTableCompiler"/> stamped with <paramref name="manifestHash"/>.
    /// </summary>
    /// <param name="manifestHash">64-bit mission <c>MANIFEST_HASH</c>.</param>
    public ParamTableCompiler(ulong manifestHash)
    {
        _manifestHash = manifestHash;
    }

    /// <summary>
    /// Compiles a versioned list of <c>(ParamId, ValueU64)</c> pairs into a CRC-32C-verified binary <c>ParamBank</c> image.
    /// </summary>
    /// <param name="version">Monotonic parameter table version number.</param>
    /// <param name="entries">Ordered list of parameter ID and 64-bit value pairs.</param>
    /// <returns>Compiled binary image ending in a 4-byte little-endian CRC-32C trailer.</returns>
    public byte[] Compile(uint version, IReadOnlyList<(uint ParamId, ulong Value)> entries)
    {
        ArgumentNullException.ThrowIfNull(entries);
        int bodyLen = HeaderSize + (entries.Count * EntrySize);
        byte[] blob = new byte[bodyLen + 4];

        BinaryPrimitives.WriteUInt32LittleEndian(blob.AsSpan(0, 4), Magic);
        BinaryPrimitives.WriteUInt64LittleEndian(blob.AsSpan(4, 8), _manifestHash);
        BinaryPrimitives.WriteUInt32LittleEndian(blob.AsSpan(12, 4), version);
        BinaryPrimitives.WriteUInt32LittleEndian(blob.AsSpan(16, 4), (uint)entries.Count);

        for (int i = 0; i < entries.Count; i++)
        {
            int off = HeaderSize + (i * EntrySize);
            BinaryPrimitives.WriteUInt32LittleEndian(blob.AsSpan(off, 4), entries[i].ParamId);
            BinaryPrimitives.WriteUInt64LittleEndian(blob.AsSpan(off + 4, 8), entries[i].Value);
        }

        uint crc = FlightCodecs.Crc32c(blob.AsSpan(0, bodyLen));
        BinaryPrimitives.WriteUInt32LittleEndian(blob.AsSpan(bodyLen, 4), crc);
        return blob;
    }

    /// <summary>
    /// Verifies and decompiles a binary <c>ParamBank</c> image back into its version and entry list.
    /// </summary>
    /// <param name="blob">Compiled binary parameter table bytes.</param>
    /// <returns>A tuple containing the table version and decoded parameter entries.</returns>
    public (uint Version, IReadOnlyList<(uint ParamId, ulong Value)> Entries) Decompile(ReadOnlySpan<byte> blob)
    {
        if (blob.Length < HeaderSize + 4)
        {
            throw new InvalidDataException("ParamTable binary is too short.");
        }

        int bodyLen = blob.Length - 4;
        uint rxCrc = BinaryPrimitives.ReadUInt32LittleEndian(blob[bodyLen..]);
        if (FlightCodecs.Crc32c(blob[..bodyLen]) != rxCrc)
        {
            throw new InvalidDataException("ParamTable CRC-32C mismatch.");
        }

        uint magic = BinaryPrimitives.ReadUInt32LittleEndian(blob[0..4]);
        ulong mhash = BinaryPrimitives.ReadUInt64LittleEndian(blob[4..12]);
        uint version = BinaryPrimitives.ReadUInt32LittleEndian(blob[12..16]);
        uint count = BinaryPrimitives.ReadUInt32LittleEndian(blob[16..20]);

        if (magic != Magic || mhash != _manifestHash)
        {
            throw new InvalidDataException("ParamTable magic or MANIFEST_HASH mismatch.");
        }

        List<(uint, ulong)> entries = new((int)count);
        for (int i = 0; i < count; i++)
        {
            int off = HeaderSize + (i * EntrySize);
            uint pid = BinaryPrimitives.ReadUInt32LittleEndian(blob.Slice(off, 4));
            ulong val = BinaryPrimitives.ReadUInt64LittleEndian(blob.Slice(off + 4, 8));
            entries.Add((pid, val));
        }

        return (version, entries);
    }
}
