#pragma once

#include <strata/core/concepts.hpp>
#include <strata/core/expected.hpp>
#include <strata/core/types.hpp>
#include <strata/ipc/blackboard.hpp>
#include <strata/services/comms/ccsds.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace strata::services::cdh {

/// Real-Time Telemetry Forwarding Mode per APID.
enum class ForwardMode : uint8_t {
    Muted        = 0U,
    StoreOnly    = 1U,
    LiveRf       = 2U,
    LiveAndStore = 3U,
};

/// Dynamic Telemetry Forwarding Control Table.
template <size_t MaxEntries = 32U>
class ForwardingControl {
public:
    [[nodiscard]] Status set_mode(uint16_t apid, ForwardMode mode) noexcept {
        for (auto& e : entries_) {
            if (e.active && e.apid == apid) {
                e.mode = mode;
                return Status::Ok;
            }
        }
        for (auto& e : entries_) {
            if (!e.active) {
                e.active = true;
                e.apid   = apid;
                e.mode   = mode;
                return Status::Ok;
            }
        }
        return Status::Full;
    }

    [[nodiscard]] ForwardMode mode(uint16_t apid) const noexcept {
        for (const auto& e : entries_) {
            if (e.active && e.apid == apid) return e.mode;
        }
        return ForwardMode::LiveAndStore;
    }

    [[nodiscard]] bool should_send_rf(uint16_t apid) const noexcept {
        const auto m = mode(apid);
        return m == ForwardMode::LiveRf || m == ForwardMode::LiveAndStore;
    }

    [[nodiscard]] bool should_store(uint16_t apid) const noexcept {
        const auto m = mode(apid);
        return m == ForwardMode::StoreOnly || m == ForwardMode::LiveAndStore;
    }

private:
    struct Entry {
        bool        active{false};
        uint16_t    apid{0U};
        ForwardMode mode{ForwardMode::LiveAndStore};
    };
    std::array<Entry, MaxEntries> entries_{};
};

/// Compile-time-verified CCSDS Telemetry Packetizer.
/// Exact byte layout of every HK snapshot is statically checked at compile time —
/// zero runtime serialized-value overflow across adjacent packet fields is possible.
template <uint16_t Apid, WirePod HkStruct>
class TlmPacketizer {
public:
    static constexpr uint16_t kApid         = Apid;
    static constexpr size_t   kPayloadBytes = sizeof(uint64_t) + sizeof(HkStruct); // TAI + HK
    static constexpr size_t   kPacketBytes  = comms::kSpacePacketHeaderBytes + kPayloadBytes;

    static_assert(kApid <= 0x07FFU, "CCSDS APID must be 11 bits");
    static_assert(kPayloadBytes <= comms::kMaxSpacePacketPayload,
                  "Telemetry struct exceeds CCSDS Space Packet MTU");

    /// Sample Seqlock Blackboard and pack directly into a CCSDS Space Packet.
    template <QoSConfig QoS>
    [[nodiscard]] Expected<size_t, Status>
    sample_and_pack(const BlackboardReader<HkStruct, QoS>& reader, TaiTimestamp now,
                    std::span<std::byte> out_buf) noexcept {
        if (out_buf.size() < kPacketBytes) {
            return Unexpected(Status::PayloadSizeError);
        }
        auto snap = reader.read(now);
        if (!snap) {
            return Unexpected(snap.error());
        }

        std::array<std::byte, kPayloadBytes> payload{};
        const uint64_t ts = snap->published_at.ns();
        std::memcpy(payload.data(), &ts, sizeof(ts));
        std::memcpy(payload.data() + sizeof(ts), &snap->value, sizeof(HkStruct));

        auto framed = comms::frame_space_packet(out_buf, kApid, seq_count_++, false, true, payload);
        if (!framed) {
            return Unexpected(Status::PayloadSizeError);
        }
        return *framed;
    }

private:
    uint16_t seq_count_{0U};
};

/// On-Board Parameter Statistics Collector (Welford's online algorithm in O(1) RAM).
/// Summarizes high-rate thermal/power/jitter signals into compact {min, max, mean, variance} reports.
struct ParamStatsSummary {
    double   min_val{0.0};
    double   max_val{0.0};
    double   mean{0.0};
    double   variance{0.0};
    uint32_t sample_count{0U};
};

class StatsCollector {
public:
    void observe(double x) noexcept {
        if (count_ == 0U) {
            min_  = x;
            max_  = x;
            mean_ = x;
            m2_   = 0.0;
            count_ = 1U;
            return;
        }
        if (x < min_) min_ = x;
        if (x > max_) max_ = x;
        ++count_;
        const double delta  = x - mean_;
        mean_              += delta / static_cast<double>(count_);
        const double delta2 = x - mean_;
        m2_                += delta * delta2;
    }

    [[nodiscard]] ParamStatsSummary snapshot() const noexcept {
        const double var = (count_ > 1U) ? (m2_ / static_cast<double>(count_ - 1U)) : 0.0;
        return ParamStatsSummary{
            .min_val      = min_,
            .max_val      = max_,
            .mean         = mean_,
            .variance     = var,
            .sample_count = count_,
        };
    }

    void reset() noexcept {
        min_   = 0.0;
        max_   = 0.0;
        mean_  = 0.0;
        m2_    = 0.0;
        count_ = 0U;
    }

private:
    double   min_{0.0};
    double   max_{0.0};
    double   mean_{0.0};
    double   m2_{0.0};
    uint32_t count_{0U};
};

} // namespace strata::services::cdh
