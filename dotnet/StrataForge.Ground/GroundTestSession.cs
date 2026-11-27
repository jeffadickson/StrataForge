namespace StrataForge.Ground;

/// <summary>
/// High-level asynchronous Integration &amp; Test (I&amp;T) automation session for FlatSat, TVAC, Host SIL, and HIL.
/// </summary>
/// <remarks>
/// Implements <c>STRATA-GDS-004</c>. Uses thread-safe atomic sequence counters (<see cref="Interlocked"/>) and
/// non-blocking <see cref="PeriodicTimer"/> waits so automated test procedures can drive live flight software
/// concurrently across multiple async tasks.
/// </remarks>
public sealed class GroundTestSession
{
    private readonly Action<byte[]> _uplinkSender;
    private int _tcSeq = -1;
    private int _pusSeq = -1;

    /// <summary>
    /// Gets the mission dictionary backing command validation.
    /// </summary>
    public MissionDictionary Dictionary { get; }

    /// <summary>
    /// Gets the live <see cref="TelemetryBroker"/> supplying telemetry, event, and PUS-C TM streams.
    /// </summary>
    public TelemetryBroker Broker { get; }

    /// <summary>
    /// Gets the 10-bit CCSDS Spacecraft ID stamped into uplinked TC Transfer Frames.
    /// </summary>
    public ushort Scid { get; }

    /// <summary>
    /// Gets the 6-bit CCSDS Virtual Channel ID stamped into uplinked TC Transfer Frames.
    /// </summary>
    public byte Vcid { get; }

    /// <summary>
    /// Initializes a new <see cref="GroundTestSession"/>.
    /// </summary>
    /// <param name="dictionary">Mission dictionary validating opcodes and payload lengths.</param>
    /// <param name="broker">Live <see cref="TelemetryBroker"/> receiving downlink traffic.</param>
    /// <param name="uplinkSender">Delegate transmitting framed bytes over UDP, serial, or SIL transport.</param>
    /// <param name="scid">10-bit CCSDS Spacecraft ID (default <c>0x123</c>).</param>
    /// <param name="vcid">6-bit CCSDS Virtual Channel ID (default 1).</param>
    public GroundTestSession(
        MissionDictionary dictionary,
        TelemetryBroker broker,
        Action<byte[]> uplinkSender,
        ushort scid = 0x0123,
        byte vcid = 1)
    {
        ArgumentNullException.ThrowIfNull(dictionary);
        ArgumentNullException.ThrowIfNull(broker);
        ArgumentNullException.ThrowIfNull(uplinkSender);
        Dictionary = dictionary;
        Broker = broker;
        _uplinkSender = uplinkSender;
        Scid = scid;
        Vcid = vcid;
    }

    /// <summary>
    /// Validates a named command, frames it in a CCSDS TC Transfer Frame with CRC-16 FECF, logs it, and transmits it.
    /// </summary>
    /// <param name="commandName">Symbolic command name from <see cref="MissionDictionary"/>.</param>
    /// <param name="argBytes">Raw argument payload matching the command's declared length.</param>
    /// <returns>The transmitted CCSDS TC Transfer Frame bytes.</returns>
    public byte[] SendCommand(string commandName, ReadOnlySpan<byte> argBytes = default)
    {
        byte seq = (byte)(Interlocked.Increment(ref _tcSeq) & 0xFF);
        byte[] frame = FlightCodecs.EncodeNamedCommand(Dictionary, commandName, argBytes, Scid, Vcid, seq);
        Broker.RecordCommand(commandName, frame);
        _uplinkSender(frame);
        return frame;
    }

    /// <summary>
    /// Encodes, logs, and transmits an ECSS-E-ST-70-41C (PUS-C) Telecommand Space Packet.
    /// </summary>
    /// <param name="serviceType">PUS-C Service Type ID.</param>
    /// <param name="serviceSubtype">PUS-C Service Subtype ID.</param>
    /// <param name="appData">Optional PUS-C application data bytes.</param>
    /// <param name="apid">11-bit destination APID (default <c>0x105</c>).</param>
    /// <param name="ackFlags">4-bit ST[01] verification mask (default <c>0x09</c>).</param>
    /// <returns>The transmitted CCSDS Space Packet bytes.</returns>
    public byte[] SendPusTc(
        byte serviceType,
        byte serviceSubtype,
        ReadOnlySpan<byte> appData = default,
        ushort apid = 0x0105,
        byte ackFlags = 0x09)
    {
        ushort seq = (ushort)(Interlocked.Increment(ref _pusSeq) & 0x3FFF);
        byte[] pkt = FlightCodecs.EncodePusCTc(apid, seq, serviceType, serviceSubtype, appData, ackFlags);
        Broker.RecordCommand($"PUS_TC({serviceType},{serviceSubtype})", pkt);
        _uplinkSender(pkt);
        return pkt;
    }

    /// <summary>
    /// Asynchronously waits until telemetry channel <paramref name="channelName"/> satisfies <paramref name="predicate"/>.
    /// </summary>
    /// <param name="channelName">Symbolic telemetry channel name.</param>
    /// <param name="predicate">Condition evaluated against each latest <see cref="TelemetrySample"/>.</param>
    /// <param name="timeout">Maximum duration to wait before throwing <see cref="TimeoutException"/>.</param>
    /// <param name="cancellationToken">Optional cancellation token.</param>
    /// <returns>The matching <see cref="TelemetrySample"/>.</returns>
    /// <exception cref="TimeoutException">Thrown if the predicate is not satisfied within <paramref name="timeout"/>.</exception>
    public async Task<TelemetrySample> WaitForTelemetryAsync(
        string channelName,
        Func<TelemetrySample, bool> predicate,
        TimeSpan? timeout = null,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(predicate);
        TimeSpan maxWait = timeout ?? TimeSpan.FromSeconds(2);
        DateTimeOffset deadline = DateTimeOffset.UtcNow + maxWait;

        using PeriodicTimer timer = new(TimeSpan.FromMilliseconds(10));
        do
        {
            if (Broker.TryGetLatestTelemetry(channelName, out TelemetrySample? sample) &&
                sample is not null &&
                predicate(sample))
            {
                return sample;
            }

            if (DateTimeOffset.UtcNow >= deadline)
            {
                throw new TimeoutException($"Timed out waiting for telemetry '{channelName}' after {maxWait.TotalSeconds:F2}s.");
            }
        }
        while (await timer.WaitForNextTickAsync(cancellationToken).ConfigureAwait(false));

        throw new TimeoutException($"Timed out waiting for telemetry '{channelName}'.");
    }

    /// <summary>
    /// Asynchronously waits until a decoded flight <c>EventRecord</c> matching <paramref name="eventId"/> arrives.
    /// </summary>
    /// <param name="eventId">Expected 32-bit event identifier.</param>
    /// <param name="timeout">Maximum duration to wait before throwing <see cref="TimeoutException"/>.</param>
    /// <param name="cancellationToken">Optional cancellation token.</param>
    /// <returns>The matching <see cref="DecodedEventRecord"/>.</returns>
    /// <exception cref="TimeoutException">Thrown if no matching event arrives before <paramref name="timeout"/>.</exception>
    public async Task<DecodedEventRecord> WaitForEventAsync(
        uint eventId,
        TimeSpan? timeout = null,
        CancellationToken cancellationToken = default)
    {
        TimeSpan maxWait = timeout ?? TimeSpan.FromSeconds(2);
        DateTimeOffset deadline = DateTimeOffset.UtcNow + maxWait;

        using PeriodicTimer timer = new(TimeSpan.FromMilliseconds(10));
        do
        {
            IReadOnlyList<DecodedEventRecord> evs = Broker.GetEventHistory();
            for (int i = evs.Count - 1; i >= 0; i--)
            {
                if (evs[i].EventId == eventId)
                {
                    return evs[i];
                }
            }

            if (DateTimeOffset.UtcNow >= deadline)
            {
                throw new TimeoutException($"Timed out waiting for EVR 0x{eventId:X4} after {maxWait.TotalSeconds:F2}s.");
            }
        }
        while (await timer.WaitForNextTickAsync(cancellationToken).ConfigureAwait(false));

        throw new TimeoutException($"Timed out waiting for EVR 0x{eventId:X4}.");
    }

    /// <summary>
    /// Asynchronously waits until a PUS-C TM packet matching <paramref name="serviceType"/> and <paramref name="serviceSubtype"/> arrives.
    /// </summary>
    /// <param name="serviceType">Expected PUS-C Service Type ID.</param>
    /// <param name="serviceSubtype">Expected PUS-C Service Subtype ID.</param>
    /// <param name="timeout">Maximum duration to wait before throwing <see cref="TimeoutException"/>.</param>
    /// <param name="cancellationToken">Optional cancellation token.</param>
    /// <returns>The matching <see cref="DecodedPusCTm"/> packet.</returns>
    /// <exception cref="TimeoutException">Thrown if no matching PUS-C TM arrives before <paramref name="timeout"/>.</exception>
    public async Task<DecodedPusCTm> WaitForPusTmAsync(
        byte serviceType,
        byte serviceSubtype,
        TimeSpan? timeout = null,
        CancellationToken cancellationToken = default)
    {
        TimeSpan maxWait = timeout ?? TimeSpan.FromSeconds(2);
        DateTimeOffset deadline = DateTimeOffset.UtcNow + maxWait;

        using PeriodicTimer timer = new(TimeSpan.FromMilliseconds(10));
        do
        {
            IReadOnlyList<DecodedPusCTm> tms = Broker.GetPusTmHistory();
            for (int i = tms.Count - 1; i >= 0; i--)
            {
                if (tms[i].ServiceType == serviceType && tms[i].ServiceSubtype == serviceSubtype)
                {
                    return tms[i];
                }
            }

            if (DateTimeOffset.UtcNow >= deadline)
            {
                throw new TimeoutException($"Timed out waiting for PUS-C TM({serviceType},{serviceSubtype}) after {maxWait.TotalSeconds:F2}s.");
            }
        }
        while (await timer.WaitForNextTickAsync(cancellationToken).ConfigureAwait(false));

        throw new TimeoutException($"Timed out waiting for PUS-C TM({serviceType},{serviceSubtype}).");
    }

    /// <summary>
    /// Asserts that all monitored channels in <see cref="Broker"/> are currently <see cref="LimitState.Nominal"/>.
    /// </summary>
    /// <exception cref="InvalidOperationException">Thrown if any monitored channel is in a Warning or Error state.</exception>
    public void AssertLimitsNominal()
    {
        if (!Broker.Limits.AllNominal())
        {
            string bad = string.Join(", ", Broker.Limits.GetNonNominalStates().Select(kv => $"{kv.Key}={kv.Value.ToWireString()}"));
            throw new InvalidOperationException($"Non-nominal telemetry limits active: {bad}");
        }
    }
}
