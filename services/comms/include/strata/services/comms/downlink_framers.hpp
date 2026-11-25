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

/// ============================================================================
/// CCSDS TM Transfer Frame Framer (CCSDS 132.0-B-3)
/// 6-byte Primary Header + Fixed Data Field + Optional 2-byte CRC-16 FECF.
/// ============================================================================
class TmTransferFramer {
public:
    static constexpr uint16_t kFirstHdrNone = 0x07FFU;
    static constexpr uint16_t kFirstHdrIdle = 0x07FEU;

    explicit TmTransferFramer(uint16_t scid, uint8_t vcid, bool emit_fecf = true,
                              bool hw_fecf_offload = false) noexcept
        : scid_(static_cast<uint16_t>(scid & 0x03FFU)),
          vcid_(static_cast<uint8_t>(vcid & 0x07U)),
          emit_fecf_(emit_fecf),
          hw_fecf_offload_(hw_fecf_offload) {}

    [[nodiscard]] Expected<size_t, WireErr>
    frame(std::span<std::byte> out, std::span<const std::byte> data_field,
          uint16_t first_hdr_ptr = 0U, bool ocf_present = false) noexcept {
        const size_t fecf_len = emit_fecf_ ? kFecfBytes : 0U;
        const size_t total    = kTmFrameHeaderBytes + data_field.size() + fecf_len;
        if (out.size() < total) {
            return Unexpected(WireErr::BufferTooSmall);
        }

        // Byte 0..1: Version(2b=0) | SCID(10b) | VCID(3b) | OCF(1b)
        const uint16_t w0 = static_cast<uint16_t>((static_cast<uint32_t>(scid_ & 0x03FFU) << 4U) |
                                                  (static_cast<uint32_t>(vcid_ & 0x07U) << 1U) |
                                                  (ocf_present ? 1U : 0U));
        write_be16(out.subspan<0, 2>(), w0);
        out[2] = static_cast<std::byte>(mcfc_++);
        out[3] = static_cast<std::byte>(vcfc_++);
        // Byte 4..5: SecHdr(1b=0) | Sync(1b=0) | PktOrder(1b=0) | SegLenId(2b=3) | FHP(11b)
        const uint16_t df_status =
            static_cast<uint16_t>(0x1800U | (first_hdr_ptr & 0x07FFU));
        write_be16(out.subspan<4, 2>(), df_status);

        if (!data_field.empty()) {
            std::memcpy(out.data() + kTmFrameHeaderBytes, data_field.data(), data_field.size());
        }
        if (emit_fecf_) {
            const size_t   body_len = kTmFrameHeaderBytes + data_field.size();
            const uint16_t fecf     = hw_fecf_offload_ ? 0U : crc16_ccitt(out.subspan(0U, body_len));
            write_be16(out.subspan(body_len).subspan<0, 2>(), fecf);
        }
        return total;
    }

    [[nodiscard]] uint8_t mcfc() const noexcept { return mcfc_; }
    [[nodiscard]] uint8_t vcfc() const noexcept { return vcfc_; }

private:
    uint16_t scid_{0U};
    uint8_t  vcid_{0U};
    uint8_t  mcfc_{0U};
    uint8_t  vcfc_{0U};
    bool     emit_fecf_{true};
    bool     hw_fecf_offload_{false};
};

/// ============================================================================
/// CCSDS AOS Space Data Link Protocol Framer (CCSDS 732.0-B-4)
/// 6-byte Primary Header (24-bit VC frame counter) + 2-byte M_PDU Header + Data + FECF.
/// ============================================================================
class AosTransferFramer {
public:
    static constexpr size_t kAosPrimaryHdrBytes = 6U;
    static constexpr size_t kMpduHdrBytes       = 2U;

    explicit AosTransferFramer(uint8_t scid, uint8_t vcid, bool emit_fecf = true,
                               bool hw_fecf_offload = false) noexcept
        : scid_(scid),
          vcid_(static_cast<uint8_t>(vcid & 0x3FU)),
          emit_fecf_(emit_fecf),
          hw_fecf_offload_(hw_fecf_offload) {}

    [[nodiscard]] Expected<size_t, WireErr>
    frame(std::span<std::byte> out, std::span<const std::byte> mpdu_data,
          uint16_t first_hdr_ptr = 0U, bool replay_flag = false) noexcept {
        const size_t fecf_len = emit_fecf_ ? kFecfBytes : 0U;
        const size_t total    = kAosPrimaryHdrBytes + kMpduHdrBytes + mpdu_data.size() + fecf_len;
        if (out.size() < total) {
            return Unexpected(WireErr::BufferTooSmall);
        }

        // Byte 0..1: Version(2b=01b -> 0x4000) | SCID(8b) | VCID(6b)
        const uint16_t w0 = static_cast<uint16_t>(0x4000U |
                                                  (static_cast<uint32_t>(scid_) << 6U) |
                                                  static_cast<uint32_t>(vcid_ & 0x3FU));
        write_be16(out.subspan<0, 2>(), w0);
        const uint32_t vc = vcfc_ & 0x00FF'FFFFU;
        vcfc_             = (vcfc_ + 1U) & 0x00FF'FFFFU;
        out[2]            = static_cast<std::byte>((vc >> 16U) & 0xFFU);
        out[3]            = static_cast<std::byte>((vc >> 8U) & 0xFFU);
        out[4]            = static_cast<std::byte>(vc & 0xFFU);
        // Signaling field: Replay(1b) | VCFC_Cycle_Use(1b=0) | Reserved(2b) | Cycle(4b)
        out[5] = static_cast<std::byte>(replay_flag ? 0x80U : 0x00U);

        // M_PDU Header: Spare(5b=0) | First Header Pointer(11b)
        write_be16(out.subspan<6, 2>(), static_cast<uint16_t>(first_hdr_ptr & 0x07FFU));

        if (!mpdu_data.empty()) {
            std::memcpy(out.data() + kAosPrimaryHdrBytes + kMpduHdrBytes, mpdu_data.data(),
                        mpdu_data.size());
        }
        if (emit_fecf_) {
            const size_t   body_len = kAosPrimaryHdrBytes + kMpduHdrBytes + mpdu_data.size();
            const uint16_t fecf     = hw_fecf_offload_ ? 0U : crc16_ccitt(out.subspan(0U, body_len));
            write_be16(out.subspan(body_len).subspan<0, 2>(), fecf);
        }
        return total;
    }

    [[nodiscard]] uint32_t vcfc() const noexcept { return vcfc_; }

private:
    uint8_t  scid_{0U};
    uint8_t  vcid_{0U};
    uint32_t vcfc_{0U};
    bool     emit_fecf_{true};
    bool     hw_fecf_offload_{false};
};

/// ============================================================================
/// CCSDS Unified Space Data Link Protocol (USLP) Framer (CCSDS 732.1-B-2)
/// 7-byte Primary Header (with 2-byte VCF counter) + 3-byte TFDZ Header + Payload + FECF.
/// ============================================================================
class UslpTransferFramer {
public:
    static constexpr size_t kPrimaryHdrBaseBytes = 7U;
    static constexpr size_t kVcfCountBytes       = 2U;
    static constexpr size_t kTfdzHdrBytes        = 3U; // Construction rules(3b)+USLP PID(5b)+FHP(16b)

    explicit UslpTransferFramer(uint16_t scid, uint8_t vcid, uint8_t map_id = 0U,
                                bool emit_fecf = true, bool hw_fecf_offload = false) noexcept
        : scid_(scid),
          vcid_(static_cast<uint8_t>(vcid & 0x3FU)),
          map_id_(static_cast<uint8_t>(map_id & 0x0FU)),
          emit_fecf_(emit_fecf),
          hw_fecf_offload_(hw_fecf_offload) {}

    [[nodiscard]] Expected<size_t, WireErr>
    frame(std::span<std::byte> out, std::span<const std::byte> tfdz_data,
          uint16_t first_hdr_ptr = 0U, uint8_t uslp_pid = 0U, bool bypass = false) noexcept {
        const size_t fecf_len = emit_fecf_ ? kFecfBytes : 0U;
        const size_t hdr_len  = kPrimaryHdrBaseBytes + kVcfCountBytes + kTfdzHdrBytes;
        const size_t total    = hdr_len + tfdz_data.size() + fecf_len;
        if (total > 65536U || out.size() < total) {
            return Unexpected(WireErr::BufferTooSmall);
        }

        // Byte 0..3: TFVN(4b=1100b=12) | SCID(16b) | SrcDst(1b=0) | VCID(6b) | MAP_ID(4b) | EndPH(1b=0)
        const uint32_t w01 = (static_cast<uint32_t>(0x0CU) << 28U) |
                             (static_cast<uint32_t>(scid_) << 12U) |
                             (static_cast<uint32_t>(vcid_ & 0x3FU) << 5U) |
                             (static_cast<uint32_t>(map_id_ & 0x0FU) << 1U);
        write_be32(out.subspan<0, 4>(), w01);

        // Byte 4..5: Frame Length (total - 1)
        write_be16(out.subspan<4, 2>(), static_cast<uint16_t>(total - 1U));

        // Byte 6: Bypass(1b) | ProtCtrl(1b=0) | Spare(2b=0) | OCF(1b=0) | VCF_Count_Len(3b=2)
        out[6] = static_cast<std::byte>((bypass ? 0x80U : 0x00U) | (kVcfCountBytes & 0x07U));

        // Byte 7..8: 16-bit VCF Counter
        write_be16(out.subspan<7, 2>(), vcfc_++);

        // Byte 9..11: TFDZ Header (Rule=000b fixed-length packets | PID(5b), 16-bit FHP)
        out[9] = static_cast<std::byte>(uslp_pid & 0x1FU);
        write_be16(out.subspan<10, 2>(), first_hdr_ptr);

        if (!tfdz_data.empty()) {
            std::memcpy(out.data() + hdr_len, tfdz_data.data(), tfdz_data.size());
        }
        if (emit_fecf_) {
            const size_t   body_len = hdr_len + tfdz_data.size();
            const uint16_t fecf     = hw_fecf_offload_ ? 0U : crc16_ccitt(out.subspan(0U, body_len));
            write_be16(out.subspan(body_len).subspan<0, 2>(), fecf);
        }
        return total;
    }

    [[nodiscard]] uint16_t vcfc() const noexcept { return vcfc_; }

private:
    uint16_t scid_{0U};
    uint8_t  vcid_{0U};
    uint8_t  map_id_{0U};
    uint16_t vcfc_{0U};
    bool     emit_fecf_{true};
    bool     hw_fecf_offload_{false};
};

/// ============================================================================
/// Bounded Zero-Heap Byte-Stream Frame Accumulator (STRATA-COM-005)
/// Searches for CCSDS Attached Sync Marker (default 0x1ACFFC1D), reads frame
/// header to determine length (or uses fixed frame size), and performs bounded
/// resynchronization on corrupted headers without dropping subsequent frames.
/// ============================================================================
template <size_t MaxFrameBytes = 1024U, size_t SyncLen = 4U>
class FrameAccumulator {
public:
    static_assert(MaxFrameBytes >= 16U && SyncLen >= 2U && SyncLen <= 8U);

    using LengthExtractorFn = size_t (*)(std::span<const std::byte> header) noexcept;

    /// Default CCSDS 32-bit Attached Sync Marker (0x1A CF FC 1D).
    static constexpr std::array<std::byte, 4U> kDefaultCcsdsAsm = {
        std::byte{0x1AU}, std::byte{0xCFU}, std::byte{0xFCU}, std::byte{0x1DU}};

    /// Built-in length extractor for CCSDS TC Transfer Frames (5-byte header).
    static size_t tc_frame_length_extractor(std::span<const std::byte> hdr) noexcept {
        if (hdr.size() < 4U) return 0U;
        const uint16_t w0 = read_be16(hdr.subspan<0, 2>());
        if (((w0 >> 14U) & 0x03U) != 0U) return 0U; // Invalid version -> trigger resync
        const uint16_t w1 = read_be16(hdr.subspan<2, 2>());
        const size_t   len = static_cast<size_t>(w1 & 0x03FFU) + 1U;
        return (len >= kTcFrameHeaderBytes + kFecfBytes && len <= MaxFrameBytes) ? len : 0U;
    }

    explicit FrameAccumulator(std::array<std::byte, SyncLen> sync_marker, size_t header_bytes,
                              LengthExtractorFn length_fn) noexcept
        : sync_marker_(sync_marker),
          header_bytes_(header_bytes),
          fixed_frame_len_(0U),
          length_fn_(length_fn) {}

    /// Fixed-length transfer frame constructor (e.g. fixed TM/AOS frames).
    explicit FrameAccumulator(std::array<std::byte, SyncLen> sync_marker,
                              size_t fixed_frame_len) noexcept
        : sync_marker_(sync_marker),
          header_bytes_(fixed_frame_len),
          fixed_frame_len_(fixed_frame_len),
          length_fn_(nullptr) {}

    void reset() noexcept {
        state_         = State::SearchingSync;
        sync_matched_  = 0U;
        collected_     = 0U;
        expected_len_  = 0U;
    }

    /// Feed raw bytes from UART/SpaceWire/RF modem; invokes `on_frame(span<const byte>)` for each
    /// complete extracted frame. Performs bounded resync if header declares invalid length.
    template <typename OnFrameFn>
    size_t ingest(std::span<const std::byte> bytes, OnFrameFn&& on_frame) noexcept {
        size_t frames_emitted = 0U;
        for (const std::byte b : bytes) {
            if (ingest_byte(b, on_frame)) {
                ++frames_emitted;
            }
        }
        return frames_emitted;
    }

    [[nodiscard]] uint32_t resync_count() const noexcept { return resync_count_; }
    [[nodiscard]] uint32_t frames_extracted() const noexcept { return frames_extracted_; }

private:
    enum class State : uint8_t {
        SearchingSync = 0U,
        Collecting    = 1U,
    };

    void feed_sync_byte(std::byte b) noexcept {
        if (b == sync_marker_[sync_matched_]) {
            ++sync_matched_;
            if (sync_matched_ == SyncLen) {
                state_        = State::Collecting;
                collected_    = 0U;
                expected_len_ = fixed_frame_len_;
                sync_matched_ = 0U;
            }
            return;
        }
        // Bounded overlap check so partial prefix matches resync immediately
        sync_matched_ = (b == sync_marker_[0]) ? 1U : 0U;
    }

    template <typename OnFrameFn>
    bool ingest_byte(std::byte b, OnFrameFn&& on_frame) noexcept {
        if (state_ == State::SearchingSync) {
            feed_sync_byte(b);
            return false;
        }

        buf_[collected_++] = b;

        if (expected_len_ == 0U && collected_ == header_bytes_) {
            const size_t len =
                (length_fn_ != nullptr)
                    ? length_fn_(std::span<const std::byte>{buf_.data(), collected_})
                    : 0U;
            if (len < header_bytes_ || len > MaxFrameBytes) {
                // Corrupted header: rescan buffered header bytes for an embedded ASM
                ++resync_count_;
                const size_t replay_n = collected_;
                state_                = State::SearchingSync;
                sync_matched_         = 0U;
                collected_            = 0U;
                expected_len_         = 0U;
                for (size_t i = 0U; i < replay_n; ++i) {
                    const std::byte rb = buf_[i];
                    if (state_ == State::SearchingSync) {
                        feed_sync_byte(rb);
                    } else if (collected_ < MaxFrameBytes) {
                        buf_[collected_++] = rb;
                    }
                }
                return false;
            }
            expected_len_ = len;
        }

        if (expected_len_ > 0U && collected_ == expected_len_) {
            const auto frame_span = std::span<const std::byte>{buf_.data(), collected_};
            ++frames_extracted_;
            state_        = State::SearchingSync;
            sync_matched_ = 0U;
            collected_    = 0U;
            expected_len_ = 0U;
            on_frame(frame_span);
            return true;
        }

        return false;
    }

    std::array<std::byte, SyncLen>       sync_marker_{};
    std::array<std::byte, MaxFrameBytes> buf_{};
    size_t                               header_bytes_{0U};
    size_t                               fixed_frame_len_{0U};
    LengthExtractorFn                    length_fn_{nullptr};
    State                                state_{State::SearchingSync};
    size_t                               sync_matched_{0U};
    size_t                               collected_{0U};
    size_t                               expected_len_{0U};
    uint32_t                             resync_count_{0U};
    uint32_t                             frames_extracted_{0U};
};

} // namespace strata::services::comms
