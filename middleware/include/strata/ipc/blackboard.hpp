#pragma once

#include <strata/core/concepts.hpp>
#include <strata/core/expected.hpp>
#include <strata/core/qos.hpp>
#include <strata/core/types.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace strata {

/// Shared-memory backing storage for a lock-free Seqlock Blackboard (Primitive 1).
/// Writer partition maps RW; Reader partition(s) map RO via Stage-2 MMU / XMPU.
/// Unifies ARINC 653 Sampling Port, iceoryx2 Blackboard, DDS TransientLocal, F' TlmChan & PolyDb.
template <WirePod T>
struct alignas(64) BlackboardStorage {
    uint32_t     seq{0U};          // Even = consistent, Odd = write in progress
    uint32_t     writer_gen{0U};   // Writer partition generation counter
    uint64_t     last_tai_ns{0U};  // Hardware TAI timestamp of last publish
    uint32_t     publish_count{0U};
    uint32_t     reserved{0U};
    alignas(8) T payload{};
};

/// Metadata + snapshot returned by BlackboardReader::read().
template <WirePod T>
struct BlackboardSample {
    T            value{};
    TaiTimestamp published_at{0U};
    GenId        writer_gen{0U};
    uint32_t     seq{0U};
};

/// Single-writer handle to a shared-memory Seqlock Blackboard.
template <WirePod T, QoSConfig QoS = kDefaultBlackboardQoS>
class BlackboardWriter {
public:
    static constexpr QoSConfig kQoS = QoS;

    explicit BlackboardWriter(BlackboardStorage<T>& storage, GenId gen = GenId{1U}) noexcept
        : storage_(&storage), writer_gen_(gen) {
        storage_->writer_gen = gen.value;
    }

    /// Lock-free ~20 ns atomic snapshot publish. Never blocks regardless of slow/hung readers.
    void publish(const T& value, TaiTimestamp now) noexcept {
        std::atomic_ref<uint32_t> seq_ref(storage_->seq);
        const uint32_t current = seq_ref.load(std::memory_order_relaxed);
        const uint32_t odd_seq = (current | 1U);
        const uint32_t writing_seq = (odd_seq == current) ? (current + 2U) : odd_seq;

        // Mark write-in-progress (odd sequence) before mutating payload
        seq_ref.store(writing_seq, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);

        storage_->writer_gen  = writer_gen_.value;
        storage_->last_tai_ns = now.ns();
        storage_->payload     = value;
        ++storage_->publish_count;

        // Release fence guarantees all payload stores are visible before publishing even sequence
        std::atomic_thread_fence(std::memory_order_release);
        seq_ref.store(writing_seq + 1U, std::memory_order_release);
    }

    void set_generation(GenId gen) noexcept { writer_gen_ = gen; }
    [[nodiscard]] uint32_t publish_count() const noexcept { return storage_->publish_count; }

private:
    BlackboardStorage<T>* storage_;
    GenId                 writer_gen_;
};

/// Lock-free, bounded-retry reader handle to a Seqlock Blackboard.
/// Enforces compile-time DDS Durability (Volatile vs. TransientLocal) and Deadline QoS.
template <WirePod T, QoSConfig QoS = kDefaultBlackboardQoS>
class BlackboardReader {
public:
    static constexpr QoSConfig kQoS        = QoS;
    static constexpr uint32_t  kMaxRetries = 4U;

    explicit BlackboardReader(const BlackboardStorage<T>& storage) noexcept
        : storage_(&storage) {
        if constexpr (QoS.durability == Durability::Volatile) {
            // Volatile readers ignore samples published before attach
            baseline_seq_ = std::atomic_ref<uint32_t>(
                                const_cast<uint32_t&>(storage_->seq))
                                .load(std::memory_order_acquire);
        }
    }

    /// Bounded non-blocking Seqlock read with compile-time Deadline & Durability verification.
    [[nodiscard]] Expected<BlackboardSample<T>, Status> read(TaiTimestamp now) const noexcept {
        // Note: const_cast is strictly for atomic_ref<uint32_t>::load on a RO shared page.
        auto seq_ref = std::atomic_ref<uint32_t>(const_cast<uint32_t&>(storage_->seq));

        for (uint32_t attempt = 0U; attempt < kMaxRetries; ++attempt) {
            const uint32_t s1 = seq_ref.load(std::memory_order_acquire);
            if ((s1 & 1U) != 0U) {
                continue; // Writer currently mid-update
            }
            if (s1 == 0U) {
                return Unexpected(Status::NoDataYet);
            }
            if constexpr (QoS.durability == Durability::Volatile) {
                if (s1 <= baseline_seq_) {
                    return Unexpected(Status::NoDataYet);
                }
            }

            BlackboardSample<T> sample{};
            sample.published_at = TaiTimestamp{storage_->last_tai_ns};
            sample.writer_gen   = GenId{static_cast<uint16_t>(storage_->writer_gen)};
            sample.value        = storage_->payload;
            sample.seq          = s1;

            std::atomic_thread_fence(std::memory_order_acquire);
            const uint32_t s2 = seq_ref.load(std::memory_order_relaxed);
            if (s1 == s2) {
                if constexpr (QoS.deadline.value_ns > 0U) {
                    if (elapsed_since(now, sample.published_at) > QoS.deadline) {
                        return Unexpected(Status::DeadlineMissed);
                    }
                }
                return sample;
            }
        }
        return Unexpected(Status::TornRead);
    }

private:
    const BlackboardStorage<T>* storage_;
    uint32_t                    baseline_seq_{0U};
};

} // namespace strata
