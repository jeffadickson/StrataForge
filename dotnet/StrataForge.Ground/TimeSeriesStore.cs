using System.Collections.Concurrent;
using System.Text;
using System.Text.Json;
using System.Threading.Channels;

namespace StrataForge.Ground;

/// <summary>
/// Summary statistics computed over a time-series window for a telemetry channel.
/// </summary>
/// <param name="Channel">Symbolic channel name.</param>
/// <param name="SampleCount">Number of samples included in the window.</param>
/// <param name="Min">Minimum engineering value observed.</param>
/// <param name="Max">Maximum engineering value observed.</param>
/// <param name="Mean">Arithmetic mean engineering value.</param>
/// <param name="FirstTimestampNs">Earliest sample timestamp in nanoseconds.</param>
/// <param name="LastTimestampNs">Latest sample timestamp in nanoseconds.</param>
public sealed record TimeSeriesWindowStats(
    string Channel,
    int SampleCount,
    double Min,
    double Max,
    double Mean,
    long FirstTimestampNs,
    long LastTimestampNs);

/// <summary>
/// Thread-safe, bounded per-channel time-series store supporting fast range queries,
/// window aggregation statistics, and optional NDJSON persistence for post-TVAC analysis.
/// </summary>
/// <remarks>
/// Implements <c>STRATA-GDS-008</c>. Uses per-channel bounded rings protected by fine-grained locks so
/// concurrent telemetry ingestion never blocks unrelated channels or live REST/SSE queries.
/// </remarks>
public sealed class TelemetryTimeSeriesStore : IDisposable
{
    private sealed class ChannelSeries
    {
        public readonly Lock Gate = new();
        public readonly Queue<TelemetrySample> Ring = new();
    }

    private readonly ConcurrentDictionary<string, ChannelSeries> _series = new(StringComparer.Ordinal);
    private readonly int _maxSamplesPerChannel;
    private readonly Lock _fileGate = new();
    private StreamWriter? _ndjsonWriter;

    /// <summary>
    /// Initializes a new <see cref="TelemetryTimeSeriesStore"/>.
    /// </summary>
    /// <param name="maxSamplesPerChannel">Maximum in-memory samples retained per channel (default 10000).</param>
    /// <param name="ndjsonArchivePath">Optional file path to append live NDJSON records for durable archival.</param>
    public TelemetryTimeSeriesStore(int maxSamplesPerChannel = 10_000, string? ndjsonArchivePath = null)
    {
        _maxSamplesPerChannel = Math.Max(16, maxSamplesPerChannel);
        if (!string.IsNullOrWhiteSpace(ndjsonArchivePath))
        {
            _ndjsonWriter = new StreamWriter(
                new FileStream(ndjsonArchivePath, FileMode.Create, FileAccess.Write, FileShare.Read),
                new UTF8Encoding(false))
            {
                AutoFlush = true,
            };
        }
    }

    /// <summary>
    /// Records a calibrated <see cref="TelemetrySample"/> into its channel ring and optional NDJSON archive.
    /// </summary>
    /// <param name="sample">Telemetry sample to record.</param>
    public void Append(TelemetrySample sample)
    {
        ArgumentNullException.ThrowIfNull(sample);
        ChannelSeries series = _series.GetOrAdd(sample.Channel, _ => new ChannelSeries());
        using (series.Gate.EnterScope())
        {
            series.Ring.Enqueue(sample);
            while (series.Ring.Count > _maxSamplesPerChannel)
            {
                series.Ring.Dequeue();
            }
        }

        if (_ndjsonWriter is not null)
        {
            string line = JsonSerializer.Serialize(new
            {
                ts = sample.TimestampNs,
                ch = sample.Channel,
                id = sample.ChannelId,
                raw = sample.RawValue,
                eng = sample.EngValue,
                unit = sample.Unit,
                state = sample.LimitState.ToWireString(),
            });
            using (_fileGate.EnterScope())
            {
                _ndjsonWriter?.WriteLine(line);
            }
        }
    }

    /// <summary>
    /// Queries samples for <paramref name="channel"/> within an optional inclusive nanosecond range
    /// and returns up to <paramref name="maxPoints"/> most-recent points.
    /// </summary>
    /// <param name="channel">Symbolic channel name.</param>
    /// <param name="startNs">Optional inclusive lower timestamp bound (nanoseconds).</param>
    /// <param name="endNs">Optional inclusive upper timestamp bound (nanoseconds).</param>
    /// <param name="maxPoints">Maximum number of points to return (0 returns all matches).</param>
    /// <returns>Chronologically ordered list of matching <see cref="TelemetrySample"/> records.</returns>
    public IReadOnlyList<TelemetrySample> QueryRange(
        string channel,
        long? startNs = null,
        long? endNs = null,
        int maxPoints = 0)
    {
        if (!_series.TryGetValue(channel, out ChannelSeries? series))
        {
            return Array.Empty<TelemetrySample>();
        }

        List<TelemetrySample> matches = [];
        using (series.Gate.EnterScope())
        {
            foreach (TelemetrySample s in series.Ring)
            {
                if (startNs.HasValue && s.TimestampNs < startNs.Value) continue;
                if (endNs.HasValue && s.TimestampNs > endNs.Value) continue;
                matches.Add(s);
            }
        }

        return (maxPoints > 0 && matches.Count > maxPoints)
            ? matches.GetRange(matches.Count - maxPoints, maxPoints)
            : matches;
    }

    /// <summary>
    /// Computes min, max, and arithmetic mean engineering statistics over a channel's time window.
    /// </summary>
    /// <param name="channel">Symbolic channel name.</param>
    /// <param name="startNs">Optional inclusive lower timestamp bound.</param>
    /// <param name="endNs">Optional inclusive upper timestamp bound.</param>
    /// <returns>Aggregated <see cref="TimeSeriesWindowStats"/>, or <see langword="null"/> if no samples matched.</returns>
    public TimeSeriesWindowStats? ComputeWindowStats(string channel, long? startNs = null, long? endNs = null)
    {
        IReadOnlyList<TelemetrySample> pts = QueryRange(channel, startNs, endNs);
        if (pts.Count == 0)
        {
            return null;
        }

        double min = double.PositiveInfinity;
        double max = double.NegativeInfinity;
        double sum = 0.0;
        foreach (TelemetrySample s in pts)
        {
            if (s.EngValue < min) min = s.EngValue;
            if (s.EngValue > max) max = s.EngValue;
            sum += s.EngValue;
        }

        return new TimeSeriesWindowStats(
            Channel: channel,
            SampleCount: pts.Count,
            Min: min,
            Max: max,
            Mean: sum / pts.Count,
            FirstTimestampNs: pts[0].TimestampNs,
            LastTimestampNs: pts[^1].TimestampNs);
    }

    /// <summary>
    /// Flushes and closes the optional NDJSON archive stream.
    /// </summary>
    public void Dispose()
    {
        using (_fileGate.EnterScope())
        {
            _ndjsonWriter?.Dispose();
            _ndjsonWriter = null;
        }
    }
}

/// <summary>
/// Asynchronous, non-blocking alert notification dispatcher backed by a bounded <see cref="Channel{T}"/>
/// with per-channel cooldown deduplication.
/// </summary>
/// <remarks>
/// Implements <c>STRATA-GDS-008</c>. Fans out Red/Yellow <see cref="LimitAlert"/> transitions to pluggable
/// asynchronous sinks (webhooks, operator consoles, log files) without blocking hot telemetry paths.
/// </remarks>
public sealed class AlertNotificationHub : IAsyncDisposable
{
    private readonly Channel<LimitAlert> _channel;
    private readonly ConcurrentBag<Func<LimitAlert, CancellationToken, ValueTask>> _sinks = [];
    private readonly ConcurrentDictionary<string, long> _lastSentNs = new(StringComparer.Ordinal);
    private readonly long _cooldownNs;
    private readonly CancellationTokenSource _cts = new();
    private readonly Task _dispatchTask;
    private long _deliveredCount;
    private long _suppressedCount;

    /// <summary>
    /// Gets the total number of alerts delivered to sinks.
    /// </summary>
    public long DeliveredCount => Volatile.Read(ref _deliveredCount);

    /// <summary>
    /// Gets the total number of alerts suppressed by per-channel cooldown deduplication.
    /// </summary>
    public long SuppressedCount => Volatile.Read(ref _suppressedCount);

    /// <summary>
    /// Initializes and starts a new <see cref="AlertNotificationHub"/> background dispatcher.
    /// </summary>
    /// <param name="cooldown">Minimum interval between repeated notifications for the same channel (default 0 = no cooldown).</param>
    /// <param name="queueCapacity">Bounded internal channel capacity (default 1024).</param>
    public AlertNotificationHub(TimeSpan? cooldown = null, int queueCapacity = 1024)
    {
        _cooldownNs = cooldown.HasValue ? (long)(cooldown.Value.TotalSeconds * 1_000_000_000.0) : 0L;
        _channel = Channel.CreateBounded<LimitAlert>(new BoundedChannelOptions(Math.Max(16, queueCapacity))
        {
            FullMode = BoundedChannelFullMode.DropOldest,
            SingleReader = true,
        });
        _dispatchTask = Task.Run(DispatchLoopAsync);
    }

    /// <summary>
    /// Registers an asynchronous notification sink (for example, webhook poster or pager callback).
    /// </summary>
    /// <param name="sink">Async delegate invoked for each non-suppressed <see cref="LimitAlert"/>.</param>
    public void AddSink(Func<LimitAlert, CancellationToken, ValueTask> sink)
    {
        ArgumentNullException.ThrowIfNull(sink);
        _sinks.Add(sink);
    }

    /// <summary>
    /// Enqueues a <see cref="LimitAlert"/> for asynchronous fan-out without blocking the caller.
    /// </summary>
    /// <param name="alert">The limit alert to publish.</param>
    public void Publish(LimitAlert alert)
    {
        ArgumentNullException.ThrowIfNull(alert);
        _channel.Writer.TryWrite(alert);
    }

    /// <summary>
    /// Completes the alert channel and awaits delivery of all queued notifications.
    /// </summary>
    public async ValueTask DisposeAsync()
    {
        _channel.Writer.TryComplete();
        await _dispatchTask.ConfigureAwait(false);
        await _cts.CancelAsync().ConfigureAwait(false);
        _cts.Dispose();
    }

    private async Task DispatchLoopAsync()
    {
        await foreach (LimitAlert alert in _channel.Reader.ReadAllAsync(_cts.Token).ConfigureAwait(false))
        {
            if (_cooldownNs > 0L &&
                _lastSentNs.TryGetValue(alert.Channel, out long prevNs) &&
                alert.TimestampNs > prevNs &&
                (alert.TimestampNs - prevNs) < _cooldownNs)
            {
                Interlocked.Increment(ref _suppressedCount);
                continue;
            }

            _lastSentNs[alert.Channel] = alert.TimestampNs;
            foreach (Func<LimitAlert, CancellationToken, ValueTask> sink in _sinks)
            {
                try
                {
                    await sink(alert, _cts.Token).ConfigureAwait(false);
                }
                catch (Exception ex) when (ex is not OperationCanceledException)
                {
                    // Isolate failing external sinks from the telemetry pipeline
                }
            }

            Interlocked.Increment(ref _deliveredCount);
        }
    }
}
