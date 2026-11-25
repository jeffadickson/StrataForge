#pragma once

#include <strata/core/concepts.hpp>
#include <strata/core/expected.hpp>
#include <strata/core/qos.hpp>
#include <strata/core/types.hpp>
#include <strata/data/frame_lease.hpp>
#include <strata/ipc/spsc_channel.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace strata {

/// True Zero-Copy 1-to-N DataTopic (Primitive 3 fan-out).
/// Only 32-byte FrameDesc values cross partition boundaries; payload bytes stay in XMPU DDR4/HBM.
template <typename PoolT, size_t MaxSubscribers, size_t RingDepth>
    requires(MaxSubscribers > 0U && MaxSubscribers <= 16U && PowerOfTwo<RingDepth>)
class DataTopic {
public:
    static constexpr size_t kMaxSubscribers = MaxSubscribers;
    static constexpr size_t kRingDepth      = RingDepth;

    explicit DataTopic(PoolT& pool) noexcept : pool_(&pool) {}

    /// Register a subscriber slot with an explicit mixed-criticality QoS profile.
    [[nodiscard]] Expected<uint8_t, Status> add_subscriber(QoSConfig qos) noexcept {
        if (sub_count_ >= MaxSubscribers) {
            return Unexpected(Status::Full);
        }
        const auto id = static_cast<uint8_t>(sub_count_++);
        subs_[id].active = true;
        subs_[id].qos    = qos;
        subs_[id].ring.reset(GenId{1U}, GenId{1U});
        return id;
    }

    /// Publish a move-only FrameLease to all active subscribers with ZERO payload copies.
    /// - Guaranteed subscribers (Class B/C): if ring is full, returns Status::Full (no data loss).
    /// - BestEffortDropOldest subscribers (Class D): if ring is full, skips cleanly & counts drop.
    [[nodiscard]] Status publish(FrameLease<PoolT>&& lease, TaiTimestamp now) noexcept {
        if (!lease.valid()) {
            return Status::InvalidArg;
        }

        // Pre-flight check: never partially drop a Guaranteed (Class B/C) subscriber
        for (size_t i = 0U; i < sub_count_; ++i) {
            if (!subs_[i].active) {
                continue;
            }
            if (subs_[i].qos.reliability == Reliability::Guaranteed) {
                SpscProducer<FrameDesc, RingDepth, kGuaranteedControlQoS> prod(
                    subs_[i].ring, static_cast<uint32_t>(i), NullNotifier{}, GenId{0U});
                if (prod.size() >= RingDepth) {
                    return Status::Full;
                }
            }
        }

        const FrameDesc desc = lease.detach_for_publish();
        uint32_t delivered_mask = 0U;

        // First compute which subscribers can accept and arm lease deadlines BEFORE publishing
        for (size_t i = 0U; i < sub_count_; ++i) {
            if (!subs_[i].active) {
                continue;
            }
            SpscProducer<FrameDesc, RingDepth, kGuaranteedControlQoS> prod(
                subs_[i].ring, static_cast<uint32_t>(i), NullNotifier{}, GenId{0U});
            if (prod.size() >= RingDepth) {
                // BestEffortDropOldest subscriber stalled — skip without blocking PL/Recorder
                ++subs_[i].dropped_frames;
                continue;
            }
            const uint64_t deadline = (subs_[i].qos.lease_timeout.ns() > 0U)
                                          ? (now.ns() + subs_[i].qos.lease_timeout.ns())
                                          : 0U;
            pool_->set_subscriber_deadline(desc.pool_slot_idx, static_cast<uint8_t>(i), deadline);
            delivered_mask |= (1U << i);
        }

        // Transfer pool ownership to the exact bitmask of accepted subscribers BEFORE pushing rings
        pool_->transfer_to_subscribers(desc.pool_slot_idx, delivered_mask, desc.length_bytes,
                                       TaiTimestamp{desc.tai_ns});

        // Push 32-byte descriptors into each accepted subscriber's lock-free SPSC ring
        for (size_t i = 0U; i < sub_count_; ++i) {
            if ((delivered_mask & (1U << i)) == 0U) {
                continue;
            }
            SpscProducer<FrameDesc, RingDepth, kGuaranteedControlQoS> prod(
                subs_[i].ring, static_cast<uint32_t>(i), NullNotifier{}, GenId{0U});
            const Status st = prod.try_push(desc);
            if (st != Status::Ok) {
                // Defensive cleanup if ring unexpectedly rejected
                pool_->release_subscriber_slot(desc.pool_slot_idx, static_cast<uint8_t>(i));
                ++subs_[i].dropped_frames;
            }
        }

        ++published_count_;
        return Status::Ok;
    }

    /// Pop one zero-copy ReadOnlyFrameView for subscriber sub_id.
    [[nodiscard]] Expected<ReadOnlyFrameView<PoolT>, Status>
    try_receive(uint8_t sub_id) noexcept {
        if (sub_id >= sub_count_ || !subs_[sub_id].active) {
            return Unexpected(Status::InvalidArg);
        }
        SpscConsumer<FrameDesc, RingDepth, kGuaranteedControlQoS> cons(subs_[sub_id].ring,
                                                                     GenId{0U});
        auto desc = cons.try_pop();
        if (!desc) {
            return Unexpected(desc.error());
        }
        return ReadOnlyFrameView<PoolT>(*pool_, *desc, sub_id);
    }

    /// Reclaim leases from stalled/hung subscribers past their QoS lease_timeout.
    uint32_t reclaim_expired(TaiTimestamp now) noexcept {
        return pool_->reclaim_expired_leases(now);
    }

    /// Crash recovery hook called by Class B FdirEngine when a subscriber partition restarts.
    /// Drains stale descriptors and revokes all pool slots held by that subscriber.
    uint32_t reset_crashed_subscriber(uint8_t sub_id, GenId new_gen) noexcept {
        if (sub_id >= sub_count_) {
            return 0U;
        }
        subs_[sub_id].ring.reset(GenId{1U}, new_gen);
        return pool_->revoke_subscriber(sub_id);
    }

    [[nodiscard]] uint32_t published_count() const noexcept { return published_count_; }

    [[nodiscard]] uint32_t subscriber_drops(uint8_t sub_id) const noexcept {
        return (sub_id < sub_count_) ? subs_[sub_id].dropped_frames : 0U;
    }

private:
    struct SubscriberSlot {
        bool                                         active{false};
        QoSConfig                                    qos{};
        SpscRingStorage<FrameDesc, RingDepth>        ring{};
        uint32_t                                     dropped_frames{0U};
    };

    PoolT*                                      pool_;
    std::array<SubscriberSlot, MaxSubscribers>  subs_{};
    size_t                                      sub_count_{0U};
    uint32_t                                    published_count_{0U};
};

} // namespace strata
