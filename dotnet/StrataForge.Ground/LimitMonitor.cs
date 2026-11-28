namespace StrataForge.Ground;

/// <summary>
/// Five-level engineering telemetry limit classification used by <see cref="LimitMonitor"/>.
/// </summary>
public enum LimitState
{
    /// <summary>Channel value is within nominal operating bounds.</summary>
    Nominal = 0,

    /// <summary>Channel value is below the lower Yellow warning threshold.</summary>
    WarnLo = 1,

    /// <summary>Channel value is above the upper Yellow warning threshold.</summary>
    WarnHi = 2,

    /// <summary>Channel value is below the lower Red error threshold.</summary>
    ErrorLo = 3,

    /// <summary>Channel value is above the upper Red error threshold.</summary>
    ErrorHi = 4,
}

/// <summary>
/// Formatting helpers converting <see cref="LimitState"/> values to canonical ground-console strings.
/// </summary>
public static class LimitStateExtensions
{
    /// <summary>
    /// Converts a <see cref="LimitState"/> enum value into its canonical uppercase protocol string
    /// (<c>NOMINAL</c>, <c>WARN_LO</c>, <c>WARN_HI</c>, <c>ERROR_LO</c>, <c>ERROR_HI</c>).
    /// </summary>
    /// <param name="state">The limit state to format.</param>
    /// <returns>The canonical uppercase state string.</returns>
    public static string ToWireString(this LimitState state) => state switch
    {
        LimitState.Nominal => "NOMINAL",
        LimitState.WarnLo => "WARN_LO",
        LimitState.WarnHi => "WARN_HI",
        LimitState.ErrorLo => "ERROR_LO",
        LimitState.ErrorHi => "ERROR_HI",
        _ => "NOMINAL",
    };
}

/// <summary>
/// Optional state-dependent gating predicate for a <see cref="LimitSpec"/> (for example,
/// enforce thermal limits only when <c>HSI_MODE == Science</c>).
/// </summary>
/// <param name="WatchChannel">Symbolic peer channel whose latest engineering value gates limit evaluation.</param>
/// <param name="Predicate">Predicate returning <see langword="true"/> when the limit is active.</param>
public sealed record LimitCondition(string WatchChannel, Func<double, bool> Predicate);

/// <summary>
/// Solved raw hardware Data Number (DN) equivalents of a telemetry channel's Red/Yellow limits.
/// </summary>
/// <param name="Channel">Symbolic channel name.</param>
/// <param name="LoErrDn">Optional lower Red error raw DN.</param>
/// <param name="LoWarnDn">Optional lower Yellow warning raw DN.</param>
/// <param name="HiWarnDn">Optional upper Yellow warning raw DN.</param>
/// <param name="HiErrDn">Optional upper Red error raw DN.</param>
public sealed record RawDnLimitThresholds(
    string Channel,
    ulong? LoErrDn,
    ulong? LoWarnDn,
    ulong? HiWarnDn,
    ulong? HiErrDn);

/// <summary>
/// Threshold specification, persistence debounce count, and optional mode-gating condition for a telemetry channel.
/// </summary>
/// <param name="Channel">Symbolic telemetry channel name.</param>
/// <param name="LoErr">Optional lower Red error limit.</param>
/// <param name="LoWarn">Optional lower Yellow warning limit.</param>
/// <param name="HiWarn">Optional upper Yellow warning limit.</param>
/// <param name="HiErr">Optional upper Red error limit.</param>
/// <param name="Persistence">Number of consecutive out-of-limit samples required before tripping an alert.</param>
/// <param name="Condition">Optional peer-channel gating condition; when unsatisfied, the channel evaluates as <see cref="LimitState.Nominal"/>.</param>
public sealed record LimitSpec(
    string Channel,
    double? LoErr = null,
    double? LoWarn = null,
    double? HiWarn = null,
    double? HiErr = null,
    int Persistence = 1,
    LimitCondition? Condition = null);

/// <summary>
/// Immutable alert record emitted whenever a channel completes a debounced <see cref="LimitState"/> transition.
/// </summary>
/// <param name="Channel">Symbolic telemetry channel name.</param>
/// <param name="State">Newly active <see cref="LimitState"/>.</param>
/// <param name="PreviousState">Prior <see cref="LimitState"/> before the transition.</param>
/// <param name="Value">Calibrated engineering value that triggered the transition.</param>
/// <param name="Unit">Engineering unit label.</param>
/// <param name="TimestampNs">Nanosecond TAI or host monotonic timestamp of the sample.</param>
public sealed record LimitAlert(
    string Channel,
    LimitState State,
    LimitState PreviousState,
    double Value,
    string Unit,
    long TimestampNs);

/// <summary>
/// Thread-safe real-time Red/Yellow telemetry limit monitor with configurable persistence debouncing.
/// </summary>
/// <remarks>
/// Implements <c>STRATA-GDS-002</c>. Uses .NET's lightweight <see cref="Lock"/> synchronization primitive
/// so concurrent telemetry worker threads and REST API readers never observe torn state transitions.
/// </remarks>
public sealed class LimitMonitor
{
    private readonly Lock _gate = new();
    private readonly Dictionary<string, LimitSpec> _specs = new(StringComparer.Ordinal);
    private readonly Dictionary<string, string> _units = new(StringComparer.Ordinal);
    private readonly Dictionary<string, LimitState> _states = new(StringComparer.Ordinal);
    private readonly Dictionary<string, LimitState> _pendingState = new(StringComparer.Ordinal);
    private readonly Dictionary<string, int> _pendingCount = new(StringComparer.Ordinal);
    private readonly List<Action<LimitAlert>> _callbacks = [];
    private readonly List<LimitAlert> _alertHistory = [];

    /// <summary>
    /// Initializes a new <see cref="LimitMonitor"/>, optionally pre-registering all channel limits declared
    /// in <paramref name="dictionary"/>.
    /// </summary>
    /// <param name="dictionary">Optional <see cref="MissionDictionary"/> from which channel limits are loaded.</param>
    public LimitMonitor(MissionDictionary? dictionary = null)
    {
        if (dictionary is null)
        {
            return;
        }

        foreach (TelemetryDef t in dictionary.TelemetryByName.Values)
        {
            _units[t.Name] = t.Unit;
            if (t.LimitLoErr.HasValue || t.LimitLoWarn.HasValue || t.LimitHiWarn.HasValue || t.LimitHiErr.HasValue)
            {
                Register(
                    new LimitSpec(t.Name, t.LimitLoErr, t.LimitLoWarn, t.LimitHiWarn, t.LimitHiErr, Persistence: 1),
                    t.Unit);
            }
        }
    }

    /// <summary>
    /// Registers or replaces the <see cref="LimitSpec"/> for a telemetry channel.
    /// </summary>
    /// <param name="spec">Limit bounds and persistence debounce count.</param>
    /// <param name="unit">Optional engineering unit string for alert formatting.</param>
    public void Register(LimitSpec spec, string unit = "")
    {
        ArgumentNullException.ThrowIfNull(spec);
        using (_gate.EnterScope())
        {
            _specs[spec.Channel] = spec;
            if (!string.IsNullOrEmpty(unit))
            {
                _units[spec.Channel] = unit;
            }

            _states.TryAdd(spec.Channel, LimitState.Nominal);
        }
    }

    /// <summary>
    /// Subscribes a callback invoked whenever a debounced limit state transition occurs.
    /// </summary>
    /// <param name="callback">Delegate receiving the newly emitted <see cref="LimitAlert"/>.</param>
    public void OnAlert(Action<LimitAlert> callback)
    {
        ArgumentNullException.ThrowIfNull(callback);
        using (_gate.EnterScope())
        {
            _callbacks.Add(callback);
        }
    }

    /// <summary>
    /// Returns the current committed <see cref="LimitState"/> for <paramref name="channel"/>.
    /// </summary>
    /// <param name="channel">Symbolic channel name.</param>
    /// <returns>The channel's active <see cref="LimitState"/> (defaults to <see cref="LimitState.Nominal"/>).</returns>
    public LimitState GetState(string channel)
    {
        using (_gate.EnterScope())
        {
            return _states.GetValueOrDefault(channel, LimitState.Nominal);
        }
    }

    /// <summary>
    /// Gets a value indicating whether every monitored channel is currently <see cref="LimitState.Nominal"/>.
    /// </summary>
    /// <returns><see langword="true"/> if all channels are nominal; otherwise, <see langword="false"/>.</returns>
    public bool AllNominal()
    {
        using (_gate.EnterScope())
        {
            return _states.Values.All(s => s == LimitState.Nominal);
        }
    }

    /// <summary>
    /// Returns a thread-safe snapshot of all non-nominal channel states.
    /// </summary>
    /// <returns>Dictionary mapping non-nominal channel names to their active <see cref="LimitState"/>.</returns>
    public IReadOnlyDictionary<string, LimitState> GetNonNominalStates()
    {
        using (_gate.EnterScope())
        {
            return _states
                .Where(kv => kv.Value != LimitState.Nominal)
                .ToDictionary(kv => kv.Key, kv => kv.Value, StringComparer.Ordinal);
        }
    }

    /// <summary>
    /// Returns a thread-safe snapshot of the alert transition history.
    /// </summary>
    /// <param name="maxRecent">Maximum number of most-recent alerts to include (0 returns all).</param>
    /// <returns>A read-only list of <see cref="LimitAlert"/> instances.</returns>
    public IReadOnlyList<LimitAlert> GetAlertHistory(int maxRecent = 0)
    {
        using (_gate.EnterScope())
        {
            if (maxRecent <= 0 || _alertHistory.Count <= maxRecent)
            {
                return [.. _alertHistory];
            }

            return _alertHistory.Skip(_alertHistory.Count - maxRecent).ToList();
        }
    }

    /// <summary>
    /// Computes raw hardware Data Number (DN) limit thresholds by inverting the calibration curve of <paramref name="tdef"/>.
    /// </summary>
    /// <param name="tdef">Telemetry definition providing polynomial calibration coefficients.</param>
    /// <returns>Solved <see cref="RawDnLimitThresholds"/> for registered bounds (or dictionary defaults).</returns>
    public RawDnLimitThresholds SolveRawDnThresholds(TelemetryDef tdef)
    {
        ArgumentNullException.ThrowIfNull(tdef);
        LimitSpec? spec;
        using (_gate.EnterScope())
        {
            _specs.TryGetValue(tdef.Name, out spec);
        }

        double? loErr = spec?.LoErr ?? tdef.LimitLoErr;
        double? loWarn = spec?.LoWarn ?? tdef.LimitLoWarn;
        double? hiWarn = spec?.HiWarn ?? tdef.LimitHiWarn;
        double? hiErr = spec?.HiErr ?? tdef.LimitHiErr;

        return new RawDnLimitThresholds(
            Channel: tdef.Name,
            LoErrDn: loErr.HasValue ? tdef.InvertToRawDn(loErr.Value) : null,
            LoWarnDn: loWarn.HasValue ? tdef.InvertToRawDn(loWarn.Value) : null,
            HiWarnDn: hiWarn.HasValue ? tdef.InvertToRawDn(hiWarn.Value) : null,
            HiErrDn: hiErr.HasValue ? tdef.InvertToRawDn(hiErr.Value) : null);
    }

    /// <summary>
    /// Evaluates a calibrated engineering value against registered Red/Yellow bounds with persistence debouncing
    /// and optional mode/state gating against <paramref name="peerChannels"/>.
    /// </summary>
    /// <param name="channel">Symbolic telemetry channel name.</param>
    /// <param name="engValue">Calibrated engineering value.</param>
    /// <param name="timestampNs">Sample timestamp in nanoseconds.</param>
    /// <param name="peerChannels">Optional snapshot of peer engineering values used to evaluate conditional limits.</param>
    /// <returns>The channel's committed <see cref="LimitState"/> after evaluating this sample.</returns>
    public LimitState Evaluate(
        string channel,
        double engValue,
        long timestampNs = 0L,
        IReadOnlyDictionary<string, double>? peerChannels = null)
    {
        LimitAlert? emittedAlert = null;
        Action<LimitAlert>[]? callbacksSnapshot = null;
        LimitState committedState;

        using (_gate.EnterScope())
        {
            if (!_specs.TryGetValue(channel, out LimitSpec? spec))
            {
                return LimitState.Nominal;
            }

            bool conditionMet = true;
            if (spec.Condition is not null)
            {
                conditionMet = peerChannels is not null &&
                               peerChannels.TryGetValue(spec.Condition.WatchChannel, out double watchVal) &&
                               spec.Condition.Predicate(watchVal);
            }

            LimitState rawState = conditionMet ? Classify(spec, engValue) : LimitState.Nominal;
            LimitState curState = _states.GetValueOrDefault(channel, LimitState.Nominal);

            if (rawState == curState)
            {
                _pendingState[channel] = curState;
                _pendingCount[channel] = 0;
                return curState;
            }

            if (_pendingState.TryGetValue(channel, out LimitState pending) && pending == rawState)
            {
                _pendingCount[channel] = _pendingCount.GetValueOrDefault(channel, 0) + 1;
            }
            else
            {
                _pendingState[channel] = rawState;
                _pendingCount[channel] = 1;
            }

            if (_pendingCount[channel] >= Math.Max(1, spec.Persistence))
            {
                _states[channel] = rawState;
                _pendingCount[channel] = 0;
                emittedAlert = new LimitAlert(
                    Channel: channel,
                    State: rawState,
                    PreviousState: curState,
                    Value: engValue,
                    Unit: _units.GetValueOrDefault(channel, string.Empty),
                    TimestampNs: timestampNs);
                _alertHistory.Add(emittedAlert);
                if (_callbacks.Count > 0)
                {
                    callbacksSnapshot = [.. _callbacks];
                }

                committedState = rawState;
            }
            else
            {
                committedState = curState;
            }
        }

        if (emittedAlert is not null && callbacksSnapshot is not null)
        {
            foreach (Action<LimitAlert> cb in callbacksSnapshot)
            {
                cb(emittedAlert);
            }
        }

        return committedState;
    }

    private static LimitState Classify(LimitSpec spec, double v)
    {
        if (spec.LoErr.HasValue && v < spec.LoErr.Value)
        {
            return LimitState.ErrorLo;
        }

        if (spec.HiErr.HasValue && v > spec.HiErr.Value)
        {
            return LimitState.ErrorHi;
        }

        if (spec.LoWarn.HasValue && v < spec.LoWarn.Value)
        {
            return LimitState.WarnLo;
        }

        if (spec.HiWarn.HasValue && v > spec.HiWarn.Value)
        {
            return LimitState.WarnHi;
        }

        return LimitState.Nominal;
    }
}
