// =============================================================================
// STRATA-20 Standard 4 KB AXI4-Lite SCSR Slave + Autonomous WDT + 1PPS Strobe
// Implements STRATA-RTL-001 and STRATA-RTL-002.
// Bit-exact hardware mirror of middleware/include/strata/hw/slot_handle.hpp.
// =============================================================================
`include "scsr_defs.svh"

module scsr_slave #(
    parameter logic [31:0] MANIFEST_HASH_LO = 32'h0000_0000,
    parameter logic [31:0] MANIFEST_HASH_HI = 32'h0000_0000,
    parameter logic [31:0] SLOT_CAP_WORD    = 32'h0000_0001
) (
    input  logic        clk,
    input  logic        rst_n,

    // AXI4-Lite Slave Interface (4 KB window: awaddr/araddr[11:0])
    input  logic [11:0] s_axil_awaddr,
    input  logic        s_axil_awvalid,
    output logic        s_axil_awready,
    input  logic [31:0] s_axil_wdata,
    input  logic [3:0]  s_axil_wstrb,
    input  logic        s_axil_wvalid,
    output logic        s_axil_wready,
    output logic [1:0]  s_axil_bresp,
    output logic        s_axil_bvalid,
    input  logic        s_axil_bready,

    input  logic [11:0] s_axil_araddr,
    input  logic        s_axil_arvalid,
    output logic        s_axil_arready,
    output logic [31:0] s_axil_rdata,
    output logic [1:0]  s_axil_rresp,
    output logic        s_axil_rvalid,
    input  logic        s_axil_rready,

    // Hardware 1PPS / Sensor Capture TAI Timestamp Strobe (STRATA-RTL-002)
    input  logic [63:0] tai_ns_i,
    input  logic        pps_strobe_i,
    input  logic        sensor_capture_strobe_i,

    // External Hardware Fault Inputs (AXI_ERR, FIFO_OVF, ECC_UE, CRC_FAIL, PLL_UNLOCK)
    input  logic [5:0]  ext_fault_pulse_i,

    // Hardware Slot Control Outputs
    output logic [31:0] slot_state_o,
    output logic        dma_quiesce_req_o,
    output logic        slot_irq_o,
    output logic [31:0] dma_tx_desc_base_o
);

    // Fault status bit positions matching strata::hw::scsr
    localparam int unsigned FAULT_BIT_WDT_EXP = 3;

    // SlotCmd encodings matching strata::SlotCmd
    localparam logic [31:0] CMD_NONE          = 32'd0;
    localparam logic [31:0] CMD_RUN_SBIT      = 32'd1;
    localparam logic [31:0] CMD_ENTER_STANDBY = 32'd2;
    localparam logic [31:0] CMD_START_ACTIVE  = 32'd3;
    localparam logic [31:0] CMD_QUIESCE       = 32'd4;
    localparam logic [31:0] CMD_SOFT_RESET    = 32'd5;

    logic [31:0] state_q;
    logic [31:0] fault_status_q;
    logic [31:0] irq_mask_q;
    logic [31:0] wdt_reload_q;
    logic [31:0] wdt_counter_q;
    logic [63:0] tai_latched_q;
    logic [31:0] dma_tx_desc_base_q;
    logic        quiesce_req_q;

    // AXI4-Lite simple single-beat write/read handshake
    logic        wr_fire;
    logic        rd_fire;
    logic [31:0] rdata_q;
    logic        rvalid_q;
    logic        bvalid_q;

    assign s_axil_awready = !bvalid_q && s_axil_awvalid && s_axil_wvalid;
    assign s_axil_wready  = s_axil_awready;
    assign s_axil_bresp   = 2'b00; // OKAY
    assign s_axil_bvalid  = bvalid_q;

    assign s_axil_arready = !rvalid_q;
    assign s_axil_rdata   = rdata_q;
    assign s_axil_rresp   = 2'b00; // OKAY
    assign s_axil_rvalid  = rvalid_q;

    assign wr_fire = s_axil_awvalid && s_axil_awready && s_axil_wvalid && s_axil_wready;
    assign rd_fire = s_axil_arvalid && s_axil_arready;

    assign slot_state_o       = state_q;
    assign dma_quiesce_req_o  = quiesce_req_q;
    assign slot_irq_o         = |(fault_status_q & irq_mask_q);
    assign dma_tx_desc_base_o = dma_tx_desc_base_q;

    // Unused byte-enable strobes acknowledged for full 32-bit SCSR register writes
    logic _unused_ok;
    assign _unused_ok = &{1'b0, s_axil_wstrb};

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state_q            <= STRATA_STATE_STANDBY;
            fault_status_q     <= 32'h0;
            irq_mask_q         <= 32'h0000_003F;
            wdt_reload_q       <= 32'd1000;
            wdt_counter_q      <= 32'd1000;
            tai_latched_q      <= 64'h0;
            dma_tx_desc_base_q <= 32'h0;
            quiesce_req_q      <= 1'b1;
            bvalid_q           <= 1'b0;
            rvalid_q           <= 1'b0;
            rdata_q            <= 32'h0;
        end else begin
            // Clear AXI response valids on handshake
            if (bvalid_q && s_axil_bready) begin
                bvalid_q <= 1'b0;
            end
            if (rvalid_q && s_axil_rready) begin
                rvalid_q <= 1'b0;
            end

            // Hardware 1PPS / Sensor Capture TAI Strobe (STRATA-RTL-002)
            if (pps_strobe_i || sensor_capture_strobe_i) begin
                tai_latched_q <= tai_ns_i;
            end

            // Latch external hardware fault pulses (bits 0..5)
            if (ext_fault_pulse_i != 6'b0) begin
                fault_status_q[5:0] <= fault_status_q[5:0] | ext_fault_pulse_i;
                if (ext_fault_pulse_i[2] || ext_fault_pulse_i[5]) begin
                    // ECC_UE or PLL_UNLOCK forces immediate fault & DMA quiesce
                    state_q       <= STRATA_STATE_FAULTED;
                    quiesce_req_q <= 1'b1;
                end
            end

            // Autonomous Hardware Slot Watchdog when Active (STRATA-RTL-002)
            if (state_q == STRATA_STATE_ACTIVE) begin
                if (wdt_counter_q == 32'd0) begin
                    // Watchdog expired: autonomously quiesce DMA and transition to Standby
                    fault_status_q[FAULT_BIT_WDT_EXP] <= 1'b1;
                    quiesce_req_q                     <= 1'b1;
                    state_q                           <= STRATA_STATE_STANDBY;
                end else begin
                    wdt_counter_q <= wdt_counter_q - 32'd1;
                end
            end

            // AXI4-Lite Write Handling
            if (wr_fire) begin
                bvalid_q <= 1'b1;
                unique case (s_axil_awaddr)
                    STRATA_REG_CMD: begin
                        unique case (s_axil_wdata)
                            CMD_RUN_SBIT: begin
                                state_q       <= STRATA_STATE_SBIT;
                                quiesce_req_q <= 1'b1;
                            end
                            CMD_ENTER_STANDBY: begin
                                state_q       <= STRATA_STATE_STANDBY;
                                quiesce_req_q <= 1'b1;
                            end
                            CMD_START_ACTIVE: begin
                                if (state_q != STRATA_STATE_FAULTED) begin
                                    state_q       <= STRATA_STATE_ACTIVE;
                                    quiesce_req_q <= 1'b0;
                                    wdt_counter_q <= (wdt_reload_q == 32'd0) ? 32'd1 : wdt_reload_q;
                                end
                            end
                            CMD_QUIESCE: begin
                                state_q       <= STRATA_STATE_QUIESCING;
                                quiesce_req_q <= 1'b1;
                            end
                            CMD_SOFT_RESET: begin
                                state_q        <= STRATA_STATE_STANDBY;
                                fault_status_q <= 32'h0;
                                quiesce_req_q  <= 1'b1;
                                wdt_counter_q  <= wdt_reload_q;
                            end
                            CMD_NONE: begin
                                // Explicit no-op command
                            end
                            default: begin
                                // Reserved command
                            end
                        endcase
                    end
                    STRATA_REG_FAULT_STATUS: begin
                        // RW1C: writing 1 clears the corresponding fault bit
                        fault_status_q <= fault_status_q & ~s_axil_wdata;
                    end
                    STRATA_REG_IRQ_MASK: begin
                        irq_mask_q <= s_axil_wdata;
                    end
                    STRATA_REG_WDT_RELOAD: begin
                        wdt_reload_q  <= s_axil_wdata;
                        wdt_counter_q <= s_axil_wdata;
                    end
                    STRATA_REG_DMA_TX_DESC_BASE: begin
                        dma_tx_desc_base_q <= s_axil_wdata;
                    end
                    default: begin
                        // RO or unmapped offset: ignore write cleanly
                    end
                endcase
            end

            // AXI4-Lite Read Handling
            if (rd_fire) begin
                rvalid_q <= 1'b1;
                unique case (s_axil_araddr)
                    STRATA_REG_MAGIC_VER:        rdata_q <= STRATA_SCSR_MAGIC;
                    STRATA_REG_MANIFEST_HASH_LO: rdata_q <= MANIFEST_HASH_LO;
                    STRATA_REG_MANIFEST_HASH_HI: rdata_q <= MANIFEST_HASH_HI;
                    STRATA_REG_SLOT_CAP:         rdata_q <= SLOT_CAP_WORD;
                    STRATA_REG_STATE:            rdata_q <= state_q;
                    STRATA_REG_FAULT_STATUS:     rdata_q <= fault_status_q;
                    STRATA_REG_IRQ_MASK:         rdata_q <= irq_mask_q;
                    STRATA_REG_WDT_RELOAD:       rdata_q <= wdt_reload_q;
                    STRATA_REG_TAI_LO:           rdata_q <= tai_latched_q[31:0];
                    STRATA_REG_TAI_HI:           rdata_q <= tai_latched_q[63:32];
                    STRATA_REG_DMA_TX_DESC_BASE: rdata_q <= dma_tx_desc_base_q;
                    default:                     rdata_q <= 32'h0;
                endcase
            end
        end
    end

endmodule
