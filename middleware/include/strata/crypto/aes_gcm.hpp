#pragma once

// =============================================================================
// STRATA-20 Freestanding Zero-Heap AES-256-GCM (FIPS 197 + NIST SP 800-38D)
// Implements full 14-round AES-256 block cipher, GCTR encryption, GF(2^128)
// GHASH authentication, and constant-time 128-bit tag verification.
// Zero heap, zero exceptions, zero RTTI, bounded stack (< 1 KB).
// =============================================================================

#include <strata/crypto/sha256.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace strata::crypto {

/// FIPS 197 AES-256 block cipher (encryption path sufficient for AES-GCM & CTR).
class Aes256 {
public:
    static constexpr size_t kKeyBytes   = 32U;
    static constexpr size_t kBlockBytes = 16U;
    static constexpr size_t kRounds     = 14U;

    void set_key(std::span<const std::byte, kKeyBytes> key) noexcept {
        std::array<uint32_t, 60U> w{};
        for (size_t i = 0U; i < 8U; ++i) {
            w[i] = load_be32(key.data() + i * 4U);
        }
        static constexpr std::array<uint32_t, 8U> kRcon = {
            0x01000000U, 0x02000000U, 0x04000000U, 0x08000000U,
            0x10000000U, 0x20000000U, 0x40000000U, 0x80000000U,
        };
        for (size_t i = 8U; i < 60U; ++i) {
            uint32_t temp = w[i - 1U];
            if ((i % 8U) == 0U) {
                temp = sub_word(rot_word(temp)) ^ kRcon[(i / 8U) - 1U];
            } else if ((i % 8U) == 4U) {
                temp = sub_word(temp);
            }
            w[i] = w[i - 8U] ^ temp;
        }
        for (size_t r = 0U; r <= kRounds; ++r) {
            for (size_t c = 0U; c < 4U; ++c) {
                store_be32(rk_[r].data() + c * 4U, w[r * 4U + c]);
            }
        }
    }

    void encrypt_block(std::span<const std::byte, kBlockBytes> in,
                       std::span<std::byte, kBlockBytes>       out) const noexcept {
        std::array<uint8_t, 16U> s{};
        for (size_t i = 0U; i < 16U; ++i) {
            s[i] = static_cast<uint8_t>(std::to_integer<uint8_t>(in[i]) ^ rk_[0][i]);
        }

        for (size_t r = 1U; r < kRounds; ++r) {
            sub_bytes(s);
            shift_rows(s);
            mix_columns(s);
            for (size_t i = 0U; i < 16U; ++i) {
                s[i] ^= rk_[r][i];
            }
        }

        sub_bytes(s);
        shift_rows(s);
        for (size_t i = 0U; i < 16U; ++i) {
            out[i] = static_cast<std::byte>(s[i] ^ rk_[kRounds][i]);
        }
    }

    ~Aes256() noexcept {
        for (auto& round : rk_) {
            volatile uint8_t* p = round.data();
            for (size_t i = 0U; i < 16U; ++i) {
                p[i] = 0U;
            }
        }
    }

private:
    static constexpr std::array<uint8_t, 256U> kSbox = {
        0x63U, 0x7CU, 0x77U, 0x7BU, 0xF2U, 0x6BU, 0x6FU, 0xC5U, 0x30U, 0x01U, 0x67U, 0x2BU, 0xFEU, 0xD7U, 0xABU, 0x76U,
        0xCAU, 0x82U, 0xC9U, 0x7DU, 0xFAU, 0x59U, 0x47U, 0xF0U, 0xADU, 0xD4U, 0xA2U, 0xAFU, 0x9CU, 0xA4U, 0x72U, 0xC0U,
        0xB7U, 0xFDU, 0x93U, 0x26U, 0x36U, 0x3FU, 0xF7U, 0xCCU, 0x34U, 0xA5U, 0xE5U, 0xF1U, 0x71U, 0xD8U, 0x31U, 0x15U,
        0x04U, 0xC7U, 0x23U, 0xC3U, 0x18U, 0x96U, 0x05U, 0x9AU, 0x07U, 0x12U, 0x80U, 0xE2U, 0xEBU, 0x27U, 0xB2U, 0x75U,
        0x09U, 0x83U, 0x2CU, 0x1AU, 0x1BU, 0x6EU, 0x5AU, 0xA0U, 0x52U, 0x3BU, 0xD6U, 0xB3U, 0x29U, 0xE3U, 0x2FU, 0x84U,
        0x53U, 0xD1U, 0x00U, 0xEDU, 0x20U, 0xFCU, 0xB1U, 0x5BU, 0x6AU, 0xCBU, 0xBEU, 0x39U, 0x4AU, 0x4CU, 0x58U, 0xCFU,
        0xD0U, 0xEFU, 0xAAU, 0xFBU, 0x43U, 0x4DU, 0x33U, 0x85U, 0x45U, 0xF9U, 0x02U, 0x7FU, 0x50U, 0x3CU, 0x9FU, 0xA8U,
        0x51U, 0xA3U, 0x40U, 0x8FU, 0x92U, 0x9DU, 0x38U, 0xF5U, 0xBCU, 0xB6U, 0xDAU, 0x21U, 0x10U, 0xFFU, 0xF3U, 0xD2U,
        0xCDU, 0x0CU, 0x13U, 0xECU, 0x5FU, 0x97U, 0x44U, 0x17U, 0xC4U, 0xA7U, 0x7EU, 0x3DU, 0x64U, 0x5DU, 0x19U, 0x73U,
        0x60U, 0x81U, 0x4FU, 0xDCU, 0x22U, 0x2AU, 0x90U, 0x88U, 0x46U, 0xEEU, 0xB8U, 0x14U, 0xDEU, 0x5EU, 0x0BU, 0xDBU,
        0xE0U, 0x32U, 0x3AU, 0x0AU, 0x49U, 0x06U, 0x24U, 0x5CU, 0xC2U, 0xD3U, 0xACU, 0x62U, 0x91U, 0x95U, 0xE4U, 0x79U,
        0xE7U, 0xC8U, 0x37U, 0x6DU, 0x8DU, 0xD5U, 0x4EU, 0xA9U, 0x6CU, 0x56U, 0xF4U, 0xEAU, 0x65U, 0x7AU, 0xAEU, 0x08U,
        0xBAU, 0x78U, 0x25U, 0x2EU, 0x1CU, 0xA6U, 0xB4U, 0xC6U, 0xE8U, 0xDDU, 0x74U, 0x1FU, 0x4BU, 0xBDU, 0x8BU, 0x8AU,
        0x70U, 0x3EU, 0xB5U, 0x66U, 0x48U, 0x03U, 0xF6U, 0x0EU, 0x61U, 0x35U, 0x57U, 0xB9U, 0x86U, 0xC1U, 0x1DU, 0x9EU,
        0xE1U, 0xF8U, 0x98U, 0x11U, 0x69U, 0xD9U, 0x8EU, 0x94U, 0x9BU, 0x1EU, 0x87U, 0xE9U, 0xCEU, 0x55U, 0x28U, 0xDFU,
        0x8CU, 0xA1U, 0x89U, 0x0DU, 0xBFU, 0xE6U, 0x42U, 0x68U, 0x41U, 0x99U, 0x2DU, 0x0FU, 0xB0U, 0x54U, 0xBBU, 0x16U,
    };

    static constexpr uint32_t rot_word(uint32_t w) noexcept { return (w << 8U) | (w >> 24U); }

    static constexpr uint32_t sub_word(uint32_t w) noexcept {
        return (static_cast<uint32_t>(kSbox[(w >> 24U) & 0xFFU]) << 24U) |
               (static_cast<uint32_t>(kSbox[(w >> 16U) & 0xFFU]) << 16U) |
               (static_cast<uint32_t>(kSbox[(w >> 8U) & 0xFFU]) << 8U) |
               static_cast<uint32_t>(kSbox[w & 0xFFU]);
    }

    static constexpr uint32_t load_be32(const std::byte* p) noexcept {
        return (static_cast<uint32_t>(std::to_integer<uint8_t>(p[0])) << 24U) |
               (static_cast<uint32_t>(std::to_integer<uint8_t>(p[1])) << 16U) |
               (static_cast<uint32_t>(std::to_integer<uint8_t>(p[2])) << 8U) |
               static_cast<uint32_t>(std::to_integer<uint8_t>(p[3]));
    }

    static constexpr void store_be32(uint8_t* p, uint32_t v) noexcept {
        p[0] = static_cast<uint8_t>((v >> 24U) & 0xFFU);
        p[1] = static_cast<uint8_t>((v >> 16U) & 0xFFU);
        p[2] = static_cast<uint8_t>((v >> 8U) & 0xFFU);
        p[3] = static_cast<uint8_t>(v & 0xFFU);
    }

    static constexpr uint8_t xtime(uint8_t x) noexcept {
        return static_cast<uint8_t>((static_cast<uint32_t>(x) << 1U) ^ (((x >> 7U) != 0U) ? 0x1BU : 0x00U));
    }

    static constexpr void sub_bytes(std::array<uint8_t, 16U>& s) noexcept {
        for (uint8_t& b : s) {
            b = kSbox[b];
        }
    }

    static constexpr void shift_rows(std::array<uint8_t, 16U>& s) noexcept {
        // Row 1: shift left by 1
        uint8_t t = s[1];
        s[1]      = s[5];
        s[5]      = s[9];
        s[9]      = s[13];
        s[13]     = t;
        // Row 2: shift left by 2
        t     = s[2];
        s[2]  = s[10];
        s[10] = t;
        t     = s[6];
        s[6]  = s[14];
        s[14] = t;
        // Row 3: shift left by 3
        t     = s[15];
        s[15] = s[11];
        s[11] = s[7];
        s[7]  = s[3];
        s[3]  = t;
    }

    static constexpr void mix_columns(std::array<uint8_t, 16U>& s) noexcept {
        for (size_t c = 0U; c < 4U; ++c) {
            const size_t  i  = c * 4U;
            const uint8_t a0 = s[i + 0U];
            const uint8_t a1 = s[i + 1U];
            const uint8_t a2 = s[i + 2U];
            const uint8_t a3 = s[i + 3U];
            const uint8_t t  = static_cast<uint8_t>(a0 ^ a1 ^ a2 ^ a3);
            s[i + 0U]        = static_cast<uint8_t>(a0 ^ t ^ xtime(static_cast<uint8_t>(a0 ^ a1)));
            s[i + 1U]        = static_cast<uint8_t>(a1 ^ t ^ xtime(static_cast<uint8_t>(a1 ^ a2)));
            s[i + 2U]        = static_cast<uint8_t>(a2 ^ t ^ xtime(static_cast<uint8_t>(a2 ^ a3)));
            s[i + 3U]        = static_cast<uint8_t>(a3 ^ t ^ xtime(static_cast<uint8_t>(a3 ^ a0)));
        }
    }

    std::array<std::array<uint8_t, 16U>, kRounds + 1U> rk_{};
};

/// NIST SP 800-38D AES-256-GCM Authenticated Encryption with Associated Data (AEAD).
class Aes256Gcm {
public:
    static constexpr size_t kKeyBytes = 32U;
    static constexpr size_t kIvBytes  = 12U;
    static constexpr size_t kTagBytes = 16U;

    [[nodiscard]] static bool encrypt_and_tag(std::span<const std::byte, kKeyBytes> key,
                                              std::span<const std::byte, kIvBytes>  iv,
                                              std::span<const std::byte>            aad,
                                              std::span<const std::byte>            plaintext,
                                              std::span<std::byte>                  ciphertext,
                                              std::span<std::byte, kTagBytes>       tag) noexcept {
        if (ciphertext.size() < plaintext.size()) {
            return false;
        }
        Aes256 aes{};
        aes.set_key(key);

        // H = AES_K(0^128)
        std::array<std::byte, 16U> zero_blk{};
        std::array<std::byte, 16U> h_subkey{};
        aes.encrypt_block(zero_blk, h_subkey);

        // J0 = IV || 0^31 || 1 (96-bit IV standard path)
        std::array<std::byte, 16U> j0{};
        std::memcpy(j0.data(), iv.data(), kIvBytes);
        j0[15] = std::byte{1U};

        // Encrypt plaintext with GCTR(inc32(J0))
        std::array<std::byte, 16U> ctr = j0;
        inc32(ctr);
        gctr(aes, ctr, plaintext, ciphertext.subspan(0U, plaintext.size()));

        // Tag = AES_K(J0) XOR GHASH(H, AAD, CT)
        std::array<std::byte, 16U> s_ghash{};
        ghash(h_subkey, aad, ciphertext.subspan(0U, plaintext.size()), s_ghash);

        std::array<std::byte, 16U> ej0{};
        aes.encrypt_block(j0, ej0);
        for (size_t i = 0U; i < 16U; ++i) {
            tag[i] = static_cast<std::byte>(std::to_integer<uint8_t>(ej0[i]) ^
                                            std::to_integer<uint8_t>(s_ghash[i]));
        }
        return true;
    }

    [[nodiscard]] static bool decrypt_and_verify(std::span<const std::byte, kKeyBytes> key,
                                                 std::span<const std::byte, kIvBytes>  iv,
                                                 std::span<const std::byte>            aad,
                                                 std::span<const std::byte>            ciphertext,
                                                 std::span<std::byte>                  plaintext,
                                                 std::span<const std::byte, kTagBytes> rx_tag) noexcept {
        if (plaintext.size() < ciphertext.size()) {
            return false;
        }
        Aes256 aes{};
        aes.set_key(key);

        std::array<std::byte, 16U> zero_blk{};
        std::array<std::byte, 16U> h_subkey{};
        aes.encrypt_block(zero_blk, h_subkey);

        std::array<std::byte, 16U> j0{};
        std::memcpy(j0.data(), iv.data(), kIvBytes);
        j0[15] = std::byte{1U};

        std::array<std::byte, 16U> s_ghash{};
        ghash(h_subkey, aad, ciphertext, s_ghash);

        std::array<std::byte, 16U> ej0{};
        aes.encrypt_block(j0, ej0);

        std::array<std::byte, 16U> expected_tag{};
        for (size_t i = 0U; i < 16U; ++i) {
            expected_tag[i] = static_cast<std::byte>(std::to_integer<uint8_t>(ej0[i]) ^
                                                     std::to_integer<uint8_t>(s_ghash[i]));
        }
        if (!ct_equal(expected_tag, rx_tag)) {
            return false;
        }

        std::array<std::byte, 16U> ctr = j0;
        inc32(ctr);
        gctr(aes, ctr, ciphertext, plaintext.subspan(0U, ciphertext.size()));
        return true;
    }

private:
    static void inc32(std::array<std::byte, 16U>& ctr) noexcept {
        uint32_t v = (static_cast<uint32_t>(std::to_integer<uint8_t>(ctr[12])) << 24U) |
                     (static_cast<uint32_t>(std::to_integer<uint8_t>(ctr[13])) << 16U) |
                     (static_cast<uint32_t>(std::to_integer<uint8_t>(ctr[14])) << 8U) |
                     static_cast<uint32_t>(std::to_integer<uint8_t>(ctr[15]));
        ++v;
        ctr[12] = static_cast<std::byte>((v >> 24U) & 0xFFU);
        ctr[13] = static_cast<std::byte>((v >> 16U) & 0xFFU);
        ctr[14] = static_cast<std::byte>((v >> 8U) & 0xFFU);
        ctr[15] = static_cast<std::byte>(v & 0xFFU);
    }

    static void gctr(const Aes256& aes, std::array<std::byte, 16U> ctr,
                     std::span<const std::byte> in, std::span<std::byte> out) noexcept {
        std::array<std::byte, 16U> ks{};
        size_t                     off = 0U;
        while (off < in.size()) {
            aes.encrypt_block(ctr, ks);
            inc32(ctr);
            const size_t n = (in.size() - off < 16U) ? (in.size() - off) : 16U;
            for (size_t i = 0U; i < n; ++i) {
                out[off + i] = static_cast<std::byte>(std::to_integer<uint8_t>(in[off + i]) ^
                                                      std::to_integer<uint8_t>(ks[i]));
            }
            off += n;
        }
    }

    static uint64_t load_be64(const std::byte* p) noexcept {
        uint64_t v = 0ULL;
        for (size_t i = 0U; i < 8U; ++i) {
            v = (v << 8U) | static_cast<uint64_t>(std::to_integer<uint8_t>(p[i]));
        }
        return v;
    }

    static void store_be64(std::byte* p, uint64_t v) noexcept {
        for (size_t i = 0U; i < 8U; ++i) {
            p[i] = static_cast<std::byte>((v >> ((7U - i) * 8U)) & 0xFFULL);
        }
    }

    /// Constant-time GF(2^128) multiplication (NIST SP 800-38D Algorithm 1, polynomial x^128 + x^7 + x^2 + x + 1).
    static void gf128_mul(std::array<std::byte, 16U>& x, const std::array<std::byte, 16U>& h) noexcept {
        uint64_t v_hi = load_be64(h.data());
        uint64_t v_lo = load_be64(h.data() + 8U);
        uint64_t z_hi = 0ULL;
        uint64_t z_lo = 0ULL;

        for (size_t byte_idx = 0U; byte_idx < 16U; ++byte_idx) {
            const uint8_t xb = std::to_integer<uint8_t>(x[byte_idx]);
            for (uint32_t bit = 0U; bit < 8U; ++bit) {
                const uint64_t mask = 0ULL - static_cast<uint64_t>((xb >> (7U - bit)) & 1U);
                z_hi ^= (v_hi & mask);
                z_lo ^= (v_lo & mask);

                const uint64_t lsb = 0ULL - (v_lo & 1ULL);
                v_lo               = (v_lo >> 1U) | (v_hi << 63U);
                v_hi               = (v_hi >> 1U) ^ (0xE100'0000'0000'0000ULL & lsb);
            }
        }
        store_be64(x.data(), z_hi);
        store_be64(x.data() + 8U, z_lo);
    }

    static void ghash_blocks(const std::array<std::byte, 16U>& h, std::span<const std::byte> data,
                             std::array<std::byte, 16U>& y) noexcept {
        size_t off = 0U;
        while (off < data.size()) {
            const size_t n = (data.size() - off < 16U) ? (data.size() - off) : 16U;
            for (size_t i = 0U; i < n; ++i) {
                y[i] = static_cast<std::byte>(std::to_integer<uint8_t>(y[i]) ^
                                              std::to_integer<uint8_t>(data[off + i]));
            }
            gf128_mul(y, h);
            off += n;
        }
    }

    static void ghash(const std::array<std::byte, 16U>& h, std::span<const std::byte> aad,
                      std::span<const std::byte> ct, std::array<std::byte, 16U>& y) noexcept {
        y.fill(std::byte{0});
        ghash_blocks(h, aad, y);
        ghash_blocks(h, ct, y);

        std::array<std::byte, 16U> len_blk{};
        store_be64(len_blk.data(), static_cast<uint64_t>(aad.size()) * 8ULL);
        store_be64(len_blk.data() + 8U, static_cast<uint64_t>(ct.size()) * 8ULL);
        for (size_t i = 0U; i < 16U; ++i) {
            y[i] = static_cast<std::byte>(std::to_integer<uint8_t>(y[i]) ^
                                          std::to_integer<uint8_t>(len_blk[i]));
        }
        gf128_mul(y, h);
    }
};

} // namespace strata::crypto
