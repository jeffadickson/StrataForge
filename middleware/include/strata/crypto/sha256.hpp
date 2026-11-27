#pragma once

// =============================================================================
// STRATA-20 Freestanding Zero-Heap Cryptographic Primitives: SHA-256 & HMAC-SHA256
// Standards: FIPS 180-4 (SHA-256), RFC 2104 (HMAC), constant-time verification.
// Zero heap, zero exceptions, zero RTTI, bounded stack (< 512 B).
// =============================================================================

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace strata::crypto {

/// Constant-time byte-slice comparison (prevents timing side channels on MAC/tag/signature checks).
[[nodiscard]] inline bool ct_equal(std::span<const std::byte> a,
                                   std::span<const std::byte> b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    uint8_t diff = 0U;
    for (size_t i = 0U; i < a.size(); ++i) {
        diff |= static_cast<uint8_t>(std::to_integer<uint8_t>(a[i]) ^
                                     std::to_integer<uint8_t>(b[i]));
    }
    return diff == 0U;
}

/// Secure zeroization using volatile writes so compilers cannot elide key clearing.
inline void secure_zero(std::span<std::byte> buf) noexcept {
    volatile std::byte* p = buf.data();
    for (size_t i = 0U; i < buf.size(); ++i) {
        p[i] = std::byte{0};
    }
}

/// FIPS 180-4 SHA-256 incremental & one-shot hash engine.
class Sha256 {
public:
    static constexpr size_t kDigestBytes = 32U;
    static constexpr size_t kBlockBytes  = 64U;

    using Digest = std::array<std::byte, kDigestBytes>;

    constexpr Sha256() noexcept { reset(); }

    constexpr void reset() noexcept {
        h_          = {0x6A09E667U, 0xBB67AE85U, 0x3C6EF372U, 0xA54FF53AU,
                       0x510E527FU, 0x9B05688CU, 0x1F83D9ABU, 0x5BE0CD19U};
        total_bits_ = 0U;
        buf_len_    = 0U;
    }

    void update(std::span<const std::byte> data) noexcept {
        size_t offset = 0U;
        while (offset < data.size()) {
            const size_t space = kBlockBytes - buf_len_;
            const size_t chunk = (data.size() - offset < space) ? (data.size() - offset) : space;
            std::memcpy(buf_.data() + buf_len_, data.data() + offset, chunk);
            buf_len_ += chunk;
            offset += chunk;
            total_bits_ += static_cast<uint64_t>(chunk) * 8ULL;
            if (buf_len_ == kBlockBytes) {
                compress(buf_.data());
                buf_len_ = 0U;
            }
        }
    }

    [[nodiscard]] Digest finalize() noexcept {
        buf_[buf_len_++] = std::byte{0x80U};
        if (buf_len_ > 56U) {
            while (buf_len_ < kBlockBytes) {
                buf_[buf_len_++] = std::byte{0U};
            }
            compress(buf_.data());
            buf_len_ = 0U;
        }
        while (buf_len_ < 56U) {
            buf_[buf_len_++] = std::byte{0U};
        }
        for (size_t i = 0U; i < 8U; ++i) {
            buf_[56U + i] = static_cast<std::byte>((total_bits_ >> ((7U - i) * 8U)) & 0xFFULL);
        }
        compress(buf_.data());

        Digest out{};
        for (size_t i = 0U; i < 8U; ++i) {
            out[i * 4U + 0U] = static_cast<std::byte>((h_[i] >> 24U) & 0xFFU);
            out[i * 4U + 1U] = static_cast<std::byte>((h_[i] >> 16U) & 0xFFU);
            out[i * 4U + 2U] = static_cast<std::byte>((h_[i] >> 8U) & 0xFFU);
            out[i * 4U + 3U] = static_cast<std::byte>(h_[i] & 0xFFU);
        }
        reset();
        return out;
    }

    [[nodiscard]] static Digest hash(std::span<const std::byte> data) noexcept {
        Sha256 ctx{};
        ctx.update(data);
        return ctx.finalize();
    }

private:
    static constexpr uint32_t rotr(uint32_t x, uint32_t n) noexcept {
        return (x >> n) | (x << (32U - n));
    }

    static constexpr std::array<uint32_t, 64U> kRoundK = {
        0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U, 0x3956C25BU, 0x59F111F1U, 0x923F82A4U, 0xAB1C5ED5U,
        0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U, 0x72BE5D74U, 0x80DEB1FEU, 0x9BDC06A7U, 0xC19BF174U,
        0xE49B69C1U, 0xEFBE4786U, 0x0FC19DC6U, 0x240CA1CCU, 0x2DE92C6FU, 0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU,
        0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U, 0xC6E00BF3U, 0xD5A79147U, 0x06CA6351U, 0x14292967U,
        0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU, 0x53380D13U, 0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U,
        0xA2BFE8A1U, 0xA81A664BU, 0xC24B8B70U, 0xC76C51A3U, 0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U,
        0x19A4C116U, 0x1E376C08U, 0x2748774CU, 0x34B0BCB5U, 0x391C0CB3U, 0x4ED8AA4AU, 0x5B9CCA4FU, 0x682E6FF3U,
        0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U, 0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U, 0xC67178F2U,
    };

    void compress(const std::byte* block) noexcept {
        std::array<uint32_t, 64U> w{};
        for (size_t i = 0U; i < 16U; ++i) {
            w[i] = (static_cast<uint32_t>(std::to_integer<uint8_t>(block[i * 4U + 0U])) << 24U) |
                   (static_cast<uint32_t>(std::to_integer<uint8_t>(block[i * 4U + 1U])) << 16U) |
                   (static_cast<uint32_t>(std::to_integer<uint8_t>(block[i * 4U + 2U])) << 8U) |
                   static_cast<uint32_t>(std::to_integer<uint8_t>(block[i * 4U + 3U]));
        }
        for (size_t i = 16U; i < 64U; ++i) {
            const uint32_t s0 = rotr(w[i - 15U], 7U) ^ rotr(w[i - 15U], 18U) ^ (w[i - 15U] >> 3U);
            const uint32_t s1 = rotr(w[i - 2U], 17U) ^ rotr(w[i - 2U], 19U) ^ (w[i - 2U] >> 10U);
            w[i]              = w[i - 16U] + s0 + w[i - 7U] + s1;
        }

        uint32_t a = h_[0];
        uint32_t b = h_[1];
        uint32_t c = h_[2];
        uint32_t d = h_[3];
        uint32_t e = h_[4];
        uint32_t f = h_[5];
        uint32_t g = h_[6];
        uint32_t h = h_[7];

        for (size_t i = 0U; i < 64U; ++i) {
            const uint32_t s1    = rotr(e, 6U) ^ rotr(e, 11U) ^ rotr(e, 25U);
            const uint32_t ch    = (e & f) ^ ((~e) & g);
            const uint32_t temp1 = h + s1 + ch + kRoundK[i] + w[i];
            const uint32_t s0    = rotr(a, 2U) ^ rotr(a, 13U) ^ rotr(a, 22U);
            const uint32_t maj   = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = s0 + maj;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
        h_[5] += f;
        h_[6] += g;
        h_[7] += h;
    }

    std::array<uint32_t, 8U>             h_{};
    uint64_t                             total_bits_{0U};
    std::array<std::byte, kBlockBytes>   buf_{};
    size_t                               buf_len_{0U};
};

/// RFC 2104 HMAC-SHA256 keyed message authentication code.
class HmacSha256 {
public:
    static constexpr size_t kTagBytes = Sha256::kDigestBytes;
    using Tag                         = Sha256::Digest;

    [[nodiscard]] static Tag compute(std::span<const std::byte> key,
                                     std::span<const std::byte> message) noexcept {
        std::array<std::byte, Sha256::kBlockBytes> k_block{};
        if (key.size() > Sha256::kBlockBytes) {
            const auto hashed = Sha256::hash(key);
            std::memcpy(k_block.data(), hashed.data(), hashed.size());
        } else if (!key.empty()) {
            std::memcpy(k_block.data(), key.data(), key.size());
        }

        std::array<std::byte, Sha256::kBlockBytes> ipad{};
        std::array<std::byte, Sha256::kBlockBytes> opad{};
        for (size_t i = 0U; i < Sha256::kBlockBytes; ++i) {
            const uint8_t kb = std::to_integer<uint8_t>(k_block[i]);
            ipad[i]          = static_cast<std::byte>(kb ^ 0x36U);
            opad[i]          = static_cast<std::byte>(kb ^ 0x5CU);
        }

        Sha256 inner{};
        inner.update(ipad);
        inner.update(message);
        const auto inner_digest = inner.finalize();

        Sha256 outer{};
        outer.update(opad);
        outer.update(inner_digest);
        const auto tag = outer.finalize();

        secure_zero(k_block);
        secure_zero(ipad);
        secure_zero(opad);
        return tag;
    }

    [[nodiscard]] static bool verify(std::span<const std::byte> key,
                                     std::span<const std::byte> message,
                                     std::span<const std::byte> expected_tag) noexcept {
        const auto computed = compute(key, message);
        return ct_equal(computed, expected_tag);
    }
};

} // namespace strata::crypto
