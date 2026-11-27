#pragma once

// =============================================================================
// STRATA-20 Freestanding Zero-Heap SHA-512 & Ed25519 (FIPS 180-4 + RFC 8032)
// Pure standard C++20 radix-2^16 field arithmetic mod (2^255 - 19).
// Zero heap, zero exceptions, zero RTTI, constant-time scalar multiplication.
// =============================================================================

#include <strata/crypto/sha256.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace strata::crypto {

/// FIPS 180-4 SHA-512 incremental & one-shot hash engine.
class Sha512 {
public:
    static constexpr size_t kDigestBytes = 64U;
    static constexpr size_t kBlockBytes  = 128U;

    using Digest = std::array<std::byte, kDigestBytes>;

    constexpr Sha512() noexcept { reset(); }

    constexpr void reset() noexcept {
        h_          = {0x6A09E667F3BCC908ULL, 0xBB67AE8584CAA73BULL, 0x3C6EF372FE94F82BULL, 0xA54FF53A5F1D36F1ULL,
                       0x510E527FADE682D1ULL, 0x9B05688C2B3E6C1FULL, 0x1F83D9ABFB41BD6BULL, 0x5BE0CD19137E2179ULL};
        total_bits_ = 0ULL;
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
        if (buf_len_ > 112U) {
            while (buf_len_ < kBlockBytes) {
                buf_[buf_len_++] = std::byte{0U};
            }
            compress(buf_.data());
            buf_len_ = 0U;
        }
        while (buf_len_ < 120U) {
            buf_[buf_len_++] = std::byte{0U};
        }
        for (size_t i = 0U; i < 8U; ++i) {
            buf_[120U + i] = static_cast<std::byte>((total_bits_ >> ((7U - i) * 8U)) & 0xFFULL);
        }
        compress(buf_.data());

        Digest out{};
        for (size_t i = 0U; i < 8U; ++i) {
            for (size_t b = 0U; b < 8U; ++b) {
                out[i * 8U + b] = static_cast<std::byte>((h_[i] >> ((7U - b) * 8U)) & 0xFFULL);
            }
        }
        reset();
        return out;
    }

    [[nodiscard]] static Digest hash(std::span<const std::byte> data) noexcept {
        Sha512 ctx{};
        ctx.update(data);
        return ctx.finalize();
    }

private:
    static constexpr uint64_t rotr(uint64_t x, uint32_t n) noexcept {
        return (x >> n) | (x << (64U - n));
    }

    static constexpr std::array<uint64_t, 80U> kRoundK = {
        0x428A2F98D728AE22ULL, 0x7137449123EF65CDULL, 0xB5C0FBCFEC4D3B2FULL, 0xE9B5DBA58189DBBCULL,
        0x3956C25BF348B538ULL, 0x59F111F1B605D019ULL, 0x923F82A4AF194F9BULL, 0xAB1C5ED5DA6D8118ULL,
        0xD807AA98A3030242ULL, 0x12835B0145706FBEULL, 0x243185BE4EE4B28CULL, 0x550C7DC3D5FFB4E2ULL,
        0x72BE5D74F27B896FULL, 0x80DEB1FE3B1696B1ULL, 0x9BDC06A725C71235ULL, 0xC19BF174CF692694ULL,
        0xE49B69C19EF14AD2ULL, 0xEFBE4786384F25E3ULL, 0x0FC19DC68B8CD5B5ULL, 0x240CA1CC77AC9C65ULL,
        0x2DE92C6F592B0275ULL, 0x4A7484AA6EA6E483ULL, 0x5CB0A9DCBD41FBD4ULL, 0x76F988DA831153B5ULL,
        0x983E5152EE66DFABULL, 0xA831C66D2DB43210ULL, 0xB00327C898FB213FULL, 0xBF597FC7BEEF0EE4ULL,
        0xC6E00BF33DA88FC2ULL, 0xD5A79147930AA725ULL, 0x06CA6351E003826FULL, 0x142929670A0E6E70ULL,
        0x27B70A8546D22FFCULL, 0x2E1B21385C26C926ULL, 0x4D2C6DFC5AC42AEDULL, 0x53380D139D95B3DFULL,
        0x650A73548BAF63DEULL, 0x766A0ABB3C77B2A8ULL, 0x81C2C92E47EDAEE6ULL, 0x92722C851482353BULL,
        0xA2BFE8A14CF10364ULL, 0xA81A664BBC423001ULL, 0xC24B8B70D0F89791ULL, 0xC76C51A30654BE30ULL,
        0xD192E819D6EF5218ULL, 0xD69906245565A910ULL, 0xF40E35855771202AULL, 0x106AA07032BBD1B8ULL,
        0x19A4C116B8D2D0C8ULL, 0x1E376C085141AB53ULL, 0x2748774CDF8EEB99ULL, 0x34B0BCB5E19B48A8ULL,
        0x391C0CB3C5C95A63ULL, 0x4ED8AA4AE3418ACBULL, 0x5B9CCA4F7763E373ULL, 0x682E6FF3D6B2B8A3ULL,
        0x748F82EE5DEFB2FCULL, 0x78A5636F43172F60ULL, 0x84C87814A1F0AB72ULL, 0x8CC702081A6439ECULL,
        0x90BEFFFA23631E28ULL, 0xA4506CEBDE82BDE9ULL, 0xBEF9A3F7B2C67915ULL, 0xC67178F2E372532BULL,
        0xCA273ECEEA26619CULL, 0xD186B8C721C0C207ULL, 0xEADA7DD6CDE0EB1EULL, 0xF57D4F7FEE6ED178ULL,
        0x06F067AA72176FBAULL, 0x0A637DC5A2C898A6ULL, 0x113F9804BEF90DAEULL, 0x1B710B35131C471BULL,
        0x28DB77F523047D84ULL, 0x32CAAB7B40C72493ULL, 0x3C9EBE0A15C9BEBCULL, 0x431D67C49C100D4CULL,
        0x4CC5D4BECB3E42B6ULL, 0x597F299CFC657E2AULL, 0x5FCB6FAB3AD6FAECULL, 0x6C44198C4A475817ULL,
    };

    void compress(const std::byte* block) noexcept {
        std::array<uint64_t, 80U> w{};
        for (size_t i = 0U; i < 16U; ++i) {
            uint64_t v = 0ULL;
            for (size_t b = 0U; b < 8U; ++b) {
                v = (v << 8U) | static_cast<uint64_t>(std::to_integer<uint8_t>(block[i * 8U + b]));
            }
            w[i] = v;
        }
        for (size_t i = 16U; i < 80U; ++i) {
            const uint64_t s0 = rotr(w[i - 15U], 1U) ^ rotr(w[i - 15U], 8U) ^ (w[i - 15U] >> 7U);
            const uint64_t s1 = rotr(w[i - 2U], 19U) ^ rotr(w[i - 2U], 61U) ^ (w[i - 2U] >> 6U);
            w[i]              = w[i - 16U] + s0 + w[i - 7U] + s1;
        }

        uint64_t a = h_[0];
        uint64_t b = h_[1];
        uint64_t c = h_[2];
        uint64_t d = h_[3];
        uint64_t e = h_[4];
        uint64_t f = h_[5];
        uint64_t g = h_[6];
        uint64_t h = h_[7];

        for (size_t i = 0U; i < 80U; ++i) {
            const uint64_t s1    = rotr(e, 14U) ^ rotr(e, 18U) ^ rotr(e, 41U);
            const uint64_t ch    = (e & f) ^ ((~e) & g);
            const uint64_t temp1 = h + s1 + ch + kRoundK[i] + w[i];
            const uint64_t s0    = rotr(a, 28U) ^ rotr(a, 34U) ^ rotr(a, 39U);
            const uint64_t maj   = (a & b) ^ (a & c) ^ (b & c);
            const uint64_t temp2 = s0 + maj;

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

    std::array<uint64_t, 8U>           h_{};
    uint64_t                           total_bits_{0ULL};
    std::array<std::byte, kBlockBytes> buf_{};
    size_t                             buf_len_{0U};
};

/// RFC 8032 PureEd25519 digital signature engine (key derivation, sign, verify).
class Ed25519 {
public:
    static constexpr size_t kSeedBytes      = 32U;
    static constexpr size_t kPublicKeyBytes = 32U;
    static constexpr size_t kSignatureBytes = 64U;

    using Seed      = std::array<std::byte, kSeedBytes>;
    using PublicKey = std::array<std::byte, kPublicKeyBytes>;
    using Signature = std::array<std::byte, kSignatureBytes>;

    /// Derive the 32-byte Ed25519 public key from a 32-byte private seed.
    [[nodiscard]] static PublicKey derive_public_key(std::span<const std::byte, kSeedBytes> seed) noexcept {
        auto h = Sha512::hash(seed);
        clamp_scalar(h);

        Point a{};
        scalarbase(a, h.data());
        PublicKey pk{};
        pack_point(pk, a);
        secure_zero(h);
        return pk;
    }

    /// Sign a message with the 32-byte private seed and corresponding 32-byte public key.
    [[nodiscard]] static Signature sign(std::span<const std::byte, kSeedBytes>      seed,
                                        std::span<const std::byte, kPublicKeyBytes> pk,
                                        std::span<const std::byte>                  message) noexcept {
        auto az = Sha512::hash(seed);
        clamp_scalar(az);

        // r = SHA-512(prefix || message) mod L
        Sha512 r_hasher{};
        r_hasher.update(std::span<const std::byte>{az.data() + 32U, 32U});
        r_hasher.update(message);
        auto r_digest = r_hasher.finalize();
        reduce_mod_l(r_digest);

        // R = r * B
        Point r_pt{};
        scalarbase(r_pt, r_digest.data());
        Signature sig{};
        std::array<std::byte, 32U> r_enc{};
        pack_point(r_enc, r_pt);
        std::memcpy(sig.data(), r_enc.data(), 32U);

        // k = SHA-512(R || A || message) mod L
        Sha512 k_hasher{};
        k_hasher.update(r_enc);
        k_hasher.update(pk);
        k_hasher.update(message);
        auto k_digest = k_hasher.finalize();
        reduce_mod_l(k_digest);

        // S = (r + k * a) mod L
        std::array<int64_t, 64U> x{};
        for (size_t i = 0U; i < 32U; ++i) {
            x[i] = static_cast<int64_t>(std::to_integer<uint8_t>(r_digest[i]));
        }
        for (size_t i = 0U; i < 32U; ++i) {
            const int64_t ki = static_cast<int64_t>(std::to_integer<uint8_t>(k_digest[i]));
            for (size_t j = 0U; j < 32U; ++j) {
                x[i + j] += ki * static_cast<int64_t>(std::to_integer<uint8_t>(az[j]));
            }
        }
        std::array<std::byte, 32U> s_enc{};
        mod_l_encode(s_enc, x);
        std::memcpy(sig.data() + 32U, s_enc.data(), 32U);

        secure_zero(az);
        secure_zero(r_digest);
        return sig;
    }

    /// Verify a 64-byte Ed25519 signature over `message` against 32-byte public key `pk`.
    [[nodiscard]] static bool verify(std::span<const std::byte, kPublicKeyBytes> pk,
                                     std::span<const std::byte>                  message,
                                     std::span<const std::byte, kSignatureBytes> sig) noexcept {
        // Reject non-canonical S >= L per RFC 8032 §5.1.7
        if (!is_canonical_scalar(std::span<const std::byte, 32U>{sig.data() + 32U, 32U})) {
            return false;
        }

        Point a_pt{};
        if (!unpack_neg_point(a_pt, pk)) {
            return false;
        }

        // k = SHA-512(R || A || message) mod L
        Sha512 k_hasher{};
        k_hasher.update(std::span<const std::byte>{sig.data(), 32U});
        k_hasher.update(pk);
        k_hasher.update(message);
        auto k_digest = k_hasher.finalize();
        reduce_mod_l(k_digest);

        // Compute R' = [S]B + [k](-A) and verify pack(R') == sig[0..31]
        Point sb{};
        Point ka{};
        scalarbase(sb, sig.data() + 32U);
        scalarmult(ka, a_pt, k_digest.data());
        add_points(sb, ka);

        std::array<std::byte, 32U> r_check{};
        pack_point(r_check, sb);
        return ct_equal(r_check, std::span<const std::byte>{sig.data(), 32U});
    }

private:
    using Fe    = std::array<int64_t, 16U>; // Field element mod (2^255 - 19), 16 limbs x 16 bits
    using Point = std::array<Fe, 4U>;       // Extended coordinates (X, Y, Z, T)

    static constexpr Fe kFe0  = {0};
    static constexpr Fe kFe1  = {1};
    static constexpr Fe kD    = {0x78A3, 0x1359, 0x4DCA, 0x75EB, 0xD8AB, 0x4141, 0x0A4D, 0x0070,
                                 0xE898, 0x7779, 0x4079, 0x8CC7, 0xFE73, 0x2B6F, 0x6CEE, 0x5203};
    static constexpr Fe kD2   = {0xF159, 0x26B2, 0x9B94, 0xEBD6, 0xB156, 0x8283, 0x149A, 0x00E0,
                                 0xD130, 0xEEF3, 0x80F2, 0x198E, 0xFCE7, 0x56DF, 0xD9DC, 0x2406};
    static constexpr Fe kBaseX = {0xD51A, 0x8F25, 0x2D60, 0xC956, 0xA7B2, 0x9525, 0xC760, 0x692C,
                                  0xDC5C, 0xFDD6, 0xE231, 0xC0A4, 0x53FE, 0xCD6E, 0x36D3, 0x2169};
    static constexpr Fe kBaseY = {0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
                                  0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666};
    static constexpr Fe kSqrtM1 = {0xA0B0, 0x4A0E, 0x1B27, 0xC4EE, 0xE478, 0xAD2F, 0x1806, 0x2F43,
                                   0xD7A7, 0x3DFB, 0x0099, 0x2B4D, 0xDF0B, 0x4FC1, 0x2480, 0x2B83};

    // Group order L = 2^252 + 27742317777372353535851937790883648493 (little-endian bytes)
    static constexpr std::array<uint8_t, 32U> kOrderL = {
        0xEDU, 0xD3U, 0xF5U, 0x5CU, 0x1AU, 0x63U, 0x12U, 0x58U, 0xD6U, 0x9CU, 0xF7U,
        0xA2U, 0xDEU, 0xF9U, 0xDEU, 0x14U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x10U,
    };

    static void clamp_scalar(Sha512::Digest& h) noexcept {
        h[0]  = static_cast<std::byte>(std::to_integer<uint8_t>(h[0]) & 0xF8U);
        h[31] = static_cast<std::byte>((std::to_integer<uint8_t>(h[31]) & 0x3FU) | 0x40U);
    }

    static bool is_canonical_scalar(std::span<const std::byte, 32U> s) noexcept {
        for (size_t i = 32U; i-- > 0U;) {
            const uint8_t sb = std::to_integer<uint8_t>(s[i]);
            if (sb < kOrderL[i]) return true;
            if (sb > kOrderL[i]) return false;
        }
        return false; // S == L is non-canonical
    }

    static void fe_carry(Fe& o) noexcept {
        for (size_t i = 0U; i < 16U; ++i) {
            o[i] += (1LL << 16);
            const int64_t c = o[i] >> 16;
            o[(i + 1U) * static_cast<size_t>(i < 15U)] += c - 1 + 37 * (c - 1) * static_cast<int64_t>(i == 15U);
            o[i] -= c << 16;
        }
    }

    static void fe_cswap(Fe& p, Fe& q, uint8_t b) noexcept {
        const int64_t mask = -static_cast<int64_t>(b & 1U);
        for (size_t i = 0U; i < 16U; ++i) {
            const int64_t t = mask & (p[i] ^ q[i]);
            p[i] ^= t;
            q[i] ^= t;
        }
    }

    static void pt_cswap(Point& p, Point& q, uint8_t b) noexcept {
        for (size_t i = 0U; i < 4U; ++i) {
            fe_cswap(p[i], q[i], b);
        }
    }

    static void fe_add(Fe& o, const Fe& a, const Fe& b) noexcept {
        for (size_t i = 0U; i < 16U; ++i) o[i] = a[i] + b[i];
    }

    static void fe_sub(Fe& o, const Fe& a, const Fe& b) noexcept {
        for (size_t i = 0U; i < 16U; ++i) o[i] = a[i] - b[i];
    }

    static void fe_mul(Fe& o, const Fe& a, const Fe& b) noexcept {
        std::array<int64_t, 31U> t{};
        for (size_t i = 0U; i < 16U; ++i) {
            for (size_t j = 0U; j < 16U; ++j) {
                t[i + j] += a[i] * b[j];
            }
        }
        for (size_t i = 0U; i < 15U; ++i) {
            t[i] += 38 * t[i + 16U];
        }
        for (size_t i = 0U; i < 16U; ++i) {
            o[i] = t[i];
        }
        fe_carry(o);
        fe_carry(o);
    }

    static void fe_sq(Fe& o, const Fe& a) noexcept { fe_mul(o, a, a); }

    static void fe_inv(Fe& o, const Fe& i) noexcept {
        Fe c = i;
        for (int a = 253; a >= 0; --a) {
            fe_sq(c, c);
            if (a != 2 && a != 4) {
                fe_mul(c, c, i);
            }
        }
        o = c;
    }

    static void fe_pow2523(Fe& o, const Fe& i) noexcept {
        Fe c = i;
        for (int a = 250; a >= 0; --a) {
            fe_sq(c, c);
            if (a != 1) {
                fe_mul(c, c, i);
            }
        }
        o = c;
    }

    static void fe_pack(std::array<std::byte, 32U>& o, const Fe& n) noexcept {
        Fe t = n;
        fe_carry(t);
        fe_carry(t);
        fe_carry(t);

        Fe m{};
        for (int j = 0; j < 2; ++j) {
            m[0] = t[0] - 0xFFED;
            for (size_t i = 1U; i < 15U; ++i) {
                m[i]     = t[i] - 0xFFFF - ((m[i - 1U] >> 16) & 1);
                m[i - 1U] &= 0xFFFF;
            }
            m[15] = t[15] - 0x7FFF - ((m[14] >> 16) & 1);
            const uint8_t b = static_cast<uint8_t>((m[15] >> 16) & 1);
            m[14] &= 0xFFFF;
            fe_cswap(t, m, static_cast<uint8_t>(1U - b));
        }
        for (size_t i = 0U; i < 16U; ++i) {
            o[2U * i]      = static_cast<std::byte>(t[i] & 0xFF);
            o[2U * i + 1U] = static_cast<std::byte>((t[i] >> 8) & 0xFF);
        }
    }

    static void fe_unpack(Fe& o, std::span<const std::byte, 32U> n) noexcept {
        for (size_t i = 0U; i < 16U; ++i) {
            o[i] = static_cast<int64_t>(std::to_integer<uint8_t>(n[2U * i])) |
                   (static_cast<int64_t>(std::to_integer<uint8_t>(n[2U * i + 1U])) << 8);
        }
        o[15] &= 0x7FFF;
    }

    static bool fe_neq(const Fe& a, const Fe& b) noexcept {
        std::array<std::byte, 32U> pa{};
        std::array<std::byte, 32U> pb{};
        fe_pack(pa, a);
        fe_pack(pb, b);
        return !ct_equal(pa, pb);
    }

    static uint8_t fe_parity(const Fe& a) noexcept {
        std::array<std::byte, 32U> d{};
        fe_pack(d, a);
        return static_cast<uint8_t>(std::to_integer<uint8_t>(d[0]) & 1U);
    }

    /// Extended twisted Edwards point addition: p = p + q.
    static void add_points(Point& p, const Point& q) noexcept {
        Fe a{}, b{}, c{}, d{}, e{}, f{}, g{}, h{}, t{};
        fe_sub(a, p[1], p[0]);
        fe_sub(t, q[1], q[0]);
        fe_mul(a, a, t);
        fe_add(b, p[0], p[1]);
        fe_add(t, q[0], q[1]);
        fe_mul(b, b, t);
        fe_mul(c, p[3], q[3]);
        fe_mul(c, c, kD2);
        fe_mul(d, p[2], q[2]);
        fe_add(d, d, d);
        fe_sub(e, b, a);
        fe_sub(f, d, c);
        fe_add(g, d, c);
        fe_add(h, b, a);

        fe_mul(p[0], e, f);
        fe_mul(p[1], h, g);
        fe_mul(p[2], g, f);
        fe_mul(p[3], e, h);
    }

    static void scalarmult(Point& p, Point q, const std::byte* s) noexcept {
        p = {kFe0, kFe1, kFe1, kFe0};
        for (int i = 255; i >= 0; --i) {
            const uint32_t byte_val = std::to_integer<uint32_t>(s[static_cast<size_t>(i) / 8U]);
            const uint8_t  b        = static_cast<uint8_t>((byte_val >> (static_cast<unsigned>(i) & 7U)) & 1U);
            pt_cswap(p, q, b);
            add_points(q, p);
            add_points(p, p);
            pt_cswap(p, q, b);
        }
    }

    static void scalarbase(Point& p, const std::byte* s) noexcept {
        Point q{};
        q[0] = kBaseX;
        q[1] = kBaseY;
        q[2] = kFe1;
        fe_mul(q[3], kBaseX, kBaseY);
        scalarmult(p, q, s);
    }

    static void pack_point(std::array<std::byte, 32U>& r, const Point& p) noexcept {
        Fe tx{}, ty{}, zi{};
        fe_inv(zi, p[2]);
        fe_mul(tx, p[0], zi);
        fe_mul(ty, p[1], zi);
        fe_pack(r, ty);
        r[31] = static_cast<std::byte>(std::to_integer<uint8_t>(r[31]) ^
                                       static_cast<uint8_t>(fe_parity(tx) << 7U));
    }

    static bool unpack_neg_point(Point& r, std::span<const std::byte, 32U> p) noexcept {
        Fe t{}, chk{}, num{}, den{}, den2{}, den4{}, den6{};
        r[2] = kFe1;
        fe_unpack(r[1], p);
        fe_sq(num, r[1]);
        fe_mul(den, num, kD);
        fe_sub(num, num, r[2]);
        fe_add(den, r[2], den);

        fe_sq(den2, den);
        fe_sq(den4, den2);
        fe_mul(den6, den4, den2);
        fe_mul(t, den6, num);
        fe_mul(t, t, den);

        fe_pow2523(t, t);
        fe_mul(t, t, num);
        fe_mul(t, t, den);
        fe_mul(t, t, den);
        fe_mul(r[0], t, den);

        fe_sq(chk, r[0]);
        fe_mul(chk, chk, den);
        if (fe_neq(chk, num)) {
            fe_mul(r[0], r[0], kSqrtM1);
        }

        fe_sq(chk, r[0]);
        fe_mul(chk, chk, den);
        if (fe_neq(chk, num)) {
            return false;
        }

        const uint8_t sign_bit = static_cast<uint8_t>(std::to_integer<uint8_t>(p[31]) >> 7U);
        if (fe_parity(r[0]) == sign_bit) {
            fe_sub(r[0], kFe0, r[0]);
        }
        fe_mul(r[3], r[0], r[1]);
        return true;
    }

    static void mod_l_encode(std::array<std::byte, 32U>& r, std::array<int64_t, 64U>& x) noexcept {
        for (size_t i = 63U; i >= 32U; --i) {
            int64_t carry = 0;
            for (size_t j = i - 32U; j < i - 12U; ++j) {
                x[j] += carry - 16 * x[i] * static_cast<int64_t>(kOrderL[j - (i - 32U)]);
                carry = (x[j] + 128) >> 8;
                x[j] -= carry << 8;
            }
            x[i - 12U] += carry;
            x[i] = 0;
        }

        int64_t carry = 0;
        for (size_t j = 0U; j < 32U; ++j) {
            x[j] += carry - (x[31] >> 4) * static_cast<int64_t>(kOrderL[j]);
            carry = x[j] >> 8;
            x[j] &= 255;
        }
        for (size_t j = 0U; j < 32U; ++j) {
            x[j] -= carry * static_cast<int64_t>(kOrderL[j]);
        }
        for (size_t i = 0U; i < 32U; ++i) {
            x[i + 1U] += x[i] >> 8;
            r[i] = static_cast<std::byte>(x[i] & 255);
        }
    }

    static void reduce_mod_l(Sha512::Digest& r) noexcept {
        std::array<int64_t, 64U> x{};
        for (size_t i = 0U; i < 64U; ++i) {
            x[i] = static_cast<int64_t>(std::to_integer<uint8_t>(r[i]));
        }
        std::array<std::byte, 32U> reduced{};
        mod_l_encode(reduced, x);
        r.fill(std::byte{0});
        std::memcpy(r.data(), reduced.data(), 32U);
    }
};

} // namespace strata::crypto
