#pragma once

#include <strata/core/concepts.hpp>
#include <strata/core/expected.hpp>
#include <strata/core/types.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace strata {

/// Static XMPU-scoped DDR4/HBM Frame Pool (Primitive 3 backing store).
/// Uses a lock-free atomic subscriber-bitmask per slot so concurrent release and
/// FDIR crash-reclamation are idempotent and double-free is mathematically impossible.
template <size_t SlotCount, size_t SlotBytes>
    requires(PowerOfTwo<SlotCount> && SlotCount <= 64U && SlotBytes > 0U)
class FramePool {
public:
    static constexpr size_t   kSlotCount     = SlotCount;
    static constexpr size_t   kSlotBytes     = SlotBytes;
    static constexpr uint32_t kProducerBit   = 1U << 31U; // Bit 31 tracks exclusive producer ownership
    static constexpr uint32_t kSubBitsMask   = 0x7FFF'FFFFU; // Up to 31 concurrent zero-copy readers

    /// Construct backed by an external XMPU/SMMU DDR4/HBM physical span.
    explicit FramePool(std::span<std::byte> backing, uint64_t phys_base = 0U) noexcept
        : backing_(backing), phys_base_(phys_base) {
        reset();
    }

    void reset() noexcept {
        for (auto& m : meta_) {
            m.owner_mask.store(0U, std::memory_order_relaxed);
            m.producer_gen.store(0U, std::memory_order_relaxed);
            m.length_bytes.store(0U, std::memory_order_relaxed);
            m.tai_ns.store(0U, std::memory_order_relaxed);
            for (auto& dl : m.sub_deadline_ns) {
                dl.store(0U, std::memory_order_relaxed);
            }
        }
        reclaimed_count_.store(0U, std::memory_order_relaxed);
    }

    /// Acquire a free slot for exclusive producer/DMA writing.
    [[nodiscard]] Expected<uint16_t, Status> allocate_slot(GenId producer_gen) noexcept {
        for (size_t i = 0U; i < SlotCount; ++i) {
            uint32_t expected = 0U;
            if (meta_[i].owner_mask.compare_exchange_strong(
                    expected, kProducerBit, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                meta_[i].producer_gen.store(producer_gen.value, std::memory_order_release);
                meta_[i].length_bytes.store(0U, std::memory_order_relaxed);
                meta_[i].tai_ns.store(0U, std::memory_order_relaxed);
                return static_cast<uint16_t>(i);
            }
        }
        return Unexpected(Status::Full);
    }

    /// Transfer exclusive producer ownership to a bitmask of zero-copy Read-Only subscribers.
    /// If sub_mask == 0, the slot is immediately freed.
    void transfer_to_subscribers(uint16_t slot_idx, uint32_t sub_mask,
                                 uint32_t length_bytes, TaiTimestamp tai) noexcept {
        auto& m = meta_[slot_idx];
        m.length_bytes.store(length_bytes, std::memory_order_relaxed);
        m.tai_ns.store(tai.ns(), std::memory_order_relaxed);
        m.owner_mask.store(sub_mask & kSubBitsMask, std::memory_order_release);
    }

    /// Record a per-subscriber lease deadline (0 = no timeout) before exposing slot to subscriber.
    void set_subscriber_deadline(uint16_t slot_idx, uint8_t sub_id, uint64_t deadline_ns) noexcept {
        if (sub_id < kMaxTrackedSubs) {
            meta_[slot_idx].sub_deadline_ns[sub_id].store(deadline_ns, std::memory_order_release);
        }
    }

    /// Release exclusive producer ownership (called by ~FrameLease if unpublished).
    void release_producer_slot(uint16_t slot_idx) noexcept {
        if (slot_idx < SlotCount) {
            meta_[slot_idx].owner_mask.fetch_and(~kProducerBit, std::memory_order_acq_rel);
        }
    }

    /// Idempotently release a single subscriber's Read-Only lease bit on slot_idx.
    /// Slot automatically becomes free (owner_mask == 0) when the last subscriber releases.
    void release_subscriber_slot(uint16_t slot_idx, uint8_t sub_id) noexcept {
        if (slot_idx < SlotCount && sub_id < kMaxTrackedSubs) {
            meta_[slot_idx].sub_deadline_ns[sub_id].store(0U, std::memory_order_relaxed);
            const uint32_t bit = 1U << sub_id;
            meta_[slot_idx].owner_mask.fetch_and(~bit, std::memory_order_acq_rel);
        }
    }

    /// Hubris-style crash-safe lease reclamation: revokes expired Class D leases past deadline.
    uint32_t reclaim_expired_leases(TaiTimestamp now) noexcept {
        uint32_t reclaimed = 0U;
        for (size_t i = 0U; i < SlotCount; ++i) {
            const uint32_t mask = meta_[i].owner_mask.load(std::memory_order_acquire);
            if ((mask & kSubBitsMask) == 0U) {
                continue;
            }
            for (uint8_t s = 0U; s < kMaxTrackedSubs; ++s) {
                const uint32_t bit = 1U << s;
                if ((mask & bit) == 0U) {
                    continue;
                }
                const uint64_t dl = meta_[i].sub_deadline_ns[s].load(std::memory_order_acquire);
                if (dl != 0U && now.ns() >= dl) {
                    meta_[i].sub_deadline_ns[s].store(0U, std::memory_order_relaxed);
                    const uint32_t prev =
                        meta_[i].owner_mask.fetch_and(~bit, std::memory_order_acq_rel);
                    if ((prev & bit) != 0U) {
                        ++reclaimed;
                        reclaimed_count_.fetch_add(1U, std::memory_order_relaxed);
                    }
                }
            }
        }
        return reclaimed;
    }

    /// Immediately revoke all outstanding leases held by a crashed/restarted subscriber partition.
    uint32_t revoke_subscriber(uint8_t sub_id) noexcept {
        if (sub_id >= kMaxTrackedSubs) {
            return 0U;
        }
        uint32_t revoked = 0U;
        const uint32_t bit = 1U << sub_id;
        for (size_t i = 0U; i < SlotCount; ++i) {
            meta_[i].sub_deadline_ns[sub_id].store(0U, std::memory_order_relaxed);
            const uint32_t prev = meta_[i].owner_mask.fetch_and(~bit, std::memory_order_acq_rel);
            if ((prev & bit) != 0U) {
                ++revoked;
                reclaimed_count_.fetch_add(1U, std::memory_order_relaxed);
            }
        }
        return revoked;
    }

    [[nodiscard]] std::span<std::byte> writable_slot(uint16_t slot_idx) noexcept {
        return backing_.subspan(static_cast<size_t>(slot_idx) * SlotBytes, SlotBytes);
    }

    [[nodiscard]] std::span<const std::byte> readonly_slot(uint16_t slot_idx,
                                                           size_t len) const noexcept {
        const size_t clamped = (len <= SlotBytes) ? len : SlotBytes;
        return backing_.subspan(static_cast<size_t>(slot_idx) * SlotBytes, clamped);
    }

    [[nodiscard]] uint64_t slot_phys_addr(uint16_t slot_idx) const noexcept {
        return phys_base_ + static_cast<uint64_t>(slot_idx) * static_cast<uint64_t>(SlotBytes);
    }

    [[nodiscard]] size_t free_count() const noexcept {
        size_t count = 0U;
        for (const auto& m : meta_) {
            if (m.owner_mask.load(std::memory_order_acquire) == 0U) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] uint32_t reclaimed_count() const noexcept {
        return reclaimed_count_.load(std::memory_order_relaxed);
    }

private:
    static constexpr size_t kMaxTrackedSubs = 16U;

    struct alignas(64) SlotMeta {
        std::atomic<uint32_t>                      owner_mask{0U};
        std::atomic<uint16_t>                      producer_gen{0U};
        std::atomic<uint32_t>                      length_bytes{0U};
        std::atomic<uint64_t>                      tai_ns{0U};
        std::array<std::atomic<uint64_t>, kMaxTrackedSubs> sub_deadline_ns{};
    };

    std::span<std::byte>            backing_;
    uint64_t                        phys_base_{0U};
    std::array<SlotMeta, SlotCount> meta_{};
    std::atomic<uint32_t>           reclaimed_count_{0U};
};

/// Move-only, [[nodiscard]] linear RAII Producer FrameLease (replaces leak-prone F' Fw::Buffer).
/// Destructor automatically returns the slot to FramePool if not transferred via DataTopic::publish().
template <typename PoolT>
class [[nodiscard]] FrameLease {
public:
    FrameLease(PoolT& pool, const FrameDesc& desc) noexcept
        : pool_(&pool), desc_(desc), valid_(true) {}

    FrameLease(const FrameLease&)            = delete;
    FrameLease& operator=(const FrameLease&) = delete;

    FrameLease(FrameLease&& other) noexcept
        : pool_(other.pool_), desc_(other.desc_), valid_(other.valid_) {
        other.valid_ = false;
        other.pool_  = nullptr;
    }

    FrameLease& operator=(FrameLease&& other) noexcept {
        if (this != &other) {
            release();
            pool_        = other.pool_;
            desc_        = other.desc_;
            valid_       = other.valid_;
            other.valid_ = false;
            other.pool_  = nullptr;
        }
        return *this;
    }

    ~FrameLease() noexcept { release(); }

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] const FrameDesc& desc() const noexcept { return desc_; }
    [[nodiscard]] FrameDesc& mutable_desc() noexcept { return desc_; }

    [[nodiscard]] std::span<std::byte> writable_bytes() noexcept {
        return pool_->writable_slot(desc_.pool_slot_idx);
    }

    void set_payload_length(uint32_t bytes) noexcept { desc_.length_bytes = bytes; }
    void set_timestamp(TaiTimestamp tai) noexcept { desc_.tai_ns = tai.ns(); }

    /// Internal handoff used by DataTopic::publish() to transfer ownership to subscribers.
    [[nodiscard]] FrameDesc detach_for_publish() noexcept {
        valid_ = false;
        return desc_;
    }

    void release() noexcept {
        if (valid_ && pool_ != nullptr) {
            pool_->release_producer_slot(desc_.pool_slot_idx);
            valid_ = false;
        }
    }

private:
    PoolT*    pool_{nullptr};
    FrameDesc desc_{};
    bool      valid_{false};
};

/// Move-only, [[nodiscard]] Read-Only Zero-Copy View held by a DataTopic subscriber.
/// Destructor idempotently clears this subscriber's bit in FramePool; last reader frees slot.
template <typename PoolT>
class [[nodiscard]] ReadOnlyFrameView {
public:
    ReadOnlyFrameView(PoolT& pool, const FrameDesc& desc, uint8_t sub_id) noexcept
        : pool_(&pool), desc_(desc), sub_id_(sub_id), valid_(true) {}

    ReadOnlyFrameView(const ReadOnlyFrameView&)            = delete;
    ReadOnlyFrameView& operator=(const ReadOnlyFrameView&) = delete;

    ReadOnlyFrameView(ReadOnlyFrameView&& other) noexcept
        : pool_(other.pool_), desc_(other.desc_), sub_id_(other.sub_id_), valid_(other.valid_) {
        other.valid_ = false;
        other.pool_  = nullptr;
    }

    ReadOnlyFrameView& operator=(ReadOnlyFrameView&& other) noexcept {
        if (this != &other) {
            release();
            pool_        = other.pool_;
            desc_        = other.desc_;
            sub_id_      = other.sub_id_;
            valid_       = other.valid_;
            other.valid_ = false;
            other.pool_  = nullptr;
        }
        return *this;
    }

    ~ReadOnlyFrameView() noexcept { release(); }

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] const FrameDesc& desc() const noexcept { return desc_; }

    /// Direct zero-copy span into the XMPU Read-Only DDR4/HBM buffer slot.
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return pool_->readonly_slot(desc_.pool_slot_idx, desc_.length_bytes);
    }

    void release() noexcept {
        if (valid_ && pool_ != nullptr) {
            pool_->release_subscriber_slot(desc_.pool_slot_idx, sub_id_);
            valid_ = false;
        }
    }

private:
    PoolT*    pool_{nullptr};
    FrameDesc desc_{};
    uint8_t   sub_id_{0U};
    bool      valid_{false};
};

} // namespace strata
