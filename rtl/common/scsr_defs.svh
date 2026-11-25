// =============================================================================
// STRATA-20 Standard 4 KB Slot Control & Status Register (SCSR) Contract
// Shared across Versal (NoC INI DFX / Spatial) and Kintex (Abstract Shell DFX)
// Instrument-specific registers begin at offset 0x1000.
// =============================================================================
`ifndef STRATA_SCSR_DEFS_SVH
`define STRATA_SCSR_DEFS_SVH

localparam logic [31:0] STRATA_SCSR_MAGIC            = 32'h5354_5241; // "STRA"
localparam logic [15:0] STRATA_SCSR_ABI_MAJOR        = 16'h0000;
localparam logic [15:0] STRATA_SCSR_ABI_MINOR        = 16'h0001;

// Standard 4 KB SCSR Register Byte Offsets
localparam logic [11:0] STRATA_REG_MAGIC_VER         = 12'h000; // RO
localparam logic [11:0] STRATA_REG_MANIFEST_HASH_LO  = 12'h004; // RO
localparam logic [11:0] STRATA_REG_MANIFEST_HASH_HI  = 12'h008; // RO
localparam logic [11:0] STRATA_REG_SLOT_CAP          = 12'h00C; // RO
localparam logic [11:0] STRATA_REG_STATE             = 12'h010; // RO
localparam logic [11:0] STRATA_REG_CMD               = 12'h014; // WO
localparam logic [11:0] STRATA_REG_FAULT_STATUS      = 12'h018; // RW1C
localparam logic [11:0] STRATA_REG_IRQ_MASK          = 12'h01C; // RW
localparam logic [11:0] STRATA_REG_WDT_RELOAD        = 12'h020; // RW
localparam logic [11:0] STRATA_REG_TAI_LO            = 12'h024; // RO
localparam logic [11:0] STRATA_REG_TAI_HI            = 12'h028; // RO
localparam logic [11:0] STRATA_REG_DMA_TX_DESC_BASE  = 12'h040; // RW

// Unified SlotState Enum Encoding (matches strata::SlotState in C++20)
localparam logic [31:0] STRATA_STATE_UNLOADED        = 32'd0;
localparam logic [31:0] STRATA_STATE_LOADING         = 32'd1;
localparam logic [31:0] STRATA_STATE_SBIT            = 32'd2;
localparam logic [31:0] STRATA_STATE_STANDBY         = 32'd3;
localparam logic [31:0] STRATA_STATE_ACTIVE          = 32'd4;
localparam logic [31:0] STRATA_STATE_QUIESCING       = 32'd5;
localparam logic [31:0] STRATA_STATE_FAULTED         = 32'd6;

`endif // STRATA_SCSR_DEFS_SVH
