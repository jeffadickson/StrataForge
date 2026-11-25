#pragma once

#include <strata/core/types.hpp>
#include <strata/hw/reg_map.hpp>

#include <cstdint>

namespace strata::hw {

/// Standard 4 KB Slot Control & Status Register (SCSR) Contract.
/// Bit-exact mirror of `rtl/common/scsr_defs.svh`.
namespace scsr {

inline constexpr uint32_t kMagic = 0x5354'5241U; // "STRA"

using RegMagicVer       = mmio::Reg<0x000U, mmio::RO, uint32_t>;
using RegManifestHashLo = mmio::Reg<0x004U, mmio::RO, uint32_t>;
using RegManifestHashHi = mmio::Reg<0x008U, mmio::RO, uint32_t>;
using RegSlotCap        = mmio::Reg<0x00CU, mmio::RO, uint32_t>;
using RegState          = mmio::Reg<0x010U, mmio::RO, SlotState>;
using RegCmd            = mmio::Reg<0x014U, mmio::WO, SlotCmd>;
using RegFaultStatus    = mmio::Reg<0x018U, mmio::RW1C, uint32_t>;
using RegIrqMask        = mmio::Reg<0x01CU, mmio::RW, uint32_t>;
using RegWdtReload      = mmio::Reg<0x020U, mmio::RW, uint32_t>;
using RegTaiLo          = mmio::Reg<0x024U, mmio::RO, uint32_t>;
using RegTaiHi          = mmio::Reg<0x028U, mmio::RO, uint32_t>;
using RegDmaTxDescBase  = mmio::Reg<0x040U, mmio::RW, uint32_t>;

/// Standard fault status bit masks in SCSR.FAULT_STATUS (0x018).
inline constexpr uint32_t kFaultAxiErr    = 1U << 0U;
inline constexpr uint32_t kFaultFifoOvf   = 1U << 1U;
inline constexpr uint32_t kFaultEccUe     = 1U << 2U;
inline constexpr uint32_t kFaultWdtExp    = 1U << 3U;
inline constexpr uint32_t kFaultCrcFail   = 1U << 4U;
inline constexpr uint32_t kFaultPllUnlock = 1U << 5U;

} // namespace scsr

/// Unified Hardware Slot Handle (Primitive 6) spanning Static Spatial PL, DFX PL, and Versal AIE.
template <RegisterTransport Transport>
class SlotHandle {
public:
    explicit SlotHandle(Transport& bus, uint16_t slot_id, uint32_t wdt_ticks = 100'000U) noexcept
        : bus_(&bus), slot_id_(slot_id), wdt_ticks_(wdt_ticks) {}

    /// Verify SCSR magic signature and 64-bit compile-time MANIFEST_HASH match RTL bitstream.
    [[nodiscard]] bool verify_manifest_hash(uint64_t expected_hash) const noexcept {
        if (scsr::RegMagicVer::read(*bus_) != scsr::kMagic) {
            return false;
        }
        const uint64_t lo = scsr::RegManifestHashLo::read(*bus_);
        const uint64_t hi = scsr::RegManifestHashHi::read(*bus_);
        const uint64_t hw_hash = (hi << 32U) | lo;
        return hw_hash == expected_hash;
    }

    [[nodiscard]] SlotState state() const noexcept { return scsr::RegState::read(*bus_); }
    [[nodiscard]] uint32_t fault_status() const noexcept { return scsr::RegFaultStatus::read(*bus_); }
    [[nodiscard]] uint16_t slot_id() const noexcept { return slot_id_; }

    void command(SlotCmd cmd) noexcept { scsr::RegCmd::write(*bus_, cmd); }
    void clear_faults(uint32_t mask) noexcept { scsr::RegFaultStatus::clear_bits(*bus_, mask); }
    void kick_watchdog() noexcept { scsr::RegWdtReload::write(*bus_, wdt_ticks_); }

    /// Read hardware-latched 64-bit TAI timestamp from SCSR (high-low-high stability check).
    [[nodiscard]] TaiTimestamp read_tai() const noexcept {
        const uint64_t hi1 = scsr::RegTaiHi::read(*bus_);
        const uint64_t lo  = scsr::RegTaiLo::read(*bus_);
        const uint64_t hi2 = scsr::RegTaiHi::read(*bus_);
        const uint64_t hi  = (hi1 == hi2) ? hi1 : hi2;
        return TaiTimestamp{(hi << 32U) | lo};
    }

    /// Access instrument-specific registers (offset >= 0x1000) with compile-time RO/WO/RW checks.
    template <typename RegT>
    [[nodiscard]] auto read_reg() const noexcept {
        return RegT::read(*bus_);
    }

    template <typename RegT, typename ValT>
    void write_reg(ValT value) noexcept {
        RegT::write(*bus_, value);
    }

private:
    Transport* bus_;
    uint16_t   slot_id_;
    uint32_t   wdt_ticks_;
};

} // namespace strata::hw
