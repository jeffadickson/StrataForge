using System.Globalization;

namespace StrataForge.Ground;

/// <summary>
/// Thread-safe, lossless rotating PCAP binary stream capture engine for long-duration TVAC and mission passes.
/// </summary>
/// <remarks>
/// Implements <c>STRATA-GDS-007</c>. Automatically rolls over to a sequentially numbered Wireshark-compatible
/// <c>.pcap</c> segment whenever <see cref="MaxBytesPerFile"/> or <see cref="MaxDurationPerFile"/> is reached,
/// preventing unbounded file growth without dropping frames.
/// </remarks>
public sealed class RotatingPcapArchiver : IDisposable
{
    private readonly Lock _gate = new();
    private readonly string _directory;
    private readonly string _filePrefix;
    private readonly List<string> _createdFiles = [];

    private PcapWriter? _activeWriter;
    private long _activeBytes;
    private long _activeStartNs;
    private int _segmentIndex;
    private bool _disposed;

    /// <summary>
    /// Gets the maximum byte threshold per segment before rotating to a new <c>.pcap</c> file.
    /// </summary>
    public long MaxBytesPerFile { get; }

    /// <summary>
    /// Gets the maximum capture duration per segment before rotating to a new <c>.pcap</c> file.
    /// </summary>
    public TimeSpan MaxDurationPerFile { get; }

    /// <summary>
    /// Gets a thread-safe snapshot of all segment file paths created by this archiver.
    /// </summary>
    public IReadOnlyList<string> CreatedFiles
    {
        get
        {
            using (_gate.EnterScope())
            {
                return [.. _createdFiles];
            }
        }
    }

    /// <summary>
    /// Initializes a new <see cref="RotatingPcapArchiver"/> writing numbered segments into <paramref name="directory"/>.
    /// </summary>
    /// <param name="directory">Output directory (created automatically if absent).</param>
    /// <param name="filePrefix">Filename prefix (segments are named <c>{prefix}_0000.pcap</c>, etc.).</param>
    /// <param name="maxBytesPerFile">Maximum segment size in bytes before rolling over (default 16 MiB).</param>
    /// <param name="maxDurationPerFile">Optional maximum time span per segment (default 1 hour).</param>
    public RotatingPcapArchiver(
        string directory,
        string filePrefix = "strata_pass",
        long maxBytesPerFile = 16L * 1024L * 1024L,
        TimeSpan? maxDurationPerFile = null)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(directory);
        ArgumentException.ThrowIfNullOrWhiteSpace(filePrefix);
        Directory.CreateDirectory(directory);
        _directory = directory;
        _filePrefix = filePrefix;
        MaxBytesPerFile = Math.Max(64L, maxBytesPerFile);
        MaxDurationPerFile = maxDurationPerFile ?? TimeSpan.FromHours(1);
    }

    /// <summary>
    /// Writes a timestamped packet to the active PCAP segment, rolling over atomically if byte or time caps are reached.
    /// </summary>
    /// <param name="data">Raw wire frame bytes.</param>
    /// <param name="timestampNs">Optional nanosecond Unix timestamp.</param>
    public void WritePacket(ReadOnlySpan<byte> data, long? timestampNs = null)
    {
        long ts = timestampNs ?? PcapWriter.GetUtcUnixNanoseconds();
        long pktBytes = 16L + data.Length;

        using (_gate.EnterScope())
        {
            if (_disposed)
            {
                return;
            }

            bool needRotate = _activeWriter is null ||
                              (_activeBytes > 24L && _activeBytes + pktBytes > MaxBytesPerFile) ||
                              (_activeBytes > 24L && (ts - _activeStartNs) >= (long)(MaxDurationPerFile.TotalSeconds * 1_000_000_000.0));

            if (needRotate)
            {
                _activeWriter?.Dispose();
                string path = Path.Combine(
                    _directory,
                    string.Create(CultureInfo.InvariantCulture, $"{_filePrefix}_{_segmentIndex:D4}.pcap"));
                _segmentIndex++;
                _activeWriter = new PcapWriter(path);
                _createdFiles.Add(path);
                _activeBytes = 24L;
                _activeStartNs = ts;
            }

            _activeWriter!.WritePacket(data, ts);
            _activeBytes += pktBytes;
        }
    }

    /// <summary>
    /// Flushes and closes the active PCAP segment.
    /// </summary>
    public void Dispose()
    {
        using (_gate.EnterScope())
        {
            if (_disposed)
            {
                return;
            }

            _disposed = true;
            _activeWriter?.Dispose();
            _activeWriter = null;
        }
    }
}

/// <summary>
/// Extracts time-bounded anomaly intervals from standard Wireshark-compatible PCAP capture files.
/// </summary>
/// <remarks>
/// Implements <c>STRATA-GDS-007</c>.
/// </remarks>
public static class PcapSlicer
{
    /// <summary>
    /// Copies all packets whose capture timestamp falls inclusively within <c>[startNs, endNs]</c>
    /// from <paramref name="inputPcapPath"/> into a new standalone <paramref name="outputPcapPath"/>.
    /// </summary>
    /// <param name="inputPcapPath">Source <c>.pcap</c> archive path.</param>
    /// <param name="outputPcapPath">Destination sliced <c>.pcap</c> path.</param>
    /// <param name="startNs">Inclusive start timestamp in nanoseconds.</param>
    /// <param name="endNs">Inclusive end timestamp in nanoseconds.</param>
    /// <returns>The number of packets copied into the output slice.</returns>
    public static int SliceByTimeWindow(string inputPcapPath, string outputPcapPath, long startNs, long endNs)
    {
        IReadOnlyList<(long TimestampNs, byte[] Payload)> packets = PcapReader.ReadAll(inputPcapPath);
        int copied = 0;
        using PcapWriter writer = new(outputPcapPath);
        foreach (var (ts, payload) in packets)
        {
            if (ts >= startNs && ts <= endNs)
            {
                writer.WritePacket(payload, ts);
                copied++;
            }
        }

        return copied;
    }
}
