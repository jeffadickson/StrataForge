#include <strata/hw/reg_map.hpp>
#include <strata/hw/slot_handle.hpp>
#include <strata/runtime/partition_runtime.hpp>

#include <cstdio>
#include <span>

namespace {

#define CHECK(cond)                                                                         \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            std::fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #cond);          \
            return 1;                                                                       \
        }                                                                                   \
    } while (false)

using RegExposureUs = strata::mmio::Reg<0x1000U, strata::mmio::RW, uint32_t>;

struct CmdSetExposure {
    uint32_t exposure_us{0U};
};

struct CmdResetCounters {
    uint32_t magic{0U};
};

struct DemoInstrument {
    uint32_t fast_ticks{0U};
    uint32_t slow_ticks{0U};
    uint32_t exposure_us{1000U};

    void step_fast(strata::TaiTimestamp) noexcept { ++fast_ticks; }
    void step_slow(strata::TaiTimestamp) noexcept { ++slow_ticks; }

    [[nodiscard]] strata::CmdResult on_set_exposure(const CmdSetExposure& c) noexcept {
        if (c.exposure_us < 100U || c.exposure_us > 50'000U) {
            return strata::CmdResult::InvalidArg;
        }
        exposure_us = c.exposure_us;
        return strata::CmdResult::Ok;
    }

    [[nodiscard]] strata::CmdResult on_reset(const CmdResetCounters&) noexcept {
        fast_ticks = 0U;
        slow_ticks = 0U;
        return strata::CmdResult::Ok;
    }
};

int test_scsr_and_slot_handle() {
    strata::mmio::MockRegisterBank<8192U> bank{};
    bank.mark_rw1c(strata::hw::scsr::RegFaultStatus::kOffset);

    constexpr uint64_t kManifestHash = 0xFDD1'4727'5E07'25EFULL;
    bank.force_word(strata::hw::scsr::RegMagicVer::kOffset, strata::hw::scsr::kMagic);
    bank.force_word(strata::hw::scsr::RegManifestHashLo::kOffset,
                    static_cast<uint32_t>(kManifestHash & 0xFFFF'FFFFULL));
    bank.force_word(strata::hw::scsr::RegManifestHashHi::kOffset,
                    static_cast<uint32_t>(kManifestHash >> 32U));
    bank.force_word(strata::hw::scsr::RegState::kOffset,
                    static_cast<uint32_t>(strata::SlotState::Standby));
    bank.force_word(strata::hw::scsr::RegFaultStatus::kOffset,
                    strata::hw::scsr::kFaultAxiErr | strata::hw::scsr::kFaultFifoOvf);
    bank.force_word(strata::hw::scsr::RegTaiLo::kOffset, 0x0000'1234U);
    bank.force_word(strata::hw::scsr::RegTaiHi::kOffset, 0x0000'0005U);

    strata::hw::SlotHandle slot(bank, 0U, 50'000U);
    CHECK(slot.verify_manifest_hash(kManifestHash));
    CHECK(!slot.verify_manifest_hash(kManifestHash ^ 1ULL));
    CHECK(slot.state() == strata::SlotState::Standby);
    CHECK(slot.read_tai().ns() == ((5ULL << 32U) | 0x1234ULL));

    // RW1C clears only requested bit
    slot.clear_faults(strata::hw::scsr::kFaultAxiErr);
    CHECK(slot.fault_status() == strata::hw::scsr::kFaultFifoOvf);

    // Instrument-specific register access + WDT kick + command
    slot.write_reg<RegExposureUs>(12'500U);
    CHECK(slot.read_reg<RegExposureUs>() == 12'500U);
    slot.kick_watchdog();
    CHECK(bank.read32(strata::hw::scsr::RegWdtReload::kOffset) == 50'000U);
    slot.command(strata::SlotCmd::StartActive);
    CHECK(bank.read32(strata::hw::scsr::RegCmd::kOffset) ==
          static_cast<uint32_t>(strata::SlotCmd::StartActive));
    return 0;
}

int test_partition_runtime() {
    DemoInstrument inst{};

    // RateGroup (period=1, phase=0) + PhasedWindow (period=5, phase=2)
    strata::runtime::CyclicScheduler sched(
        strata::runtime::ScheduledTask<1U, 0U, DemoInstrument, &DemoInstrument::step_fast>{&inst},
        strata::runtime::ScheduledTask<5U, 2U, DemoInstrument, &DemoInstrument::step_slow>{&inst});

    for (uint64_t t = 0U; t < 10U; ++t) {
        sched.tick(strata::TaiTimestamp{t * 10'000'000ULL});
    }
    CHECK(inst.fast_ticks == 10U);
    CHECK(inst.slow_ticks == 2U); // Fired at ticks 2 and 7

    // Compile-time CommandRouter (duplicate opcodes fail static_assert)
    strata::runtime::CommandRouter router(
        strata::runtime::CommandRoute<0x0101U, CmdSetExposure, DemoInstrument,
                                      &DemoInstrument::on_set_exposure>{&inst},
        strata::runtime::CommandRoute<0x0102U, CmdResetCounters, DemoInstrument,
                                      &DemoInstrument::on_reset>{&inst});

    const CmdSetExposure ok_cmd{.exposure_us = 2500U};
    CHECK(router.dispatch(0x0101U, std::as_bytes(std::span<const CmdSetExposure, 1>{&ok_cmd, 1U})) ==
          strata::CmdResult::Ok);
    CHECK(inst.exposure_us == 2500U);

    const CmdSetExposure bad_cmd{.exposure_us = 10U};
    CHECK(router.dispatch(0x0101U,
                          std::as_bytes(std::span<const CmdSetExposure, 1>{&bad_cmd, 1U})) ==
          strata::CmdResult::InvalidArg);

    CHECK(router.dispatch(0x9999U, {}) == strata::CmdResult::UnknownOpcode);

    const CmdResetCounters rst{.magic = 1U};
    CHECK(router.dispatch(0x0102U,
                          std::as_bytes(std::span<const CmdResetCounters, 1>{&rst, 1U})) ==
          strata::CmdResult::Ok);
    CHECK(inst.fast_ticks == 0U && inst.slow_ticks == 0U);
    return 0;
}

} // namespace

int main() {
    if (test_scsr_and_slot_handle() != 0) return 1;
    if (test_partition_runtime() != 0) return 1;
    std::printf("PASS: strata.unit.hw_and_runtime\n");
    return 0;
}
