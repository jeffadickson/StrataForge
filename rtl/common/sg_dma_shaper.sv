// =============================================================================
// STRATA-20 Scatter-Gather 32-Byte Descriptor DMA & Credit Bandwidth Shaper
// Implements STRATA-RTL-003.
// Consumes 256-bit (32-byte) FrameDesc rings matching strata::FrameDesc in C++20,
// enforces hardware token-bucket credit rate shaping so Class C/D payload slots
// cannot saturate Class B memory bandwidth, and halts cleanly on quiesce_req_i.
// =============================================================================
module sg_dma_shaper #(
    parameter int unsigned BEAT_BYTES      = 16,
    parameter int unsigned CREDIT_MAX      = 16,
    parameter int unsigned CREDIT_REFILL   = 1,
    parameter int unsigned REFILL_INTERVAL = 4
) (
    input  logic         clk,
    input  logic         rst_n,

    // Quiesce request from SCSR watchdog or DFX SlotManager
    input  logic         quiesce_req_i,

    // Inbound 32-byte FrameDesc (256 bits) matching strata::FrameDesc
    // [63:0]=phys_addr, [127:64]=tai_ns, [159:128]=length_bytes,
    // [175:160]=pool_slot_idx, [191:176]=src_slot_id, [207:192]=producer_gen,
    // [223:208]=flags, [255:224]=crc32c
    input  logic [255:0] desc_in_data_i,
    input  logic         desc_in_valid_i,
    output logic         desc_in_ready_o,

    // Shaped AXI/NoC Write Beat Output
    output logic [63:0]  mem_wr_addr_o,
    output logic         mem_wr_valid_o,
    input  logic         mem_wr_ready_i,

    // Completed 32-byte FrameDesc Output
    output logic [255:0] desc_out_data_o,
    output logic         desc_out_valid_o,
    input  logic         desc_out_ready_i,

    // Diagnostics & Status
    output logic         dma_idle_o,
    output logic [31:0]  beats_issued_o,
    output logic [31:0]  throttle_cycles_o
);

    typedef enum logic [1:0] {
        ST_IDLE     = 2'd0,
        ST_TRANSFER = 2'd1,
        ST_COMPLETE = 2'd2
    } dma_state_t;

    dma_state_t  state_q;
    logic [255:0] cur_desc_q;
    logic [63:0]  cur_addr_q;
    logic [31:0]  rem_bytes_q;

    logic [15:0]  credits_q;
    logic [15:0]  refill_cnt_q;
    logic [31:0]  beats_issued_q;
    logic [31:0]  throttle_cycles_q;

    assign desc_in_ready_o   = (state_q == ST_IDLE) && !quiesce_req_i;
    assign mem_wr_addr_o     = cur_addr_q;
    assign mem_wr_valid_o    = (state_q == ST_TRANSFER) && !quiesce_req_i && (credits_q != 16'd0);
    assign desc_out_data_o   = cur_desc_q;
    assign desc_out_valid_o  = (state_q == ST_COMPLETE);
    assign dma_idle_o        = (state_q == ST_IDLE);
    assign beats_issued_o    = beats_issued_q;
    assign throttle_cycles_o = throttle_cycles_q;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state_q           <= ST_IDLE;
            cur_desc_q        <= 256'h0;
            cur_addr_q        <= 64'h0;
            rem_bytes_q       <= 32'h0;
            credits_q         <= CREDIT_MAX[15:0];
            refill_cnt_q      <= 16'd0;
            beats_issued_q    <= 32'd0;
            throttle_cycles_q <= 32'd0;
        end else begin
            // Token-bucket credit refill logic
            logic do_refill;
            logic do_consume;
            do_refill  = (refill_cnt_q == (REFILL_INTERVAL[15:0] - 16'd1));
            do_consume = mem_wr_valid_o && mem_wr_ready_i;

            if (do_refill) begin
                refill_cnt_q <= 16'd0;
            end else begin
                refill_cnt_q <= refill_cnt_q + 16'd1;
            end

            // Update credits atomically for simultaneous consume + refill
            begin
                logic [15:0] next_cred;
                next_cred = credits_q;
                if (do_consume && (next_cred != 16'd0)) begin
                    next_cred = next_cred - 16'd1;
                end
                if (do_refill) begin
                    if ((next_cred + CREDIT_REFILL[15:0]) >= CREDIT_MAX[15:0]) begin
                        next_cred = CREDIT_MAX[15:0];
                    end else begin
                        next_cred = next_cred + CREDIT_REFILL[15:0];
                    end
                end
                credits_q <= next_cred;
            end

            // Track cycles where DMA is ready to write but held back by credit shaper
            if ((state_q == ST_TRANSFER) && !quiesce_req_i && (credits_q == 16'd0)) begin
                throttle_cycles_q <= throttle_cycles_q + 32'd1;
            end

            unique case (state_q)
                ST_IDLE: begin
                    if (desc_in_valid_i && desc_in_ready_o) begin
                        cur_desc_q  <= desc_in_data_i;
                        cur_addr_q  <= desc_in_data_i[63:0];
                        rem_bytes_q <= desc_in_data_i[159:128];
                        if (desc_in_data_i[159:128] == 32'd0) begin
                            state_q <= ST_COMPLETE;
                        end else begin
                            state_q <= ST_TRANSFER;
                        end
                    end
                end

                ST_TRANSFER: begin
                    if (do_consume) begin
                        beats_issued_q <= beats_issued_q + 32'd1;
                        cur_addr_q     <= cur_addr_q + 64'(BEAT_BYTES);
                        if (rem_bytes_q <= 32'(BEAT_BYTES)) begin
                            rem_bytes_q <= 32'd0;
                            // Stamp CRC_OK + EOF flags (bits 0 and 2 of flags [223:208])
                            cur_desc_q[210:208] <= cur_desc_q[210:208] | 3'b101;
                            state_q             <= ST_COMPLETE;
                        end else begin
                            rem_bytes_q <= rem_bytes_q - 32'(BEAT_BYTES);
                        end
                    end
                end

                ST_COMPLETE: begin
                    if (desc_out_ready_i) begin
                        state_q <= ST_IDLE;
                    end
                end

                default: begin
                    state_q <= ST_IDLE;
                end
            endcase
        end
    end

endmodule
