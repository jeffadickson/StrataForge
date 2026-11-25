#pragma once

#include <strata/core/expected.hpp>
#include <strata/core/types.hpp>
#include <strata/hw/slot_handle.hpp>
#include <strata/services/storage/data_products.hpp>

#include <cstdint>

namespace strata::services::storage {

/// Hardware PL compression algorithm identifiers (matches DpHeader::compression_alg).
enum class PlCompressionAlg : uint8_t {
    Raw          = 0U,
    Ccsds123B2   = 1U, // Hyperspectral lossless/near-lossless hardware IP
    Lz4Hardware  = 2U,
    ZstdHardware = 3U,
};

/// Pluggable hardware PL compression slot submission backend concept.
template <typename T>
concept PlCompressionBackend = requires(T& b, const FrameDesc& src, PlCompressionAlg alg,
                                        uint64_t dst_phys_addr) {
    { b.compress_dma(src, alg, dst_phys_addr) } noexcept -> std::same_as<Expected<FrameDesc, Status>>;
};

/// ============================================================================
/// Optional Hardware PL Compression Pipeline Stage (STRATA-STOR-003)
/// Routes a 32-byte FrameDesc through a hardware programmable logic compression
/// slot before non-volatile storage commit without touching payload bytes on CPU.
/// ============================================================================
template <PlCompressionBackend Backend>
class PlCompressionStage {
public:
    explicit PlCompressionStage(Backend& backend, PlCompressionAlg alg = PlCompressionAlg::Ccsds123B2) noexcept
        : backend_(&backend), alg_(alg) {}

    void set_algorithm(PlCompressionAlg alg) noexcept { alg_ = alg; }
    [[nodiscard]] PlCompressionAlg algorithm() const noexcept { return alg_; }

    /// Process a source descriptor zero-copy through PL compression hardware and populate a
    /// 64-byte DpHeader stamped with the compressed payload length and algorithm ID.
    [[nodiscard]] Expected<FrameDesc, Status>
    process_frame(const FrameDesc& src_desc, uint64_t dst_phys_addr, DpHeader& out_header) noexcept {
        if (alg_ == PlCompressionAlg::Raw) {
            out_header.compression_alg = static_cast<uint8_t>(PlCompressionAlg::Raw);
            out_header.payload_bytes   = src_desc.length_bytes;
            out_header.payload_crc32c  = src_desc.crc32c;
            ++bypassed_frames_;
            return src_desc;
        }

        auto res = backend_->compress_dma(src_desc, alg_, dst_phys_addr);
        if (!res.has_value()) {
            ++failed_frames_;
            return Unexpected(res.error());
        }

        const auto& out_desc       = res.value();
        out_header.compression_alg = static_cast<uint8_t>(alg_);
        out_header.payload_bytes   = out_desc.length_bytes;
        out_header.payload_crc32c  = out_desc.crc32c;
        raw_bytes_in_ += src_desc.length_bytes;
        compressed_bytes_out_ += out_desc.length_bytes;
        ++compressed_frames_;
        return out_desc;
    }

    [[nodiscard]] uint64_t raw_bytes_in() const noexcept { return raw_bytes_in_; }
    [[nodiscard]] uint64_t compressed_bytes_out() const noexcept { return compressed_bytes_out_; }
    [[nodiscard]] uint32_t compressed_frames() const noexcept { return compressed_frames_; }
    [[nodiscard]] uint32_t bypassed_frames() const noexcept { return bypassed_frames_; }
    [[nodiscard]] uint32_t failed_frames() const noexcept { return failed_frames_; }

private:
    Backend*         backend_;
    PlCompressionAlg alg_{PlCompressionAlg::Ccsds123B2};
    uint64_t         raw_bytes_in_{0U};
    uint64_t         compressed_bytes_out_{0U};
    uint32_t         compressed_frames_{0U};
    uint32_t         bypassed_frames_{0U};
    uint32_t         failed_frames_{0U};
};

} // namespace strata::services::storage
