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
//
// The restore direction is the mirror image. `load_req` reads the window back
// out of DDR3 and checks it -- magic, version, length and the payload CRC,
// cheapest gate first. Only once the last gate has passed does it freeze the
// machine, stream the state vector into ss_serdes's load side, and write chip
// RAM back through ss_dma in write_mode. `load_ok` means the machine was
// restored, not merely that the file looked plausible.
//
// Two orderings in that sequence are load-bearing, and both are asserted in
// ss_ctrl_tb.v by snapshotting counters at the falling edge of `freeze` rather
// than by checking that each event merely happened:
//
//  - Chip RAM and the state vector are both fully in place BEFORE the freeze
//    is released. A CPU let go against half-restored memory does not crash
//    where the bug is; it runs on and misbehaves later, which reads like a
//    game bug rather than a save state bug.
//  - The freeze falls exactly ONCE. A sequence that dropped and re-raised it
//    would run the Amiga for a few instructions mid-restore, with the same
//    consequence.
//
// The counter at window word 0 is never touched on restore. It is the host
// poller's save handshake (user_io.cpp:1969); writing it would make the host
// believe a new save had appeared and persist the window straight back over
// the file that was just loaded. Nothing on the restore path calls
// queue_word(), so the path issues no DDR3 write at all -- which the bench
// checks directly, because that is the property the counter rule follows from.
//
// Validation runs with the machine still running. Nothing before the last gate
// asserts either of ss_quiesce's two `req` sources, so no refusal can freeze
// the Amiga. That is not incidental. A rejected restore has to be a no-op: a
// validator that stopped the machine in order to say no would be worse than
// the corrupt file it was guarding against, since the user is left with a dead
// Amiga and a file that was never loaded anyway. ss_ctrl_tb.v holds a sticky
// flag on `freeze` across every refusal case to keep it that way.

module ss_ctrl
#(
	parameter STATE_W    = 64,
	parameter CHIP_WORDS = 24'h100000,  // 16-bit words; 0x100000 = 2 MB
	// 32-bit words in one DDR3 slot: 0x400000 bytes / 4. Only the restore
	// path uses it, as the upper bound on a header length field that arrives
	// from a file and is therefore untrusted.
	parameter SLOT_WORDS = 32'h100000
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

	// Restore. load_req is edge triggered: a level held high by an OSD row
	// that has not been let go of must not start a second restore the instant
	// the first one finishes. load_ok / load_fail / load_fail_code are levels,
	// held until the next attempt starts -- a fail code that vanished a cycle
	// after the refusal could not reach a toast or a log. load_ok means the
	// machine now holds the saved state, not that the file parsed.
	input                     load_req,
	output reg                load_busy,
	output reg                load_ok,
	output reg                load_fail,
	output reg [3:0]          load_fail_code,

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
	output     [15:0]         sd_wr,
	input      [15:0]         sd_rd,
	input                     sd_ready,

	output reg [28:0]         ddr_address,
	output reg [63:0]         ddr_writedata,
	output reg [7:0]          ddr_byteenable,
	output reg                ddr_write,
	output reg                ddr_read,
	input      [63:0]         ddr_readdata,
	input                     ddr_readdatavalid,
	input                     ddr_waitrequest
);

localparam STATE_WORDS = (STATE_W + 31) / 32;
localparam CHIP_PAIRS  = CHIP_WORDS / 2;         // 32-bit words of packed chip RAM
localparam CORE_WORDS  = STATE_WORDS + CHIP_PAIRS;

localparam SS_MAGIC   = 32'h53534341;
localparam SS_VERSION = 32'h00010000;

// Sized copies of CORE_WORDS for the comparisons on the restore path, so a
// 24-bit counter and a 32-bit header word are each compared against something
// of their own width rather than against an unsized integer.
localparam [23:0] CORE_WORDS_24  = CORE_WORDS;
localparam [31:0] CORE_WORDS_32  = CORE_WORDS;
localparam [23:0] STATE_WORDS_24 = STATE_WORDS;

// `length` counts the words after word 1: the six-word header tail (words
// 2..7) plus the payload. A file this core can restore must carry at least
// its own core payload, and cannot claim more words than fit in the slot.
// The host parser applies the same two bounds (minimig_savestate.cpp).
localparam [31:0] MIN_LENGTH = 32'd6 + CORE_WORDS_32;
localparam [31:0] MAX_LENGTH = SLOT_WORDS - 32'd2;

localparam [4:0] S_IDLE        = 5'd0;
localparam [4:0] S_WAIT        = 5'd1;
localparam [4:0] S_STATE_CAP   = 5'd2;
localparam [4:0] S_STATE_DRAIN = 5'd3;
localparam [4:0] S_CHIP_ISSUE  = 5'd4;
localparam [4:0] S_CHIP_WAIT   = 5'd5;
localparam [4:0] S_CHIP_DRAIN  = 5'd6;
localparam [4:0] S_HEADER      = 5'd7;
localparam [4:0] S_COUNT       = 5'd8;
localparam [4:0] S_DONE        = 5'd9;
localparam [4:0] S_FAIL        = 5'd10;

// Restore. S_L_RD_* is a shared single-word read step: callers set rd_idx and
// rd_ret and jump to S_L_RD_ISSUE, and land back in rd_ret with rd_data
// holding the 64-bit DDR3 word that contains it.
localparam [4:0] S_L_RD_ISSUE  = 5'd11;
localparam [4:0] S_L_RD_WAIT   = 5'd12;
localparam [4:0] S_L_RD_DATA   = 5'd13;
localparam [4:0] S_L_MAGIC     = 5'd14;
localparam [4:0] S_L_LEN_CAP   = 5'd15;
localparam [4:0] S_L_LEN_CHK   = 5'd16;
localparam [4:0] S_L_CRC_CAP   = 5'd17;
localparam [4:0] S_L_PAY_FETCH = 5'd18;
localparam [4:0] S_L_PAY_FEED  = 5'd19;

// Restore proper, entered only once every gate above has passed.
localparam [4:0] S_L_FREEZE    = 5'd20;
localparam [4:0] S_L_ST_FETCH  = 5'd21;
localparam [4:0] S_L_ST_FEED   = 5'd22;
localparam [4:0] S_L_ST_WAIT   = 5'd23;
localparam [4:0] S_L_CH_FETCH  = 5'd24;
localparam [4:0] S_L_CH_ISSUE  = 5'd25;
localparam [4:0] S_L_CH_WAIT   = 5'd26;
localparam [4:0] S_L_RELEASE   = 5'd27;

// Refusal reasons, as seen by the OSD. 5 is reserved for the Kickstart
// fingerprint, which is not captured yet.
localparam [3:0] FAIL_MAGIC   = 4'd1;
localparam [3:0] FAIL_VERSION = 4'd2;
localparam [3:0] FAIL_LENGTH  = 4'd3;
localparam [3:0] FAIL_CRC     = 4'd4;
// The file was good but the machine would not stand still long enough to have
// it put back. Distinct from the four above, which are verdicts on the file:
// this one says try again, not throw the file away. Nothing has been written
// at the point it can fire, so it is as much a no-op as the other four.
localparam [3:0] FAIL_QUIESCE = 4'd6;

// ---------------------------------------------------------------- quiesce

wire quiesced;
wire timeout;

// restore_busy is the restore path's own freeze request, and it is raised in
// exactly one place: S_L_PAY_FETCH, after the payload CRC has matched. Every
// refusal happens strictly before that, so no rejected file can stop the
// Amiga -- see the module header, and the sticky freeze watch in
// ss_ctrl_tb.v that holds this design to it.
reg restore_busy;

ss_quiesce quiesce
(
	.clk(clk), .rst_n(rst_n),
	.req(save_busy || restore_busy),
	.blit_busy(blit_busy), .disk_busy(disk_busy), .audio_busy(audio_busy),
	.cpu_boundary(cpu_boundary), .frame_tick(frame_tick),
	.freeze(freeze), .quiesced(quiesced), .timeout(timeout)
);

// ---------------------------------------------------------------- serdes

reg               ser_save_start;
wire              ser_word_valid;
wire [31:0]       ser_word_out;
wire              ser_save_done;

reg               ser_load_start;
reg               ser_word_in_valid;
reg  [31:0]       ser_word_in;
wire              ser_load_done;

ss_serdes #(.WIDTH(STATE_W)) serdes
(
	.clk(clk), .rst_n(rst_n),
	.save_start(ser_save_start), .state_in(state_in),
	.word_valid(ser_word_valid), .word_out(ser_word_out),
	.save_done(ser_save_done),
	.load_start(ser_load_start), .word_in_valid(ser_word_in_valid),
	.word_in(ser_word_in),
	.load_done(ser_load_done), .state_out(state_out), .state_we(state_we)
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
reg         dma_write_mode;
reg  [15:0] dma_word_in;
wire        dma_word_req;
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
	// Both directions. write_mode is low for the whole save and high for the
	// whole chip RAM half of a restore; it is never changed with a transfer in
	// flight, because it also selects the bus encoding (sd_state 2 vs 3) that
	// the address currently on the wire was issued under.
	.write_mode(dma_write_mode), .word_in(dma_word_in),
	.word_req(dma_word_req), .sd_wr(sd_wr),
	.word_valid(dma_word_valid), .word_out(dma_word_out), .done(dma_done)
);

reg [24:1] chip_addr;
reg [23:0] pair_count;
reg [15:0] chip_low;
reg        pending_low_held;

// Restore side of the pair: the second (odd address) half is held here while
// ss_dma is still writing the first, and handed over on its word_req.
reg [15:0] chip_high;
reg        chip_wr_half;

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
//
// The restore path feeds the CRC without any DDR3 write attached, so it waits
// on crc_busy alone; the same one-cycle argument applies to it, and for the
// same reason. Sampling crc_value while crc_wr is still up compares a CRC
// that is missing the payload's last byte against the file's, which would
// reject a perfectly good state file (or, for a file whose corruption happens
// to live in that last byte, accept a bad one).
wire crc_busy  = (crc_bytes_left != 3'd0) || crc_wr;
wire word_busy = crc_busy || pending_valid || ddr_write;

// ---------------------------------------------------------------- writer

reg [4:0]  state;
reg [23:0] word_idx;      // 32-bit word index within the window
reg [31:0] pending_word;
reg        pending_valid;
reg [31:0] saved_crc;

// ---------------------------------------------------------------- reader

reg        load_req_d;
reg [23:0] rd_idx;        // 32-bit word index being read
reg [63:0] rd_data;       // the 64-bit DDR3 word that contains it
reg [4:0]  rd_ret;        // state to resume in once rd_data is valid
reg [31:0] hdr_length;
reg [31:0] want_crc;
reg [23:0] pay_idx;       // payload word index, 0 .. CORE_WORDS

wire load_req_rise = load_req && !load_req_d;

// The payload starts at window word 8, which is even, so each 64-bit read
// carries two consecutive payload words and bit 0 of the payload index picks
// the half. One expression for all three consumers (the CRC scan, the state
// vector feed and the chip RAM writeback), because a second one that disagreed
// would read half the payload out of a file it had just checked.
wire [31:0] pay_word = pay_idx[0] ? rd_data[63:32] : rd_data[31:0];

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

// CRC a 32-bit word without writing anything: the restore path is checking
// the payload, not producing it. Callers must only invoke this when crc_busy
// is false.
task crc_word;
	input [31:0] w;
begin
	crc_shift      <= w;
	crc_bytes_left <= 3'd4;
end
endtask

// Refuse the file and go back to idle with the machine untouched. There is no
// partial-restore path to unwind because nothing has been restored: every
// gate runs before the freeze is ever requested.
task load_reject;
	input [3:0] code;
begin
	load_fail_code <= code;
	load_fail      <= 1'b1;
	load_busy      <= 1'b0;
	ddr_read       <= 1'b0;
	state          <= S_IDLE;
end
endtask

always @(posedge clk) begin
	if (!rst_n) begin
		state            <= S_IDLE;
		save_busy        <= 1'b0;
		save_ok          <= 1'b0;
		save_fail        <= 1'b0;
		load_busy        <= 1'b0;
		load_ok          <= 1'b0;
		load_fail        <= 1'b0;
		load_fail_code   <= 4'd0;
		load_req_d       <= 1'b0;
		ddr_read         <= 1'b0;
		ddr_write        <= 1'b0;
		ddr_byteenable   <= 8'h00;
		pending_valid    <= 1'b0;
		ser_save_start   <= 1'b0;
		ser_load_start   <= 1'b0;
		ser_word_in_valid<= 1'b0;
		dma_start        <= 1'b0;
		dma_write_mode   <= 1'b0;
		restore_busy     <= 1'b0;
		chip_wr_half     <= 1'b0;
		crc_init         <= 1'b0;
		cap_idx          <= 16'd0;
		drain_idx        <= 16'd0;
		pair_count       <= 24'd0;
		pending_low_held <= 1'b0;
		crc_wr           <= 1'b0;
		crc_bytes_left   <= 3'd0;
	end
	else begin
		ser_save_start    <= 1'b0;
		ser_load_start    <= 1'b0;
		ser_word_in_valid <= 1'b0;
		dma_start         <= 1'b0;
		crc_init          <= 1'b0;
		load_req_d        <= load_req;

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
			else if (load_req_rise) begin
				// Cheapest gate first, so a file that is not ours costs one
				// DDR3 read rather than a scan of the whole payload. Word 1
				// of the window holds magic in its low half and version in
				// its high half, which is both of them for that one read.
				load_busy      <= 1'b1;
				load_ok        <= 1'b0;
				load_fail      <= 1'b0;
				load_fail_code <= 4'd0;
				rd_idx         <= 24'd2;
				rd_ret         <= S_L_MAGIC;
				state          <= S_L_RD_ISSUE;
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
					// Byte-swap each 16-bit word into the pair. The pair is
					// written to DDR3 as one little-endian 32-bit word, so
					// without this each Amiga word's two bytes land reversed
					// in the file and nothing in the payload can be read as
					// Amiga memory -- measured on hardware on the first real
					// save state, 2026-08-10.
					//
					// Done here rather than in ss_dma, which stays a plain
					// memory reader with no opinion about endianness.
					queue_word({dma_word_out[7:0], dma_word_out[15:8],
					            chip_low[7:0],     chip_low[15:8]});
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

		// ------------------------------------------------------- restore
		//
		// One 64-bit DDR3 read, addressed exactly as the save path addresses
		// its writes: the 64-bit address is the 32-bit word index shifted
		// down one, and bit 0 of the index picks the half. Two schemes here
		// would mean a file this module cannot read back.
		S_L_RD_ISSUE: begin
			ddr_address <= slot_base + {6'd0, rd_idx[23:1]};
			ddr_read    <= 1'b1;
			state       <= S_L_RD_WAIT;
		end

		// Avalon: the request is accepted on the first cycle waitrequest is
		// low, and the data arrives later on its own readdatavalid beat.
		// Dropping ddr_read on acceptance is what keeps that to one beat.
		S_L_RD_WAIT: begin
			if (!ddr_waitrequest) begin
				ddr_read <= 1'b0;
				state    <= S_L_RD_DATA;
			end
		end

		S_L_RD_DATA: begin
			if (ddr_readdatavalid) begin
				rd_data <= ddr_readdata;
				state   <= rd_ret;
			end
		end

		S_L_MAGIC: begin
			if (rd_data[31:0] != SS_MAGIC) load_reject(FAIL_MAGIC);
			else if (rd_data[63:32] != SS_VERSION) load_reject(FAIL_VERSION);
			else begin
				// Word 0 holds the counter, word 1 the length.
				rd_idx <= 24'd0;
				rd_ret <= S_L_LEN_CAP;
				state  <= S_L_RD_ISSUE;
			end
		end

		S_L_LEN_CAP: begin
			hdr_length <= rd_data[63:32];
			// Word 4 holds core_words, word 5 host_words.
			rd_idx     <= 24'd4;
			rd_ret     <= S_L_LEN_CHK;
			state      <= S_L_RD_ISSUE;
		end

		// core_words is checked here rather than left to the CRC. A file
		// whose payload is a different length is another build's state file,
		// and saying so is a better diagnosis than the CRC mismatch it would
		// otherwise produce -- the user can act on "wrong core", not on
		// "corrupt".
		S_L_LEN_CHK: begin
			if ((hdr_length < MIN_LENGTH) || (hdr_length > MAX_LENGTH) ||
			    (rd_data[31:0] != CORE_WORDS_32))
				load_reject(FAIL_LENGTH);
			else begin
				// Word 6 holds core_crc32, word 7 host_crc32.
				rd_idx <= 24'd6;
				rd_ret <= S_L_CRC_CAP;
				state  <= S_L_RD_ISSUE;
			end
		end

		S_L_CRC_CAP: begin
			want_crc <= rd_data[31:0];
			crc_init <= 1'b1;
			pay_idx  <= 24'd0;
			state    <= S_L_PAY_FETCH;
		end

		// The payload starts at word 8, which is even, so each 64-bit read
		// carries two consecutive payload words: fetch on the even index,
		// reuse the held rd_data on the odd one.
		S_L_PAY_FETCH: begin
			if (pay_idx == CORE_WORDS_24) begin
				// Wait for the feeder to drain before reading crc_value.
				// The last word's final byte is still being presented to
				// ss_crc32 while crc_wr is up, and is only folded into
				// crc_out on the edge that clears it -- see word_busy above
				// for the save-side bug this exact off-by-one cycle caused.
				if (!crc_busy) begin
					if (crc_value == want_crc) begin
						// Last gate passed. Only now may the machine be
						// stopped: everything above this line is a verdict
						// on the file, delivered with the Amiga still
						// running.
						restore_busy <= 1'b1;
						state        <= S_L_FREEZE;
					end
					else load_reject(FAIL_CRC);
				end
			end
			else if (pay_idx[0]) state <= S_L_PAY_FEED;
			else begin
				rd_idx <= 24'd8 + pay_idx;
				rd_ret <= S_L_PAY_FEED;
				state  <= S_L_RD_ISSUE;
			end
		end

		S_L_PAY_FEED: begin
			if (!crc_busy) begin
				crc_word(pay_word);
				pay_idx <= pay_idx + 24'd1;
				state   <= S_L_PAY_FETCH;
			end
		end

		// The freeze the whole restore runs under. It is raised once, here,
		// and dropped once, in S_L_RELEASE -- never in between. A restore that
		// let the freeze fall and rise again would run the Amiga for a handful
		// of instructions against a machine that is half old and half new,
		// which resurfaces later as a game bug rather than as a save state
		// bug. ss_ctrl_tb.v counts the falling edges for exactly that reason.
		S_L_FREEZE: begin
			if (timeout) begin
				// The file was fine; the machine would not hold still. Nothing
				// has been written yet, so dropping the request here leaves
				// the Amiga exactly as it was found.
				restore_busy <= 1'b0;
				load_reject(FAIL_QUIESCE);
			end
			else if (quiesced) begin
				ser_load_start <= 1'b1;
				pay_idx        <= 24'd0;
				state          <= S_L_ST_FETCH;
			end
		end

		// State vector first, chip RAM second. Either order would satisfy
		// "both before the release", but this one keeps the machine's own
		// registers correct for the whole of the much longer memory pass.
		S_L_ST_FETCH: begin
			if (pay_idx == STATE_WORDS_24) state <= S_L_ST_WAIT;
			else if (pay_idx[0]) state <= S_L_ST_FEED;
			else begin
				rd_idx <= 24'd8 + pay_idx;
				rd_ret <= S_L_ST_FEED;
				state  <= S_L_RD_ISSUE;
			end
		end

		// ss_serdes takes one word per word_in_valid pulse with no rate
		// requirement of its own, so unlike the save direction there is
		// nothing to buffer here: the DDR3 read step sets the pace and the
		// pulse is simply issued when a word is in hand.
		S_L_ST_FEED: begin
			ser_word_in       <= pay_word;
			ser_word_in_valid <= 1'b1;
			pay_idx           <= pay_idx + 24'd1;
			state             <= S_L_ST_FETCH;
		end

		// ser_load_done is one cycle behind state_we (see ss_serdes.v), so
		// waiting on it rather than on state_we guarantees the fan-out has
		// already happened before anything downstream of here runs.
		S_L_ST_WAIT: begin
			if (ser_load_done) begin
				if (CHIP_PAIRS == 0) state <= S_L_RELEASE;
				else begin
					chip_addr <= chip_base;
					state     <= S_L_CH_FETCH;
				end
			end
		end

		// Chip RAM. pay_idx carries straight on from the state words, so the
		// payload is walked exactly once, in order, in both directions.
		S_L_CH_FETCH: begin
			if (pay_idx == CORE_WORDS_24) state <= S_L_RELEASE;
			else if (pay_idx[0]) state <= S_L_CH_ISSUE;
			else begin
				rd_idx <= 24'd8 + pay_idx;
				rd_ret <= S_L_CH_ISSUE;
				state  <= S_L_RD_ISSUE;
			end
		end

		// Undo the capture-side byte swap, symmetrically. The file holds chip
		// RAM in 68k order -- the Amiga word at the lower address contributes
		// its high byte at the lower file offset -- so the 32-bit payload word
		// 0xHHLL_hhll carries bytes ll,hh,LL,HH and the two Amiga words are
		// {ll,hh} at the even address and {LL,HH} at the odd one.
		//
		// ss_dma wants the first word presented before `start`, which is why
		// the low half goes straight into dma_word_in here and the high half
		// is parked in chip_high until its word_req.
		S_L_CH_ISSUE: begin
			dma_word_in    <= {pay_word[7:0],   pay_word[15:8]};
			chip_high      <= {pay_word[23:16], pay_word[31:24]};
			dma_write_mode <= 1'b1;
			dma_base       <= chip_addr;
			dma_start      <= 1'b1;
			chip_wr_half   <= 1'b0;
			pay_idx        <= pay_idx + 24'd1;
			state          <= S_L_CH_WAIT;
		end

		// Count our own two word_req pulses rather than watching `done`, for
		// the same reason the save direction counts word_valid: ss_dma commits
		// `done` on the same edge as the final word's handshake, from the same
		// always block, so an if/else-if that tried to see both would lose one
		// of them (see module header).
		//
		// The second pulse means the second word's sd_ready has already been
		// taken, i.e. both writes have landed -- which is what makes it safe
		// for the release below to follow immediately.
		S_L_CH_WAIT: begin
			if (dma_word_req) begin
				if (!chip_wr_half) begin
					dma_word_in  <= chip_high;
					chip_wr_half <= 1'b1;
				end
				else begin
					chip_addr <= chip_addr + 24'd2;
					state     <= S_L_CH_FETCH;
				end
			end
		end

		// Both halves of the machine are back in place, so it may run again.
		// Word 0 of the window -- the counter -- has not been touched: it is
		// the host poller's save handshake, and bumping it here would make the
		// host believe a new save had appeared and write the window back over
		// the file it was just restored from.
		S_L_RELEASE: begin
			restore_busy   <= 1'b0;
			dma_write_mode <= 1'b0;
			load_ok        <= 1'b1;
			load_busy      <= 1'b0;
			state          <= S_IDLE;
		end

		default: state <= S_IDLE;
		endcase
	end
end

endmodule
