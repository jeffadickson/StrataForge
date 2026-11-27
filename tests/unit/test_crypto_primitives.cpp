#include <strata/crypto/aes_gcm.hpp>
#include <strata/crypto/ed25519.hpp>
#include <strata/crypto/sha256.hpp>
#include <strata/services/autonomy/aot_loader.hpp>
#include <strata/services/comms/ccsds.hpp>
#include <strata/services/fdir/slot_manager.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>

#define STRATA_CHECK(expr)                                                                   \
    do {                                                                                     \
        if (!(expr)) {                                                                       \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            std::abort();                                                                    \
        }                                                                                    \
    } while (false)

namespace {

template <size_t N>
std::array<std::byte, N> from_hex(const char* hex) noexcept {
    std::array<std::byte, N> out{};
    auto nibble = [](char c) noexcept -> uint8_t {
        if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
        return 0U;
    };
    for (size_t i = 0U; i < N; ++i) {
        out[i] = static_cast<std::byte>((nibble(hex[i * 2U]) << 4U) | nibble(hex[i * 2U + 1U]));
    }
    return out;
}

std::span<const std::byte> as_bytes_str(const char* s) noexcept {
    return std::span<const std::byte>{reinterpret_cast<const std::byte*>(s), std::strlen(s)};
}

} // namespace

int main() {
    using namespace strata;
    using namespace strata::crypto;

    // 1. FIPS 180-4 SHA-256 Official Test Vectors ("abc" and empty string)
    {
        const auto d_empty = Sha256::hash({});
        const auto exp_empty =
            from_hex<32>("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        STRATA_CHECK(ct_equal(d_empty, exp_empty));

        const auto d_abc = Sha256::hash(as_bytes_str("abc"));
        const auto exp_abc =
            from_hex<32>("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        STRATA_CHECK(ct_equal(d_abc, exp_abc));
    }

    // 2. RFC 4231 Test Case 2: HMAC-SHA256("Jefe", "what do ya want for nothing?")
    {
        const auto tag = HmacSha256::compute(as_bytes_str("Jefe"),
                                             as_bytes_str("what do ya want for nothing?"));
        const auto exp =
            from_hex<32>("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
        STRATA_CHECK(ct_equal(tag, exp));
        STRATA_CHECK(HmacSha256::verify(as_bytes_str("Jefe"),
                                  as_bytes_str("what do ya want for nothing?"), exp));
    }

    // 3. NIST SP 800-38D AES-256-GCM Official Test Case 13 (zero key/IV, empty PT/AAD)
    //    and Test Case 16 (60-byte PT + 20-byte AAD)
    {
        const std::array<std::byte, 32> key0{};
        const std::array<std::byte, 12> iv0{};
        std::array<std::byte, 16>       tag0{};
        STRATA_CHECK(Aes256Gcm::encrypt_and_tag(key0, iv0, {}, {}, {}, tag0));
        const auto exp_tag0 = from_hex<16>("530f8afbc74536b9a963b4f1c4cb738b");
        STRATA_CHECK(ct_equal(tag0, exp_tag0));

        const auto key16 = from_hex<32>(
            "feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308");
        const auto iv16  = from_hex<12>("cafebabefacedbaddecaf888");
        const auto aad16 = from_hex<20>("feedfacedeadbeeffeedfacedeadbeefabaddad2");
        const auto pt16  = from_hex<60>(
            "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
            "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39");
        const auto exp_ct16 = from_hex<60>(
            "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa"
            "8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662");
        const auto exp_tag16 = from_hex<16>("76fc6ece0f4e1768cddf8853bb2d551b");

        std::array<std::byte, 60> ct16{};
        std::array<std::byte, 16> tag16{};
        STRATA_CHECK(Aes256Gcm::encrypt_and_tag(key16, iv16, aad16, pt16, ct16, tag16));
        STRATA_CHECK(ct_equal(ct16, exp_ct16));
        STRATA_CHECK(ct_equal(tag16, exp_tag16));

        std::array<std::byte, 60> dec16{};
        STRATA_CHECK(Aes256Gcm::decrypt_and_verify(key16, iv16, aad16, ct16, dec16, tag16));
        STRATA_CHECK(ct_equal(dec16, pt16));

        // Flip 1 bit in ciphertext -> decrypt_and_verify must reject!
        ct16[17] ^= std::byte{0x01U};
        STRATA_CHECK(!Aes256Gcm::decrypt_and_verify(key16, iv16, aad16, ct16, dec16, tag16));
    }

    // 4. FIPS 180-4 SHA-512 & RFC 8032 §7.1 Ed25519 Official Test Vectors 1, 2, 3
    {
        const auto sha512_abc = Sha512::hash(as_bytes_str("abc"));
        const auto exp_512    = from_hex<64>(
            "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
            "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
        STRATA_CHECK(ct_equal(sha512_abc, exp_512));

        // RFC 8032 Test 1: empty message
        const auto seed1 = from_hex<32>(
            "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
        const auto exp_pk1 = from_hex<32>(
            "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
        const auto exp_sig1 = from_hex<64>(
            "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
            "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
        const auto pk1 = Ed25519::derive_public_key(seed1);
        STRATA_CHECK(ct_equal(pk1, exp_pk1));
        const auto sig1 = Ed25519::sign(seed1, pk1, {});
        STRATA_CHECK(ct_equal(sig1, exp_sig1));
        STRATA_CHECK(Ed25519::verify(pk1, {}, sig1));

        // RFC 8032 Test 2: 1-byte message 0x72
        const auto seed2 = from_hex<32>(
            "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb");
        const auto exp_pk2 = from_hex<32>(
            "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c");
        const auto msg2     = from_hex<1>("72");
        const auto exp_sig2 = from_hex<64>(
            "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
            "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00");
        const auto pk2  = Ed25519::derive_public_key(seed2);
        STRATA_CHECK(ct_equal(pk2, exp_pk2));
        const auto sig2 = Ed25519::sign(seed2, pk2, msg2);
        STRATA_CHECK(ct_equal(sig2, exp_sig2));
        STRATA_CHECK(Ed25519::verify(pk2, msg2, sig2));

        // Tamper rejection: flipping 1 bit in signature or message fails verification
        auto bad_sig = sig2;
        bad_sig[10] ^= std::byte{0x80U};
        STRATA_CHECK(!Ed25519::verify(pk2, msg2, bad_sig));
        const auto bad_msg = from_hex<1>("73");
        STRATA_CHECK(!Ed25519::verify(pk2, bad_msg, sig2));

        // RFC 8032 Test 3: 2-byte message 0xaf82
        const auto seed3 = from_hex<32>(
            "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7");
        const auto exp_pk3 = from_hex<32>(
            "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025");
        const auto msg3     = from_hex<2>("af82");
        const auto exp_sig3 = from_hex<64>(
            "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
            "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a");
        const auto pk3  = Ed25519::derive_public_key(seed3);
        STRATA_CHECK(ct_equal(pk3, exp_pk3));
        const auto sig3 = Ed25519::sign(seed3, pk3, msg3);
        STRATA_CHECK(ct_equal(sig3, exp_sig3));
        STRATA_CHECK(Ed25519::verify(pk3, msg3, sig3));
    }

    // 5. Integrated CCSDS SDLS Engine with real AES-256-GCM + anti-replay
    {
        using namespace strata::services::comms;
        SdlsEngine<Aes256GcmSdlsBackend> sdls{};
        const auto key = from_hex<32>(
            "00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef");
        STRATA_CHECK(sdls.install_sa(7U, key) == Status::Ok);

        const auto aad = as_bytes_str("CCSDS-TC-HDR");
        const auto pt  = as_bytes_str("SET_INTEGRATION_TIME=4500us");
        std::array<std::byte, 128> enc{};
        const auto enc_len = sdls.protect(7U, aad, pt, enc);
        STRATA_CHECK(enc_len.has_value());

        std::array<std::byte, 128> dec{};
        const auto dec_len =
            sdls.unprotect(aad, std::span<const std::byte>{enc.data(), enc_len.value()}, dec);
        STRATA_CHECK(dec_len.has_value() && dec_len.value() == pt.size());
        STRATA_CHECK(ct_equal(std::span<const std::byte>{dec.data(), dec_len.value()}, pt));

        // Replay must be rejected
        STRATA_CHECK(sdls.unprotect(aad, std::span<const std::byte>{enc.data(), enc_len.value()}, dec).error() ==
               WireErr::ReplayDetected);
    }

    // 6. Integrated SignedAotModuleLoader (HMAC-SHA256) + Ed25519SlotImageAuth
    {
        using namespace strata::services::autonomy;
        const auto hmac_key = from_hex<32>(
            "a1b2c3d4e5f60718293a4b5c6d7e8f90112233445566778899aabbccddeeff00");
        constexpr uint64_t kHash = 0x1122'3344'5566'7788ULL;

        std::array<SandboxInstr, 2> prog{{
            {SandboxOp::ReadTlmGt, 0U, 0U, 42},
            {SandboxOp::Halt, 0U, 0U, 0},
        }};
        const auto code_bytes = std::as_bytes(std::span<const SandboxInstr>{prog});

        SignedAotModuleHeader hdr{};
        hdr.instruction_count = 2U;
        hdr.module_id         = 99U;
        hdr.manifest_hash     = kHash;
        hdr.code_crc32c       = Crc32c::compute(code_bytes);
        HmacSha256AotVerifier::compute_tag(hmac_key, hdr, code_bytes, hdr.signature_tag);

        std::array<std::byte, sizeof(SignedAotModuleHeader) + sizeof(prog)> img{};
        std::memcpy(img.data(), &hdr, sizeof(hdr));
        std::memcpy(img.data() + sizeof(hdr), code_bytes.data(), code_bytes.size());

        SignedAotModuleLoader<HmacSha256AotVerifier> loader{kHash, HmacSha256AotVerifier{hmac_key}};
        STRATA_CHECK(loader.load_from_image(img) == Status::Ok);

        // Tamper signature byte -> rejected
        img[sizeof(hdr) - 1U] ^= std::byte{0x01U};
        STRATA_CHECK(loader.load_from_image(img) == Status::PermissionDenied);

        // Ed25519SlotImageAuth signing & verification
        using namespace strata::services::fdir;
        const auto slot_seed = from_hex<32>(
            "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
        const auto slot_pk = Ed25519::derive_public_key(slot_seed);
        SignedSlotImage simg{};
        simg.image_id               = 3U;
        simg.expected_manifest_hash = kHash;
        simg.storage_addr           = 0x0400'0000ULL;
        simg.image_bytes            = 262'144ULL;
        simg.staged_in_verified_vol = true;
        Ed25519SlotImageAuth::sign_image(simg, slot_seed, slot_pk);
        STRATA_CHECK(Ed25519SlotImageAuth::verify_image(simg, slot_pk));
        simg.image_bytes += 4ULL;
        STRATA_CHECK(!Ed25519SlotImageAuth::verify_image(simg, slot_pk));
    }

    std::printf(
        "PASS: Hardware-grade crypto (FIPS 180-4 SHA-256/512, RFC 4231 HMAC-SHA256, "
        "NIST SP 800-38D AES-256-GCM, RFC 8032 Ed25519, SDLS/AOT/Slot auth) verified\n");
    return 0;
}
