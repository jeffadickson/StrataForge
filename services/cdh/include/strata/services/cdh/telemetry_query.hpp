#pragma once

#include <strata/core/concepts.hpp>
#include <strata/core/expected.hpp>
#include <strata/core/types.hpp>
#include <strata/ipc/blackboard.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace strata::services::cdh {

/// Snapshot metadata returned by an on-demand single-channel telemetry query.
struct ChannelQueryReply {
    uint16_t     channel_id{0U};
    uint16_t     payload_bytes{0U};
    TaiTimestamp published_at{};
    GenId        writer_gen{};
    uint32_t     seq{0U};
};

/// ============================================================================
/// On-Demand Single-Channel Telemetry Snapshot Query Adapter (STRATA-CDH-004)
/// Zero-vtable, zero-heap registry allowing ground operators or onboard autonomy
/// to request the latest BlackboardStorage<T> snapshot for any registered channel
/// ID outside the periodic packetizer cadence.
/// ============================================================================
template <size_t MaxChannels = 64U>
class ChannelQueryAdapter {
public:
    template <WirePod T>
    [[nodiscard]] Status register_channel(uint16_t channel_id,
                                          const BlackboardStorage<T>& bb) noexcept {
        for (const auto& e : entries_) {
            if (e.active && e.channel_id == channel_id) {
                return Status::InvalidArg;
            }
        }
        for (auto& e : entries_) {
            if (!e.active) {
                e.channel_id = channel_id;
                e.bb_ptr     = &bb;
                e.read_fn    = &read_thunk<T>;
                e.active     = true;
                ++count_;
                return Status::Ok;
            }
        }
        return Status::Full;
    }

    /// Snapshot one registered channel directly into caller-supplied byte buffer.
    [[nodiscard]] Expected<ChannelQueryReply, Status>
    query(uint16_t channel_id, std::span<std::byte> out_buf) const noexcept {
        for (const auto& e : entries_) {
            if (e.active && e.channel_id == channel_id) {
                return e.read_fn(e.bb_ptr, channel_id, out_buf);
            }
        }
        return Unexpected(Status::InvalidArg);
    }

    /// Typed convenience query for onboard autonomy or unit tests.
    template <WirePod T>
    [[nodiscard]] Expected<BlackboardSample<T>, Status>
    query_typed(uint16_t channel_id) const noexcept {
        std::array<std::byte, sizeof(T)> raw{};
        auto                             res = query(channel_id, raw);
        if (!res.has_value()) {
            return Unexpected(res.error());
        }
        if (res.value().payload_bytes != sizeof(T)) {
            return Unexpected(Status::PayloadSizeError);
        }
        BlackboardSample<T> sample{};
        std::memcpy(&sample.value, raw.data(), sizeof(T));
        sample.published_at = res.value().published_at;
        sample.writer_gen   = res.value().writer_gen;
        sample.seq          = res.value().seq;
        return sample;
    }

    [[nodiscard]] size_t channel_count() const noexcept { return count_; }

private:
    using ReadFn = Expected<ChannelQueryReply, Status> (*)(const void* bb_ptr, uint16_t channel_id,
                                                           std::span<std::byte> out_buf) noexcept;

    template <WirePod T>
    static Expected<ChannelQueryReply, Status>
    read_thunk(const void* bb_ptr, uint16_t channel_id, std::span<std::byte> out_buf) noexcept {
        if (out_buf.size() < sizeof(T)) {
            return Unexpected(Status::PayloadSizeError);
        }
        const auto* bb = static_cast<const BlackboardStorage<T>*>(bb_ptr);
        BlackboardReader<T> reader(*bb);
        const auto sample = reader.read(TaiTimestamp{0U});
        if (!sample.has_value()) {
            return Unexpected(sample.error());
        }
        std::memcpy(out_buf.data(), &sample.value().value, sizeof(T));
        return ChannelQueryReply{
            .channel_id    = channel_id,
            .payload_bytes = static_cast<uint16_t>(sizeof(T)),
            .published_at  = sample.value().published_at,
            .writer_gen    = sample.value().writer_gen,
            .seq           = sample.value().seq,
        };
    }

    struct Entry {
        uint16_t    channel_id{0U};
        const void* bb_ptr{nullptr};
        ReadFn      read_fn{nullptr};
        bool        active{false};
    };

    std::array<Entry, MaxChannels> entries_{};
    size_t                         count_{0U};
};

} // namespace strata::services::cdh
