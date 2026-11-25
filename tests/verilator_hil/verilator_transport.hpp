#pragma once

#include <strata/core/concepts.hpp>

#include <cstdint>

namespace strata::hil {

/// ============================================================================
/// Verilator AXI4-Lite RegisterTransport Backend (STRATA-VER-003)
/// Bridges unmodified flight C++20 `strata::hw::SlotHandle<Transport>` directly
/// onto any Verilated SystemVerilog module exposing standard AXI4-Lite SCSR ports.
/// ============================================================================
template <typename VerilatedTop>
class VerilatorAxiLiteTransport {
public:
    explicit VerilatorAxiLiteTransport(VerilatedTop& dut) noexcept : dut_(&dut) {}

    void step_clock(uint32_t cycles = 1U) const noexcept {
        for (uint32_t i = 0U; i < cycles; ++i) {
            dut_->clk = 0U;
            dut_->eval();
            dut_->clk = 1U;
            dut_->eval();
            ++cycle_count_;
        }
    }

    void reset(uint32_t cycles = 4U) noexcept {
        dut_->rst_n          = 0U;
        dut_->s_axil_awaddr  = 0U;
        dut_->s_axil_awvalid = 0U;
        dut_->s_axil_wdata   = 0U;
        dut_->s_axil_wstrb   = 0xFU;
        dut_->s_axil_wvalid  = 0U;
        dut_->s_axil_bready  = 1U;
        dut_->s_axil_araddr  = 0U;
        dut_->s_axil_arvalid = 0U;
        dut_->s_axil_rready  = 1U;
        step_clock(cycles);
        dut_->rst_n = 1U;
        step_clock(2U);
    }

    [[nodiscard]] uint32_t read32(uint32_t offset) const noexcept {
        dut_->s_axil_araddr  = static_cast<uint16_t>(offset & 0x0FFFU);
        dut_->s_axil_arvalid = 1U;
        dut_->s_axil_rready  = 1U;
        for (uint32_t guard = 0U; guard < 32U; ++guard) {
            step_clock(1U);
            if (dut_->s_axil_rvalid != 0U) {
                const uint32_t val   = dut_->s_axil_rdata;
                dut_->s_axil_arvalid = 0U;
                step_clock(1U);
                return val;
            }
            dut_->s_axil_arvalid = 0U;
        }
        dut_->s_axil_arvalid = 0U;
        return 0U;
    }

    void write32(uint32_t offset, uint32_t value) noexcept {
        dut_->s_axil_awaddr  = static_cast<uint16_t>(offset & 0x0FFFU);
        dut_->s_axil_awvalid = 1U;
        dut_->s_axil_wdata   = value;
        dut_->s_axil_wstrb   = 0xFU;
        dut_->s_axil_wvalid  = 1U;
        dut_->s_axil_bready  = 1U;

        step_clock(1U);
        dut_->s_axil_awvalid = 0U;
        dut_->s_axil_wvalid  = 0U;
        for (uint32_t guard = 0U; guard < 32U; ++guard) {
            if (dut_->s_axil_bvalid != 0U) {
                step_clock(1U);
                return;
            }
            step_clock(1U);
        }
    }

    [[nodiscard]] uint64_t cycle_count() const noexcept { return cycle_count_; }

private:
    VerilatedTop*    dut_;
    mutable uint64_t cycle_count_{0U};
};

} // namespace strata::hil
