#pragma once

#include <strata/core/concepts.hpp>
#include <strata/core/expected.hpp>
#include <strata/core/qos.hpp>
#include <strata/core/types.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace strata {

/// Shared-memory backing storage for a cache-line-isolated lock-free SPSC ring (Primitive 2).
/// Supports bounded inter-partition command/event queuing with generation tracking and doorbells.
template <WirePod T, size_t Capacity>
    requires PowerOfTwo<Capacity>
struct alignas(64) SpscRingStorage {
    static constexpr size_t kCapacity = Capacity;

    SpscHeader             hdr{};
    std::array<T, Capacity> slots{};

    constexpr SpscRingStorage() noexcept {
        hdr.ring_mask = static_cast<uint32_t>(Capacity - 1U);
        hdr.elem_size = static_cast<uint32_t>(sizeof(T));
    }

    void reset(GenId prod_gen = GenId{1U}, GenId cons_gen = GenId{1U}) noexcept {
        hdr.head         = 0U;
        hdr.tail         = 0U;
        hdr.notify_armed = 0U;
        hdr.drop_count   = 0U;
        hdr.prod_gen     = prod_gen.value;
        hdr.cons_gen     = cons_gen.value;
        hdr.ring_mask    = static_cast<uint32_t>(Capacity - 1U);
        hdr.elem_size    = static_cast<uint32_t>(sizeof(T));
    }
};

/// Single-producer handle for a lock-free SPSC channel with interrupt-coalesced doorbell.
template <WirePod T, size_t Capacity, QoSConfig QoS = kGuaranteedControlQoS,
          DoorbellNotifier Notifier = NullNotifier>
    requires PowerOfTwo<Capacity>
class SpscProducer {
public:
    static constexpr QoSConfig kQoS      = QoS;
    static constexpr size_t    kCapacity = Capacity;

    SpscProducer(SpscRingStorage<T, Capacity>& storage, uint32_t channel_id = 0U,
                 Notifier notifier = {}, GenId expected_cons_gen = GenId{1U}) noexcept
        : storage_(&storage),
          channel_id_(channel_id),
          notifier_(notifier),
          expected_cons_gen_(expected_cons_gen) {}

    /// Push one element lock-free. Checks consumer generation counter before writing.
    [[nodiscard]] Status try_push(const T& item) noexcept {
        auto head_ref = std::atomic_ref<uint32_t>(storage_->hdr.head);
        auto tail_ref = std::atomic_ref<uint32_t>(storage_->hdr.tail);
        auto cgen_ref = std::atomic_ref<uint32_t>(storage_->hdr.cons_gen);

        if (expected_cons_gen_.value != 0U &&
            cgen_ref.load(std::memory_order_acquire) != expected_cons_gen_.value) {
            return Status::StaleGeneration;
        }

        const uint32_t head = head_ref.load(std::memory_order_relaxed);
        const uint32_t tail = tail_ref.load(std::memory_order_acquire);
        const uint32_t used = head - tail;

        if (used >= static_cast<uint32_t>(Capacity)) {
            if constexpr (QoS.reliability == Reliability::BestEffortDropOldest) {
                // Non-blocking Class D policy: record drop without stalling producer
                std::atomic_ref<uint32_t>(storage_->hdr.drop_count)
                    .fetch_add(1U, std::memory_order_relaxed);
                return Status::Full;
            } else {
                return Status::Full;
            }
        }

        storage_->slots[head & storage_->hdr.ring_mask] = item;
        head_ref.store(head + 1U, std::memory_order_release);

        // Coalesced hardware doorbell: ring only if consumer explicitly armed wakeup
        auto armed_ref = std::atomic_ref<uint32_t>(storage_->hdr.notify_armed);
        if (armed_ref.load(std::memory_order_acquire) != 0U) {
            armed_ref.store(0U, std::memory_order_relaxed);
            notifier_.notify(channel_id_);
        }
        return Status::Ok;
    }

    void acknowledge_consumer_generation(GenId new_gen) noexcept { expected_cons_gen_ = new_gen; }

    void advance_producer_generation(GenId new_gen) noexcept {
        std::atomic_ref<uint32_t>(storage_->hdr.prod_gen)
            .store(new_gen.value, std::memory_order_release);
    }

    [[nodiscard]] uint32_t size() const noexcept {
        const uint32_t head = std::atomic_ref<uint32_t>(storage_->hdr.head)
                                  .load(std::memory_order_relaxed);
        const uint32_t tail = std::atomic_ref<uint32_t>(storage_->hdr.tail)
                                  .load(std::memory_order_acquire);
        return head - tail;
    }

    [[nodiscard]] uint32_t drop_count() const noexcept {
        return std::atomic_ref<uint32_t>(storage_->hdr.drop_count)
            .load(std::memory_order_relaxed);
    }

private:
    SpscRingStorage<T, Capacity>* storage_;
    uint32_t                      channel_id_;
    [[no_unique_address]] Notifier notifier_;
    GenId                         expected_cons_gen_;
};

/// Single-consumer handle for a lock-free SPSC channel with bounded WCET draining.
template <WirePod T, size_t Capacity, QoSConfig QoS = kGuaranteedControlQoS>
    requires PowerOfTwo<Capacity>
class SpscConsumer {
public:
    static constexpr QoSConfig kQoS      = QoS;
    static constexpr size_t    kCapacity = Capacity;

    explicit SpscConsumer(SpscRingStorage<T, Capacity>& storage,
                          GenId expected_prod_gen = GenId{1U}) noexcept
        : storage_(&storage), expected_prod_gen_(expected_prod_gen) {}

    [[nodiscard]] Expected<T, Status> try_pop() noexcept {
        auto head_ref = std::atomic_ref<uint32_t>(storage_->hdr.head);
        auto tail_ref = std::atomic_ref<uint32_t>(storage_->hdr.tail);
        auto pgen_ref = std::atomic_ref<uint32_t>(storage_->hdr.prod_gen);

        if (expected_prod_gen_.value != 0U &&
            pgen_ref.load(std::memory_order_acquire) != expected_prod_gen_.value) {
            return Unexpected(Status::StaleGeneration);
        }

        const uint32_t tail = tail_ref.load(std::memory_order_relaxed);
        const uint32_t head = head_ref.load(std::memory_order_acquire);
        if (tail == head) {
            return Unexpected(Status::Empty);
        }

        T item = storage_->slots[tail & storage_->hdr.ring_mask];
        tail_ref.store(tail + 1U, std::memory_order_release);
        return item;
    }

    /// Bounded WCET drain: processes at most max_items per minor-frame tick.
    template <typename Fn>
    uint32_t drain_up_to(uint32_t max_items, Fn&& handler) noexcept {
        uint32_t count = 0U;
        while (count < max_items) {
            auto res = try_pop();
            if (!res) {
                break;
            }
            handler(*res);
            ++count;
        }
        return count;
    }

    /// Arm doorbell wakeup before calling hyp_wait_event().
    void arm_notify() noexcept {
        std::atomic_ref<uint32_t>(storage_->hdr.notify_armed)
            .store(1U, std::memory_order_release);
    }

    void acknowledge_producer_generation(GenId new_gen) noexcept { expected_prod_gen_ = new_gen; }

    void advance_consumer_generation(GenId new_gen) noexcept {
        std::atomic_ref<uint32_t>(storage_->hdr.cons_gen)
            .store(new_gen.value, std::memory_order_release);
    }

    [[nodiscard]] uint32_t available() const noexcept {
        const uint32_t head = std::atomic_ref<uint32_t>(storage_->hdr.head)
                                  .load(std::memory_order_acquire);
        const uint32_t tail = std::atomic_ref<uint32_t>(storage_->hdr.tail)
                                  .load(std::memory_order_relaxed);
        return head - tail;
    }

private:
    SpscRingStorage<T, Capacity>* storage_;
    GenId                         expected_prod_gen_;
};

} // namespace strata
