#include <strata/hw/shadow_replay.hpp>
#include <strata/hw/slot_handle.hpp>
#include "verilator_transport.hpp"

#include "Vkintex_platform_shell.h"
#include "Vversal_platform_shell.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

namespace {

/// Same MANIFEST_HASH configured as Verilog parameters at Verilation time.
constexpr uint64_t kExpectedManifestHash = 0x14A6'F9D1'9243'B827ULL;

/// Run unmodified flight bring-up sequence against any RegisterTransport (used for live RTL + offline replay).
template <strata::RegisterTransport Bus>
void run_flight_slot_bringup(strata::hw::SlotHandle<Bus>& slot) {
    CHECK(slot.verify_manifest_hash(kExpectedManifestHash));
    CHECK(slot.state() == strata::SlotState::Standby);

    slot.command(strata::SlotCmd::RunSbit);
    CHECK(slot.state() == strata::SlotState::Sbit);

    slot.command(strata::SlotCmd::EnterStandby);
    CHECK(slot.state() == strata::SlotState::Standby);

    // Program short watchdog window (12 clocks) and transition to Active
    slot.kick_watchdog();
    slot.command(strata::SlotCmd::StartActive);
    CHECK(slot.state() == strata::SlotState::Active);
}

} // namespace

int main() {
    using namespace strata;

    // =========================================================================
    // 1. Live Verilator Co-Simulation: Versal Platform Shell
    //    (scsr_slave + sg_dma_shaper + XilSEM + NoC INI DFX + AIE Isolation)
    // =========================================================================
    {
        Vversal_platform_shell versal{};
        versal.tai_ns_i                = 0ULL;
        versal.pps_strobe_i            = 0U;
        versal.sensor_capture_strobe_i = 0U;
        versal.dfx_reconfig_req_i      = 0U;
        versal.xilsem_paused_i         = 0U;
        versal.xilsem_uncorr_err_i     = 0U;
        versal.aie_col_reset_req_i     = 0U;
        versal.desc_in_valid_i         = 0U;
        versal.noc_wr_ready_i          = 1U;
        versal.desc_out_ready_i        = 1U;

        hil::VerilatorAxiLiteTransport<Vversal_platform_shell> raw_bus(versal);
        raw_bus.reset(4U);

        // Wrap Verilator bus in ShadowRecordingTransport for bit-exact offline replay (STRATA-VER-003)
        hw::ShadowRecordingTransport<hil::VerilatorAxiLiteTransport<Vversal_platform_shell>, 128U>
            rec_bus(raw_bus);
        hw::SlotHandle<decltype(rec_bus)> slot(rec_bus, /*slot_id=*/0U, /*wdt_ticks=*/40U);

        // Execute unmodified flight C++20 bring-up against real SystemVerilog RTL!
        run_flight_slot_bringup(slot);

        // Hardware 1PPS TAI timestamp strobe latching (STRATA-RTL-002)
        constexpr uint64_t kLatchedTai = 0x0000'1234'5678'9ABCULL;
        versal.tai_ns_i                = kLatchedTai;
        versal.pps_strobe_i            = 1U;
        raw_bus.step_clock(1U);
        versal.pps_strobe_i = 0U;
        CHECK(slot.read_tai().ns() == kLatchedTai);

        // 32-byte FrameDesc Scatter-Gather DMA + Credit Bandwidth Shaper (STRATA-RTL-003)
        // Submit a 512-byte transfer (32 beats of 16B) exceeding CREDIT_MAX=16 to trigger shaping
        FrameDesc in_desc{};
        in_desc.phys_addr     = 0x0000'0008'0000'1000ULL;
        in_desc.tai_ns        = kLatchedTai;
        in_desc.length_bytes  = 512U;
        in_desc.pool_slot_idx = 5U;
        in_desc.src_slot_id   = 0U;
        in_desc.producer_gen  = 1U;
        in_desc.flags         = 0U;
        in_desc.crc32c        = 0xCAFE'BABEU;

        uint32_t words[8]{};
        std::memcpy(words, &in_desc, sizeof(words));
        for (int i = 0; i < 8; ++i) {
            versal.desc_in_data_i[i] = words[i];
        }
        // Reload WDT with 400 ticks before asserting desc_in_valid_i so DMA starts cleanly on cycle 0
        rec_bus.write32(0x020U, 400U);
        versal.desc_in_valid_i = 1U;

        uint32_t beats_seen      = 0U;
        uint32_t total_cycles    = 0U;
        bool     desc_completed  = false;
        FrameDesc out_desc{};
        for (uint32_t cyc = 0U; cyc < 200U; ++cyc) {
            if (cyc == 1U) versal.desc_in_valid_i = 0U;
            // Sample valid beat BEFORE rising edge commits it (matches AXI handshake)
            if (versal.noc_wr_valid_o != 0U && versal.noc_wr_ready_i != 0U) {
                ++beats_seen;
            }
            raw_bus.step_clock(1U);
            ++total_cycles;
            if (versal.desc_out_valid_o != 0U) {
                uint32_t out_w[8]{};
                for (int i = 0; i < 8; ++i) out_w[i] = versal.desc_out_data_o[i];
                std::memcpy(&out_desc, out_w, sizeof(out_desc));
                desc_completed = true;
                break;
            }
        }
        CHECK(desc_completed);
        CHECK(beats_seen == 32U);
        CHECK(total_cycles > 32U); // Proves hardware credit shaper throttled beyond CREDIT_MAX=16!
        CHECK(out_desc.phys_addr == in_desc.phys_addr);
        CHECK((out_desc.flags & 0x05U) == 0x05U); // CRC_OK + EOF stamped by DMA RTL

        // Versal NoC INI DFX decouple + XilSEM pause/auto-reinit + AIE column reset (STRATA-RTL-004)
        versal.dfx_reconfig_req_i  = 1U;
        versal.aie_col_reset_req_i = 0x3U; // Reset AIE columns 0 & 1
        raw_bus.step_clock(2U);
        CHECK(versal.xilsem_pause_req_o == 1U);
        CHECK(versal.noc_ini_decouple_o == 1U);
        CHECK(versal.aie_col_rst_n_o == 0xCU); // Columns 2 & 3 stay active!
        versal.dfx_reconfig_req_i  = 0U;
        versal.aie_col_reset_req_i = 0U;
        raw_bus.step_clock(1U);
        CHECK(versal.xilsem_reinit_req_o == 1U); // Automatic post-DFX XilSEM reinit pulse!

        // Autonomous Hardware Watchdog expiry without software kick (STRATA-RTL-002)
        slot.kick_watchdog(); // Reloads 40 ticks while Active
        raw_bus.step_clock(50U);
        CHECK(slot.state() == SlotState::Standby);
        CHECK((slot.fault_status() & hw::scsr::kFaultWdtExp) != 0U);
        CHECK(versal.slot_irq_o == 1U);
        // RW1C clear clears fault and deasserts IRQ
        slot.clear_faults(hw::scsr::kFaultWdtExp);
        CHECK((slot.fault_status() & hw::scsr::kFaultWdtExp) == 0U);

        // Bit-Exact Offline Flight Shadow Replay (STRATA-VER-003)
        // Replay captured MMIO trace through ShadowReplayTransport with zero RTL present!
        hw::ShadowReplayTransport replay_bus(rec_bus.trace());
        hw::SlotHandle<hw::ShadowReplayTransport> replay_slot(replay_bus, 0U, 40U);
        run_flight_slot_bringup(replay_slot);
        CHECK(replay_slot.read_tai().ns() == kLatchedTai);
        // Drain remaining recorded WDT kicks/reads
        CHECK(replay_bus.mismatch_count() == 0U);
    }

    // =========================================================================
    // 2. Live Verilator Co-Simulation: Kintex Platform Shell
    //    (Selective TMR + DFX AXI Shutdown/Decoupler + DMA ICAPE3 + Ext Supervisor)
    // =========================================================================
    {
        Vkintex_platform_shell kintex{};
        kintex.tai_ns_i                = 0ULL;
        kintex.pps_strobe_i            = 0U;
        kintex.sensor_capture_strobe_i = 0U;
        kintex.ext_sup_dfx_req_i       = 0U;
        kintex.ext_scrubber_paused_i   = 0U;
        kintex.ext_scrubber_ue_i       = 0U;
        kintex.icap_dma_data_i         = 0xAA99'5566U; // ICAPE3 sync word
        kintex.icap_dma_valid_i        = 0U;
        kintex.desc_in_valid_i         = 0U;
        kintex.axi_wr_ready_i          = 1U;
        kintex.desc_out_ready_i        = 1U;

        hil::VerilatorAxiLiteTransport<Vkintex_platform_shell> kbus(kintex);
        kbus.reset(4U);
        hw::SlotHandle<decltype(kbus)> kslot(kbus, /*slot_id=*/1U, /*wdt_ticks=*/100U);
        CHECK(kslot.verify_manifest_hash(kExpectedManifestHash));
        CHECK(kintex.tmr_disagreement_o == 0U);

        // Trigger external rad-hard supervisor DFX sequence -> Shutdown -> Scrubber Pause -> Decouple -> ICAPE3
        kintex.ext_sup_dfx_req_i = 1U;
        kbus.step_clock(2U);
        CHECK(kintex.dfx_axi_shutdown_o == 1U);
        CHECK(kintex.ext_scrubber_pause_req_o == 1U);
        CHECK(kintex.dfx_decouple_o == 0U); // Waits until external scrubber confirms pause!

        kintex.ext_scrubber_paused_i = 1U;
        kbus.step_clock(2U);
        CHECK(kintex.dfx_decouple_o == 1U);
        CHECK(kintex.icap_dma_ready_o == 1U);

        // Stream ICAPE3 bitstream word -> CSIB asserts active-low
        kintex.icap_dma_valid_i = 1U;
        kbus.step_clock(1U);
        CHECK(kintex.icape3_csib_o == 0U);
        CHECK(kintex.icape3_i_o == 0xAA99'5566U);

        // Complete DFX -> TMR FSM recovers back to NORMAL
        kintex.icap_dma_valid_i      = 0U;
        kintex.ext_sup_dfx_req_i     = 0U;
        kintex.ext_scrubber_paused_i = 0U;
        kbus.step_clock(3U);
        CHECK(kintex.dfx_axi_shutdown_o == 0U);
        CHECK(kintex.dfx_decouple_o == 0U);
        CHECK(kintex.tmr_disagreement_o == 0U);
    }

    std::printf("PASS: test_verilator_scsr_dma (Versal & Kintex live RTL co-sim + WDT + 1PPS + DMA shaper + Shadow Replay)\n");
    return 0;
}
