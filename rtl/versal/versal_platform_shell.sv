// =============================================================================
// STRATA-20 Versal Platform Shell (XQRVC1902 / XQRVE2102)
// Implements STRATA-RTL-004:
//  - Integrates 4 KB SCSR slave + 32-byte FrameDesc DMA & NoC credit shaper
//  - PMC XilSEM CRAM scrub pause / post-DFX reinitialization handshake
//  - NoC EXCLUSIVE_ROUTING_GROUP critical-path isolation guard
//  - NoC INI hardware quiescing & decoupling for dynamic DFX slots
//  - Isolated AI Engine (AIE) column reset & clock-gate control
// =============================================================================
`include "scsr_defs.svh"

module versal_platform_shell #(
    parameter logic [31:0] MANIFEST_HASH_LO      = 32'h0000_0000,
    parameter logic [31:0] MANIFEST_HASH_HI      = 32'h0000_0000,
    parameter bit          IS_CRITICAL_NOC_GROUP = 1'b0,
    parameter int unsigned AIE_COLUMN_COUNT      = 4
) (
    input  logic                        clk,
    input  logic                        rst_n,

    // AXI4-Lite SCSR Control Interface
    input  logic [11:0]                 s_axil_awaddr,
    input  logic                        s_axil_awvalid,
    output logic                        s_axil_awready,
    input  logic [31:0]                 s_axil_wdata,
    input  logic [3:0]                  s_axil_wstrb,
    input  logic                        s_axil_wvalid,
    output logic                        s_axil_wready,
    output logic [1:0]                  s_axil_bresp,
    output logic                        s_axil_bvalid,
    input  logic                        s_axil_bready,
    input  logic [11:0]                 s_axil_araddr,
    input  logic                        s_axil_arvalid,
    output logic                        s_axil_arready,
    output logic [31:0]                 s_axil_rdata,
    output logic [1:0]                  s_axil_rresp,
    output logic                        s_axil_rvalid,
    input  logic                        s_axil_rready,

    // Global 1PPS TAI Strobe
    input  logic [63:0]                 tai_ns_i,
    input  logic                        pps_strobe_i,
    input  logic                        sensor_capture_strobe_i,

    // PMC XilSEM Scrubber Handshake & DFX Control from Class B RPU/PMC
    input  logic                        dfx_reconfig_req_i,
    input  logic                        xilsem_paused_i,
    input  logic                        xilsem_uncorr_err_i,
    output logic                        xilsem_pause_req_o,
    output logic                        xilsem_reinit_req_o,
    output logic                        noc_ini_decouple_o,
    output logic                        noc_excl_group_active_o,

    // AI Engine Column Partition Isolation Controls
    input  logic [AIE_COLUMN_COUNT-1:0] aie_col_reset_req_i,
    output logic [AIE_COLUMN_COUNT-1:0] aie_col_rst_n_o,
    output logic [AIE_COLUMN_COUNT-1:0] aie_col_clk_en_o,

    // 32-Byte Descriptor DMA & NoC Output
    input  logic [255:0]                desc_in_data_i,
    input  logic                        desc_in_valid_i,
    output logic                        desc_in_ready_o,
    output logic [63:0]                 noc_wr_addr_o,
    output logic                        noc_wr_valid_o,
    input  logic                        noc_wr_ready_i,
    output logic [255:0]                desc_out_data_o,
    output logic                        desc_out_valid_o,
    input  logic                        desc_out_ready_i,
    output logic                        slot_irq_o
);

    logic [31:0] slot_state;
    logic        scsr_quiesce_req;
    logic        dma_idle;
    logic [31:0] dma_tx_desc_base;
    logic [31:0] beats_issued;
    logic [31:0] throttle_cycles;
    logic        dfx_reconfig_prev_q;

    // Combine SCSR watchdog quiesce with PMC DFX reconfiguration quiesce
    logic combined_quiesce;
    assign combined_quiesce = scsr_quiesce_req || dfx_reconfig_req_i;

    // NoC INI hardware decoupler asserts once DMA is idle under DFX quiesce
    assign noc_ini_decouple_o      = dfx_reconfig_req_i && dma_idle;
    assign xilsem_pause_req_o      = dfx_reconfig_req_i;
    assign noc_excl_group_active_o = IS_CRITICAL_NOC_GROUP && !noc_ini_decouple_o;

    // Pulse XilSEM golden-CRAM reinitialization automatically on falling edge of dfx_reconfig_req_i
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            dfx_reconfig_prev_q <= 1'b0;
            xilsem_reinit_req_o <= 1'b0;
        end else begin
            dfx_reconfig_prev_q <= dfx_reconfig_req_i;
            xilsem_reinit_req_o <= dfx_reconfig_prev_q && !dfx_reconfig_req_i;
        end
    end

    // Isolated AI Engine column reset & clock gating
    assign aie_col_rst_n_o  = ~aie_col_reset_req_i;
    assign aie_col_clk_en_o = ~aie_col_reset_req_i;

    // Feed XilSEM uncorrectable CRAM error into SCSR ECC_UE fault bit (bit 2)
    logic [5:0] ext_faults;
    assign ext_faults = {3'b000, xilsem_uncorr_err_i, 2'b00};

    scsr_slave #(
        .MANIFEST_HASH_LO (MANIFEST_HASH_LO),
        .MANIFEST_HASH_HI (MANIFEST_HASH_HI),
        .SLOT_CAP_WORD    ({16'h0001, 8'(AIE_COLUMN_COUNT), 7'b0, IS_CRITICAL_NOC_GROUP})
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
        .CREDIT_MAX      (16),
        .CREDIT_REFILL   (2),
        .REFILL_INTERVAL (4)
    ) u_dma (
        .clk               (clk),
        .rst_n             (rst_n),
        .quiesce_req_i     (combined_quiesce),
        .desc_in_data_i    (desc_in_data_i),
        .desc_in_valid_i   (desc_in_valid_i),
        .desc_in_ready_o   (desc_in_ready_o),
        .mem_wr_addr_o     (noc_wr_addr_o),
        .mem_wr_valid_o    (noc_wr_valid_o),
        .mem_wr_ready_i    (noc_wr_ready_i && !noc_ini_decouple_o),
        .desc_out_data_o   (desc_out_data_o),
        .desc_out_valid_o  (desc_out_valid_o),
        .desc_out_ready_i  (desc_out_ready_i),
        .dma_idle_o        (dma_idle),
        .beats_issued_o    (beats_issued),
        .throttle_cycles_o (throttle_cycles)
    );

    logic _unused_ok;
    assign _unused_ok = &{1'b0, xilsem_paused_i, slot_state, dma_tx_desc_base, beats_issued, throttle_cycles};

endmodule
