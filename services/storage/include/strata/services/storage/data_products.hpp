#pragma once

#include <strata/core/concepts.hpp>
#include <strata/core/crc32c.hpp>
#include <strata/core/expected.hpp>
#include <strata/core/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace strata::services::storage {

/// 64-byte CRC-32C-protected Data Product Container Header (Zero-Copy Data Products 2.0).
/// Stamped directly onto DDR4/HBM FrameLease payloads without CPU copying (upgrades F' Fw::Dp).
struct alignas(64) DpHeader {
    static constexpr uint32_t kMagic = 0x5354'4450U; // "STDP"

    uint32_t magic{kMagic};
    uint32_t product_id{0U};     // Unique monotonic product ID in catalog
    uint16_t container_id{0U};   // Dictionary container type ID
    uint8_t  priority{128U};     // 0 = highest priority, 255 = lowest priority
    uint8_t  compression_alg{0U}; // 0 = Raw, 1 = PL CCSDS-123, 2 = LZ4, 3 = Zstd
    uint32_t record_count{0U};
    uint64_t tai_start_ns{0U};
    uint64_t tai_end_ns{0U};
    uint64_t payload_bytes{0U};  // Full 64-bit size (eliminates F' 4 GiB limit)
    uint64_t storage_offset{0U}; // Offset on NV solid-state recorder
    uint32_t payload_crc32c{0U};
    uint32_t header_crc32c{0U};
    uint8_t  reserved[8]{};
};

static_assert(sizeof(DpHeader) == 64U, "DpHeader must be exactly one 64B cache line");
static_assert(WirePod<DpHeader>);

/// Catalog record tracking one persisted Data Product on non-volatile storage.
struct DpCatalogEntry {
    uint32_t product_id{0U};
    uint16_t container_id{0U};
    uint8_t  priority{128U}; // 0 = highest downlink priority
    uint8_t  compression_alg{0U};
    uint64_t tai_created_ns{0U};
    uint64_t total_bytes{0U};
    uint64_t downlinked_offset{0U}; // Resume pointer for interrupted contact passes
    uint64_t storage_lba{0U};
    uint32_t crc32c{0U};
    bool     active{false};
};

/// Priority-Driven Data Product Catalog (F' Svc::DpCatalog parity + 64-bit sizes + auto-eviction).
template <size_t MaxProducts = 64U>
class DpCatalog {
public:
    explicit DpCatalog(uint64_t capacity_bytes) noexcept : capacity_bytes_(capacity_bytes) {}

    /// Register a persisted product. If storage exceeds capacity, automatically evicts
    /// strictly lower-priority (numerically larger) oldest products to protect high-priority science.
    [[nodiscard]] Expected<uint32_t, Status>
    register_product(uint16_t container_id, uint8_t priority, uint8_t compression_alg,
                     TaiTimestamp created, uint64_t total_bytes, uint64_t storage_lba,
                     uint32_t crc32c) noexcept {
        if (total_bytes == 0U || total_bytes > capacity_bytes_) {
            return Unexpected(Status::InvalidArg);
        }

        while (used_bytes_ + total_bytes > capacity_bytes_ || active_count_ >= MaxProducts) {
            if (!evict_one_lower_priority(priority)) {
                return Unexpected(Status::Full);
            }
        }

        for (auto& e : entries_) {
            if (!e.active) {
                const uint32_t id = next_id_++;
                e = DpCatalogEntry{
                    .product_id        = id,
                    .container_id      = container_id,
                    .priority          = priority,
                    .compression_alg   = compression_alg,
                    .tai_created_ns    = created.ns(),
                    .total_bytes       = total_bytes,
                    .downlinked_offset = 0U,
                    .storage_lba       = storage_lba,
                    .crc32c            = crc32c,
                    .active            = true,
                };
                used_bytes_ += total_bytes;
                ++active_count_;
                return id;
            }
        }
        return Unexpected(Status::Full);
    }

    /// Select the highest-priority (lowest numeric priority, oldest tie-breaker) pending product.
    [[nodiscard]] Expected<DpCatalogEntry, Status> select_next_downlink() const noexcept {
        const DpCatalogEntry* best = nullptr;
        for (const auto& e : entries_) {
            if (!e.active || e.downlinked_offset >= e.total_bytes) {
                continue;
            }
            if (best == nullptr || e.priority < best->priority ||
                (e.priority == best->priority && e.tai_created_ns < best->tai_created_ns)) {
                best = &e;
            }
        }
        if (best == nullptr) {
            return Unexpected(Status::Empty);
        }
        return *best;
    }

    /// Update partial downlink progress (supports seamless resume across ground passes).
    [[nodiscard]] Status advance_downlink(uint32_t product_id, uint64_t new_offset) noexcept {
        for (auto& e : entries_) {
            if (e.active && e.product_id == product_id) {
                e.downlinked_offset = (new_offset > e.total_bytes) ? e.total_bytes : new_offset;
                return Status::Ok;
            }
        }
        return Status::InvalidArg;
    }

    [[nodiscard]] Status delete_product(uint32_t product_id) noexcept {
        for (auto& e : entries_) {
            if (e.active && e.product_id == product_id) {
                e.active = false;
                used_bytes_ -= e.total_bytes;
                --active_count_;
                return Status::Ok;
            }
        }
        return Status::InvalidArg;
    }

    [[nodiscard]] uint64_t used_bytes() const noexcept { return used_bytes_; }
    [[nodiscard]] size_t   active_count() const noexcept { return active_count_; }
    [[nodiscard]] uint32_t evicted_count() const noexcept { return evicted_count_; }

private:
    bool evict_one_lower_priority(uint8_t incoming_prio) noexcept {
        DpCatalogEntry* victim = nullptr;
        for (auto& e : entries_) {
            if (!e.active || e.priority <= incoming_prio) {
                continue;
            }
            if (victim == nullptr || e.priority > victim->priority ||
                (e.priority == victim->priority && e.tai_created_ns < victim->tai_created_ns)) {
                victim = &e;
            }
        }
        if (victim == nullptr) {
            return false;
        }
        victim->active = false;
        used_bytes_ -= victim->total_bytes;
        --active_count_;
        ++evicted_count_;
        return true;
    }

    std::array<DpCatalogEntry, MaxProducts> entries_{};
    uint64_t                                capacity_bytes_{0U};
    uint64_t                                used_bytes_{0U};
    size_t                                  active_count_{0U};
    uint32_t                                next_id_{1U};
    uint32_t                                evicted_count_{0U};
};

} // namespace strata::services::storage
