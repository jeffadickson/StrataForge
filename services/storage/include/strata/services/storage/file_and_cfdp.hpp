#pragma once

#include <strata/core/crc32c.hpp>
#include <strata/core/expected.hpp>
#include <strata/core/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace strata::services::storage {

/// 64-bit Partition-Jailed Storage Volume (eliminates F' FileDownlink 4 GiB ceiling).
struct StorageVolume {
    uint8_t  volume_id{0U};
    uint8_t  owner_partition{0U};
    uint64_t quota_bytes{0U};
    uint64_t used_bytes{0U};
    bool     allow_dfx_staging{false};
    bool     enabled{false};
};

/// 64-bit Sandboxed File Service & Authenticated DFX/PDI Staging Manager.
template <size_t MaxVolumes = 8U, size_t MaxFiles = 32U>
class FileService {
public:
    struct FileEntry {
        uint32_t file_id{0U};
        uint8_t  volume_id{0U};
        uint64_t size_bytes{0U};
        uint32_t crc32c{0U};
        bool     verified_dfx_image{false};
        bool     active{false};
    };

    [[nodiscard]] Status register_volume(const StorageVolume& vol) noexcept {
        for (auto& v : volumes_) {
            if (!v.enabled) {
                v         = vol;
                v.enabled = true;
                return Status::Ok;
            }
        }
        return Status::Full;
    }

    /// Create/commit a file (supports >4 GiB 64-bit files) enforcing per-partition volume jail & quota.
    [[nodiscard]] Expected<uint32_t, Status>
    commit_file(uint8_t caller_partition, uint8_t volume_id, uint64_t size_bytes,
                uint32_t crc32c) noexcept {
        auto* vol = find_volume(volume_id);
        if (vol == nullptr) return Unexpected(Status::InvalidArg);
        if (vol->owner_partition != caller_partition) return Unexpected(Status::PermissionDenied);
        if (size_bytes == 0U || vol->used_bytes + size_bytes > vol->quota_bytes) {
            return Unexpected(Status::Full);
        }

        for (auto& f : files_) {
            if (!f.active) {
                const uint32_t id = next_file_id_++;
                f = FileEntry{
                    .file_id            = id,
                    .volume_id          = volume_id,
                    .size_bytes         = size_bytes,
                    .crc32c             = crc32c,
                    .verified_dfx_image = false,
                    .active             = true,
                };
                vol->used_bytes += size_bytes;
                return id;
            }
        }
        return Unexpected(Status::Full);
    }

    /// Authenticate a staged partial PDI / bitstream file before SlotManager can load it.
    [[nodiscard]] Status authenticate_dfx_stage(uint32_t file_id, uint32_t expected_crc) noexcept {
        for (auto& f : files_) {
            if (!f.active || f.file_id != file_id) continue;
            const auto* vol = find_volume(f.volume_id);
            if (vol == nullptr || !vol->allow_dfx_staging) return Status::PermissionDenied;
            if (f.crc32c != expected_crc) return Status::CrcMismatch;
            f.verified_dfx_image = true;
            return Status::Ok;
        }
        return Status::InvalidArg;
    }

    [[nodiscard]] bool is_verified_dfx_image(uint32_t file_id) const noexcept {
        for (const auto& f : files_) {
            if (f.active && f.file_id == file_id) return f.verified_dfx_image;
        }
        return false;
    }

private:
    StorageVolume* find_volume(uint8_t vid) noexcept {
        for (auto& v : volumes_) {
            if (v.enabled && v.volume_id == vid) return &v;
        }
        return nullptr;
    }

    const StorageVolume* find_volume(uint8_t vid) const noexcept {
        for (const auto& v : volumes_) {
            if (v.enabled && v.volume_id == vid) return &v;
        }
        return nullptr;
    }

    std::array<StorageVolume, MaxVolumes> volumes_{};
    std::array<FileEntry, MaxFiles>       files_{};
    uint32_t                              next_file_id_{1U};
};

/// 64-bit CCSDS CFDP (CCSDS 727.0-B-5) Class 1 & Class 2 Receiver Transaction Engine.
/// Tracks received byte extents and generates exact missing-gap NAK lists for lossy links.
struct ByteExtent {
    uint64_t start{0U}; // Inclusive
    uint64_t end{0U};   // Exclusive
};

template <size_t MaxExtents = 32U>
class CfdpRxTransaction {
public:
    void start(uint64_t transaction_id, uint64_t expected_file_size, bool class2_reliable) noexcept {
        tx_id_        = transaction_id;
        expected_len_ = expected_file_size;
        class2_       = class2_reliable;
        extent_cnt_   = 0U;
        eof_received_ = false;
        active_       = true;
    }

    /// Record arrival of a 64-bit File Data PDU [offset, offset + len), coalescing contiguous spans.
    [[nodiscard]] Status on_file_data(uint64_t offset, uint64_t len) noexcept {
        if (!active_ || len == 0U || offset > expected_len_ || offset + len > expected_len_) {
            return Status::InvalidArg;
        }
        const uint64_t new_start = offset;
        const uint64_t new_end   = offset + len;

        // Try merging into existing overlapping/adjacent extents
        uint64_t merged_start = new_start;
        uint64_t merged_end   = new_end;
        size_t   write_idx    = 0U;
        for (size_t i = 0U; i < extent_cnt_; ++i) {
            const auto e = extents_[i];
            if (e.end >= merged_start && merged_end >= e.start) {
                if (e.start < merged_start) merged_start = e.start;
                if (e.end > merged_end) merged_end = e.end;
            } else {
                extents_[write_idx++] = e;
            }
        }
        if (write_idx >= MaxExtents) {
            return Status::Full;
        }
        extents_[write_idx++] = ByteExtent{merged_start, merged_end};
        extent_cnt_           = write_idx;

        // Keep extents sorted by start offset (tiny insertion sort, O(MaxExtents))
        for (size_t i = 1U; i < extent_cnt_; ++i) {
            const auto key = extents_[i];
            size_t     j   = i;
            while (j > 0U && extents_[j - 1U].start > key.start) {
                extents_[j] = extents_[j - 1U];
                --j;
            }
            extents_[j] = key;
        }
        return Status::Ok;
    }

    void on_eof() noexcept { eof_received_ = true; }

    [[nodiscard]] bool is_complete() const noexcept {
        return active_ && eof_received_ && extent_cnt_ == 1U && extents_[0].start == 0U &&
               extents_[0].end == expected_len_;
    }

    /// Compute missing byte gaps for Class 2 CFDP NAK PDU generation.
    [[nodiscard]] size_t collect_missing_extents(std::span<ByteExtent> out_gaps) const noexcept {
        if (!active_ || !class2_) return 0U;
        size_t   gap_cnt = 0U;
        uint64_t cursor  = 0U;
        for (size_t i = 0U; i < extent_cnt_; ++i) {
            if (extents_[i].start > cursor && gap_cnt < out_gaps.size()) {
                out_gaps[gap_cnt++] = ByteExtent{cursor, extents_[i].start};
            }
            if (extents_[i].end > cursor) {
                cursor = extents_[i].end;
            }
        }
        if (cursor < expected_len_ && gap_cnt < out_gaps.size()) {
            out_gaps[gap_cnt++] = ByteExtent{cursor, expected_len_};
        }
        return gap_cnt;
    }

    [[nodiscard]] uint64_t transaction_id() const noexcept { return tx_id_; }

private:
    std::array<ByteExtent, MaxExtents> extents_{};
    size_t                             extent_cnt_{0U};
    uint64_t                           tx_id_{0U};
    uint64_t                           expected_len_{0U};
    bool                               class2_{false};
    bool                               eof_received_{false};
    bool                               active_{false};
};

} // namespace strata::services::storage
