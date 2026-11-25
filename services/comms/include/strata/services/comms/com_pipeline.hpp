#pragma once

#include <strata/core/expected.hpp>
#include <strata/core/types.hpp>
#include <strata/services/comms/ccsds.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace strata::services::comms {

/// Downlink priority tiers (strict priority so bulk science never starves C&DH HK/Events).
enum class ComPriority : uint8_t {
    CriticalHk = 0U, // Highest priority
    Events     = 1U,
    CmdAck     = 2U,
    Cfdp       = 3U,
    BulkDp     = 4U, // Lowest priority
    Count      = 5U,
};

/// Fixed-capacity Priority ComQueue (F' Svc::ComQueue parity, zero heap, zero RTOS threads).
template <size_t DepthPerTier, size_t MaxPacketBytes>
class ComQueue {
public:
    static constexpr size_t kNumTiers = static_cast<size_t>(ComPriority::Count);

    struct PacketSlot {
        uint16_t                          length{0U};
        std::array<std::byte, MaxPacketBytes> data{};
    };

    [[nodiscard]] Status enqueue(ComPriority prio, std::span<const std::byte> packet) noexcept {
        const auto tier = static_cast<size_t>(prio);
        if (tier >= kNumTiers || packet.empty() || packet.size() > MaxPacketBytes) {
            return Status::InvalidArg;
        }
        auto& q = queues_[tier];
        if (q.count >= DepthPerTier) {
            ++q.drops;
            return Status::Full;
        }
        auto& slot  = q.slots[q.tail];
        slot.length = static_cast<uint16_t>(packet.size());
        std::memcpy(slot.data.data(), packet.data(), packet.size());
        q.tail = (q.tail + 1U) % DepthPerTier;
        ++q.count;
        return Status::Ok;
    }

    /// Dequeue the highest-priority pending packet into dst.
    [[nodiscard]] Expected<size_t, Status> dequeue_highest(std::span<std::byte> dst) noexcept {
        for (size_t tier = 0U; tier < kNumTiers; ++tier) {
            auto& q = queues_[tier];
            if (q.count == 0U) {
                continue;
            }
            const auto& slot = q.slots[q.head];
            if (dst.size() < slot.length) {
                return Unexpected(Status::InvalidArg);
            }
            std::memcpy(dst.data(), slot.data.data(), slot.length);
            const size_t len = slot.length;
            q.head           = (q.head + 1U) % DepthPerTier;
            --q.count;
            return len;
        }
        return Unexpected(Status::Empty);
    }

    [[nodiscard]] size_t pending_total() const noexcept {
        size_t total = 0U;
        for (const auto& q : queues_) {
            total += q.count;
        }
        return total;
    }

    [[nodiscard]] uint32_t drops(ComPriority prio) const noexcept {
        return queues_[static_cast<size_t>(prio)].drops;
    }

private:
    struct TierQueue {
        std::array<PacketSlot, DepthPerTier> slots{};
        size_t                               head{0U};
        size_t                               tail{0U};
        size_t                               count{0U};
        uint32_t                             drops{0U};
    };

    std::array<TierQueue, kNumTiers> queues_{};
};

/// CCSDS ComAggregator (F' Svc::ComAggregator parity): packs variable-size Space Packets
/// into fixed-size Transfer Frame Data Fields and pads residual bytes with CCSDS Idle Packets (APID 0x7FF).
template <size_t FrameDataFieldBytes>
    requires(FrameDataFieldBytes > kSpacePacketHeaderBytes + 1U)
class ComAggregator {
public:
    /// Append one Space Packet; returns Status::Full if flushing is required first.
    [[nodiscard]] Status append(std::span<const std::byte> space_packet) noexcept {
        if (space_packet.empty() || space_packet.size() > FrameDataFieldBytes) {
            return Status::InvalidArg;
        }
        if (used_ + space_packet.size() > FrameDataFieldBytes) {
            return Status::Full;
        }
        std::memcpy(buf_.data() + used_, space_packet.data(), space_packet.size());
        used_ += space_packet.size();
        return Status::Ok;
    }

    /// Flush current aggregate to fixed-size FrameDataFieldBytes, inserting a valid CCSDS Idle Packet if needed.
    [[nodiscard]] std::span<const std::byte, FrameDataFieldBytes> flush_with_idle_fill() noexcept {
        const size_t remaining = FrameDataFieldBytes - used_;
        if (remaining >= kSpacePacketHeaderBytes + 1U) {
            const size_t idle_payload = remaining - kSpacePacketHeaderBytes;
            std::memset(buf_.data() + used_ + kSpacePacketHeaderBytes, 0x55, idle_payload);
            const uint16_t w0 = kIdleApid; // TM idle packet
            const uint16_t w1 = static_cast<uint16_t>(0xC000U | (idle_seq_++ & 0x3FFFU));
            const uint16_t w2 = static_cast<uint16_t>(idle_payload - 1U);
            write_be16(std::span<std::byte>(buf_).subspan(used_).template subspan<0, 2>(), w0);
            write_be16(std::span<std::byte>(buf_).subspan(used_ + 2U).template subspan<0, 2>(), w1);
            write_be16(std::span<std::byte>(buf_).subspan(used_ + 4U).template subspan<0, 2>(), w2);
        } else if (remaining > 0U) {
            std::memset(buf_.data() + used_, 0x00, remaining);
        }
        used_ = 0U;
        return std::span<const std::byte, FrameDataFieldBytes>(buf_);
    }

    [[nodiscard]] size_t used_bytes() const noexcept { return used_; }

private:
    std::array<std::byte, FrameDataFieldBytes> buf_{};
    size_t                                     used_{0U};
    uint16_t                                   idle_seq_{0U};
};

} // namespace strata::services::comms
