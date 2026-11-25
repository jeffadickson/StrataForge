#pragma once

#include <cstddef>
#include <cstdint>

namespace strata::hyp {

/// Canonical STRATA-20 Bare-Metal Hypervisor ABI Version (8-hypcall Class B TCB surface)
inline constexpr uint16_t kAbiMajor = 0U;
inline constexpr uint16_t kAbiMinor = 1U;

/// Minimal auditable hypercall surface (EL1->EL2 HVC, R5F SVC, MicroBlaze trap, or Host SIL mock).
enum class HypCall : uint32_t {
    Yield       = 0U, // Voluntarily yield remaining minor-frame CPU slice
    WaitEvent   = 1U, // Block until doorbell channel, assigned IRQ, or TAI deadline
    Notify      = 2U, // Rate-limited doorbell notify on statically authorized SPSC channel
    TimeRead    = 3U, // Fallback TAI read (fast path reads RO Shared Time Page zero-trap)
    FaultReport = 4U, // Self-report assertion/SBIT fault to Class B FdirEngine
    SlotOp      = 5U, // [Privileged Class B] Quiesce, decouple, authenticated DFX/AIE reload
    PartCtl     = 6U, // [Privileged Class B] Warm/cold restart partition & increment generation
    DiagTrace   = 7U, // Bounded write to hardware-protected flight recorder ring
};

/// Privileged hardware slot operations issued only by Class B SlotManager / FdirEngine.
enum class SlotOpCode : uint32_t {
    Quiesce        = 0U,
    ForceDecouple  = 1U,
    LoadPartialPdi = 2U,
    ReleaseSlot    = 3U,
    AieColumnReset = 4U,
    AieColumnLoad  = 5U,
};

/// Privileged partition lifecycle actions (ARINC 653 HM + Oxide Hubris generation restart).
enum class PartCtlOp : uint32_t {
    Halt        = 0U,
    WarmRestart = 1U, // Preserves CriticalStore<T>, scrubs stack/.bss, increments GenId
    ColdRestart = 2U, // Clears CriticalStore<T> + stack/.bss, increments GenId
    SetIdle     = 3U,
};

/// Read-Only Shared Time Page updated by the static hardware 1PPS + TAI shell.
struct alignas(64) SharedTimePage {
    volatile uint32_t seq;          // Seqlock sequence (even = stable, odd = updating)
    uint32_t          epoch;        // Monotonic time-sync epoch counter
    uint64_t          tai_ns;       // Hardware-synchronized TAI nanoseconds
    uint64_t          pps_count;    // Cumulative 1PPS hardware pulses observed
    uint64_t          tick_hz;      // Hardware counter frequency for sub-tick interpolation
    uint8_t           reserved[32];
};

static_assert(sizeof(SharedTimePage) == 64U, "SharedTimePage must occupy exactly one 64B cache line");

} // namespace strata::hyp
