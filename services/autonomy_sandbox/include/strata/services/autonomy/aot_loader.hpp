#pragma once

#include <strata/core/crc32c.hpp>
#include <strata/core/expected.hpp>
#include <strata/core/types.hpp>
#include <strata/services/autonomy/sandbox_runner.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace strata::services::autonomy {

/// Binary header for a cryptographically signed Ahead-of-Time (AOT) autonomy module
/// (Wasm-AOT or bounded bytecode) loaded from a jailed storage volume without JIT or heap.
struct SignedAotModuleHeader {
    static constexpr uint32_t kMagic = 0x5354'414FU; // "STAO"

    uint32_t                   magic{kMagic};
    uint16_t                   abi_version{1U};
    uint16_t                   instruction_count{0U};
    uint32_t                   module_id{0U};
    uint32_t                   max_steps_bound{1024U};
    uint64_t                   manifest_hash{0U};
    uint32_t                   code_crc32c{0U};
    uint32_t                   reserved{0U};
    std::array<std::byte, 32U> signature_tag{}; // Hardware/PKI verified signature digest
};
static_assert(sizeof(SignedAotModuleHeader) == 64U);

/// Pluggable cryptographic signature verifier for AOT autonomy modules.
template <typename T>
concept AotSignatureVerifier = requires(const T& v, const SignedAotModuleHeader& hdr,
                                        std::span<const std::byte> code_bytes) {
    { v.verify(hdr, code_bytes) } noexcept -> std::same_as<bool>;
};

/// Reference host-SIL keyed signature verifier (replace with PMC/PKI ECDSA-P384 in flight).
struct ReferenceAotVerifier {
    uint64_t trusted_key_seed{0xA5A5'5A5A'1357'9BDFULL};

    static void compute_tag(uint64_t key_seed, const SignedAotModuleHeader& hdr,
                            std::span<const std::byte> code_bytes,
                            std::span<std::byte, 32U>  out_tag) noexcept {
        uint32_t c = Crc32c::compute(std::as_bytes(std::span<const uint64_t, 1>{&key_seed, 1U}));
        c = Crc32c::update(c, std::as_bytes(std::span<const uint64_t, 1>{&hdr.manifest_hash, 1U}));
        c = Crc32c::update(c, code_bytes);
        uint64_t state = key_seed ^ (static_cast<uint64_t>(c) << 32U) ^ hdr.module_id;
        for (size_t i = 0U; i < 4U; ++i) {
            state += 0x9E37'79B9'7F4A'7C15ULL;
            state = (state ^ (state >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
            state = (state ^ (state >> 27U)) * 0x94D0'49BB'1331'11EBULL;
            state = state ^ (state >> 31U);
            std::memcpy(out_tag.data() + i * 8U, &state, 8U);
        }
    }

    [[nodiscard]] bool verify(const SignedAotModuleHeader& hdr,
                              std::span<const std::byte>   code_bytes) const noexcept {
        std::array<std::byte, 32U> expected{};
        compute_tag(trusted_key_seed, hdr, code_bytes, expected);
        uint8_t diff = 0U;
        for (size_t i = 0U; i < 32U; ++i) {
            diff |= static_cast<uint8_t>(std::to_integer<uint8_t>(expected[i]) ^
                                         std::to_integer<uint8_t>(hdr.signature_tag[i]));
        }
        return diff == 0U;
    }
};
static_assert(AotSignatureVerifier<ReferenceAotVerifier>);

/// ============================================================================
/// Cryptographically Signed AOT Autonomy Module Loader (STRATA-AUTO-002)
/// Verifies header magic, MANIFEST_HASH, CRC-32C, cryptographic signature, and
/// static instruction-slot bounds before staging executable instructions into a
/// fixed zero-heap buffer ready for AutonomySandbox execution.
/// ============================================================================
template <AotSignatureVerifier Verifier = ReferenceAotVerifier, size_t MaxInstructions = 128U>
class SignedAotModuleLoader {
public:
    explicit SignedAotModuleLoader(uint64_t expected_manifest_hash, Verifier verifier = {}) noexcept
        : expected_manifest_hash_(expected_manifest_hash), verifier_(verifier) {}

    /// Parse, verify, and load a signed AOT binary blob from a jailed storage volume.
    [[nodiscard]] Status load_from_image(std::span<const std::byte> raw_image) noexcept {
        loaded_ = false;
        if (raw_image.size() < sizeof(SignedAotModuleHeader)) {
            return Status::PayloadSizeError;
        }
        SignedAotModuleHeader hdr{};
        std::memcpy(&hdr, raw_image.data(), sizeof(hdr));
        if (hdr.magic != SignedAotModuleHeader::kMagic || hdr.abi_version != 1U) {
            return Status::ValidationFailed;
        }
        if (hdr.manifest_hash != expected_manifest_hash_) {
            return Status::ManifestMismatch;
        }
        if (hdr.instruction_count == 0U || hdr.instruction_count > MaxInstructions) {
            return Status::PayloadSizeError;
        }
        const size_t expected_code_bytes =
            static_cast<size_t>(hdr.instruction_count) * sizeof(SandboxInstr);
        if (raw_image.size() != sizeof(SignedAotModuleHeader) + expected_code_bytes) {
            return Status::PayloadSizeError;
        }

        const auto code_bytes = raw_image.subspan(sizeof(SignedAotModuleHeader), expected_code_bytes);
        if (Crc32c::compute(code_bytes) != hdr.code_crc32c) {
            return Status::CrcMismatch;
        }
        if (!verifier_.verify(hdr, code_bytes)) {
            return Status::PermissionDenied;
        }

        std::memcpy(code_buf_.data(), code_bytes.data(), expected_code_bytes);
        active_hdr_ = hdr;
        loaded_     = true;
        return Status::Ok;
    }

    [[nodiscard]] bool is_loaded() const noexcept { return loaded_; }
    [[nodiscard]] const SignedAotModuleHeader& header() const noexcept { return active_hdr_; }

    [[nodiscard]] std::span<const SandboxInstr> loaded_program() const noexcept {
        if (!loaded_) return {};
        return std::span<const SandboxInstr>{code_buf_.data(), active_hdr_.instruction_count};
    }

private:
    uint64_t                                 expected_manifest_hash_{0U};
    [[no_unique_address]] Verifier           verifier_;
    SignedAotModuleHeader                    active_hdr_{};
    std::array<SandboxInstr, MaxInstructions> code_buf_{};
    bool                                     loaded_{false};
};

} // namespace strata::services::autonomy
