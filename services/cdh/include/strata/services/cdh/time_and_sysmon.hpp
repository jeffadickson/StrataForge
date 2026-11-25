#pragma once

#include <strata/core/concepts.hpp>
#include <strata/core/expected.hpp>
#include <strata/core/types.hpp>
#include <strata/ipc/blackboard.hpp>
#include <strata/ipc/event_ring.hpp>
#include <strata/services/cdh/flight_ops.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace strata::services::cdh {

/// ============================================================================
/// Deterministic Time Base Converter & 1PPS Drift Monitor (STRATA-CDH-011)
/// Translates among hardware TAI nanoseconds, Spacecraft Clock (SCLK), and
/// UTC/mission epoch with leap-second offset table and 1PPS anomaly detection.
/// ============================================================================
struct SclkTime {
    uint32_t coarse_sec{0U};
    uint32_t fine_subsec{0U}; // Fractional 1/2^32 second ticks
};

struct UtcEpochTime {
    uint64_t utc_sec{0U};
    uint32_t subsec_ns{0U};
    int32_t  tai_minus_utc_sec{37};
};

struct LeapSecondEntry {
    uint64_t effective_tai_sec{0U};
    int32_t  tai_minus_utc_sec{37};
    bool     active{false};
};

template <size_t MaxLeapEntries = 16U>
class TimeConverter {
public:
    static constexpr uint64_t kNsPerSec = 1'000'000'000ULL;

    explicit TimeConverter(uint64_t mission_epoch_tai_sec = 0ULL,
                           int32_t  initial_tai_utc_sec   = 37,
                           uint64_t max_pps_drift_ns      = 50'000ULL) noexcept
        : epoch_tai_sec_(mission_epoch_tai_sec),
          base_tai_utc_sec_(initial_tai_utc_sec),
          max_pps_drift_ns_(max_pps_drift_ns) {}

    [[nodiscard]] Status add_leap_second(uint64_t effective_tai_sec,
                                         int32_t  tai_minus_utc_sec) noexcept {
        for (auto& e : leaps_) {
            if (!e.active) {
                e = LeapSecondEntry{effective_tai_sec, tai_minus_utc_sec, true};
                return Status::Ok;
            }
        }
        return Status::Full;
    }

    [[nodiscard]] int32_t leap_offset_for_tai(uint64_t tai_sec) const noexcept {
        int32_t  offset   = base_tai_utc_sec_;
        uint64_t best_sec = 0U;
        for (const auto& e : leaps_) {
            if (e.active && e.effective_tai_sec <= tai_sec && e.effective_tai_sec >= best_sec) {
                best_sec = e.effective_tai_sec;
                offset   = e.tai_minus_utc_sec;
            }
        }
        return offset;
    }

    [[nodiscard]] SclkTime tai_to_sclk(TaiTimestamp tai) const noexcept {
        const uint64_t total_sec = tai.ns() / kNsPerSec;
        const uint64_t rem_ns    = tai.ns() % kNsPerSec;
        const uint64_t rel_sec   = (total_sec >= epoch_tai_sec_) ? (total_sec - epoch_tai_sec_) : 0U;
        // Convert rem_ns in [0, 1e9) to fractional 2^32 ticks without overflow
        const uint64_t fine = (rem_ns * 4'294'967'296ULL) / kNsPerSec;
        return SclkTime{
            .coarse_sec  = static_cast<uint32_t>(rel_sec),
            .fine_subsec = static_cast<uint32_t>(fine),
        };
    }

    [[nodiscard]] TaiTimestamp sclk_to_tai(SclkTime sclk) const noexcept {
        const uint64_t sec_ns  = (epoch_tai_sec_ + static_cast<uint64_t>(sclk.coarse_sec)) * kNsPerSec;
        const uint64_t fine_ns = (static_cast<uint64_t>(sclk.fine_subsec) * kNsPerSec) >> 32U;
        return TaiTimestamp{sec_ns + fine_ns};
    }

    [[nodiscard]] UtcEpochTime tai_to_utc(TaiTimestamp tai) const noexcept {
        const uint64_t tai_sec = tai.ns() / kNsPerSec;
        const uint32_t rem_ns  = static_cast<uint32_t>(tai.ns() % kNsPerSec);
        const int32_t  leap    = leap_offset_for_tai(tai_sec);
        const uint64_t utc_sec =
            (leap >= 0 && tai_sec >= static_cast<uint64_t>(leap))
                ? (tai_sec - static_cast<uint64_t>(leap))
                : 0U;
        return UtcEpochTime{.utc_sec = utc_sec, .subsec_ns = rem_ns, .tai_minus_utc_sec = leap};
    }

    /// Observe a hardware 1PPS edge timestamp and detect drift outside +/- max_pps_drift_ns_.
    [[nodiscard]] Status observe_pps(TaiTimestamp pps_tai) noexcept {
        if (!has_last_pps_) {
            last_pps_     = pps_tai;
            has_last_pps_ = true;
            ++pps_edges_;
            return Status::Ok;
        }
        const uint64_t delta =
            (pps_tai.ns() >= last_pps_.ns()) ? (pps_tai.ns() - last_pps_.ns()) : 0U;
        last_pps_ = pps_tai;
        ++pps_edges_;

        const int64_t drift = static_cast<int64_t>(delta) - static_cast<int64_t>(kNsPerSec);
        last_drift_ns_      = drift;
        const uint64_t abs_drift =
            (drift >= 0) ? static_cast<uint64_t>(drift) : static_cast<uint64_t>(-drift);
        if (abs_drift > max_pps_drift_ns_) {
            ++drift_anomalies_;
            return Status::DeadlineMissed;
        }
        return Status::Ok;
    }

    [[nodiscard]] int64_t  last_drift_ns() const noexcept { return last_drift_ns_; }
    [[nodiscard]] uint32_t pps_edges() const noexcept { return pps_edges_; }
    [[nodiscard]] uint32_t drift_anomalies() const noexcept { return drift_anomalies_; }

private:
    std::array<LeapSecondEntry, MaxLeapEntries> leaps_{};
    uint64_t                                    epoch_tai_sec_{0U};
    int32_t                                     base_tai_utc_sec_{37};
    uint64_t                                    max_pps_drift_ns_{50'000ULL};
    TaiTimestamp                                last_pps_{};
    bool                                        has_last_pps_{false};
    int64_t                                     last_drift_ns_{0};
    uint32_t                                    pps_edges_{0U};
    uint32_t                                    drift_anomalies_{0U};
};

/// ============================================================================
/// Live System Resource Monitor & Hardware/Software Build Identity (STRATA-CDH-012)
/// ============================================================================
struct PartitionResourceMetrics {
    uint8_t  partition_id{0U};
    uint8_t  active{0U};
    uint16_t gen_id{0U};
    uint32_t budget_us{0U};
    uint32_t used_us{0U};
    uint32_t min_headroom_us{0xFFFF'FFFFU};
    uint32_t jitter_min_ns{0xFFFF'FFFFU};
    uint32_t jitter_max_ns{0U};
    uint32_t overrun_count{0U};
};

struct PoolResourceMetrics {
    uint16_t pool_id{0U};
    uint16_t capacity_slots{0U};
    uint16_t in_use_slots{0U};
    uint16_t high_water_slots{0U};
};

template <size_t MaxPartitions = 8U, size_t MaxPools = 8U>
struct SysMonitorSnapshot {
    BuildManifestInfo                                   build_info{};
    std::array<PartitionResourceMetrics, MaxPartitions> partitions{};
    std::array<PoolResourceMetrics, MaxPools>           pools{};
    uint64_t                                            noc_stall_cycles{0U};
    uint32_t                                            seu_correctable_count{0U};
    uint32_t                                            seu_uncorrectable_count{0U};
    uint32_t                                            scsr_hash_mismatch_count{0U};
};

template <size_t MaxPartitions = 8U, size_t MaxPools = 8U>
class SysMonitor {
public:
    using SnapshotT = SysMonitorSnapshot<MaxPartitions, MaxPools>;
    static_assert(WirePod<SnapshotT>);

    explicit SysMonitor(const BuildManifestInfo& build_info) noexcept {
        state_.build_info = build_info;
    }

    void record_partition_frame(uint8_t partition_id, uint16_t gen_id, uint32_t budget_us,
                                uint32_t used_us, uint32_t jitter_ns) noexcept {
        if (partition_id >= MaxPartitions) return;
        auto& p      = state_.partitions[partition_id];
        p.partition_id = partition_id;
        p.active       = 1U;
        p.gen_id       = gen_id;
        p.budget_us    = budget_us;
        p.used_us      = used_us;
        if (used_us > budget_us) {
            ++p.overrun_count;
            p.min_headroom_us = 0U;
        } else {
            const uint32_t headroom = budget_us - used_us;
            if (headroom < p.min_headroom_us) p.min_headroom_us = headroom;
        }
        if (jitter_ns < p.jitter_min_ns) p.jitter_min_ns = jitter_ns;
        if (jitter_ns > p.jitter_max_ns) p.jitter_max_ns = jitter_ns;
    }

    void record_pool_usage(uint16_t pool_id, uint16_t capacity, uint16_t in_use) noexcept {
        if (pool_id >= MaxPools) return;
        auto& m          = state_.pools[pool_id];
        m.pool_id        = pool_id;
        m.capacity_slots = capacity;
        m.in_use_slots   = in_use;
        if (in_use > m.high_water_slots) m.high_water_slots = in_use;
    }

    void record_fabric_and_seu(uint64_t noc_stalls_delta, uint32_t seu_ce_delta,
                               uint32_t seu_ue_delta) noexcept {
        state_.noc_stall_cycles += noc_stalls_delta;
        state_.seu_correctable_count += seu_ce_delta;
        state_.seu_uncorrectable_count += seu_ue_delta;
    }

    /// Reconcile a live hardware SCSR MANIFEST_HASH against compile-time BuildManifestInfo.
    [[nodiscard]] bool verify_slot_identity(uint64_t scsr_manifest_hash) noexcept {
        if (scsr_manifest_hash != state_.build_info.manifest_hash) {
            ++state_.scsr_hash_mismatch_count;
            return false;
        }
        return true;
    }

    void publish(TaiTimestamp now) noexcept {
        writer_.publish(state_, now);
    }

    [[nodiscard]] const BlackboardStorage<SnapshotT>& storage() const noexcept { return storage_; }

private:
    SnapshotT                    state_{};
    BlackboardStorage<SnapshotT> storage_{};
    BlackboardWriter<SnapshotT>  writer_{storage_};
};

/// ============================================================================
/// Optional Compile-Time Console Diagnostic Text Sink (STRATA-CDH-013)
/// Zero-heap, snprintf-free formatter converting binary EventRecords into
/// bounded ASCII lines for benchtop UART/JTAG/stdout. Compiles to zero overhead
/// when `Enabled == false` in flight builds.
/// ============================================================================
template <bool Enabled = true, size_t LineCap = 128U>
class TextLogSink {
public:
    static constexpr bool kEnabled = Enabled;

    template <typename EmitLineFn>
    void emit(const EventRecord& ev, EmitLineFn&& emit_line) const noexcept {
        if constexpr (!Enabled) {
            (void)ev;
            (void)emit_line;
            return;
        } else {
            std::array<char, LineCap> buf{};
            size_t                    pos = 0U;
            append_str(buf, pos, "[TAI=");
            append_u64_dec(buf, pos, ev.last_tai_ns);
            append_str(buf, pos, " SEV=");
            append_u64_dec(buf, pos, static_cast<uint64_t>(ev.severity));
            append_str(buf, pos, " ID=0x");
            append_hex(buf, pos, ev.event_id, 4U);
            append_str(buf, pos, " PART=");
            append_u64_dec(buf, pos, ev.partition_id);
            append_str(buf, pos, " A0=0x");
            append_hex(buf, pos, ev.arg0, 8U);
            append_str(buf, pos, " A1=0x");
            append_hex(buf, pos, ev.arg1, 8U);
            append_str(buf, pos, "]");
            emit_line(std::span<const char>{buf.data(), pos});
        }
    }

private:
    static void append_char(std::array<char, LineCap>& buf, size_t& pos, char c) noexcept {
        if (pos + 1U < LineCap) {
            buf[pos++] = c;
            buf[pos]   = '\0';
        }
    }

    static void append_str(std::array<char, LineCap>& buf, size_t& pos, const char* s) noexcept {
        while (*s != '\0') {
            append_char(buf, pos, *s++);
        }
    }

    static void append_u64_dec(std::array<char, LineCap>& buf, size_t& pos, uint64_t val) noexcept {
        std::array<char, 24U> tmp{};
        size_t                n = 0U;
        do {
            tmp[n++] = static_cast<char>('0' + (val % 10U));
            val /= 10U;
        } while (val != 0U && n < tmp.size());
        while (n > 0U) {
            append_char(buf, pos, tmp[--n]);
        }
    }

    static void append_hex(std::array<char, LineCap>& buf, size_t& pos, uint64_t val,
                           size_t digits) noexcept {
        static constexpr char kHex[] = "0123456789ABCDEF";
        for (size_t i = digits; i > 0U; --i) {
            const auto nibble = static_cast<size_t>((val >> ((i - 1U) * 4U)) & 0x0FU);
            append_char(buf, pos, kHex[nibble]);
        }
    }
};

} // namespace strata::services::cdh
