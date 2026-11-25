#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>

namespace strata {

/// Strongly typed nanosecond TAI timestamp (prevents raw uint64_t unit mix-ups).
struct TaiTimestamp {
    uint64_t value_ns{0U};

    [[nodiscard]] constexpr uint64_t ns() const noexcept { return value_ns; }
    [[nodiscard]] constexpr auto operator<=>(const TaiTimestamp&) const noexcept = default;
};

/// Strongly typed nanosecond duration.
struct DurationNs {
    uint64_t value_ns{0U};

    [[nodiscard]] constexpr uint64_t ns() const noexcept { return value_ns; }
    [[nodiscard]] constexpr auto operator<=>(const DurationNs&) const noexcept = default;
};

[[nodiscard]] constexpr TaiTimestamp operator+(TaiTimestamp t, DurationNs d) noexcept {
    return TaiTimestamp{t.value_ns + d.value_ns};
}

[[nodiscard]] constexpr DurationNs elapsed_since(TaiTimestamp now, TaiTimestamp earlier) noexcept {
    return (now.value_ns >= earlier.value_ns) ? DurationNs{now.value_ns - earlier.value_ns}
                                              : DurationNs{0U};
}

namespace literals {
[[nodiscard]] consteval DurationNs operator""_ns(unsigned long long v) noexcept {
    return DurationNs{static_cast<uint64_t>(v)};
}
[[nodiscard]] consteval DurationNs operator""_us(unsigned long long v) noexcept {
    return DurationNs{static_cast<uint64_t>(v) * 1'000ULL};
}
[[nodiscard]] consteval DurationNs operator""_ms(unsigned long long v) noexcept {
    return DurationNs{static_cast<uint64_t>(v) * 1'000'000ULL};
}
[[nodiscard]] consteval DurationNs operator""_s(unsigned long long v) noexcept {
    return DurationNs{static_cast<uint64_t>(v) * 1'000'000'000ULL};
}
} // namespace literals

/// Monotonic generation counter incremented on every partition or PL/AIE slot restart (Hubris pattern).
struct GenId {
    uint16_t value{0U};

    [[nodiscard]] constexpr auto operator<=>(const GenId&) const noexcept = default;
    constexpr GenId next() const noexcept { return GenId{static_cast<uint16_t>(value + 1U)}; }
};

/// NASA NPR 7150.2D / mixed-criticality classification.
enum class Criticality : uint8_t {
    ClassB = 0U, // Flight-critical TCB (C&DH, Comms, FDIR, Hypervisor, SlotManager)
    ClassC = 1U, // Mission-critical data handling / primary science
    ClassD = 2U, // Lower-criticality payload / experimental autonomy
};

/// ARINC 653-aligned partition boot & operating modes.
enum class PartitionMode : uint8_t {
    ColdStart = 0U,
    WarmStart = 1U, // Triggered after fault recovery with CriticalStore<T> preserved
    Normal    = 2U,
    Idle      = 3U,
};

/// Unified hardware slot lifecycle spanning static spatial PL, dynamic DFX PL, and Versal AIE columns.
enum class SlotState : uint32_t {
    Unloaded  = 0U,
    Loading   = 1U,
    Sbit      = 2U,
    Standby   = 3U,
    Active    = 4U,
    Quiescing = 5U,
    Faulted   = 6U,
};

/// Uniform SCSR commands sent to any hardware slot.
enum class SlotCmd : uint32_t {
    None         = 0U,
    RunSbit      = 1U,
    EnterStandby = 2U,
    StartActive  = 3U,
    Quiesce      = 4U,
    SoftReset    = 5U,
};

/// Standard F' / ECSS PUS event severity levels.
enum class EventSeverity : uint8_t {
    Diagnostic = 0U,
    Command    = 1U,
    ActivityLo = 2U,
    ActivityHi = 3U,
    WarningLo  = 4U,
    WarningHi  = 5U,
    Fatal      = 6U,
};

/// Framework-wide deterministic status codes (zero exceptions).
enum class Status : uint16_t {
    Ok                 = 0U,
    Empty              = 1U,
    Full               = 2U,
    NoDataYet          = 3U,
    StaleGeneration    = 4U,
    DeadlineMissed     = 5U,
    TornRead           = 6U,
    CrcMismatch        = 7U,
    ManifestMismatch   = 8U,
    InvalidArg         = 9U,
    PermissionDenied   = 10U,
    SlotFaulted        = 11U,
    Timeout            = 12U,
    ValidationFailed   = 13U,
    NoPreviousBank     = 14U,
    UnhandledEvent     = 15U,
    GuardRejected      = 16U,
    UnknownOpcode      = 17U,
    PayloadSizeError   = 18U,
    PreconditionFailed = 19U,
    Busy               = 20U,
    InProgress         = 21U,
};

/// 4-Stage ECSS PUS ST[01] command verification states.
enum class CmdStage : uint8_t {
    Accepted  = 0U,
    Started   = 1U,
    Progress  = 2U,
    Completed = 3U,
};

/// Command execution result (enforced [[nodiscard]] across handlers).
enum class CmdResult : uint16_t {
    Ok                 = 0U,
    InProgress         = 1U,
    InvalidArg         = 2U,
    Denied             = 3U,
    Busy               = 4U,
    PreconditionFailed = 5U,
    SlotFaulted        = 6U,
    UnknownOpcode      = 7U,
    ExecutionError     = 8U,
};

/// Canonical 32-byte Zero-Copy Data Plane Descriptor (2 descriptors per 64B cache line).
struct alignas(32) FrameDesc {
    uint64_t phys_addr{0U};      // Physical / IPA address inside XMPU-scoped DDR4/HBM pool
    uint64_t tai_ns{0U};         // Hardware-latched TAI timestamp at sensor capture
    uint32_t length_bytes{0U};   // Valid payload bytes written by PL/AIE DMA
    uint16_t pool_slot_idx{0U};  // Static buffer slot index for O(1) free-return
    uint16_t src_slot_id{0U};    // Originating PL/AIE slot ID
    uint16_t producer_gen{0U};   // Incremented on every slot/partition restart
    uint16_t flags{0U};          // Bit 0: CRC_OK, Bit 1: ECC_CE, Bit 2: EOF, Bit 3: CALIB
    uint32_t crc32c{0U};         // Optional hardware CRC-32C
};

static_assert(sizeof(FrameDesc) == 32U, "FrameDesc must be exactly 32 bytes");
static_assert(alignof(FrameDesc) == 32U, "FrameDesc must be 32-byte aligned");

/// Canonical 128-byte Cache-Line-Isolated Lock-Free SPSC Ring Header (2 x 64B cache lines).
struct alignas(64) SpscHeader {
    // Cache Line 0: Producer-owned
    uint32_t head{0U};
    uint32_t prod_gen{0U};
    uint32_t ring_mask{0U}; // depth - 1 (depth must be power of two)
    uint32_t elem_size{0U};
    uint8_t  pad0[48]{};

    // Cache Line 1: Consumer-owned
    uint32_t tail{0U};
    uint32_t notify_armed{0U}; // Interrupt coalescing flag
    uint32_t cons_gen{0U};
    uint32_t drop_count{0U};
    uint8_t  pad1[48]{};
};

static_assert(sizeof(SpscHeader) == 128U, "SpscHeader must span exactly two 64B cache lines");

} // namespace strata
