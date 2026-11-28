using System.Globalization;

namespace StrataForge.Ground;

/// <summary>
/// Represents an immutable Spacecraft Clock (SCLK) timestamp composed of coarse seconds and fine subsecond ticks.
/// </summary>
/// <param name="CoarseSeconds">Monotonic coarse spacecraft clock seconds.</param>
/// <param name="FineTicks">Subsecond fine ticks in <c>[0, FineModulus - 1]</c>.</param>
/// <param name="FineModulus">Number of fine ticks per coarse second (default 65536 for 16-bit CUC fine field).</param>
public readonly record struct SclkTime(uint CoarseSeconds, uint FineTicks, uint FineModulus = 65536U)
{
    /// <summary>
    /// Converts this SCLK reading to total fractional spacecraft seconds.
    /// </summary>
    /// <returns>Fractional SCLK seconds as a double-precision value.</returns>
    public double ToFractionalSeconds() =>
        CoarseSeconds + (FineTicks / (double)(FineModulus == 0U ? 65536U : FineModulus));

    /// <summary>
    /// Formats this SCLK value in canonical <c>CCCCCCCCC:FFFFF</c> ground-operations notation.
    /// </summary>
    /// <returns>Formatted SCLK string.</returns>
    public override string ToString() =>
        string.Create(CultureInfo.InvariantCulture, $"{CoarseSeconds:D10}:{FineTicks:D5}");
}

/// <summary>
/// Parameters anchoring a linear SCLK-to-TAI correlation segment with oscillator drift compensation.
/// </summary>
/// <param name="SclkEpochSeconds">Reference SCLK value (in fractional seconds) at correlation epoch.</param>
/// <param name="TaiEpochNs">Corresponding exact nanosecond TAI timestamp at epoch (since 1970-01-01T00:00:00 TAI).</param>
/// <param name="DriftRatePpm">Oscillator drift rate in parts-per-million (positive means SCLK runs faster than TAI).</param>
/// <param name="FineModulus">Subsecond ticks per coarse SCLK second (default 65536).</param>
public sealed record SclkCorrelationConfig(
    double SclkEpochSeconds,
    long TaiEpochNs,
    double DriftRatePpm = 0.0,
    uint FineModulus = 65536U);

/// <summary>
/// Bidirectional Spacecraft Clock (<c>SCLK</c>) ↔ <c>TAI</c> ↔ <c>SCET/UTC</c> time correlation engine
/// with leap-second handling and oscillator drift compensation.
/// </summary>
/// <remarks>
/// Implements <c>STRATA-GDS-006</c>. Thread-safe for concurrent telemetry decoding and post-pass TVAC log correlation.
/// </remarks>
public sealed class SpacecraftTimeCorrelator
{
    private readonly Lock _gate = new();
    private readonly List<(DateTimeOffset UtcEffective, int TaiMinusUtcSeconds)> _leapSeconds =
    [
        (new DateTimeOffset(1972, 1, 1, 0, 0, 0, TimeSpan.Zero), 10),
        (new DateTimeOffset(2009, 1, 1, 0, 0, 0, TimeSpan.Zero), 34),
        (new DateTimeOffset(2012, 7, 1, 0, 0, 0, TimeSpan.Zero), 35),
        (new DateTimeOffset(2015, 7, 1, 0, 0, 0, TimeSpan.Zero), 36),
        (new DateTimeOffset(2017, 1, 1, 0, 0, 0, TimeSpan.Zero), 37),
    ];

    private SclkCorrelationConfig _config;

    /// <summary>
    /// Initializes a new <see cref="SpacecraftTimeCorrelator"/> with the specified epoch and drift parameters.
    /// </summary>
    /// <param name="config">Initial SCLK-to-TAI correlation anchor.</param>
    public SpacecraftTimeCorrelator(SclkCorrelationConfig config)
    {
        ArgumentNullException.ThrowIfNull(config);
        _config = config;
    }

    /// <summary>
    /// Updates the active SCLK-to-TAI correlation parameters atomically (for example, after a ground time-sync pass).
    /// </summary>
    /// <param name="config">New SCLK correlation segment.</param>
    public void UpdateCorrelation(SclkCorrelationConfig config)
    {
        ArgumentNullException.ThrowIfNull(config);
        using (_gate.EnterScope())
        {
            _config = config;
        }
    }

    /// <summary>
    /// Registers an additional IERS leap-second transition into the correlation table.
    /// </summary>
    /// <param name="utcEffective">UTC instant at which the new TAI−UTC offset takes effect.</param>
    /// <param name="taiMinusUtcSeconds">Cumulative TAI minus UTC offset in whole seconds.</param>
    public void AddLeapSecond(DateTimeOffset utcEffective, int taiMinusUtcSeconds)
    {
        using (_gate.EnterScope())
        {
            _leapSeconds.Add((utcEffective.ToUniversalTime(), taiMinusUtcSeconds));
            _leapSeconds.Sort((a, b) => a.UtcEffective.CompareTo(b.UtcEffective));
        }
    }

    /// <summary>
    /// Returns the cumulative <c>TAI - UTC</c> leap-second offset in seconds applicable at <paramref name="utc"/>.
    /// </summary>
    /// <param name="utc">UTC timestamp to query.</param>
    /// <returns>TAI minus UTC offset in whole seconds.</returns>
    public int GetTaiMinusUtcSeconds(DateTimeOffset utc)
    {
        DateTimeOffset u = utc.ToUniversalTime();
        using (_gate.EnterScope())
        {
            int offset = 0;
            foreach (var (eff, sec) in _leapSeconds)
            {
                if (u >= eff)
                {
                    offset = sec;
                }
                else
                {
                    break;
                }
            }

            return offset;
        }
    }

    /// <summary>
    /// Converts a spacecraft clock reading (<see cref="SclkTime"/>) into drift-corrected nanosecond TAI.
    /// </summary>
    /// <param name="sclk">Spacecraft clock value.</param>
    /// <returns>Correlated nanosecond TAI timestamp since the Unix/TAI 1970 epoch.</returns>
    public long SclkToTaiNs(SclkTime sclk)
    {
        SclkCorrelationConfig cfg;
        using (_gate.EnterScope())
        {
            cfg = _config;
        }

        double deltaSclkSec = sclk.ToFractionalSeconds() - cfg.SclkEpochSeconds;
        double rateFactor = 1.0 / (1.0 + (cfg.DriftRatePpm * 1e-6));
        double deltaTaiNs = deltaSclkSec * rateFactor * 1_000_000_000.0;
        return cfg.TaiEpochNs + (long)Math.Round(deltaTaiNs);
    }

    /// <summary>
    /// Converts a nanosecond TAI timestamp into the corresponding drift-corrected <see cref="SclkTime"/>.
    /// </summary>
    /// <param name="taiNs">Nanosecond TAI timestamp.</param>
    /// <returns>Correlated <see cref="SclkTime"/>.</returns>
    public SclkTime TaiNsToSclk(long taiNs)
    {
        SclkCorrelationConfig cfg;
        using (_gate.EnterScope())
        {
            cfg = _config;
        }

        double deltaTaiSec = (taiNs - cfg.TaiEpochNs) / 1_000_000_000.0;
        double rateFactor = 1.0 + (cfg.DriftRatePpm * 1e-6);
        double totalSclkSec = Math.Max(0.0, cfg.SclkEpochSeconds + (deltaTaiSec * rateFactor));
        uint coarse = (uint)Math.Floor(totalSclkSec);
        uint modulus = cfg.FineModulus == 0U ? 65536U : cfg.FineModulus;
        uint fine = (uint)Math.Clamp(Math.Round((totalSclkSec - coarse) * modulus), 0.0, modulus - 1U);
        return new SclkTime(coarse, fine, modulus);
    }

    /// <summary>
    /// Converts a nanosecond TAI timestamp into a leap-second-adjusted UTC (<c>SCET</c>) <see cref="DateTimeOffset"/>.
    /// </summary>
    /// <param name="taiNs">Nanosecond TAI timestamp since 1970-01-01T00:00:00 TAI.</param>
    /// <returns>Corresponding UTC <see cref="DateTimeOffset"/>.</returns>
    public DateTimeOffset TaiNsToUtc(long taiNs)
    {
        DateTimeOffset approxUtc = DateTimeOffset.UnixEpoch.AddTicks(taiNs / 100L);
        int leapSec = GetTaiMinusUtcSeconds(approxUtc);
        long utcNs = taiNs - (leapSec * 1_000_000_000L);
        DateTimeOffset refined = DateTimeOffset.UnixEpoch.AddTicks(utcNs / 100L);
        int refinedLeap = GetTaiMinusUtcSeconds(refined);
        if (refinedLeap != leapSec)
        {
            utcNs = taiNs - (refinedLeap * 1_000_000_000L);
            refined = DateTimeOffset.UnixEpoch.AddTicks(utcNs / 100L);
        }

        return refined;
    }

    /// <summary>
    /// Converts a UTC (<c>SCET</c>) <see cref="DateTimeOffset"/> into nanosecond TAI including leap seconds.
    /// </summary>
    /// <param name="utc">UTC timestamp.</param>
    /// <returns>Corresponding nanosecond TAI timestamp.</returns>
    public long UtcToTaiNs(DateTimeOffset utc)
    {
        DateTimeOffset u = utc.ToUniversalTime();
        int leapSec = GetTaiMinusUtcSeconds(u);
        long utcNs = (u.Ticks - DateTimeOffset.UnixEpoch.Ticks) * 100L;
        return utcNs + (leapSec * 1_000_000_000L);
    }

    /// <summary>
    /// Converts a <see cref="SclkTime"/> directly into a leap-second-adjusted UTC (<c>SCET</c>) timestamp.
    /// </summary>
    /// <param name="sclk">Spacecraft clock reading.</param>
    /// <returns>Corresponding UTC <see cref="DateTimeOffset"/>.</returns>
    public DateTimeOffset SclkToUtc(SclkTime sclk) => TaiNsToUtc(SclkToTaiNs(sclk));
}
