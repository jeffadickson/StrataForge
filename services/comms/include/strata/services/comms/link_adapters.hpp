#pragma once

#include <strata/core/crc32c.hpp>
#include <strata/core/expected.hpp>
#include <strata/core/types.hpp>
#include <strata/ipc/spsc_channel.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace strata::services::comms {

/// ============================================================================
/// Bounded Link Back-Pressure Retry Adapter (STRATA-COM-009)
/// Retries sending FrameDesc descriptors to a busy radio/link driver with
/// bounded retry count and tick backoff without dynamic allocation.
/// ============================================================================
template <size_t MaxPending = 16U>
class ComRetry {
public:
    explicit ComRetry(uint8_t max_retries = 3U, uint32_t backoff_ticks = 2U) noexcept
        : max_retries_(max_retries), backoff_ticks_(backoff_ticks) {}

    [[nodiscard]] Status enqueue(const FrameDesc& desc) noexcept {
        for (auto& e : entries_) {
            if (!e.active) {
                e = PendingEntry{
                    .desc         = desc,
                    .attempts     = 0U,
                    .wait_ticks   = 0U,
                    .active       = true,
                };
                ++active_count_;
                return Status::Ok;
            }
        }
        ++overflow_drops_;
        return Status::Full;
    }

    /// Attempt to transmit pending descriptors; `driver_send(const FrameDesc&)` returns
    /// `Status::Ok` on accept, `Status::Busy` on back-pressure, or fatal error.
    template <typename DriverSendFn>
    size_t tick(DriverSendFn&& driver_send) noexcept {
        size_t sent = 0U;
        for (auto& e : entries_) {
            if (!e.active) continue;
            if (e.wait_ticks > 0U) {
                --e.wait_ticks;
                continue;
            }
            const Status st = driver_send(e.desc);
            if (st == Status::Ok) {
                e.active = false;
                --active_count_;
                ++sent_total_;
                ++sent;
            } else if (st == Status::Busy && e.attempts < max_retries_) {
                ++e.attempts;
                ++retries_total_;
                e.wait_ticks = backoff_ticks_;
            } else {
                e.active = false;
                --active_count_;
                ++exhausted_drops_;
            }
        }
        return sent;
    }

    [[nodiscard]] size_t   pending_count() const noexcept { return active_count_; }
    [[nodiscard]] uint32_t sent_total() const noexcept { return sent_total_; }
    [[nodiscard]] uint32_t retries_total() const noexcept { return retries_total_; }
    [[nodiscard]] uint32_t exhausted_drops() const noexcept { return exhausted_drops_; }
    [[nodiscard]] uint32_t overflow_drops() const noexcept { return overflow_drops_; }

private:
    struct PendingEntry {
        FrameDesc desc{};
        uint8_t   attempts{0U};
        uint32_t  wait_ticks{0U};
        bool      active{false};
    };

    std::array<PendingEntry, MaxPending> entries_{};
    uint8_t                              max_retries_{3U};
    uint32_t                             backoff_ticks_{2U};
    size_t                               active_count_{0U};
    uint32_t                             sent_total_{0U};
    uint32_t                             retries_total_{0U};
    uint32_t                             exhausted_drops_{0U};
    uint32_t                             overflow_drops_{0U};
};

/// ============================================================================
/// Zero-Copy Flight Recorder Tap (STRATA-COM-009)
/// Mirrors uplink/downlink 32-byte FrameDesc descriptors to both primary output
/// and an onboard recorder ring without copying packet payload bytes.
/// ============================================================================
template <size_t PrimaryDepth = 32U, size_t TapDepth = 32U>
    requires(PowerOfTwo<PrimaryDepth> && PowerOfTwo<TapDepth>)
class ComTee {
public:
    ComTee(SpscRingStorage<FrameDesc, PrimaryDepth>& primary_out,
           SpscRingStorage<FrameDesc, TapDepth>&     recorder_tap) noexcept
        : primary_(primary_out, 0U, NullNotifier{}, GenId{0U}),
          tap_(recorder_tap, 0U, NullNotifier{}, GenId{0U}) {}

    void set_tap_enabled(bool enabled) noexcept { tap_enabled_ = enabled; }
    [[nodiscard]] bool tap_enabled() const noexcept { return tap_enabled_; }

    /// Forward descriptor zero-copy to primary output and mirror to recorder tap.
    /// Recorder back-pressure never blocks or fails primary flight communications.
    [[nodiscard]] Status forward(const FrameDesc& desc) noexcept {
        if (tap_enabled_) {
            if (tap_.try_push(desc) == Status::Ok) {
                ++tapped_count_;
            } else {
                ++tap_drop_count_;
            }
        }
        const Status st = primary_.try_push(desc);
        if (st == Status::Ok) {
            ++primary_count_;
        }
        return st;
    }

    [[nodiscard]] uint32_t primary_count() const noexcept { return primary_count_; }
    [[nodiscard]] uint32_t tapped_count() const noexcept { return tapped_count_; }
    [[nodiscard]] uint32_t tap_drop_count() const noexcept { return tap_drop_count_; }

private:
    SpscProducer<FrameDesc, PrimaryDepth> primary_;
    SpscProducer<FrameDesc, TapDepth>     tap_;
    bool                                  tap_enabled_{true};
    uint32_t                              primary_count_{0U};
    uint32_t                              tapped_count_{0U};
    uint32_t                              tap_drop_count_{0U};
};

/// ============================================================================
/// Multi-Board InterNodeBridge Transport (STRATA-COM-010)
/// Zero-heap framing for tunneling control rings and 32-byte FrameDesc transfers
/// across physical multi-board links (SpaceWire, SpaceFibre, MIL-STD-1553,
/// Ethernet, or PCIe DMA) with CRC-32C integrity and node/channel routing.
/// ============================================================================
enum class BridgeLinkProfile : uint8_t {
    SpaceWire   = 0U,
    SpaceFibre  = 1U,
    MilStd1553  = 2U,
    Ethernet    = 3U,
    PcieDma     = 4U,
};

struct alignas(4) BridgeWireHeader {
    static constexpr uint32_t kMagic = 0x5354'4252U; // "STBR"

    uint32_t magic{kMagic};
    uint8_t  src_node{0U};
    uint8_t  dst_node{0U};
    uint8_t  channel_id{0U};
    uint8_t  profile{0U};
    uint32_t seq_num{0U};
    uint16_t payload_bytes{0U};
    uint16_t flags{0U}; // Bit 0 = FrameDesc payload, Bit 1 = Control ring message
    uint32_t crc32c{0U};
};
static_assert(sizeof(BridgeWireHeader) == 20U);

template <size_t MaxPayloadBytes = 512U>
class InterNodeBridge {
public:
    static constexpr size_t kMaxWireBytes = sizeof(BridgeWireHeader) + MaxPayloadBytes;

    explicit InterNodeBridge(uint8_t local_node_id, BridgeLinkProfile profile) noexcept
        : local_node_(local_node_id), profile_(profile) {}

    /// Encode a control-ring payload or descriptor into a wire frame with CRC-32C.
    [[nodiscard]] Expected<size_t, Status>
    encode(uint8_t dst_node, uint8_t channel_id, uint16_t flags,
           std::span<const std::byte> payload, std::span<std::byte> out_wire) noexcept {
        if (payload.size() > MaxPayloadBytes || out_wire.size() < sizeof(BridgeWireHeader) + payload.size()) {
            return Unexpected(Status::InvalidArg);
        }
        BridgeWireHeader hdr{};
        hdr.src_node      = local_node_;
        hdr.dst_node      = dst_node;
        hdr.channel_id    = channel_id;
        hdr.profile       = static_cast<uint8_t>(profile_);
        hdr.seq_num       = tx_seq_++;
        hdr.payload_bytes = static_cast<uint16_t>(payload.size());
        hdr.flags         = flags;
        hdr.crc32c        = 0U;

        uint32_t crc = Crc32c::compute(std::as_bytes(std::span<const BridgeWireHeader, 1>{&hdr, 1U}));
        crc          = Crc32c::update(crc, payload);
        hdr.crc32c   = crc;

        std::memcpy(out_wire.data(), &hdr, sizeof(hdr));
        if (!payload.empty()) {
            std::memcpy(out_wire.data() + sizeof(hdr), payload.data(), payload.size());
        }
        ++tx_frames_;
        return sizeof(hdr) + payload.size();
    }

    /// Convenience encoder for tunneling a 32-byte FrameDesc across boards.
    [[nodiscard]] Expected<size_t, Status>
    encode_descriptor(uint8_t dst_node, uint8_t channel_id, const FrameDesc& desc,
                      std::span<std::byte> out_wire) noexcept {
        return encode(dst_node, channel_id, /*flags=*/0x0001U,
                      std::as_bytes(std::span<const FrameDesc, 1>{&desc, 1U}), out_wire);
    }

    struct DecodedFrame {
        BridgeWireHeader           header{};
        std::span<const std::byte> payload{};
    };

    /// Decode and CRC-verify an incoming multi-board bridge frame addressed to this node.
    [[nodiscard]] Expected<DecodedFrame, Status>
    decode(std::span<const std::byte> wire_frame) noexcept {
        if (wire_frame.size() < sizeof(BridgeWireHeader)) {
            ++rx_errors_;
            return Unexpected(Status::PayloadSizeError);
        }
        BridgeWireHeader hdr{};
        std::memcpy(&hdr, wire_frame.data(), sizeof(hdr));
        if (hdr.magic != BridgeWireHeader::kMagic) {
            ++rx_errors_;
            return Unexpected(Status::ValidationFailed);
        }
        if (hdr.dst_node != local_node_ && hdr.dst_node != 0xFFU) {
            return Unexpected(Status::PermissionDenied);
        }
        if (hdr.payload_bytes > MaxPayloadBytes ||
            wire_frame.size() < sizeof(BridgeWireHeader) + hdr.payload_bytes) {
            ++rx_errors_;
            return Unexpected(Status::PayloadSizeError);
        }

        const auto     payload   = wire_frame.subspan(sizeof(BridgeWireHeader), hdr.payload_bytes);
        const uint32_t rx_crc    = hdr.crc32c;
        hdr.crc32c               = 0U;
        uint32_t computed_crc    = Crc32c::compute(std::as_bytes(std::span<const BridgeWireHeader, 1>{&hdr, 1U}));
        computed_crc             = Crc32c::update(computed_crc, payload);
        if (computed_crc != rx_crc) {
            ++rx_errors_;
            return Unexpected(Status::CrcMismatch);
        }
        hdr.crc32c = rx_crc;
        ++rx_frames_;
        return DecodedFrame{.header = hdr, .payload = payload};
    }

    [[nodiscard]] uint8_t           local_node() const noexcept { return local_node_; }
    [[nodiscard]] BridgeLinkProfile profile() const noexcept { return profile_; }
    [[nodiscard]] uint32_t          tx_frames() const noexcept { return tx_frames_; }
    [[nodiscard]] uint32_t          rx_frames() const noexcept { return rx_frames_; }
    [[nodiscard]] uint32_t          rx_errors() const noexcept { return rx_errors_; }

private:
    uint8_t           local_node_{0U};
    BridgeLinkProfile profile_{BridgeLinkProfile::SpaceWire};
    uint32_t          tx_seq_{1U};
    uint32_t          tx_frames_{0U};
    uint32_t          rx_frames_{0U};
    uint32_t          rx_errors_{0U};
};

} // namespace strata::services::comms
