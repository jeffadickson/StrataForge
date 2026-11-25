#pragma once

#include <strata/core/crc32c.hpp>
#include <strata/core/expected.hpp>
#include <strata/core/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace strata::services::storage {

/// 64-bit Partition-Jailed Storage Volume.
struct StorageVolume {
    uint8_t  volume_id{0U};
    uint8_t  owner_partition{0U};
    uint64_t quota_bytes{0U};
    uint64_t used_bytes{0U};
    bool     allow_dfx_staging{false};
    bool     enabled{false};
};

/// 64-bit Sandboxed File Service & Authenticated DFX/PDI Staging Manager (STRATA-STOR-004).
/// Supports commit, move, delete, directory listing, CRC-32C verification, and DFX staging auth.
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

    /// Move a file between two volumes owned by the calling partition (updating quotas atomically).
    [[nodiscard]] Status move_file(uint8_t caller_partition, uint32_t file_id,
                                   uint8_t dst_volume_id) noexcept {
        auto* f = find_file(file_id);
        if (f == nullptr) return Status::InvalidArg;
        auto* src_vol = find_volume(f->volume_id);
        auto* dst_vol = find_volume(dst_volume_id);
        if (src_vol == nullptr || dst_vol == nullptr) return Status::InvalidArg;
        if (src_vol->owner_partition != caller_partition ||
            dst_vol->owner_partition != caller_partition) {
            return Status::PermissionDenied;
        }
        if (f->volume_id == dst_volume_id) return Status::Ok;
        if (dst_vol->used_bytes + f->size_bytes > dst_vol->quota_bytes) {
            return Status::Full;
        }
        src_vol->used_bytes -= f->size_bytes;
        dst_vol->used_bytes += f->size_bytes;
        f->volume_id          = dst_volume_id;
        f->verified_dfx_image = false; // Must re-authenticate after moving into DFX staging
        return Status::Ok;
    }

    /// Delete a file owned by caller_partition and reclaim volume quota bytes.
    [[nodiscard]] Status delete_file(uint8_t caller_partition, uint32_t file_id) noexcept {
        auto* f = find_file(file_id);
        if (f == nullptr) return Status::InvalidArg;
        auto* vol = find_volume(f->volume_id);
        if (vol == nullptr || vol->owner_partition != caller_partition) {
            return Status::PermissionDenied;
        }
        vol->used_bytes -= f->size_bytes;
        f->active             = false;
        f->verified_dfx_image = false;
        return Status::Ok;
    }

    /// List all active files in a volume into caller-provided span (enforcing ownership jail).
    [[nodiscard]] Expected<size_t, Status>
    list_directory(uint8_t caller_partition, uint8_t volume_id,
                   std::span<FileEntry> out_entries) const noexcept {
        const auto* vol = find_volume(volume_id);
        if (vol == nullptr) return Unexpected(Status::InvalidArg);
        if (vol->owner_partition != caller_partition) return Unexpected(Status::PermissionDenied);
        size_t cnt = 0U;
        for (const auto& f : files_) {
            if (f.active && f.volume_id == volume_id) {
                if (cnt < out_entries.size()) {
                    out_entries[cnt] = f;
                }
                ++cnt;
            }
        }
        return cnt;
    }

    /// Verify stored CRC-32C against a caller-supplied payload span or expected digest.
    [[nodiscard]] Status verify_crc32c(uint32_t file_id,
                                       std::span<const std::byte> data) const noexcept {
        const auto* f = find_file(file_id);
        if (f == nullptr) return Status::InvalidArg;
        const uint32_t computed = Crc32c::compute(data);
        return (computed == f->crc32c) ? Status::Ok : Status::CrcMismatch;
    }

    /// Authenticate a staged partial PDI / bitstream file before SlotManager can load it.
    [[nodiscard]] Status authenticate_dfx_stage(uint32_t file_id, uint32_t expected_crc) noexcept {
        auto* f = find_file(file_id);
        if (f == nullptr) return Status::InvalidArg;
        const auto* vol = find_volume(f->volume_id);
        if (vol == nullptr || !vol->allow_dfx_staging) return Status::PermissionDenied;
        if (f->crc32c != expected_crc) return Status::CrcMismatch;
        f->verified_dfx_image = true;
        return Status::Ok;
    }

    [[nodiscard]] bool is_verified_dfx_image(uint32_t file_id) const noexcept {
        const auto* f = find_file(file_id);
        return (f != nullptr) && f->verified_dfx_image;
    }

    [[nodiscard]] Expected<uint64_t, Status> volume_used_bytes(uint8_t volume_id) const noexcept {
        const auto* vol = find_volume(volume_id);
        if (vol == nullptr) return Unexpected(Status::InvalidArg);
        return vol->used_bytes;
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

    FileEntry* find_file(uint32_t fid) noexcept {
        for (auto& f : files_) {
            if (f.active && f.file_id == fid) return &f;
        }
        return nullptr;
    }

    const FileEntry* find_file(uint32_t fid) const noexcept {
        for (const auto& f : files_) {
            if (f.active && f.file_id == fid) return &f;
        }
        return nullptr;
    }

    std::array<StorageVolume, MaxVolumes> volumes_{};
    std::array<FileEntry, MaxFiles>       files_{};
    uint32_t                              next_file_id_{1U};
};

/// 64-bit CCSDS CFDP (CCSDS 727.0-B-5) Byte Extent.
struct ByteExtent {
    uint64_t start{0U}; // Inclusive
    uint64_t end{0U};   // Exclusive
};

/// 64-bit CCSDS CFDP Class 1 & Class 2 Receiver Transaction Engine (STRATA-STOR-005).
template <size_t MaxExtents = 32U>
class CfdpRxTransaction {
public:
    void start(uint64_t transaction_id, uint64_t expected_file_size, bool class2_reliable) noexcept {
        tx_id_        = transaction_id;
        expected_len_ = expected_file_size;
        class2_       = class2_reliable;
        extent_cnt_   = 0U;
        eof_received_ = false;
        suspended_    = false;
        active_       = true;
    }

    void suspend() noexcept { suspended_ = true; }
    void resume() noexcept { suspended_ = false; }
    [[nodiscard]] bool is_suspended() const noexcept { return suspended_; }

    /// Record arrival of a 64-bit File Data PDU [offset, offset + len), coalescing contiguous spans.
    [[nodiscard]] Status on_file_data(uint64_t offset, uint64_t len) noexcept {
        if (!active_ || suspended_ || len == 0U || offset > expected_len_ ||
            offset + len > expected_len_) {
            return Status::InvalidArg;
        }
        const uint64_t new_start = offset;
        const uint64_t new_end   = offset + len;

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
    bool                               suspended_{false};
    bool                               active_{false};
};

/// 64-bit CCSDS CFDP Class 1 & Class 2 Transmitter Transaction Engine (STRATA-STOR-005).
/// Supports sequential segmented transmission, NAK-driven selective retransmission,
/// and suspend/resume checkpointing across ground contact passes.
struct CfdpTxCheckpoint {
    uint64_t transaction_id{0U};
    uint64_t total_bytes{0U};
    uint64_t cursor_offset{0U};
    bool     eof_sent{false};
    bool     class2_reliable{false};
};

template <size_t MaxRetransmitExtents = 32U>
class CfdpTxTransaction {
public:
    struct TxSegment {
        uint64_t offset{0U};
        uint64_t length{0U};
        bool     is_retransmit{false};
        bool     is_eof{false};
    };

    void start(uint64_t transaction_id, uint64_t total_bytes, uint64_t max_segment_bytes,
               bool class2_reliable) noexcept {
        tx_id_        = transaction_id;
        total_bytes_  = total_bytes;
        max_seg_      = (max_segment_bytes > 0U) ? max_segment_bytes : 1024U;
        cursor_       = 0U;
        retx_cnt_     = 0U;
        eof_sent_     = false;
        finished_ack_ = false;
        suspended_    = false;
        class2_       = class2_reliable;
        active_       = true;
    }

    /// Resume a previously checkpointed TX transaction after contact pass loss or warm restart.
    void restore(const CfdpTxCheckpoint& cp, uint64_t max_segment_bytes) noexcept {
        tx_id_        = cp.transaction_id;
        total_bytes_  = cp.total_bytes;
        max_seg_      = (max_segment_bytes > 0U) ? max_segment_bytes : 1024U;
        cursor_       = cp.cursor_offset;
        retx_cnt_     = 0U;
        eof_sent_     = cp.eof_sent;
        finished_ack_ = false;
        suspended_    = false;
        class2_       = cp.class2_reliable;
        active_       = true;
    }

    [[nodiscard]] CfdpTxCheckpoint checkpoint() const noexcept {
        return CfdpTxCheckpoint{
            .transaction_id  = tx_id_,
            .total_bytes     = total_bytes_,
            .cursor_offset   = cursor_,
            .eof_sent        = eof_sent_,
            .class2_reliable = class2_,
        };
    }

    void suspend() noexcept { suspended_ = true; }
    void resume() noexcept { suspended_ = false; }

    /// Queue missing extents reported by a remote Class 2 NAK PDU for selective retransmission.
    [[nodiscard]] Status on_nak(std::span<const ByteExtent> missing_extents) noexcept {
        if (!active_ || !class2_) return Status::InvalidArg;
        for (const auto& ext : missing_extents) {
            if (ext.end <= ext.start || ext.end > total_bytes_) return Status::InvalidArg;
            if (retx_cnt_ >= MaxRetransmitExtents) return Status::Full;
            retx_queue_[retx_cnt_++] = ext;
        }
        return Status::Ok;
    }

    /// Mark Class 2 Finished PDU acknowledged by receiver.
    void on_finished_ack() noexcept { finished_ack_ = true; }

    /// Produce the next outbound segment (prioritizing NAK retransmissions over fresh extents).
    [[nodiscard]] Expected<TxSegment, Status> next_segment() noexcept {
        if (!active_ || suspended_) return Unexpected(Status::PreconditionFailed);

        // 1. Prioritize Class 2 selective retransmission extents
        if (retx_cnt_ > 0U) {
            auto&          front = retx_queue_[0];
            const uint64_t avail = front.end - front.start;
            const uint64_t len   = (avail > max_seg_) ? max_seg_ : avail;
            const uint64_t off   = front.start;
            front.start += len;
            if (front.start >= front.end) {
                for (size_t i = 1U; i < retx_cnt_; ++i) {
                    retx_queue_[i - 1U] = retx_queue_[i];
                }
                --retx_cnt_;
            }
            return TxSegment{.offset = off, .length = len, .is_retransmit = true, .is_eof = false};
        }

        // 2. Fresh sequential data segments
        if (cursor_ < total_bytes_) {
            const uint64_t remain = total_bytes_ - cursor_;
            const uint64_t len    = (remain > max_seg_) ? max_seg_ : remain;
            const uint64_t off    = cursor_;
            cursor_ += len;
            return TxSegment{.offset = off, .length = len, .is_retransmit = false, .is_eof = false};
        }

        // 3. Emit EOF PDU once all bytes have been sent
        if (!eof_sent_) {
            eof_sent_ = true;
            return TxSegment{.offset = total_bytes_, .length = 0U, .is_retransmit = false, .is_eof = true};
        }

        return Unexpected(Status::Empty);
    }

    [[nodiscard]] bool is_complete() const noexcept {
        if (!active_ || !eof_sent_ || retx_cnt_ > 0U) return false;
        return class2_ ? finished_ack_ : true;
    }

private:
    std::array<ByteExtent, MaxRetransmitExtents> retx_queue_{};
    size_t                                       retx_cnt_{0U};
    uint64_t                                     tx_id_{0U};
    uint64_t                                     total_bytes_{0U};
    uint64_t                                     max_seg_{1024U};
    uint64_t                                     cursor_{0U};
    bool                                         eof_sent_{false};
    bool                                         finished_ack_{false};
    bool                                         suspended_{false};
    bool                                         class2_{false};
    bool                                         active_{false};
};

/// ============================================================================
/// Lightweight Fast File Chunk Uplink & Downlink Protocol (STRATA-STOR-006)
/// Minimal zero-heap chunked file transfer with CRC-32C integrity and inter-file
/// link cooldown throttling for rapid FlatSat benchtop testing & high-SNR links.
/// ============================================================================
struct FastChunkHeader {
    static constexpr uint32_t kMagic = 0x5354'4643U; // "STFC"

    uint32_t magic{kMagic};
    uint32_t file_id{0U};
    uint32_t chunk_index{0U};
    uint32_t total_chunks{0U};
    uint32_t payload_bytes{0U};
    uint32_t payload_crc32c{0U};
};

template <size_t MaxChunkBytes = 512U>
class FastFileChunker {
public:
    explicit FastFileChunker(DurationNs inter_file_cooldown = DurationNs{5'000'000ULL}) noexcept
        : cooldown_(inter_file_cooldown) {}

    /// Begin a new file downlink/uplink session, enforcing inter-file cooldown throttling.
    [[nodiscard]] Status begin_file(uint32_t file_id, uint32_t total_chunks,
                                    TaiTimestamp now) noexcept {
        if (total_chunks == 0U) return Status::InvalidArg;
        if (has_completed_file_ && elapsed_since(now, last_file_done_tai_) < cooldown_) {
            return Status::Busy;
        }
        active_       = true;
        file_id_      = file_id;
        total_chunks_ = total_chunks;
        next_chunk_   = 0U;
        return Status::Ok;
    }

    /// Encode one chunk into `out_wire` ([FastChunkHeader][payload]).
    [[nodiscard]] Expected<size_t, Status>
    encode_chunk(std::span<const std::byte> payload, std::span<std::byte> out_wire,
                 TaiTimestamp now) noexcept {
        if (!active_ || next_chunk_ >= total_chunks_) return Unexpected(Status::PreconditionFailed);
        if (payload.size() > MaxChunkBytes ||
            out_wire.size() < sizeof(FastChunkHeader) + payload.size()) {
            return Unexpected(Status::InvalidArg);
        }
        FastChunkHeader hdr{};
        hdr.file_id        = file_id_;
        hdr.chunk_index    = next_chunk_++;
        hdr.total_chunks   = total_chunks_;
        hdr.payload_bytes  = static_cast<uint32_t>(payload.size());
        hdr.payload_crc32c = Crc32c::compute(payload);

        std::memcpy(out_wire.data(), &hdr, sizeof(hdr));
        if (!payload.empty()) {
            std::memcpy(out_wire.data() + sizeof(hdr), payload.data(), payload.size());
        }
        if (next_chunk_ == total_chunks_) {
            active_             = false;
            has_completed_file_ = true;
            last_file_done_tai_ = now;
        }
        return sizeof(hdr) + payload.size();
    }

    struct DecodedChunk {
        FastChunkHeader            header{};
        std::span<const std::byte> payload{};
    };

    /// Decode and CRC-verify an incoming fast chunk wire buffer.
    [[nodiscard]] static Expected<DecodedChunk, Status>
    decode_chunk(std::span<const std::byte> wire) noexcept {
        if (wire.size() < sizeof(FastChunkHeader)) {
            return Unexpected(Status::PayloadSizeError);
        }
        FastChunkHeader hdr{};
        std::memcpy(&hdr, wire.data(), sizeof(hdr));
        if (hdr.magic != FastChunkHeader::kMagic) {
            return Unexpected(Status::ValidationFailed);
        }
        if (hdr.payload_bytes > MaxChunkBytes ||
            wire.size() < sizeof(FastChunkHeader) + hdr.payload_bytes) {
            return Unexpected(Status::PayloadSizeError);
        }
        const auto payload = wire.subspan(sizeof(FastChunkHeader), hdr.payload_bytes);
        if (Crc32c::compute(payload) != hdr.payload_crc32c) {
            return Unexpected(Status::CrcMismatch);
        }
        return DecodedChunk{.header = hdr, .payload = payload};
    }

    [[nodiscard]] bool active() const noexcept { return active_; }

private:
    DurationNs   cooldown_{DurationNs{5'000'000ULL}};
    TaiTimestamp last_file_done_tai_{};
    bool         has_completed_file_{false};
    bool         active_{false};
    uint32_t     file_id_{0U};
    uint32_t     total_chunks_{0U};
    uint32_t     next_chunk_{0U};
};

} // namespace strata::services::storage
