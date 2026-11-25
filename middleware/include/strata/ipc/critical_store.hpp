#pragma once

#include <strata/core/concepts.hpp>
#include <strata/core/crc32c.hpp>
#include <strata/core/expected.hpp>
#include <strata/core/types.hpp>

#include <array>
#include <cstdint>

namespace strata {

/// Ping-pong crash-atomic persistent state store (Primitive 5).
/// Unifies NASA cFS Critical Data Store (CDS) and DLR ScOSA task checkpointing.
/// Placed in a hypervisor-preserved ECC SRAM/OCM/FRAM region outside partition .bss so
/// WarmStart / SEU restart / DFX slot failover restores verified state in < 2 ms.
template <WirePod StateT>
struct alignas(64) CriticalStoreStorage {
    static constexpr uint32_t kMagic = 0x5343'4453U; // "SCDS"

    struct Slot {
        uint32_t magic{0U};
        uint32_t seq_num{0U};
        uint64_t manifest_hash{0U};
        uint32_t crc32c{0U};
        uint32_t reserved{0U};
        StateT   payload{};
    };

    std::array<Slot, 2U> slots{};
};

template <WirePod StateT>
class CriticalStore {
public:
    CriticalStore(CriticalStoreStorage<StateT>& storage, uint64_t manifest_hash) noexcept
        : storage_(&storage), manifest_hash_(manifest_hash) {}

    /// Crash-atomic ping-pong checkpoint write. Never overwrites the most recently committed slot.
    void checkpoint(const StateT& state) noexcept {
        const auto best = find_best_valid_slot();
        const uint8_t target_idx = best.has_value ? static_cast<uint8_t>(1U - best.index) : 0U;
        const uint32_t next_seq  = best.has_value ? (best.seq_num + 1U) : 1U;

        auto& slot = storage_->slots[target_idx];
        // Invalidate magic while writing so a mid-write crash/SEU cleanly falls back to peer slot
        slot.magic         = 0U;
        slot.manifest_hash = manifest_hash_;
        slot.seq_num       = next_seq;
        slot.payload       = state;
        slot.crc32c        = compute_slot_crc(slot);
        slot.magic         = CriticalStoreStorage<StateT>::kMagic;
    }

    /// Restore the newest CRC-32C + MANIFEST_HASH verified checkpoint after WarmStart / failover.
    [[nodiscard]] Expected<StateT, Status> restore() const noexcept {
        const auto best = find_best_valid_slot();
        if (!best.has_value) {
            return Unexpected(Status::NoDataYet);
        }
        return storage_->slots[best.index].payload;
    }

    /// Clear both ping-pong slots explicitly (used on ColdStart).
    void clear() noexcept {
        for (auto& s : storage_->slots) {
            s.magic   = 0U;
            s.seq_num = 0U;
            s.crc32c  = 0U;
        }
    }

    [[nodiscard]] uint32_t latest_sequence() const noexcept {
        const auto best = find_best_valid_slot();
        return best.has_value ? best.seq_num : 0U;
    }

private:
    struct BestSlot {
        bool     has_value{false};
        uint8_t  index{0U};
        uint32_t seq_num{0U};
    };

    [[nodiscard]] static uint32_t compute_slot_crc(
        const typename CriticalStoreStorage<StateT>::Slot& s) noexcept {
        uint32_t crc = Crc32c::compute_pod(s.seq_num);
        crc          = Crc32c::update(
            crc, std::as_bytes(std::span<const uint64_t, 1>{&s.manifest_hash, 1U}));
        crc = Crc32c::update(crc, std::as_bytes(std::span<const StateT, 1>{&s.payload, 1U}));
        return crc;
    }

    [[nodiscard]] bool is_slot_valid(uint8_t idx) const noexcept {
        const auto& s = storage_->slots[idx];
        return s.magic == CriticalStoreStorage<StateT>::kMagic &&
               s.manifest_hash == manifest_hash_ &&
               s.seq_num > 0U &&
               s.crc32c == compute_slot_crc(s);
    }

    [[nodiscard]] BestSlot find_best_valid_slot() const noexcept {
        const bool v0 = is_slot_valid(0U);
        const bool v1 = is_slot_valid(1U);
        if (v0 && v1) {
            const uint32_t s0 = storage_->slots[0].seq_num;
            const uint32_t s1 = storage_->slots[1].seq_num;
            return (s1 >= s0) ? BestSlot{true, 1U, s1} : BestSlot{true, 0U, s0};
        }
        if (v0) {
            return BestSlot{true, 0U, storage_->slots[0].seq_num};
        }
        if (v1) {
            return BestSlot{true, 1U, storage_->slots[1].seq_num};
        }
        return BestSlot{};
    }

    CriticalStoreStorage<StateT>* storage_;
    uint64_t                      manifest_hash_;
};

} // namespace strata
