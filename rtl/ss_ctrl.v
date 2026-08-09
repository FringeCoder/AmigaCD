`timescale 1ns/1ns

// Save state controller.
//
// Sequences a save: wait for a quiet instant, freeze the machine, serialise
// the state vector, stream chip RAM, CRC the payload, write the header, then
// bump the counter. The host poller (user_io.cpp:1969) notices the counter
// within a second and writes the window to disk.
//
// The counter is bumped LAST and only on success. A save that timed out or
// never completed leaves the previous counter value, so the host never
// persists a partial payload.
//
// This does not implement the phase-1A plan's ss_ctrl verbatim. The plan was
// written against an earlier ss_serdes/ss_dma and is wrong about both in
// ways that matter to the sequencing below:
//
//  - ss_serdes has no backpressure on its save path: once started it emits
//    exactly one 32-bit word per clock, every clock, until the vector is
//    exhausted, with no way to ask it to pause. ss_crc32 costs four clocks
//    per word (one byte at a time -- see its own header for why). A design
//    that hands each serdes word straight to a single pending-write register
//    shared with the CRC feeder (as the plan does) loses words: the second
//    back-to-back word overwrites the first before the retire logic has even
//    looked at it, and the plan's if/else-if retire step forces a second
//    idle cycle on top of that. Confirmed by building the plan's ss_ctrl
//    verbatim against its own testbench: "state word 1" comes back as
//    whatever garbage was left in the register, not the vector's high word.
//    The fix here is structural: capture the whole burst into a local
//    buffer sized STATE_WORDS (bounded and small -- this is register state,
//    not chip RAM) as it streams in, at the one-word-per-clock rate serdes
//    demands, then drain that buffer into the CRC/DDR3 path one word at a
//    time, only pulling the next buffered word once the previous one has
//    fully cleared the CRC feeder and the DDR3 write. That makes the CRC
//    feeder -- not the producer's raw rate -- the pacing authority, which is
//    the assumption its own header comment already makes ("nothing against
//    a save the user asked for").
//
//  - ss_dma's `done` commits on the exact same clock edge as the final
//    word's `word_valid`, from the same always block (see its testbench
//    header). The plan's `if (dma_word_valid) ... else if (dma_done)` can
//    only ever see one of the two on that edge, and word_valid wins --
//    `done` is a single-cycle pulse with nothing to re-trigger it, so it is
//    lost forever. Confirmed empirically: building the plan's ss_ctrl
//    verbatim and running it against its own testbench hangs -- a watchdog
//    dump shows it parked in S_CHIP indefinitely with pending_valid and
//    ddr_write both already low, i.e. simply never told the transfer ended.
//    The fix here sidesteps the race rather than patching around its exact
//    timing: chip RAM is read two half-words (one packed 32-bit word) per
//    ss_dma `start`, and completion is tracked locally by counting the two
//    word_valid pulses ss_ctrl itself requested, never by watching `done`.
//    This also means chip RAM reads are self-paced the same way the state
//    vector is: the next pair is not requested until the current one has
//    cleared the CRC feeder and the DDR3 write.

module ss_ctrl
#(
	parameter STATE_W    = 64,
	parameter CHIP_WORDS = 24'h100000   // 16-bit words; 0x100000 = 2 MB
)
(
	input                     clk,
	input                     rst_n,

	// Base of this slot's DDR3 window, as a 64-bit word address.
	input      [28:0]         slot_base,

	input                     save_req,
	output reg                save_busy,
	output reg                save_ok,
	output reg                save_fail,

	input      [STATE_W-1:0]  state_in,
	output     [STATE_W-1:0]  state_out,
	output                    state_we,

	input                     blit_busy,
	input                     disk_busy,
	input                     audio_busy,
	input                     cpu_boundary,
	input                     frame_tick,
	output                    freeze,

	input      [24:1]         chip_base,

	output     [24:1]         sd_addr,
	output                    sd_cs,
	output     [1:0]          sd_state,
	output                    sd_uds_n,
	output                    sd_lds_n,
	output                    sd_cache_inhibit,
	input      [15:0]         sd_rd,
	input                     sd_ready,

	output reg [28:0]         ddr_address,
	output reg [63:0]         ddr_writedata,
	output reg [7:0]          ddr_byteenable,
	output reg                ddr_write,
	output                    ddr_read,
	input      [63:0]         ddr_readdata,
	input                     ddr_readdatavalid,
	input                     ddr_waitrequest
);

localparam STATE_WORDS = (STATE_W + 31) / 32;
localparam CHIP_PAIRS  = CHIP_WORDS / 2;         // 32-bit words of packed chip RAM
localparam CORE_WORDS  = STATE_WORDS + CHIP_PAIRS;

localparam SS_MAGIC   = 32'h53534341;
localparam SS_VERSION = 32'h00010000;

localparam [3:0] S_IDLE        = 4'd0;
localparam [3:0] S_WAIT        = 4'd1;
localparam [3:0] S_STATE_CAP   = 4'd2;
localparam [3:0] S_STATE_DRAIN = 4'd3;
localparam [3:0] S_CHIP_ISSUE  = 4'd4;
localparam [3:0] S_CHIP_WAIT   = 4'd5;
localparam [3:0] S_CHIP_DRAIN  = 4'd6;
localparam [3:0] S_HEADER      = 4'd7;
localparam [3:0] S_COUNT       = 4'd8;
localparam [3:0] S_DONE        = 4'd9;
localparam [3:0] S_FAIL        = 4'd10;

assign ddr_read       = 1'b0;

// ---------------------------------------------------------------- quiesce

wire quiesced;
wire timeout;

ss_quiesce quiesce
(
	.clk(clk), .rst_n(rst_n),
	.req(save_busy),
	.blit_busy(blit_busy), .disk_busy(disk_busy), .audio_busy(audio_busy),
	.cpu_boundary(cpu_boundary), .frame_tick(frame_tick),
	.freeze(freeze), .quiesced(quiesced), .timeout(timeout)
);

// ---------------------------------------------------------------- serdes

reg               ser_save_start;
wire              ser_word_valid;
wire [31:0]       ser_word_out;
wire              ser_save_done;

ss_serdes #(.WIDTH(STATE_W)) serdes
(
	.clk(clk), .rst_n(rst_n),
	.save_start(ser_save_start), .state_in(state_in),
	.word_valid(ser_word_valid), .word_out(ser_word_out),
	.save_done(ser_save_done),
	.load_start(1'b0), .word_in_valid(1'b0), .word_in(32'd0),
	.load_done(), .state_out(state_out), .state_we(state_we)
);

// State vector words captured at serdes's own one-per-clock rate, drained
// into the CRC/DDR3 path afterwards at whatever rate that path can sustain.
// See module header for why this buffer exists.
reg [31:0] state_buf [0:STATE_WORDS-1];
reg [15:0] cap_idx;
reg [15:0] drain_idx;

// ---------------------------------------------------------------- chip dma

reg         dma_start;
reg  [24:1] dma_base;
wire        dma_word_valid;
wire [15:0] dma_word_out;
wire        dma_done;

ss_dma dma
(
	.clk(clk), .rst_n(rst_n),
	.start(dma_start), .base_addr(dma_base), .word_count(24'd2),
	.sd_addr(sd_addr), .sd_cs(sd_cs), .sd_state(sd_state),
	.sd_uds_n(sd_uds_n), .sd_lds_n(sd_lds_n),
	.sd_cache_inhibit(sd_cache_inhibit),
	.sd_rd(sd_rd), .sd_ready(sd_ready),
	.word_valid(dma_word_valid), .word_out(dma_word_out), .done(dma_done)
);

reg [24:1] chip_addr;
reg [23:0] pair_count;
reg [15:0] chip_low;
reg        pending_low_held;

// ---------------------------------------------------------------- crc

reg        crc_init;
reg        crc_wr;
reg  [7:0] crc_byte;
wire [31:0] crc_value;

ss_crc32 crc (.clk(clk), .init(crc_init), .wr(crc_wr), .byte_in(crc_byte), .crc_out(crc_value));

// A 32-bit payload word is fed to the CRC as four bytes, least significant
// first, so the result matches crc32_compute() over the little-endian
// buffer. crc_bytes_left is a plain down-counter rather than the plan's
// phase register plus separate run flag: it is armed once per queued word
// by queue_word() below and nothing else touches it, which keeps its
// relationship to "is a word still being fed" unambiguous. wr is
// deasserted between queued words (while ss_ctrl is waiting on the DDR3
// write, or on the next state/chip word) and reasserted without an
// intervening init -- ss_crc32 has no notion of a "word", only a running
// byte stream, so gaps here are exactly as safe as gaps anywhere else in
// that stream, and this is exercised on every save this module makes,
// including the one in ss_ctrl_tb.v.
reg [31:0] crc_shift;
reg [2:0]  crc_bytes_left;
// The feeder itself sits inside the writer's always block below, next to
// queue_word(), which is the only other thing that drives these two.

// A word is "in flight" until the CRC has consumed all four of its bytes
// and the DDR3 write that carries it has been accepted. Nothing queues a
// new word while this is true: that is what makes the single pending_word/
// ddr_write register pair below safe despite having no depth of its own.
//
// crc_wr must be included, not just crc_bytes_left: crc_bytes_left already
// reads 0 on the very cycle the *last* byte's crc_wr pulse is still being
// presented to ss_crc32, because that pulse (like crc_byte and
// crc_bytes_left itself) is a registered value set one cycle behind the
// feeder's own decision to send it. ss_crc32 folds that byte into its
// internal register on this same edge, so the result is only visible in
// crc_out starting the cycle after. Gating solely on crc_bytes_left reads
// "not busy" one cycle before crc_out actually reflects the last byte fed,
// so an S_CHIP_DRAIN that samples crc_value right then captures a
// core_crc32 that is missing the payload's final byte. Confirmed
// empirically: dropping crc_wr from this expression reproduced exactly
// that -- every payload word landed correctly in DDR3 but core_crc32 came
// back wrong -- against ss_ctrl_tb.v's independently computed reference.
wire word_busy = (crc_bytes_left != 3'd0) || crc_wr || pending_valid || ddr_write;

// ---------------------------------------------------------------- writer

reg [3:0]  state;
reg [23:0] word_idx;      // 32-bit word index within the window
reg [31:0] pending_word;
reg        pending_valid;
reg [31:0] saved_crc;

// Queue a 32-bit payload word: CRC it and write it into DDR3. Callers must
// only invoke this when word_busy is false.
task queue_word;
	input [31:0] w;
begin
	pending_word   <= w;
	pending_valid  <= 1'b1;
	crc_shift      <= w;
	crc_bytes_left <= 3'd4;
end
endtask

always @(posedge clk) begin
	if (!rst_n) begin
		state            <= S_IDLE;
		save_busy        <= 1'b0;
		save_ok          <= 1'b0;
		save_fail        <= 1'b0;
		ddr_write        <= 1'b0;
		ddr_byteenable   <= 8'h00;
		pending_valid    <= 1'b0;
		ser_save_start   <= 1'b0;
		dma_start        <= 1'b0;
		crc_init         <= 1'b0;
		cap_idx          <= 16'd0;
		drain_idx        <= 16'd0;
		pair_count       <= 24'd0;
		pending_low_held <= 1'b0;
		crc_wr           <= 1'b0;
		crc_bytes_left   <= 3'd0;
	end
	else begin
		ser_save_start <= 1'b0;
		dma_start      <= 1'b0;
		crc_init       <= 1'b0;

		// CRC byte feeder. It lives in this block, rather than one of its
		// own, because queue_word() below also drives crc_shift and
		// crc_bytes_left: two always blocks writing the same reg is a
		// multiple-driver error in synthesis (Quartus 10028) even though
		// Icarus accepts it, so the split version could never be built.
		// The two never fire on the same cycle -- queue_word is only
		// called when word_busy is false, and word_busy includes
		// crc_bytes_left != 0 -- and where they would, queue_word's later
		// assignment wins, which is the precedence the split version's
		// behaviour relied on anyway.
		crc_wr <= 1'b0;
		if (crc_bytes_left != 3'd0) begin
			crc_byte       <= crc_shift[7:0];
			crc_wr         <= 1'b1;
			crc_shift      <= {8'd0, crc_shift[31:8]};
			crc_bytes_left <= crc_bytes_left - 3'd1;
		end

		// DDR3 write handshake: hold until the arbiter accepts. Safe as a
		// single-entry buffer because word_busy gates every producer of
		// pending_word to at most one outstanding word at a time.
		if (ddr_write && !ddr_waitrequest) begin
			ddr_write     <= 1'b0;
			pending_valid <= 1'b0;
		end
		else if (pending_valid && !ddr_write) begin
			// Two 32-bit words share one 64-bit DDR3 word, so this must be
			// a masked write: word 0 (the counter) and word 1 (length) share
			// address 0 but are written a whole save apart (word 1 in
			// S_HEADER, word 0 last in S_COUNT), and every state/chip word
			// pairs with a sibling that is written a queue_word() apart. A
			// byteenable tied to a constant 8'hFF -- an early version of
			// this file did exactly that -- turns every write into a full
			// 64-bit overwrite, so the second half of any pair silently
			// zeroes the first half instead of merging with it. Confirmed
			// empirically: without this, ss_ctrl_tb.v's "state word 0" came
			// back 0 because "state word 1"'s write clobbered it.
			ddr_address   <= slot_base + {5'd0, word_idx[23:1]};
			ddr_writedata <= word_idx[0] ? {pending_word, 32'd0} : {32'd0, pending_word};
			ddr_byteenable <= word_idx[0] ? 8'hF0 : 8'h0F;
			ddr_write     <= 1'b1;
			word_idx      <= word_idx + 24'd1;
		end

		case (state)
		S_IDLE: begin
			save_ok   <= 1'b0;
			save_fail <= 1'b0;
			if (save_req) begin
				save_busy <= 1'b1;
				state     <= S_WAIT;
			end
		end

		S_WAIT: begin
			if (timeout) state <= S_FAIL;
			else if (quiesced) begin
				crc_init       <= 1'b1;
				word_idx       <= 24'd8;      // payload starts at word 8
				cap_idx        <= 16'd0;
				ser_save_start <= 1'b1;
				state          <= S_STATE_CAP;
			end
		end

		// Capture every word serdes emits into the local buffer. serdes
		// cannot be paused, but a plain array write keeps up with its
		// one-word-per-clock rate with no risk of loss; word_valid and
		// save_done never coincide (see ss_serdes.v), so this if/else-if is
		// safe here even though the equivalent pattern is not safe for dma.
		S_STATE_CAP: begin
			if (ser_word_valid) begin
				state_buf[cap_idx] <= ser_word_out;
				cap_idx            <= cap_idx + 16'd1;
			end
			else if (ser_save_done) begin
				drain_idx <= 16'd0;
				state     <= S_STATE_DRAIN;
			end
		end

		// Feed the buffered state words to the CRC/DDR3 path one at a time,
		// at the pace word_busy allows.
		S_STATE_DRAIN: begin
			if (!word_busy) begin
				if (drain_idx == STATE_WORDS[15:0]) begin
					if (CHIP_PAIRS == 0) begin
						saved_crc <= crc_value;
						word_idx  <= 24'd1;
						state     <= S_HEADER;
					end
					else begin
						chip_addr  <= chip_base;
						pair_count <= 24'd0;
						state      <= S_CHIP_ISSUE;
					end
				end
				else begin
					queue_word(state_buf[drain_idx]);
					drain_idx <= drain_idx + 16'd1;
				end
			end
		end

		// Ask ss_dma for exactly one packed word's worth (two half-words).
		S_CHIP_ISSUE: begin
			dma_start        <= 1'b1;
			dma_base         <= chip_addr;
			pending_low_held <= 1'b0;
			state            <= S_CHIP_WAIT;
		end

		// Collect the two halves ourselves rather than trusting `done`:
		// ss_dma's `done` commits on the same edge as the second half's
		// word_valid (see module header), so counting our own two pulses
		// sidesteps that race entirely instead of trying to catch both
		// signals on the same clock.
		S_CHIP_WAIT: begin
			if (dma_word_valid) begin
				if (!pending_low_held) begin
					chip_low         <= dma_word_out;
					pending_low_held <= 1'b1;
				end
				else begin
					queue_word({dma_word_out, chip_low});
					pending_low_held <= 1'b0;
					state            <= S_CHIP_DRAIN;
				end
			end
		end

		S_CHIP_DRAIN: begin
			if (!word_busy) begin
				if (pair_count == CHIP_PAIRS - 24'd1) begin
					saved_crc <= crc_value;
					word_idx  <= 24'd1;
					state     <= S_HEADER;
				end
				else begin
					pair_count <= pair_count + 24'd1;
					chip_addr  <= chip_addr + 24'd2;
					state      <= S_CHIP_ISSUE;
				end
			end
		end

		// Header words 1..7 are written after the payload, because
		// core_crc32 is not known until the payload is complete. They are
		// not fed to the CRC.
		S_HEADER: begin
			if (!pending_valid && !ddr_write) begin
				case (word_idx)
				24'd1: pending_word <= 6 + CORE_WORDS;
				24'd2: pending_word <= SS_MAGIC;
				24'd3: pending_word <= SS_VERSION;
				24'd4: pending_word <= CORE_WORDS;
				24'd5: pending_word <= 32'd0;
				24'd6: pending_word <= saved_crc;
				default: pending_word <= 32'd0;
				endcase
				pending_valid <= 1'b1;
				if (word_idx == 24'd7) state <= S_COUNT;
			end
		end

		S_COUNT: begin
			if (!pending_valid && !ddr_write) begin
				// Bumping the counter is what publishes the save. Doing it
				// last means a partial payload is never persisted.
				word_idx      <= 24'd0;
				pending_word  <= 32'd1;
				pending_valid <= 1'b1;
				state         <= S_DONE;
			end
		end

		S_DONE: begin
			if (!pending_valid && !ddr_write) begin
				save_ok   <= 1'b1;
				save_busy <= 1'b0;
				state     <= S_IDLE;
			end
		end

		S_FAIL: begin
			save_fail <= 1'b1;
			save_busy <= 1'b0;
			state     <= S_IDLE;
		end

		default: state <= S_IDLE;
		endcase
	end
end

endmodule
