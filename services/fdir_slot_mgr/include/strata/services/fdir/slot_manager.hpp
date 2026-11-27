#pragma once

#include <strata/core/expected.hpp>
#include <strata/core/types.hpp>
#include <strata/crypto/ed25519.hpp>
#include <strata/hyp/abi.hpp>
#include <strata/hw/slot_handle.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace strata::services::fdir {

/// Hardware slot target domain (Versal NoC INI DFX, Versal AIE Column, or Kintex ICAPE3 DFX).
enum class SlotFabricKind : uint8_t {
    PlStaticSpatial = 0U,
    VersalPlDfx     = 1U,
    VersalAieColumn = 2U,
    KintexPlDfx     = 3U,
};

/// Signed partial PDI / bitstream / AIE image descriptor staged in verified storage.
struct SignedSlotImage {
    uint32_t                   image_id{0U};
    uint64_t                   expected_manifest_hash{0U};
    uint64_t                   storage_addr{0U};
    uint64_t                   image_bytes{0U};
    std::array<std::byte, 48U> sha384_digest{};    // 384-bit SHA-384 digest
    std::array<std::byte, 64U> signature_prefix{}; // RSA-4096 / ECDSA-P384 signature handle/prefix
    bool                       staged_in_verified_vol{false};
};

/// RFC 8032 Ed25519 authentication helper for SignedSlotImage (bitstreams & AIE columns).
/// Packs canonical image metadata + digest into a deterministic 76-byte signed transcript.
struct Ed25519SlotImageAuth {
    static std::array<std::byte, 76U> canonical_transcript(const SignedSlotImage& img) noexcept {
        std::array<std::byte, 76U> msg{};
        std::memcpy(msg.data() + 0U, &img.image_id, 4U);
        std::memcpy(msg.data() + 4U, &img.expected_manifest_hash, 8U);
        std::memcpy(msg.data() + 12U, &img.storage_addr, 8U);
        std::memcpy(msg.data() + 20U, &img.image_bytes, 8U);
        std::memcpy(msg.data() + 28U, img.sha384_digest.data(), 48U);
        return msg;
    }

    static void sign_image(SignedSlotImage& img,
                           std::span<const std::byte, 32U> seed,
                           std::span<const std::byte, 32U> public_key) noexcept {
        const auto msg    = canonical_transcript(img);
        img.signature_prefix = strata::crypto::Ed25519::sign(seed, public_key, msg);
    }

    [[nodiscard]] static bool verify_image(const SignedSlotImage& img,
                                           std::span<const std::byte, 32U> public_key) noexcept {
        const auto msg = canonical_transcript(img);
        return strata::crypto::Ed25519::verify(public_key, msg, img.signature_prefix);
    }
};

/// Pluggable hardware/hypervisor backend concept for SlotManager.
template <typename T>
concept SlotControlBackend = requires(T& b, uint16_t slot_id, SlotFabricKind kind,
                                      const SignedSlotImage& img) {
    { b.verify_signature(img) } noexcept -> std::same_as<bool>;
    { b.quiesce_and_decouple(slot_id, kind) } noexcept -> std::same_as<Status>;
    { b.pause_scrubber(slot_id, kind) } noexcept -> std::same_as<Status>;
    { b.load_bitstream(slot_id, kind, img) } noexcept -> std::same_as<Status>;
    { b.reinit_scrubber(slot_id, kind) } noexcept -> std::same_as<Status>;
    { b.release_decoupler(slot_id, kind) } noexcept -> std::same_as<Status>;
    { b.run_sbit(slot_id) } noexcept -> std::same_as<Status>;
    { b.read_scsr_manifest_hash(slot_id) } noexcept -> std::same_as<uint64_t>;
    { b.set_slot_active(slot_id) } noexcept -> std::same_as<Status>;
};

/// ============================================================================
/// Authenticated End-to-End DFX & Versal AIE SlotManager (STRATA-FDIR-007)
/// Orchestrates Unloaded -> Loading -> Sbit -> Standby -> Active -> Quiescing -> Faulted
/// with cryptographic signature verification, bounded quiesce, NoC INI / DFX
/// Decoupler isolation, PMC PLM / ICAPE3 loading, XilSEM scrub pause/reinit,
/// and SCSR MANIFEST_HASH validation.
/// ============================================================================
template <SlotControlBackend Backend, size_t MaxSlots = 16U>
class SlotManager {
public:
    struct ManagedSlot {
        uint16_t       slot_id{0U};
        SlotFabricKind kind{SlotFabricKind::VersalPlDfx};
        SlotState      state{SlotState::Unloaded};
        GenId          gen{0U};
        uint32_t       active_image_id{0U};
        uint32_t       reconfig_count{0U};
        uint32_t       fault_count{0U};
        DurationNs     quiesce_timeout{DurationNs{500'000ULL}};
        bool           registered{false};
    };

    explicit SlotManager(Backend& backend) noexcept : backend_(&backend) {}

    [[nodiscard]] Status register_slot(uint16_t slot_id, SlotFabricKind kind,
                                       DurationNs quiesce_timeout = DurationNs{500'000ULL}) noexcept {
        for (auto& s : slots_) {
            if (!s.registered) {
                s = ManagedSlot{
                    .slot_id         = slot_id,
                    .kind            = kind,
                    .state           = SlotState::Unloaded,
                    .gen             = GenId{0U},
                    .active_image_id = 0U,
                    .reconfig_count  = 0U,
                    .fault_count     = 0U,
                    .quiesce_timeout = quiesce_timeout,
                    .registered      = true,
                };
                return Status::Ok;
            }
        }
        return Status::Full;
    }

    /// Perform authenticated end-to-end partial reconfiguration (or AIE column reload)
    /// transitioning safely through Quiescing -> Loading -> Sbit -> Standby.
    [[nodiscard]] Status load_and_verify(uint16_t slot_id, const SignedSlotImage& image) noexcept {
        auto* slot = find_slot(slot_id);
        if (slot == nullptr) return Status::InvalidArg;
        if (slot->kind == SlotFabricKind::PlStaticSpatial) return Status::PermissionDenied;

        // 1. Require staged verified volume & cryptographic SHA-384 / RSA-4096 / ECDSA-P384 check
        if (!image.staged_in_verified_vol || !backend_->verify_signature(image)) {
            ++slot->fault_count;
            return Status::PermissionDenied;
        }

        // 2. Bounded quiesce & hardware decoupling (Versal NoC INI or Kintex DFX Decoupler)
        slot->state = SlotState::Quiescing;
        if (const Status st = backend_->quiesce_and_decouple(slot_id, slot->kind); st != Status::Ok) {
            slot->state = SlotState::Faulted;
            ++slot->fault_count;
            return st;
        }

        // 3. Pause CRAM scrubber (XilSEM on Versal / external rad-hard scrubber on Kintex)
        if (const Status st = backend_->pause_scrubber(slot_id, slot->kind); st != Status::Ok) {
            slot->state = SlotState::Faulted;
            ++slot->fault_count;
            return st;
        }

        // 4. Load bitstream via PMC PLM (Versal) or DMA ICAPE3 (Kintex)
        slot->state = SlotState::Loading;
        if (const Status st = backend_->load_bitstream(slot_id, slot->kind, image); st != Status::Ok) {
            slot->state = SlotState::Faulted;
            ++slot->fault_count;
            (void)backend_->reinit_scrubber(slot_id, slot->kind);
            return st;
        }

        // 5. Reinitialize scrubber golden ECC/CRAM frames for the newly loaded RM
        if (const Status st = backend_->reinit_scrubber(slot_id, slot->kind); st != Status::Ok) {
            slot->state = SlotState::Faulted;
            ++slot->fault_count;
            return st;
        }

        // 6. Release hardware decoupler & verify live SCSR MANIFEST_HASH
        if (const Status st = backend_->release_decoupler(slot_id, slot->kind); st != Status::Ok) {
            slot->state = SlotState::Faulted;
            ++slot->fault_count;
            return st;
        }
        const uint64_t hw_hash = backend_->read_scsr_manifest_hash(slot_id);
        if (hw_hash != image.expected_manifest_hash) {
            slot->state = SlotState::Faulted;
            ++slot->fault_count;
            return Status::ManifestMismatch;
        }

        // 7. Execute Self Built-In Test (SBIT) before entering Standby
        slot->state = SlotState::Sbit;
        if (const Status st = backend_->run_sbit(slot_id); st != Status::Ok) {
            slot->state = SlotState::Faulted;
            ++slot->fault_count;
            return st;
        }

        slot->gen             = slot->gen.next();
        slot->active_image_id = image.image_id;
        slot->state           = SlotState::Standby;
        ++slot->reconfig_count;
        return Status::Ok;
    }

    /// Transition a verified Standby slot into Active science operation.
    [[nodiscard]] Status activate(uint16_t slot_id) noexcept {
        auto* slot = find_slot(slot_id);
        if (slot == nullptr) return Status::InvalidArg;
        if (slot->state != SlotState::Standby) return Status::PreconditionFailed;
        const Status st = backend_->set_slot_active(slot_id);
        if (st != Status::Ok) {
            slot->state = SlotState::Faulted;
            ++slot->fault_count;
            return st;
        }
        slot->state = SlotState::Active;
        return Status::Ok;
    }

    /// Quiesce an Active slot cleanly back to Standby.
    [[nodiscard]] Status quiesce(uint16_t slot_id) noexcept {
        auto* slot = find_slot(slot_id);
        if (slot == nullptr) return Status::InvalidArg;
        slot->state = SlotState::Quiescing;
        const Status st = backend_->quiesce_and_decouple(slot_id, slot->kind);
        if (st != Status::Ok) {
            slot->state = SlotState::Faulted;
            ++slot->fault_count;
            return st;
        }
        slot->state = SlotState::Standby;
        return Status::Ok;
    }

    [[nodiscard]] Expected<ManagedSlot, Status> inspect(uint16_t slot_id) const noexcept {
        for (const auto& s : slots_) {
            if (s.registered && s.slot_id == slot_id) return s;
        }
        return Unexpected(Status::InvalidArg);
    }

private:
    ManagedSlot* find_slot(uint16_t slot_id) noexcept {
        for (auto& s : slots_) {
            if (s.registered && s.slot_id == slot_id) return &s;
        }
        return nullptr;
    }

    Backend*                         backend_;
    std::array<ManagedSlot, MaxSlots> slots_{};
};

} // namespace strata::services::fdir
