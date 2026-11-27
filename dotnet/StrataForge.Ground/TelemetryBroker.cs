using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.Globalization;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading.Channels;

namespace StrataForge.Ground;

/// <summary>
/// Represents a single calibrated telemetry sample with its evaluated <see cref="LimitState"/>.
/// </summary>
/// <param name="Channel">Symbolic telemetry channel name.</param>
/// <param name="ChannelId">16-bit telemetry channel identifier.</param>
/// <param name="RawValue">Raw unsigned integer value received from flight software.</param>
/// <param name="EngValue">Calibrated engineering-unit value.</param>
/// <param name="Unit">Engineering unit string.</param>
/// <param name="LimitState">Evaluated Red/Yellow/Nominal limit classification.</param>
/// <param name="TimestampNs">Nanosecond capture timestamp.</param>
public sealed record TelemetrySample(
    string Channel,
    ushort ChannelId,
    ulong RawValue,
    double EngValue,
    string Unit,
    LimitState LimitState,
    long TimestampNs);

/// <summary>
/// Represents an audited command uplink history entry recorded by <see cref="TelemetryBroker"/>.
/// </summary>
/// <param name="TimestampNs">Nanosecond uplink timestamp.</param>
/// <param name="Command">Symbolic command or PUS-C service label.</param>
/// <param name="Hex">Uppercase hexadecimal wire payload transmitted.</param>
public sealed record CommandHistoryEntry(
    long TimestampNs,
    string Command,
    string Hex);

/// <summary>
/// Thread-safe, Wireshark-compatible PCAP binary stream writer (<c>DLT_USER0 = 147</c>).
/// </summary>
/// <remarks>
/// Uses a dedicated <see cref="Lock"/> to serialize packet writes cleanly across concurrent
/// uplink and downlink threads while flushing each packet for lossless I&amp;T capture.
/// </remarks>
public sealed class PcapWriter : IDisposable
{
    private readonly FileStream _stream;
    private readonly Lock _writeLock = new();
    private bool _disposed;

    /// <summary>
    /// Opens a new PCAP capture file at <paramref name="path"/> and writes the 24-byte global PCAP header.
    /// </summary>
    /// <param name="path">Destination <c>.pcap</c> file path.</param>
    public PcapWriter(string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        _stream = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.Read);

        Span<byte> hdr = stackalloc byte[24];
        BinaryPrimitives.WriteUInt32LittleEndian(hdr[0..4], 0xA1B2_C3D4U); // Standard little-endian PCAP magic
        BinaryPrimitives.WriteUInt16LittleEndian(hdr[4..6], 2);             // Major version
        BinaryPrimitives.WriteUInt16LittleEndian(hdr[6..8], 4);             // Minor version
        BinaryPrimitives.WriteInt32LittleEndian(hdr[8..12], 0);             // Thiszone
        BinaryPrimitives.WriteUInt32LittleEndian(hdr[12..16], 0U);          // Sigfigs
        BinaryPrimitives.WriteUInt32LittleEndian(hdr[16..20], 65535U);      // Snaplen
        BinaryPrimitives.WriteUInt32LittleEndian(hdr[20..24], 147U);        // DLT_USER0
        _stream.Write(hdr);
        _stream.Flush();
    }

    /// <summary>
    /// Appends a timestamped raw wire packet to the PCAP archive in a thread-safe manner.
    /// </summary>
    /// <param name="data">Raw wire frame bytes.</param>
    /// <param name="timestampNs">Optional nanosecond Unix timestamp (defaults to current UTC time).</param>
    public void WritePacket(ReadOnlySpan<byte> data, long? timestampNs = null)
    {
        long tsNs = timestampNs ?? GetUtcUnixNanoseconds();
        uint sec = (uint)((tsNs / 1_000_000_000L) & 0xFFFF_FFFFL);
        uint usec = (uint)((tsNs % 1_000_000_000L) / 1_000L);
        uint len = (uint)data.Length;

        Span<byte> recHdr = stackalloc byte[16];
        BinaryPrimitives.WriteUInt32LittleEndian(recHdr[0..4], sec);
        BinaryPrimitives.WriteUInt32LittleEndian(recHdr[4..8], usec);
        BinaryPrimitives.WriteUInt32LittleEndian(recHdr[8..12], len);
        BinaryPrimitives.WriteUInt32LittleEndian(recHdr[12..16], len);

        using (_writeLock.EnterScope())
        {
            if (_disposed)
            {
                return;
            }

            _stream.Write(recHdr);
            _stream.Write(data);
            _stream.Flush();
        }
    }

    /// <summary>
    /// Flushes and closes the underlying PCAP file handle.
    /// </summary>
    public void Dispose()
    {
        using (_writeLock.EnterScope())
        {
            if (_disposed)
            {
                return;
            }

            _disposed = true;
            _stream.Flush();
            _stream.Dispose();
        }
    }

    internal static long GetUtcUnixNanoseconds() =>
        (DateTimeOffset.UtcNow.Ticks - DateTimeOffset.UnixEpoch.Ticks) * 100L;
}

/// <summary>
/// Reads timestamped binary flight packets back from a standard Wireshark-compatible PCAP archive.
/// </summary>
public static class PcapReader
{
    /// <summary>
    /// Reads all captured packets and nanosecond timestamps sequentially from <paramref name="path"/>.
    /// </summary>
    /// <param name="path">Path to the <c>.pcap</c> file.</param>
    /// <returns>Ordered list of <c>(TimestampNs, Payload)</c> tuples.</returns>
    /// <exception cref="InvalidDataException">Thrown if the file is truncated or lacks the standard PCAP magic.</exception>
    public static IReadOnlyList<(long TimestampNs, byte[] Payload)> ReadAll(string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        byte[] raw = File.ReadAllBytes(path);
        if (raw.Length < 24)
        {
            throw new InvalidDataException("Truncated PCAP file.");
        }

        uint magic = BinaryPrimitives.ReadUInt32LittleEndian(raw.AsSpan(0, 4));
        if (magic != 0xA1B2_C3D4U)
        {
            throw new InvalidDataException($"Unsupported PCAP magic: 0x{magic:X8}.");
        }

        List<(long, byte[])> packets = [];
        int pos = 24;
        while (pos + 16 <= raw.Length)
        {
            uint sec = BinaryPrimitives.ReadUInt32LittleEndian(raw.AsSpan(pos, 4));
            uint usec = BinaryPrimitives.ReadUInt32LittleEndian(raw.AsSpan(pos + 4, 4));
            int inclLen = (int)BinaryPrimitives.ReadUInt32LittleEndian(raw.AsSpan(pos + 8, 4));
            pos += 16;
            if (inclLen < 0 || pos + inclLen > raw.Length)
            {
                break;
            }

            byte[] payload = raw.AsSpan(pos, inclLen).ToArray();
            pos += inclLen;
            long tsNs = (sec * 1_000_000_000L) + (usec * 1_000L);
            packets.Add((tsNs, payload));
        }

        return packets;
    }
}

/// <summary>
/// High-throughput multithreaded Telemetry/Command Stream Broker built on <see cref="Channel{T}"/>,
/// <see cref="ConcurrentDictionary{TKey, TValue}"/>, and async <see cref="UdpClient"/> workers.
/// </summary>
/// <remarks>
/// <para>
/// Implements <c>STRATA-GDS-004</c>:
/// </para>
/// <list type="bullet">
///   <item><description>Dedicated async UDP socket receiver pushes raw datagrams into a bounded lock-free <see cref="Channel{T}"/>.</description></item>
///   <item><description>Configurable parallel worker tasks drain the channel concurrently, decoding <c>STTM</c> telemetry, <c>STEV</c> 32-byte <c>EventRecord</c>s, and CCSDS PUS-C TM packets.</description></item>
///   <item><description>Archives every uplink/downlink frame to standard Wireshark <c>.pcap</c> via <see cref="PcapWriter"/> and exports calibrated histories to CSV.</description></item>
/// </list>
/// </remarks>
public sealed class TelemetryBroker : IAsyncDisposable
{
    private sealed record InboundDatagram(byte[] Payload, long TimestampNs);

    private readonly Lock _pcapGate = new();
    private readonly ConcurrentBag<Action<byte[], long>> _handlers = [];
    private readonly ConcurrentDictionary<string, TelemetrySample> _latestTlm = new(StringComparer.Ordinal);
    private readonly ConcurrentQueue<TelemetrySample> _tlmHistory = new();
    private readonly ConcurrentQueue<DecodedEventRecord> _evrHistory = new();
    private readonly ConcurrentQueue<DecodedPusCTm> _pusTmHistory = new();
    private readonly ConcurrentQueue<CommandHistoryEntry> _cmdHistory = new();

    private PcapWriter? _pcap;
    private UdpClient? _udpClient;
    private CancellationTokenSource? _udpCts;
    private Channel<InboundDatagram>? _ingressChannel;
    private Task? _receiverTask;
    private Task[]? _workerTasks;

    /// <summary>
    /// Gets the mission dictionary governing calibration and channel lookup.
    /// </summary>
    public MissionDictionary Dictionary { get; }

    /// <summary>
    /// Gets the thread-safe real-time Red/Yellow limit monitor attached to this broker.
    /// </summary>
    public LimitMonitor Limits { get; }

    /// <summary>
    /// Initializes a new multithreaded <see cref="TelemetryBroker"/>.
    /// </summary>
    /// <param name="dictionary">Single-source-of-truth <see cref="MissionDictionary"/>.</param>
    /// <param name="limits">Optional <see cref="LimitMonitor"/> (created automatically from <paramref name="dictionary"/> if omitted).</param>
    public TelemetryBroker(MissionDictionary dictionary, LimitMonitor? limits = null)
    {
        ArgumentNullException.ThrowIfNull(dictionary);
        Dictionary = dictionary;
        Limits = limits ?? new LimitMonitor(dictionary);
    }

    /// <summary>
    /// Registers a pluggable packet handler callback invoked for every ingested raw frame.
    /// </summary>
    /// <param name="handler">Callback accepting the raw wire bytes and nanosecond timestamp.</param>
    public void AddHandler(Action<byte[], long> handler)
    {
        ArgumentNullException.ThrowIfNull(handler);
        _handlers.Add(handler);
    }

    /// <summary>
    /// Starts lossless Wireshark-compatible PCAP binary capture to <paramref name="path"/>.
    /// </summary>
    /// <param name="path">Destination <c>.pcap</c> file path.</param>
    public void StartPcapCapture(string path)
    {
        using (_pcapGate.EnterScope())
        {
            _pcap?.Dispose();
            _pcap = new PcapWriter(path);
        }
    }

    /// <summary>
    /// Flushes and closes the active PCAP capture file, if any.
    /// </summary>
    public void StopPcapCapture()
    {
        using (_pcapGate.EnterScope())
        {
            _pcap?.Dispose();
            _pcap = null;
        }
    }

    /// <summary>
    /// Binds an async UDP telemetry ingress socket and spawns a bounded <see cref="Channel{T}"/>
    /// backed by <paramref name="workerCount"/> parallel packet-processing tasks.
    /// </summary>
    /// <param name="host">Local IP address to bind (default <c>127.0.0.1</c>).</param>
    /// <param name="port">UDP port number (<c>0</c> selects an ephemeral OS port).</param>
    /// <param name="workerCount">Number of concurrent channel consumer worker tasks (default 2).</param>
    /// <returns>The actual bound UDP port number.</returns>
    public int StartUdpListener(string host = "127.0.0.1", int port = 0, int workerCount = 2)
    {
        IPAddress ip = IPAddress.Parse(host);
        _udpClient = new UdpClient(new IPEndPoint(ip, port));
        int boundPort = ((IPEndPoint)_udpClient.Client.LocalEndPoint!).Port;

        _udpCts = new CancellationTokenSource();
        CancellationToken ct = _udpCts.Token;

        _ingressChannel = Channel.CreateBounded<InboundDatagram>(new BoundedChannelOptions(4096)
        {
            SingleWriter = true,
            SingleReader = workerCount <= 1,
            FullMode = BoundedChannelFullMode.DropOldest,
        });

        _receiverTask = Task.Run(async () =>
        {
            while (!ct.IsCancellationRequested)
            {
                try
                {
                    UdpReceiveResult rx = await _udpClient.ReceiveAsync(ct).ConfigureAwait(false);
                    long ts = PcapWriter.GetUtcUnixNanoseconds();
                    _ingressChannel.Writer.TryWrite(new InboundDatagram(rx.Buffer, ts));
                }
                catch (OperationCanceledException)
                {
                    break;
                }
                catch (ObjectDisposedException)
                {
                    break;
                }
                catch (SocketException)
                {
                    if (ct.IsCancellationRequested)
                    {
                        break;
                    }
                }
            }

            _ingressChannel.Writer.TryComplete();
        }, CancellationToken.None);

        int workers = Math.Max(1, workerCount);
        _workerTasks = new Task[workers];
        for (int i = 0; i < workers; i++)
        {
            _workerTasks[i] = Task.Run(async () =>
            {
                await foreach (InboundDatagram dgram in _ingressChannel.Reader.ReadAllAsync(CancellationToken.None).ConfigureAwait(false))
                {
                    IngestRaw(dgram.Payload, dgram.TimestampNs);
                }
            }, CancellationToken.None);
        }

        return boundPort;
    }

    /// <summary>
    /// Gracefully stops the UDP listener and drains all in-flight channel worker tasks.
    /// </summary>
    public async Task StopUdpListenerAsync()
    {
        if (_udpCts is not null)
        {
            await _udpCts.CancelAsync().ConfigureAwait(false);
        }

        if (_udpClient is not null)
        {
            _udpClient.Close();
            _udpClient.Dispose();
            _udpClient = null;
        }

        if (_receiverTask is not null)
        {
            await _receiverTask.ConfigureAwait(false);
            _receiverTask = null;
        }

        if (_workerTasks is not null)
        {
            await Task.WhenAll(_workerTasks).ConfigureAwait(false);
            _workerTasks = null;
        }

        _udpCts?.Dispose();
        _udpCts = null;
    }

    /// <summary>
    /// Records an uplinked command in the audit history and active PCAP archive.
    /// </summary>
    /// <param name="name">Symbolic command name or PUS-C service label.</param>
    /// <param name="wireBytes">Transmitted wire bytes.</param>
    /// <param name="timestampNs">Optional nanosecond timestamp.</param>
    public void RecordCommand(string name, byte[] wireBytes, long? timestampNs = null)
    {
        ArgumentNullException.ThrowIfNull(wireBytes);
        long ts = timestampNs ?? PcapWriter.GetUtcUnixNanoseconds();
        _cmdHistory.Enqueue(new CommandHistoryEntry(ts, name, Convert.ToHexString(wireBytes)));
        WriteToPcap(wireBytes, ts);
    }

    /// <summary>
    /// Calibrates a raw telemetry sample, evaluates Red/Yellow limits, and updates the live snapshot &amp; history.
    /// </summary>
    /// <param name="channelId">16-bit telemetry channel identifier.</param>
    /// <param name="rawValue">Raw unsigned 64-bit value.</param>
    /// <param name="timestampNs">Optional nanosecond timestamp.</param>
    /// <returns>The calibrated <see cref="TelemetrySample"/>.</returns>
    public TelemetrySample IngestTelemetryChannel(ushort channelId, ulong rawValue, long? timestampNs = null)
    {
        long ts = timestampNs ?? PcapWriter.GetUtcUnixNanoseconds();
        TelemetryDef tdef = Dictionary.GetTelemetry(channelId);
        double eng = tdef.Calibrate(rawValue);
        LimitState state = Limits.Evaluate(tdef.Name, eng, ts);
        TelemetrySample sample = new(tdef.Name, tdef.ChannelId, rawValue, eng, tdef.Unit, state, ts);
        _latestTlm[tdef.Name] = sample;
        _tlmHistory.Enqueue(sample);
        return sample;
    }

    /// <summary>
    /// Decodes a 32-byte binary <c>strata::EventRecord</c> and appends it to the concurrent event history.
    /// </summary>
    /// <param name="raw32">Exactly 32 bytes representing a flight <c>EventRecord</c>.</param>
    /// <returns>The decoded <see cref="DecodedEventRecord"/>.</returns>
    public DecodedEventRecord IngestEventBytes(ReadOnlySpan<byte> raw32)
    {
        DecodedEventRecord ev = FlightCodecs.DecodeEventRecord(raw32);
        _evrHistory.Enqueue(ev);
        return ev;
    }

    /// <summary>
    /// Processes an inbound raw wire frame: archives to PCAP, dispatches <c>STTM</c>, <c>STEV</c>,
    /// or CCSDS PUS-C TM packets, and invokes all registered custom handlers.
    /// </summary>
    /// <param name="packet">Raw inbound wire bytes.</param>
    /// <param name="timestampNs">Optional nanosecond capture timestamp.</param>
    public void IngestRaw(byte[] packet, long? timestampNs = null)
    {
        ArgumentNullException.ThrowIfNull(packet);
        long ts = timestampNs ?? PcapWriter.GetUtcUnixNanoseconds();
        WriteToPcap(packet, ts);

        if (packet.Length == 14 &&
            packet[0] == (byte)'S' && packet[1] == (byte)'T' &&
            packet[2] == (byte)'T' && packet[3] == (byte)'M')
        {
            ushort chId = BinaryPrimitives.ReadUInt16BigEndian(packet.AsSpan(4, 2));
            ulong rawVal = BinaryPrimitives.ReadUInt64BigEndian(packet.AsSpan(6, 8));
            if (Dictionary.TelemetryById.ContainsKey(chId))
            {
                IngestTelemetryChannel(chId, rawVal, ts);
            }
        }
        else if (packet.Length == 36 &&
                 packet[0] == (byte)'S' && packet[1] == (byte)'T' &&
                 packet[2] == (byte)'E' && packet[3] == (byte)'V')
        {
            IngestEventBytes(packet.AsSpan(4, 32));
        }
        else if (packet.Length >= 21 && (packet[0] & 0xF8) == 0x08 && (packet[6] >> 4) == 2)
        {
            try
            {
                DecodedPusCTm tm = FlightCodecs.DecodePusCTm(packet);
                _pusTmHistory.Enqueue(tm);
            }
            catch (InvalidDataException)
            {
                // Ignore malformed packets safely without interrupting telemetry workers
            }
        }

        foreach (Action<byte[], long> handler in _handlers)
        {
            handler(packet, ts);
        }
    }

    /// <summary>
    /// Attempts to retrieve the most recent calibrated sample for <paramref name="channelName"/>.
    /// </summary>
    /// <param name="channelName">Symbolic channel name.</param>
    /// <param name="sample">When found, receives the latest <see cref="TelemetrySample"/>.</param>
    /// <returns><see langword="true"/> if at least one sample has arrived; otherwise, <see langword="false"/>.</returns>
    public bool TryGetLatestTelemetry(string channelName, out TelemetrySample? sample) =>
        _latestTlm.TryGetValue(channelName, out sample);

    /// <summary>
    /// Returns a snapshot of the most recent sample for every observed telemetry channel.
    /// </summary>
    /// <returns>Read-only list of latest <see cref="TelemetrySample"/> records.</returns>
    public IReadOnlyList<TelemetrySample> GetLatestTelemetrySnapshot() => _latestTlm.Values.ToList();

    /// <summary>
    /// Returns a snapshot of all ingested telemetry samples.
    /// </summary>
    /// <returns>Read-only list of <see cref="TelemetrySample"/> history entries.</returns>
    public IReadOnlyList<TelemetrySample> GetTelemetryHistory() => _tlmHistory.ToArray();

    /// <summary>
    /// Returns a snapshot of decoded flight <c>EventRecord</c>s.
    /// </summary>
    /// <param name="maxRecent">Maximum number of most-recent events to return (0 returns all).</param>
    /// <returns>Read-only list of <see cref="DecodedEventRecord"/> entries.</returns>
    public IReadOnlyList<DecodedEventRecord> GetEventHistory(int maxRecent = 0)
    {
        DecodedEventRecord[] arr = _evrHistory.ToArray();
        return (maxRecent > 0 && arr.Length > maxRecent) ? arr[^maxRecent..] : arr;
    }

    /// <summary>
    /// Returns a snapshot of decoded PUS-C TM packets.
    /// </summary>
    /// <param name="maxRecent">Maximum number of most-recent packets to return (0 returns all).</param>
    /// <returns>Read-only list of <see cref="DecodedPusCTm"/> entries.</returns>
    public IReadOnlyList<DecodedPusCTm> GetPusTmHistory(int maxRecent = 0)
    {
        DecodedPusCTm[] arr = _pusTmHistory.ToArray();
        return (maxRecent > 0 && arr.Length > maxRecent) ? arr[^maxRecent..] : arr;
    }

    /// <summary>
    /// Returns a snapshot of transmitted uplink command audit records.
    /// </summary>
    /// <param name="maxRecent">Maximum number of most-recent commands to return (0 returns all).</param>
    /// <returns>Read-only list of <see cref="CommandHistoryEntry"/> records.</returns>
    public IReadOnlyList<CommandHistoryEntry> GetCommandHistory(int maxRecent = 0)
    {
        CommandHistoryEntry[] arr = _cmdHistory.ToArray();
        return (maxRecent > 0 && arr.Length > maxRecent) ? arr[^maxRecent..] : arr;
    }

    /// <summary>
    /// Exports calibrated telemetry samples and limit states asynchronously to a CSV file.
    /// </summary>
    /// <param name="samples">Sequence of calibrated <see cref="TelemetrySample"/> entries.</param>
    /// <param name="outPath">Destination <c>.csv</c> file path.</param>
    /// <param name="cancellationToken">Cancellation token for cooperative async cancellation.</param>
    public static async Task ExportTelemetryCsvAsync(
        IReadOnlyList<TelemetrySample> samples,
        string outPath,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(samples);
        ArgumentException.ThrowIfNullOrWhiteSpace(outPath);

        StringBuilder sb = new(samples.Count * 64 + 80);
        sb.AppendLine("timestamp_ns,channel_id,channel,raw_value,eng_value,unit,limit_state");
        foreach (TelemetrySample s in samples)
        {
            sb.Append(s.TimestampNs.ToString(CultureInfo.InvariantCulture)).Append(',')
              .Append(CultureInfo.InvariantCulture, $"0x{s.ChannelId:X4}").Append(',')
              .Append(s.Channel).Append(',')
              .Append(s.RawValue.ToString(CultureInfo.InvariantCulture)).Append(',')
              .Append(s.EngValue.ToString("G6", CultureInfo.InvariantCulture)).Append(',')
              .Append(s.Unit).Append(',')
              .AppendLine(s.LimitState.ToWireString());
        }

        await File.WriteAllTextAsync(outPath, sb.ToString(), new UTF8Encoding(false), cancellationToken).ConfigureAwait(false);
    }

    /// <summary>
    /// Stops any active UDP listener and flushes/closes active PCAP files.
    /// </summary>
    public async ValueTask DisposeAsync()
    {
        await StopUdpListenerAsync().ConfigureAwait(false);
        StopPcapCapture();
    }

    private void WriteToPcap(byte[] data, long ts)
    {
        using (_pcapGate.EnterScope())
        {
            _pcap?.WritePacket(data, ts);
        }
    }
}
