#pragma once

#include <strata/core/crc32c.hpp>
#include <strata/core/expected.hpp>
#include <strata/core/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace strata::services::cdh {

/// Authorized memory span entry (prevents MemDwell / MemPatch bus aborts on unmapped MMIO).
struct MemoryRegionRule {
    uintptr_t base{0U};
    size_t    size_bytes{0U};
    bool      writable{false};
    bool      enabled{false};
};

/// On-orbit Memory Dwell, authenticated two-step Memory Patch, and background
/// software checksum scrubbing over static code/data segments.
template <size_t MaxRegions = 16U, size_t MaxChecksumSegments = 8U>
class FlightMemoryOps {
public:
    [[nodiscard]] Status allow_region(uintptr_t base, size_t size_bytes, bool writable) noexcept {
        for (auto& r : regions_) {
            if (!r.enabled) {
                r = MemoryRegionRule{
                    .base       = base,
                    .size_bytes = size_bytes,
                    .writable   = writable,
                    .enabled    = true,
                };
                return Status::Ok;
            }
        }
        return Status::Full;
    }

    /// Memory Dwell: safely sample an authorized address range into a telemetry buffer.
    [[nodiscard]] Expected<size_t, Status>
    dwell_read(uintptr_t addr, size_t len, std::span<std::byte> dst) const noexcept {
        if (len == 0U || dst.size() < len) {
            return Unexpected(Status::InvalidArg);
        }
        if (!is_range_allowed(addr, len, false)) {
            return Unexpected(Status::PermissionDenied);
        }
        std::memcpy(dst.data(), reinterpret_cast<const void*>(addr), len);
        return len;
    }

    /// Memory Patch Step 1: Arm a surgical patch (verifies write allowlist + expected CRC).
    [[nodiscard]] Status arm_patch(uintptr_t addr, std::span<const std::byte> data,
                                   uint32_t expected_crc) noexcept {
        if (data.empty() || data.size() > kMaxPatchBytes) {
            return Status::InvalidArg;
        }
        if (!is_range_allowed(addr, data.size(), true)) {
            return Status::PermissionDenied;
        }
        if (Crc32c::compute(data) != expected_crc) {
            return Status::CrcMismatch;
        }
        armed_patch_.armed = true;
        armed_patch_.addr  = addr;
        armed_patch_.len   = data.size();
        armed_patch_.crc   = expected_crc;
        std::memcpy(armed_patch_.buf.data(), data.data(), data.size());
        return Status::Ok;
    }

    /// Memory Patch Step 2: Execute the armed patch only if confirm CRC matches arm token.
    [[nodiscard]] Status commit_patch(uint32_t confirm_crc) noexcept {
        if (!armed_patch_.armed) {
            return Status::PreconditionFailed;
        }
        if (confirm_crc != armed_patch_.crc ||
            Crc32c::compute(std::span<const std::byte>(armed_patch_.buf.data(), armed_patch_.len)) !=
                armed_patch_.crc) {
            armed_patch_.armed = false;
            return Status::CrcMismatch;
        }
        std::memcpy(reinterpret_cast<void*>(armed_patch_.addr), armed_patch_.buf.data(),
                    armed_patch_.len);
        armed_patch_.armed = false;
        ++patches_applied_;
        return Status::Ok;
    }

    /// Background Checksum: register immutable .text / .rodata / table segment to scrub.
    [[nodiscard]] Status register_checksum_segment(uint16_t seg_id,
                                                   std::span<const std::byte> segment) noexcept {
        for (auto& s : segments_) {
            if (!s.enabled) {
                s.enabled      = true;
                s.seg_id       = seg_id;
                s.span         = segment;
                s.baseline_crc = Crc32c::compute(segment);
                return Status::Ok;
            }
        }
        return Status::Full;
    }

    /// Scrub one registered software segment per background tick; returns corrupted seg_id if SEU hit!
    [[nodiscard]] Expected<void, uint16_t> scrub_next_segment() noexcept {
        for (size_t i = 0U; i < MaxChecksumSegments; ++i) {
            const size_t idx = (scrub_cursor_ + i) % MaxChecksumSegments;
            const auto&  seg = segments_[idx];
            if (!seg.enabled) continue;
            scrub_cursor_ = (idx + 1U) % MaxChecksumSegments;
            if (Crc32c::compute(seg.span) != seg.baseline_crc) {
                ++scrub_mismatches_;
                return Unexpected(seg.seg_id);
            }
            return {};
        }
        return {};
    }

    [[nodiscard]] uint32_t patches_applied() const noexcept { return patches_applied_; }
    [[nodiscard]] uint32_t scrub_mismatches() const noexcept { return scrub_mismatches_; }

private:
    static constexpr size_t kMaxPatchBytes = 256U;

    struct ArmedPatch {
        bool                              armed{false};
        uintptr_t                         addr{0U};
        size_t                            len{0U};
        uint32_t                          crc{0U};
        std::array<std::byte, kMaxPatchBytes> buf{};
    };

    struct ChecksumSegment {
        bool                       enabled{false};
        uint16_t                   seg_id{0U};
        std::span<const std::byte> span{};
        uint32_t                   baseline_crc{0U};
    };

    [[nodiscard]] bool is_range_allowed(uintptr_t addr, size_t len,
                                        bool require_write) const noexcept {
        if (addr > UINTPTR_MAX - len) return false;
        const uintptr_t end = addr + len;
        for (const auto& r : regions_) {
            if (!r.enabled) continue;
            if (require_write && !r.writable) continue;
            if (addr >= r.base && end <= r.base + r.size_bytes) {
                return true;
            }
        }
        return false;
    }

    std::array<MemoryRegionRule, MaxRegions>           regions_{};
    ArmedPatch                                         armed_patch_{};
    std::array<ChecksumSegment, MaxChecksumSegments>   segments_{};
    size_t                                             scrub_cursor_{0U};
    uint32_t                                           patches_applied_{0U};
    uint32_t                                           scrub_mismatches_{0U};
};

/// Build Version & Live FPGA Manifest Identity Reporter.
struct BuildManifestInfo {
    uint64_t manifest_hash{0U};
    uint32_t sw_git_sha_short{0U};
    uint16_t abi_major{0U};
    uint16_t abi_minor{1U};
    uint32_t active_slot_mask{0U};
};

} // namespace strata::services::cdh
