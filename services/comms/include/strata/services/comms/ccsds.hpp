#pragma once

#include <strata/core/concepts.hpp>
#include <strata/core/crc32c.hpp>
#include <strata/core/expected.hpp>
#include <strata/core/types.hpp>
#include <strata/crypto/aes_gcm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace strata::services::comms {

/// Total-function wire parser errors (zero asserts on untrusted external input).
enum class WireErr : uint16_t {
    TruncatedHeader  = 1U,
    LengthMismatch   = 2U,
    InvalidVersion   = 3U,
    BufferTooSmall   = 4U,
    FecfCrcMismatch  = 5U,
    UnknownSpi       = 6U,
    AuthTagMismatch  = 7U,
    ReplayDetected   = 8U,
    InvalidApid      = 9U,
};

inline constexpr size_t   kSpacePacketHeaderBytes = 6U;
inline constexpr uint16_t kIdleApid               = 0x07FFU;
inline constexpr size_t   kMaxSpacePacketPayload  = 65536U;
inline constexpr size_t   kTcFrameHeaderBytes     = 5U;
inline constexpr size_t   kTmFrameHeaderBytes     = 6U;
inline constexpr size_t   kFecfBytes              = 2U;

/// Big-endian helpers for CCSDS network byte order.
[[nodiscard]] constexpr uint16_t read_be16(std::span<const std::byte, 2U> s) noexcept {
    return static_cast<uint16_t>((static_cast<uint16_t>(std::to_integer<uint8_t>(s[0])) << 8U) |
                                 static_cast<uint16_t>(std::to_integer<uint8_t>(s[1])));
}

constexpr void write_be16(std::span<std::byte, 2U> dst, uint16_t val) noexcept {
    dst[0] = static_cast<std::byte>((val >> 8U) & 0xFFU);
    dst[1] = static_cast<std::byte>(val & 0xFFU);
}

[[nodiscard]] constexpr uint32_t read_be32(std::span<const std::byte, 4U> s) noexcept {
    return (static_cast<uint32_t>(std::to_integer<uint8_t>(s[0])) << 24U) |
           (static_cast<uint32_t>(std::to_integer<uint8_t>(s[1])) << 16U) |
           (static_cast<uint32_t>(std::to_integer<uint8_t>(s[2])) << 8U) |
           static_cast<uint32_t>(std::to_integer<uint8_t>(s[3]));
}

constexpr void write_be32(std::span<std::byte, 4U> dst, uint32_t val) noexcept {
    dst[0] = static_cast<std::byte>((val >> 24U) & 0xFFU);
    dst[1] = static_cast<std::byte>((val >> 16U) & 0xFFU);
    dst[2] = static_cast<std::byte>((val >> 8U) & 0xFFU);
    dst[3] = static_cast<std::byte>(val & 0xFFU);
}

/// CCSDS CRC-16-CCITT (polynomial 0x1021, init 0xFFFF) for Frame Error Control Field (FECF).
[[nodiscard]] constexpr uint16_t crc16_ccitt(std::span<const std::byte> data) noexcept {
    uint16_t crc = 0xFFFFU;
    for (const std::byte b : data) {
        crc ^= static_cast<uint16_t>(static_cast<uint16_t>(std::to_integer<uint8_t>(b)) << 8U);
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            const uint32_t shifted = static_cast<uint32_t>(crc) << 1U;
            crc = ((crc & 0x8000U) != 0U) ? static_cast<uint16_t>(shifted ^ 0x1021U)
                                          : static_cast<uint16_t>(shifted);
        }
    }
    return crc;
}

/// Zero-copy parsed CCSDS Space Packet (CCSDS 133.0-B-2).
struct SpacePacketView {
    uint16_t                   apid{0U};
    uint16_t                   seq_count{0U};
    uint8_t                    seq_flags{3U}; // 3 = unsegmented
    bool                       is_tc{false};
    bool                       has_sec_hdr{false};
    std::span<const std::byte> payload{};
};

/// Total-function Space Packet deframer (never asserts on malformed or truncated frames).
[[nodiscard]] constexpr Expected<SpacePacketView, WireErr>
parse_space_packet(std::span<const std::byte> raw) noexcept {
    if (raw.size() < kSpacePacketHeaderBytes) {
        return Unexpected(WireErr::TruncatedHeader);
    }

    const uint16_t w0 = read_be16(raw.subspan<0, 2>());
    const uint16_t w1 = read_be16(raw.subspan<2, 2>());
    const uint16_t w2 = read_be16(raw.subspan<4, 2>());

    const uint8_t version = static_cast<uint8_t>((w0 >> 13U) & 0x07U);
    if (version != 0U) {
        return Unexpected(WireErr::InvalidVersion);
    }

    const size_t payload_len = static_cast<size_t>(w2) + 1U;
    if (raw.size() < kSpacePacketHeaderBytes + payload_len) {
        return Unexpected(WireErr::LengthMismatch);
    }

    return SpacePacketView{
        .apid        = static_cast<uint16_t>(w0 & 0x07FFU),
        .seq_count   = static_cast<uint16_t>(w1 & 0x3FFFU),
        .seq_flags   = static_cast<uint8_t>((w1 >> 14U) & 0x03U),
        .is_tc       = ((w0 >> 12U) & 0x01U) != 0U,
        .has_sec_hdr = ((w0 >> 11U) & 0x01U) != 0U,
        .payload     = raw.subspan(kSpacePacketHeaderBytes, payload_len),
    };
}

/// Zero-copy Space Packet framer writing directly into caller-supplied span.
[[nodiscard]] inline Expected<size_t, WireErr>
frame_space_packet(std::span<std::byte> out, uint16_t apid, uint16_t seq_count, bool is_tc,
                   bool has_sec_hdr, std::span<const std::byte> payload) noexcept {
    if (apid > 0x07FFU) {
        return Unexpected(WireErr::InvalidApid);
    }
    if (payload.empty() || payload.size() > kMaxSpacePacketPayload) {
        return Unexpected(WireErr::LengthMismatch);
    }
    const size_t total = kSpacePacketHeaderBytes + payload.size();
    if (out.size() < total) {
        return Unexpected(WireErr::BufferTooSmall);
    }

    const uint16_t w0 = static_cast<uint16_t>(
        (is_tc ? 0x1000U : 0x0000U) | (has_sec_hdr ? 0x0800U : 0x0000U) | (apid & 0x07FFU));
    const uint16_t w1 = static_cast<uint16_t>(0xC000U | (seq_count & 0x3FFFU)); // Unsegmented
    const uint16_t w2 = static_cast<uint16_t>(payload.size() - 1U);

    write_be16(out.subspan<0, 2>(), w0);
    write_be16(out.subspan<2, 2>(), w1);
    write_be16(out.subspan<4, 2>(), w2);
    std::memcpy(out.data() + kSpacePacketHeaderBytes, payload.data(), payload.size());
    return total;
}

/// Zero-copy CCSDS TC Transfer Frame (CCSDS 232.0-B-4) with FECF CRC-16 validation.
struct TcFrameView {
    uint16_t                   scid{0U};
    uint8_t                    vcid{0U};
    uint8_t                    frame_seq{0U};
    bool                       bypass{false};
    bool                       control{false};
    std::span<const std::byte> data_field{};
};

[[nodiscard]] constexpr Expected<TcFrameView, WireErr>
parse_tc_frame(std::span<const std::byte> raw) noexcept {
    if (raw.size() < kTcFrameHeaderBytes + kFecfBytes) {
        return Unexpected(WireErr::TruncatedHeader);
    }
    const uint16_t w0 = read_be16(raw.subspan<0, 2>());
    const uint16_t w1 = read_be16(raw.subspan<2, 2>());
    const uint8_t  b4 = std::to_integer<uint8_t>(raw[4]);

    const uint8_t version = static_cast<uint8_t>((w0 >> 14U) & 0x03U);
    if (version != 0U) {
        return Unexpected(WireErr::InvalidVersion);
    }
    const size_t declared_len = static_cast<size_t>(w1 & 0x03FFU) + 1U;
    if (declared_len < kTcFrameHeaderBytes + kFecfBytes || raw.size() < declared_len) {
        return Unexpected(WireErr::LengthMismatch);
    }

    const size_t   body_len      = declared_len - kFecfBytes;
    const uint16_t computed_fecf = crc16_ccitt(raw.subspan(0U, body_len));
    const uint16_t rx_fecf       = read_be16(raw.subspan(body_len).subspan<0, 2>());
    if (computed_fecf != rx_fecf) {
        return Unexpected(WireErr::FecfCrcMismatch);
    }

    return TcFrameView{
        .scid       = static_cast<uint16_t>(w0 & 0x03FFU),
        .vcid       = static_cast<uint8_t>((w1 >> 10U) & 0x3FU),
        .frame_seq  = b4,
        .bypass     = ((w0 >> 13U) & 0x01U) != 0U,
        .control    = ((w0 >> 12U) & 0x01U) != 0U,
        .data_field = raw.subspan(kTcFrameHeaderBytes, body_len - kTcFrameHeaderBytes),
    };
}

[[nodiscard]] inline Expected<size_t, WireErr>
frame_tc_frame(std::span<std::byte> out, uint16_t scid, uint8_t vcid, uint8_t frame_seq,
               bool bypass, bool control, std::span<const std::byte> data_field) noexcept {
    const size_t total = kTcFrameHeaderBytes + data_field.size() + kFecfBytes;
    if (total > 1024U || out.size() < total) {
        return Unexpected(WireErr::BufferTooSmall);
    }
    const uint16_t w0 = static_cast<uint16_t>(
        (bypass ? 0x2000U : 0x0000U) | (control ? 0x1000U : 0x0000U) | (scid & 0x03FFU));
    const uint16_t w1 = static_cast<uint16_t>(
        (static_cast<uint16_t>(vcid & 0x3FU) << 10U) | static_cast<uint16_t>((total - 1U) & 0x03FFU));

    write_be16(out.subspan<0, 2>(), w0);
    write_be16(out.subspan<2, 2>(), w1);
    out[4] = static_cast<std::byte>(frame_seq);
    if (!data_field.empty()) {
        std::memcpy(out.data() + kTcFrameHeaderBytes, data_field.data(), data_field.size());
    }
    const size_t   body_len = kTcFrameHeaderBytes + data_field.size();
    const uint16_t fecf     = crc16_ccitt(out.subspan(0U, body_len));
    write_be16(out.subspan(body_len).subspan<0, 2>(), fecf);
    return total;
}

/// Pluggable SDLS (CCSDS 355.0-B-2) Crypto Backend concept.
/// Bind to Versal PMC AES-GCM hardware or PL crypto slot in flight; use ReferenceAeadBackend in SIL.
template <typename T>
concept SdlsCryptoBackend = requires(const T& c, std::span<const std::byte> key, uint64_t iv,
                                     std::span<const std::byte> aad,
                                     std::span<const std::byte> in, std::span<std::byte> out,
                                     std::span<std::byte, 16U> tag) {
    { c.encrypt_and_tag(key, iv, aad, in, out, tag) } noexcept -> std::same_as<bool>;
    { c.decrypt_and_verify(key, iv, aad, in, out, tag) } noexcept -> std::same_as<bool>;
};

/// Lightweight host-SIL SDLS AEAD backend (deterministic keyed stream + 128-bit auth tag).
/// Replace with hardware AES-256-GCM (Versal PMC / PL IP) for flight builds.
struct SilReferenceCryptoBackend {
    static constexpr uint64_t mix64(uint64_t x) noexcept {
        x += 0x9E37'79B9'7F4A'7C15ULL;
        x = (x ^ (x >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
        x = (x ^ (x >> 27U)) * 0x94D0'49BB'1331'11EBULL;
        return x ^ (x >> 31U);
    }

    static void xor_stream(std::span<const std::byte> key, uint64_t iv,
                           std::span<const std::byte> in, std::span<std::byte> out) noexcept {
        uint64_t k0 = 0U;
        for (size_t i = 0U; i < key.size(); ++i) {
            k0 = mix64(k0 ^ (static_cast<uint64_t>(std::to_integer<uint8_t>(key[i])) << ((i & 7U) * 8U)));
        }
        uint64_t ctr = mix64(k0 ^ iv);
        for (size_t i = 0U; i < in.size(); ++i) {
            if ((i & 7U) == 0U) {
                ctr = mix64(ctr + i + 1U);
            }
            const auto ks = static_cast<uint8_t>((ctr >> ((i & 7U) * 8U)) & 0xFFU);
            out[i]        = static_cast<std::byte>(std::to_integer<uint8_t>(in[i]) ^ ks);
        }
    }

    static void compute_tag(std::span<const std::byte> key, uint64_t iv,
                            std::span<const std::byte> aad, std::span<const std::byte> ct,
                            std::span<std::byte, 16U> tag) noexcept {
        uint32_t c0 = Crc32c::compute(key);
        c0          = Crc32c::update(c0, std::as_bytes(std::span<const uint64_t, 1>{&iv, 1U}));
        c0          = Crc32c::update(c0, aad);
        c0          = Crc32c::update(c0, ct);
        const uint64_t h0 = mix64(static_cast<uint64_t>(c0) ^ iv);
        const uint64_t h1 = mix64(h0 ^ static_cast<uint64_t>(ct.size()));
        std::memcpy(tag.data(), &h0, 8U);
        std::memcpy(tag.data() + 8U, &h1, 8U);
    }

    [[nodiscard]] bool encrypt_and_tag(std::span<const std::byte> key, uint64_t iv,
                                       std::span<const std::byte> aad,
                                       std::span<const std::byte> pt, std::span<std::byte> ct,
                                       std::span<std::byte, 16U> tag) const noexcept {
        if (ct.size() < pt.size()) return false;
        xor_stream(key, iv, pt, ct);
        compute_tag(key, iv, aad, ct.subspan(0U, pt.size()), tag);
        return true;
    }

    [[nodiscard]] bool decrypt_and_verify(std::span<const std::byte> key, uint64_t iv,
                                          std::span<const std::byte> aad,
                                          std::span<const std::byte> ct, std::span<std::byte> pt,
                                          std::span<std::byte, 16U> rx_tag) const noexcept {
        if (pt.size() < ct.size()) return false;
        std::array<std::byte, 16U> expected{};
        compute_tag(key, iv, aad, ct, expected);
        // Constant-time tag comparison
        uint8_t diff = 0U;
        for (size_t i = 0U; i < 16U; ++i) {
            diff |= static_cast<uint8_t>(std::to_integer<uint8_t>(expected[i]) ^
                                         std::to_integer<uint8_t>(rx_tag[i]));
        }
        if (diff != 0U) return false;
        xor_stream(key, iv, ct, pt);
        return true;
    }
};
static_assert(SdlsCryptoBackend<SilReferenceCryptoBackend>);

/// Flight-grade NIST SP 800-38D AES-256-GCM SDLS AEAD backend (zero heap, constant-time tag verify).
/// Forms the 96-bit GCM IV as [salt_be32 (4B) || seq_num_be64 (8B)] per CCSDS 355.0-B-2.
struct Aes256GcmSdlsBackend {
    uint32_t iv_salt{0x5354'5241U}; // "STRA" mission salt

    [[nodiscard]] std::array<std::byte, 12U> make_iv(uint64_t seq) const noexcept {
        std::array<std::byte, 12U> iv{};
        write_be32(std::span<std::byte, 4U>{iv.data(), 4U}, iv_salt);
        write_be32(std::span<std::byte, 4U>{iv.data() + 4U, 4U}, static_cast<uint32_t>(seq >> 32U));
        write_be32(std::span<std::byte, 4U>{iv.data() + 8U, 4U}, static_cast<uint32_t>(seq & 0xFFFF'FFFFULL));
        return iv;
    }

    [[nodiscard]] bool encrypt_and_tag(std::span<const std::byte> key, uint64_t iv_seq,
                                       std::span<const std::byte> aad,
                                       std::span<const std::byte> pt, std::span<std::byte> ct,
                                       std::span<std::byte, 16U> tag) const noexcept {
        if (key.size() != strata::crypto::Aes256Gcm::kKeyBytes) return false;
        const auto iv = make_iv(iv_seq);
        return strata::crypto::Aes256Gcm::encrypt_and_tag(
            std::span<const std::byte, 32U>{key.data(), 32U}, iv, aad, pt, ct, tag);
    }

    [[nodiscard]] bool decrypt_and_verify(std::span<const std::byte> key, uint64_t iv_seq,
                                          std::span<const std::byte> aad,
                                          std::span<const std::byte> ct, std::span<std::byte> pt,
                                          std::span<std::byte, 16U> rx_tag) const noexcept {
        if (key.size() != strata::crypto::Aes256Gcm::kKeyBytes) return false;
        const auto iv = make_iv(iv_seq);
        return strata::crypto::Aes256Gcm::decrypt_and_verify(
            std::span<const std::byte, 32U>{key.data(), 32U}, iv, aad, ct, pt, rx_tag);
    }
};
static_assert(SdlsCryptoBackend<Aes256GcmSdlsBackend>);

/// CCSDS Space Data Link Security (SDLS) Engine with 64-sequence sliding anti-replay window.
/// Wire layout: [SPI: 2B][SeqNum: 8B][Ciphertext: N][AuthTag: 16B].
template <SdlsCryptoBackend Backend = SilReferenceCryptoBackend, size_t MaxSAs = 8U>
class SdlsEngine {
public:
    static constexpr size_t kSecHdrBytes = 10U; // 2B SPI + 8B Anti-Replay Sequence
    static constexpr size_t kTagBytes    = 16U;

    explicit SdlsEngine(Backend backend = {}) noexcept : backend_(backend) {}

    [[nodiscard]] Status install_sa(uint16_t spi, std::span<const std::byte, 32U> key) noexcept {
        for (auto& sa : sas_) {
            if (!sa.active || sa.spi == spi) {
                sa.active = true;
                sa.spi    = spi;
                std::memcpy(sa.key.data(), key.data(), 32U);
                sa.tx_seq        = 1U;
                sa.rx_highest    = 0U;
                sa.replay_bitmap = 0U;
                return Status::Ok;
            }
        }
        return Status::Full;
    }

    [[nodiscard]] Expected<size_t, WireErr>
    protect(uint16_t spi, std::span<const std::byte> aad, std::span<const std::byte> plaintext,
            std::span<std::byte> out) noexcept {
        auto* sa = find_sa(spi);
        if (sa == nullptr) return Unexpected(WireErr::UnknownSpi);

        const size_t total = kSecHdrBytes + plaintext.size() + kTagBytes;
        if (out.size() < total) return Unexpected(WireErr::BufferTooSmall);

        const uint64_t seq = sa->tx_seq++;
        write_be16(out.subspan<0, 2>(), spi);
        write_be32(out.subspan<2, 4>(), static_cast<uint32_t>(seq >> 32U));
        write_be32(out.subspan<6, 4>(), static_cast<uint32_t>(seq & 0xFFFF'FFFFULL));

        auto ct_span  = out.subspan(kSecHdrBytes, plaintext.size());
        auto tag_span = out.subspan(kSecHdrBytes + plaintext.size()).template subspan<0, 16U>();
        if (!backend_.encrypt_and_tag(sa->key, seq, aad, plaintext, ct_span, tag_span)) {
            return Unexpected(WireErr::AuthTagMismatch);
        }
        return total;
    }

    [[nodiscard]] Expected<size_t, WireErr>
    unprotect(std::span<const std::byte> aad, std::span<const std::byte> protected_frame,
              std::span<std::byte> out_plaintext) noexcept {
        if (protected_frame.size() < kSecHdrBytes + kTagBytes) {
            return Unexpected(WireErr::TruncatedHeader);
        }
        const uint16_t spi = read_be16(protected_frame.subspan<0, 2>());
        const uint64_t seq =
            (static_cast<uint64_t>(read_be32(protected_frame.subspan<2, 4>())) << 32U) |
            static_cast<uint64_t>(read_be32(protected_frame.subspan<6, 4>()));

        auto* sa = find_sa(spi);
        if (sa == nullptr) return Unexpected(WireErr::UnknownSpi);
        if (!check_replay(*sa, seq)) return Unexpected(WireErr::ReplayDetected);

        const size_t ct_len = protected_frame.size() - kSecHdrBytes - kTagBytes;
        if (out_plaintext.size() < ct_len) return Unexpected(WireErr::BufferTooSmall);

        auto ct_span  = protected_frame.subspan(kSecHdrBytes, ct_len);
        auto tag_copy = std::array<std::byte, 16U>{};
        std::memcpy(tag_copy.data(), protected_frame.data() + kSecHdrBytes + ct_len, kTagBytes);

        if (!backend_.decrypt_and_verify(sa->key, seq, aad, ct_span, out_plaintext, tag_copy)) {
            return Unexpected(WireErr::AuthTagMismatch);
        }

        // Only update anti-replay window AFTER cryptographic authentication succeeds!
        commit_replay(*sa, seq);
        return ct_len;
    }

private:
    struct SecurityAssociation {
        bool                        active{false};
        uint16_t                    spi{0U};
        std::array<std::byte, 32U>  key{};
        uint64_t                    tx_seq{1U};
        uint64_t                    rx_highest{0U};
        uint64_t                    replay_bitmap{0U};
    };

    SecurityAssociation* find_sa(uint16_t spi) noexcept {
        for (auto& sa : sas_) {
            if (sa.active && sa.spi == spi) return &sa;
        }
        return nullptr;
    }

    static bool check_replay(const SecurityAssociation& sa, uint64_t seq) noexcept {
        if (seq == 0U) return false;
        if (seq > sa.rx_highest) return true;
        const uint64_t delta = sa.rx_highest - seq;
        if (delta >= 64U) return false; // Too old (outside 64-frame window)
        return (sa.replay_bitmap & (1ULL << delta)) == 0U;
    }

    static void commit_replay(SecurityAssociation& sa, uint64_t seq) noexcept {
        if (seq > sa.rx_highest) {
            const uint64_t shift = seq - sa.rx_highest;
            sa.replay_bitmap     = (shift >= 64U) ? 1ULL : ((sa.replay_bitmap << shift) | 1ULL);
            sa.rx_highest        = seq;
        } else {
            const uint64_t delta = sa.rx_highest - seq;
            sa.replay_bitmap |= (1ULL << delta);
        }
    }

    [[no_unique_address]] Backend           backend_;
    std::array<SecurityAssociation, MaxSAs> sas_{};
};

} // namespace strata::services::comms
