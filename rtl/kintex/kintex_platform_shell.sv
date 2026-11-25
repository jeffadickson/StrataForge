// =============================================================================
// STRATA-20 Kintex Platform Shell (XQRKU060 / UltraScale+)
// Implements STRATA-RTL-005:
//  - Selective Triple Modular Redundancy (TMR 2-of-3 majority voter) on
//    persistent-critical static shell control state
//  - DFX AXI Shutdown Manager + DFX Decoupler isolation fence
//  - DMA-fed ICAPE3 partial reconfiguration streaming port
//  - External rad-hard supervisor / CRAM scrubber handshake
// =============================================================================
`include "scsr_defs.svh"

module kintex_platform_shell #(
    parameter logic [31:0] MANIFEST_HASH_LO = 32'h0000_0000,
    parameter logic [31:0] MANIFEST_HASH_HI = 32'h0000_0000
) (
    input  logic         clk,
    input  logic         rst_n,

    // AXI4-Lite SCSR Control Interface
    input  logic [11:0]  s_axil_awaddr,
    input  logic         s_axil_awvalid,
    output logic         s_axil_awready,
    input  logic [31:0]  s_axil_wdata,
    input  logic [3:0]   s_axil_wstrb,
    input  logic         s_axil_wvalid,
    output logic         s_axil_wready,
    output logic [1:0]   s_axil_bresp,
    output logic         s_axil_bvalid,
    input  logic         s_axil_bready,
    input  logic [11:0]  s_axil_araddr,
    input  logic         s_axil_arvalid,
    output logic         s_axil_arready,
    output logic [31:0]  s_axil_rdata,
    output logic [1:0]   s_axil_rresp,
    output logic         s_axil_rvalid,
    input  logic         s_axil_rready,

    // Global 1PPS TAI Strobe
    input  logic [63:0]  tai_ns_i,
    input  logic         pps_strobe_i,
    input  logic         sensor_capture_strobe_i,

    // External Rad-Hard Supervisor & Scrubber Interface
    input  logic         ext_sup_dfx_req_i,
    input  logic         ext_scrubber_paused_i,
    input  logic         ext_scrubber_ue_i,
    output logic         ext_scrubber_pause_req_o,
    output logic         dfx_axi_shutdown_o,
    output logic         dfx_decouple_o,
    output logic         tmr_disagreement_o,

    // DMA-Fed ICAPE3 Partial Reconfiguration Stream Interface
    input  logic [31:0]  icap_dma_data_i,
    input  logic         icap_dma_valid_i,
    output logic         icap_dma_ready_o,
    output logic [31:0]  icape3_i_o,
    output logic         icape3_csib_o,
    output logic         icape3_rdwrb_o,

    // 32-Byte Descriptor DMA & Static AXI Master Output
    input  logic [255:0] desc_in_data_i,
    input  logic         desc_in_valid_i,
    output logic         desc_in_ready_o,
    output logic [63:0]  axi_wr_addr_o,
    output logic         axi_wr_valid_o,
    input  logic         axi_wr_ready_i,
    output logic [255:0] desc_out_data_o,
    output logic         desc_out_valid_o,
    input  logic         desc_out_ready_i,
    output logic         slot_irq_o
);

    // -------------------------------------------------------------------------
    // Selective TMR (2-of-3 Majority Voter) for DFX Shutdown / Decoupler FSM
    // -------------------------------------------------------------------------
    // 2-bit state: 00=NORMAL, 01=SHUTDOWN_WAIT, 10=DECOUPLED_ICAP, 11=RECOVERY
    (* dont_touch = "true" *) logic [1:0] fsm_a_q;
    (* dont_touch = "true" *) logic [1:0] fsm_b_q;
    (* dont_touch = "true" *) logic [1:0] fsm_c_q;

    logic [1:0] fsm_voted;
    assign fsm_voted = (fsm_a_q & fsm_b_q) | (fsm_b_q & fsm_c_q) | (fsm_a_q & fsm_c_q);
    assign tmr_disagreement_o = (fsm_a_q != fsm_b_q) || (fsm_b_q != fsm_c_q);

    logic [31:0] slot_state;
    logic        scsr_quiesce_req;
    logic        dma_idle;
    logic [31:0] dma_tx_desc_base;
    logic [31:0] beats_issued;
    logic [31:0] throttle_cycles;

    logic [1:0] fsm_next;
    always_comb begin
        fsm_next = fsm_voted;
        unique case (fsm_voted)
            2'b00: begin
                if (ext_sup_dfx_req_i) fsm_next = 2'b01;
            end
            2'b01: begin
                if (dma_idle && ext_scrubber_paused_i) fsm_next = 2'b10;
            end
            2'b10: begin
                if (!ext_sup_dfx_req_i) fsm_next = 2'b11;
            end
            2'b11: begin
                fsm_next = 2'b00;
            end
            default: fsm_next = 2'b00;
        endcase
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            fsm_a_q <= 2'b00;
            fsm_b_q <= 2'b00;
            fsm_c_q <= 2'b00;
        end else begin
            fsm_a_q <= fsm_next;
            fsm_b_q <= fsm_next;
            fsm_c_q <= fsm_next;
        end
    end

    assign ext_scrubber_pause_req_o = (fsm_voted == 2'b01) || (fsm_voted == 2'b10);
    assign dfx_axi_shutdown_o       = (fsm_voted != 2'b00);
    assign dfx_decouple_o           = (fsm_voted == 2'b10);

    // DMA-fed ICAPE3 write gating: only enabled while DFX Decoupler & external scrubber pause are active
    assign icap_dma_ready_o = (fsm_voted == 2'b10);
    assign icape3_i_o       = icap_dma_data_i;
    assign icape3_csib_o    = !(icap_dma_valid_i && icap_dma_ready_o); // Active-low chip select
    assign icape3_rdwrb_o   = 1'b0;                                    // Write mode

    logic [5:0] ext_faults;
    assign ext_faults = {3'b000, ext_scrubber_ue_i, 2'b00};

    scsr_slave #(
        .MANIFEST_HASH_LO (MANIFEST_HASH_LO),
        .MANIFEST_HASH_HI (MANIFEST_HASH_HI),
        .SLOT_CAP_WORD    (32'h0002_0001) // Kintex Abstract Shell + TMR capability
    ) u_scsr (
        .clk                     (clk),
        .rst_n                   (rst_n),
        .s_axil_awaddr           (s_axil_awaddr),
        .s_axil_awvalid          (s_axil_awvalid),
        .s_axil_awready          (s_axil_awready),
        .s_axil_wdata            (s_axil_wdata),
        .s_axil_wstrb            (s_axil_wstrb),
        .s_axil_wvalid           (s_axil_wvalid),
        .s_axil_wready           (s_axil_wready),
        .s_axil_bresp            (s_axil_bresp),
        .s_axil_bvalid           (s_axil_bvalid),
        .s_axil_bready           (s_axil_bready),
        .s_axil_araddr           (s_axil_araddr),
        .s_axil_arvalid          (s_axil_arvalid),
        .s_axil_arready          (s_axil_arready),
        .s_axil_rdata            (s_axil_rdata),
        .s_axil_rresp            (s_axil_rresp),
        .s_axil_rvalid           (s_axil_rvalid),
        .s_axil_rready           (s_axil_rready),
        .tai_ns_i                (tai_ns_i),
        .pps_strobe_i            (pps_strobe_i),
        .sensor_capture_strobe_i (sensor_capture_strobe_i),
        .ext_fault_pulse_i       (ext_faults),
        .slot_state_o            (slot_state),
        .dma_quiesce_req_o       (scsr_quiesce_req),
        .slot_irq_o              (slot_irq_o),
        .dma_tx_desc_base_o      (dma_tx_desc_base)
    );

    sg_dma_shaper #(
        .BEAT_BYTES      (16),
        .CREDIT_MAX      (8),
        .CREDIT_REFILL   (1),
        .REFILL_INTERVAL (4)
    ) u_dma (
        .clk               (clk),
        .rst_n             (rst_n),
        .quiesce_req_i     (scsr_quiesce_req || dfx_axi_shutdown_o),
        .desc_in_data_i    (desc_in_data_i),
        .desc_in_valid_i   (desc_in_valid_i),
        .desc_in_ready_o   (desc_in_ready_o),
        .mem_wr_addr_o     (axi_wr_addr_o),
        .mem_wr_valid_o    (axi_wr_valid_o),
        .mem_wr_ready_i    (axi_wr_ready_i && !dfx_decouple_o),
        .desc_out_data_o   (desc_out_data_o),
        .desc_out_valid_o  (desc_out_valid_o),
        .desc_out_ready_i  (desc_out_ready_i),
        .dma_idle_o        (dma_idle),
        .beats_issued_o    (beats_issued),
        .throttle_cycles_o (throttle_cycles)
    );

    logic _unused_ok;
    assign _unused_ok = &{1'b0, slot_state, dma_tx_desc_base, beats_issued, throttle_cycles};

endmodule
