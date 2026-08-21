// Save state diagnostics — Main_MiSTer side.
//
// A debugging aid, not a feature. Two faults in the save state path are
// invisible from userspace and this exists to make them visible:
//
//   1. The core's OSD toast never appears. A save produces only the host's own
//      "Saving the state" from process_ss(); the core's "Save state: saved"
//      never shows. That could be the core never raising info_req, hps_io never
//      latching it, the host never reading it back, or show_core_info()
//      resolving it to an empty string — and from outside, all four look the
//      same.
//   2. A restore runs and then crashes the Amiga. Which validation gates
//      passed, whether the sequence completed, and where it died are all
//      unrecorded.
//
// TRANSPORT. hps_ext.v publishes a read-only status window on UIO class 0xF600
// (io_din[15:9] == 7'b1111011), assembled in Minimig.sv from ss_ctrl's
// diagnostic ports and its own outcome synchronisers. It is deliberately NOT on
// the info_req / UIO_INFO_GET path, because that path is itself under suspicion:
// a diagnostic carried on the channel it is meant to diagnose cannot tell its
// own silence from the fault it is looking for.
//
// The read is a plain status mux in hps_ext with no strobe and no state, so
// polling it cannot disturb what it observes — no FIFO is popped, no ack is
// cleared, and ss_ctrl never sees it at all.
//
// WINDOW LAYOUT. Nine 16-bit words per read: a fixed 0x55AA signature followed
// by eight payload words. Kept in step with the `assign ss_diag` block in
// Minimig.sv; word 7 carries a layout version so the two can disagree out loud.
//
//   sig   0x55AA
//   w0    [5:0] live ss_ctrl state, [13:8] last non-idle state
//   w1    [3:0] {load_fail, load_ok, save_fail, save_ok}
//         [7:4] load_fail_code
//         [11:8] {rom_scan, freeze, load_busy, save_busy}
//   w2    state-change counter (wraps at 16 bits)
//   w3    progress index, low  16 bits
//   w4    progress index, high  8 bits
//   w5    [7:0] ss_state_fanout completions, [15:8] freeze falling edges
//   w6    [7:0] code of the last OSD info request, [15:8] how many were raised
//   w7    layout version
//
// WHY THE INTERESTING PART IS LATCHED IN RTL. ss_ctrl runs at 113.5 MHz and
// most of its states last a handful of clocks. Sampling the live state at any
// rate a userspace poller can manage would show S_IDLE essentially always. The
// last non-idle state and the change counter are therefore latched in hardware;
// this file mostly formats them.

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../spi.h"
#include "../../user_io.h"
#include "../../hardware.h"
#include "minimig_ssdiag.h"

#define SS_DIAG_LOG        "/tmp/ss_dbg.log"
#define SS_DIAG_ADDR       0xF600

// Live memory peek, same UIO class with bit 5 set. The request rides in the
// 32-bit address: class word carries address bits [24:17] in its spare bits,
// the second word carries [16:1]. Ask in one transaction, read the answer in
// the next -- the core needs a few microseconds to borrow the SDRAM port.
//
// Driven by a file so it can be used from a shell: write a hex WORD address
// into /tmp/ss_peek and the next poll reads four longwords from there into
// the log. Chip RAM byte address B is word address B/2.
#define SS_PEEK_BIT        0x0020
#define SS_PEEK_ARM        0x0010   // io_din[4]: fetch. Without it, read only.
#define SS_PEEK_TRIGGER    "/tmp/ss_peek"
// 13 peek words, then 8 of PC samples and 4 of fingerprints.
#define SS_PEEK_WORDS      52   // byte_cnt 3..54
#define SS_DIAG_SIG        0x55AA
#define SS_DIAG_PAYLOAD    8
// Two words of slack past the signature's expected position. The alignment of
// the first readable word after spi32_w() is a convention of the UIO bus, not
// something stated anywhere in the RTL, so the signature is searched for rather
// than assumed — and a shift, if there ever is one, shows up in the log as a
// number instead of as silently wrong data.
#define SS_DIAG_SLACK      2
#define SS_DIAG_WORDS      (1 + SS_DIAG_PAYLOAD + SS_DIAG_SLACK)

#define SS_DIAG_PERIOD_MS  50
// Consecutive signature misses before this concludes the core has no window and
// stops. Matches the version-skew guard cdtv_push_sector() already applies to
// the sector credit: the core and this binary live in separate repositories and
// are deployed independently, so "reading zeros" is a state that has to be
// named rather than logged forever as a plausible all-idle machine.
#define SS_DIAG_GIVEUP     20

// ss_ctrl.v's state encoding. Duplicated deliberately: these numbers are the
// interface, and a table generated from the same source as the RTL could not
// catch a renumbering. rtl/tb/ss_ctrl_tb.v pins the four that matter.
static const char *ss_state_name(unsigned s)
{
	switch (s)
	{
	case 0:  return "S_IDLE";
	case 1:  return "S_WAIT";
	case 2:  return "S_STATE_CAP";
	case 3:  return "S_STATE_DRAIN";
	case 4:  return "S_CHIP_ISSUE";
	case 5:  return "S_CHIP_WAIT";
	case 6:  return "S_CHIP_DRAIN";
	case 7:  return "S_HEADER";
	case 8:  return "S_COUNT";
	case 9:  return "S_DONE";
	case 10: return "S_FAIL";
	case 11: return "S_L_RD_ISSUE";
	case 12: return "S_L_RD_WAIT";
	case 13: return "S_L_RD_DATA";
	case 14: return "S_L_MAGIC";
	case 15: return "S_L_LEN_CAP";
	case 16: return "S_L_LEN_CHK";
	case 17: return "S_L_CRC_CAP";
	case 18: return "S_L_PAY_FETCH";
	case 19: return "S_L_PAY_FEED";
	case 20: return "S_L_FREEZE";
	case 21: return "S_L_ST_FETCH";
	case 22: return "S_L_ST_FEED";
	case 23: return "S_L_ST_WAIT";
	case 24: return "S_L_CH_FETCH";
	case 25: return "S_L_CH_ISSUE";
	case 26: return "S_L_CH_WAIT";
	case 27: return "S_L_RELEASE";
	case 28: return "S_KICK_ISSUE";
	case 29: return "S_KICK_WAIT";
	case 30: return "S_KICK_DRAIN";
	case 31: return "S_PAY_KICK";
	case 32: return "S_L_KICK_CHK";
	default: return "?";
	}
}

static const char *ss_fail_name(unsigned c)
{
	switch (c)
	{
	case 0: return "none";
	case 1: return "MAGIC";
	case 2: return "VERSION";
	case 3: return "LENGTH";
	case 4: return "CRC";
	case 5: return "KICK";
	case 6: return "QUIESCE";
	default: return "?";
	}
}

// Same writer as akiko_diag() / cdtv_diag(): open, append one timestamped line,
// flush, close. Opening per line rather than holding the file costs nothing at
// this rate and means a log survives whatever the core does next, including a
// reset that takes the process with it.
static void ss_diag_write(const char *fmt, va_list ap)
{
	FILE *f = fopen(SS_DIAG_LOG, "a");
	if (!f) return;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	fprintf(f, "[%6lu.%06lu] ", (unsigned long)ts.tv_sec, (unsigned long)(ts.tv_nsec / 1000));
	vfprintf(f, fmt, ap);
	fputc('\n', f);
	fflush(f);
	fclose(f);
}

static void ss_diag(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void ss_diag(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	ss_diag_write(fmt, ap);
	va_end(ap);
}

void minimig_ss_diag_log(const char *fmt, ...)
{
	if (!is_minimig()) return;
	va_list ap;
	va_start(ap, fmt);
	ss_diag_write(fmt, ap);
	va_end(ap);
}

// One transaction, SS_DIAG_WORDS words. Mirrors cdtv_sec_space_bytes()'s shape
// exactly: command byte, 32-bit sub-channel address, then reads.
static void ss_diag_read(uint16_t *w)
{
	EnableIO();
	spi8(UIO_DMA_READ);
	spi32_w(SS_DIAG_ADDR);
	for (int i = 0; i < SS_DIAG_WORDS; i++) w[i] = (uint16_t)spi_w(0);
	DisableIO();
}

// One peek transaction. `byte_addr` is an AMIGA BYTE address; the core's
// ss_peek_addr is a [24:1] WORD vector, so it is halved on the way in.
//
// Byte in, word inside, because every use of a peek is a comparison against a
// save file and the file is byte-addressed. Taking words here cost most of a
// debugging session: a peek of $0357C0 read $06AF80, the values disagreed with
// the file, and that read as "chip RAM is only partly restored" -- a bug that
// did not exist. The units are not written on the trigger file, and a caller
// with an address out of a disassembly has no reason to halve it.
static void ss_peek_request(uint32_t byte_addr)
{
	uint32_t addr = byte_addr >> 1;
	// hps_ext.v:257 loads ss_peek_addr[16:1] from the low word, so that word
	// carries address bits 15:0 and the class-word field has to continue at
	// bit 16 -- not 17. Shifting by 17 dropped bit 16 and shifted every higher
	// bit down one, so a peek of $039CD8 read $019CD8 and quietly returned the
	// wrong memory. Only addresses under $10000 were unaffected, which is why
	// the first checks against a save file agreed and the ones at a real PC
	// did not.
	uint32_t hi = (addr >> 16) & 0x7F;
	uint16_t cls = (uint16_t)(SS_DIAG_ADDR | SS_PEEK_BIT | SS_PEEK_ARM
	                          | ((hi & 0x70) << 2)   // -> io_din[8:6]
	                          | (hi & 0x0F));        // -> io_din[3:0]
	EnableIO();
	spi8(UIO_DMA_READ);
	spi_w(cls);
	spi_w((uint16_t)(addr & 0xFFFF));
	DisableIO();
}

static void ss_peek_read(uint16_t *w)
{
	EnableIO();
	spi8(UIO_DMA_READ);
	// No arm bit: this reads the last fetch back and must not start another,
	// or it would clear the valid flag it is here to report.
	spi_w((uint16_t)(SS_DIAG_ADDR | SS_PEEK_BIT));
	spi_w(0);
	for (int i = 0; i < SS_PEEK_WORDS; i++) w[i] = (uint16_t)spi_w(0);
	DisableIO();
}

// Poll the trigger file. Two-step so the core has time to fetch: the address
// is requested on one call and read back on the next.
static void ss_peek_service(void)
{
	static uint32_t pending = 0;
	static int      armed   = 0;

	if (armed) {
		uint16_t w[SS_PEEK_WORDS] = {};
		ss_peek_read(w);
		armed = 0;
		if (w[0] != 0x5A5A) {
			ss_diag("peek $%06X: core has no peek window (sig %04X)", pending, w[0]);
			return;
		}

		// The restore post-mortem shares this window and does NOT depend on the
		// peek succeeding -- it is filled by the last restore, not by this
		// request. Logged before the validity check for exactly that reason:
		// with it after, a peek that timed out took the post-mortem with it,
		// which is what happened on the first machine that needed it.
		// w[0] is the word at byte_cnt 3, so byte_cnt N is w[N-3]. The PC
		// samples start at byte_cnt 13 and the fingerprints at 21. Getting this
		// wrong shifted every field by three and printed a fingerprint mismatch
		// that did not exist.
		ss_diag("  after restore: PC %04X%04X %04X%04X %04X%04X %04X%04X",
		        w[11], w[10], w[13], w[12], w[15], w[14], w[17], w[16]);
		ss_diag("  kickstart: live %04X%04X  file %04X%04X%s",
		        w[19], w[18], w[21], w[20],
		        (w[19] == w[21] && w[18] == w[20]) ? "  (match)" : "  (DIFFER)");

		// byte_cnt 12 is the valid flag (hps_ext.v:396), so it is w[9] -- NOT
		// w[12], which is byte_cnt 15, the high half of a PC sample. That word
		// is even for any PC in chip RAM, so the test read "invalid" every
		// time and no peek ever reported a byte of memory. The same off-by-
		// three was fixed in the field prints above and missed here.
		// Live chipset state, byte_cnt 25-27 -> w[22..24]. Read every time the
		// window is, because the question after a restore that resumes into a
		// frame-wait loop is not what the file held but what the machine holds
		// now: whether Paula's enable mask survived the replay, and whether
		// vertical blanks are still happening at all.
		ss_diag("  live: INTENA %04X  INTREQ %04X  frames %u",
		        w[22], w[23], (unsigned)(w[24] & 0xFF));
		// The beam, byte_cnt 50-54 -> w[47..51]. "frames" above says whether
		// any vertical blank happened at all; these say why not. A vpos_max
		// far below the frame height with hpos_max at htotal means the line
		// counter is running and the frame never ends; vbl_int is Paula's
		// VERTB request, one step upstream of the interrupt the game waits on.
		ss_diag("  beam: vpos %u  vpos_max %u  hpos_max %u  vbl_int %u",
		        (unsigned)(w[47] & 0x7FF), (unsigned)(w[48] & 0x7FF),
		        (unsigned)(w[49] & 0x1FF), (unsigned)(w[50] & 0xFF));
		ss_diag("  geometry: htotal %u  varbeamen %u  harddis %u",
		        (unsigned)(w[51] & 0x1FF), (unsigned)((w[51] >> 9) & 1),
		        (unsigned)((w[51] >> 10) & 1));
		// Latched since the last restore STARTED, so this names whatever
		// rebooted the Amiga as a result of that restore. All three land on
		// the same reset wire in minimig.v and are indistinguishable from the
		// outside: the game just restarts from the beginning.
		ss_diag("  reset since restore: %s%s%s%s",
		        (w[25] & 1) ? "HOST(cpurst) " : "",
		        (w[25] & 2) ? "sys_reset " : "",
		        (w[25] & 4) ? "CPU-RESET-instruction " : "",
		        (w[25] & 7) ? "" : "(none)");
		if (w[25] & 4)
			ss_diag("  first RESET instruction at PC $%04X%04X", w[27], w[26]);
		// The fault that put the CPU where it should not be. Vector offsets:
		// 08 bus error, 0C address error, 10 illegal, 14 divide by zero,
		// 20 privilege violation, 2C line-F. Interrupts ($60+) are counted
		// instead, so a zero count means none were ever taken.
		{
			const char *what = "?";
			switch (w[28]) {
				case 0x00: what = "none";                break;
				case 0x08: what = "BUS ERROR";           break;
				case 0x0C: what = "ADDRESS ERROR";       break;
				case 0x10: what = "ILLEGAL INSTRUCTION"; break;
				case 0x14: what = "DIVIDE BY ZERO";      break;
				case 0x20: what = "PRIVILEGE VIOLATION"; break;
				case 0x28: what = "LINE-A";              break;
				case 0x2C: what = "LINE-F";              break;
			}
			// All of these freeze at the reboot, so they describe the window
			// between the restore and the reset and nothing after it. A vector
			// of $00 with a nonzero interrupt count means the machine took
			// interrupts, faulted at nothing, and still reset -- i.e. the code
			// executed RESET on purpose.
			ss_diag("  first fault: vector $%02X (%s) at PC $%04X%04X  SR %04X",
			        w[28], what, w[30], w[29], w[32]);
			ss_diag("  before the reset: %u interrupts, %u frames",
			        (unsigned)(w[31] & 0xFF), (unsigned)(w[24] & 0xFF));
			// Live VBR against what the state vector recorded. This game
			// contains movec-to-VBR code and its level-3 vector at physical
			// $6C points mid-routine into ROM, which a running machine could
			// not survive if it were really using it.
			ss_diag("  live: VBR $%04X%04X  SR %04X", w[34], w[33], w[35]);
			// Where the first interrupt actually went. The vector at $6C is
			// $00F8679A, which disassembles as the middle of a ROM routine --
			// if the entry below reads that, the CPU really did use it and the
			// question becomes why the running machine survives the same
			// dispatch. Anything else means the vector fetch itself is wrong.
			if (w[36])
				ss_diag("  first interrupt: vector $%02X from PC $%04X%04X -> entry $%04X%04X",
				        w[36], w[38], w[37], w[40], w[39]);
			else
				ss_diag("  first interrupt: none recorded");
			// Free-running, so these describe normal running rather than a
			// restore. A level-3 count that stays at zero while the game plays
			// means the machine never takes that interrupt, and the restore
			// introduces it.
			ss_diag("  since power-on: %u level-3 interrupts, %u interrupts total",
			        w[41], w[42]);
			// The most recent NORMAL level-3 dispatch. If this matches the
			// entry recorded after a restore, the same handler runs both times
			// and the restore differs only in the register context it hands
			// it. If it differs, the vector the CPU fetches is not the one in
			// the memory we peek.
			ss_diag("  last level-3: from PC $%04X%04X -> entry $%04X%04X",
			        w[46], w[45], w[44], w[43]);
		}

		if (!(w[9] & 1)) {
			ss_diag("peek $%06X: not valid yet", pending);
			return;
		}
		// Words 1..8 are four longwords, low address first, 68k byte order --
		// the same order the chip RAM section of a save state uses, so the two
		// can be compared directly.
		ss_diag("peek $%06X: %04X%04X %04X%04X %04X%04X %04X%04X",
		        pending, w[8], w[7], w[6], w[5], w[4], w[3], w[2], w[1]);

		return;
	}

	FILE *f = fopen(SS_PEEK_TRIGGER, "r");
	if (!f) return;
	char buf[32] = {};
	if (fgets(buf, sizeof(buf), f)) {
		pending = (uint32_t)strtoul(buf, NULL, 0) & 0xFFFFFF;
		ss_peek_request(pending);
		armed = 1;
	}
	fclose(f);
	remove(SS_PEEK_TRIGGER);
}

void minimig_ss_diag_poll(void)
{
	static unsigned long timer = 0;
	static int  give_up = 0;
	static int  misses = 0;
	static int  primed = 0;
	static uint16_t prev[SS_DIAG_PAYLOAD];

	if (!is_minimig() || give_up) return;
	if (timer && !CheckTimer(timer)) return;
	timer = GetTimer(SS_DIAG_PERIOD_MS);

	ss_peek_service();

	uint16_t raw[SS_DIAG_WORDS];
	ss_diag_read(raw);

	// Find the signature rather than assuming it sits at raw[0]. A core without
	// the window drives nothing for this class and every word reads back zero,
	// which is exactly why the window starts with a constant that zero is not.
	int base = -1;
	for (int i = 0; i <= SS_DIAG_SLACK; i++)
	{
		if (raw[i] == SS_DIAG_SIG) { base = i + 1; break; }
	}

	if (base < 0)
	{
		if (++misses >= SS_DIAG_GIVEUP)
		{
			give_up = 1;
			ss_diag("diag: no 0x%04X signature in %d polls (raw %04x %04x %04x) -- "
			        "core has no 0xF600 window; giving up",
			        (unsigned)SS_DIAG_SIG, misses,
			        (unsigned)raw[0], (unsigned)raw[1], (unsigned)raw[2]);
		}
		return;
	}
	misses = 0;

	const uint16_t *w = raw + base;

	if (!primed)
	{
		primed = 1;
		ss_diag("diag: window found at word %d, layout version %u, raw %04x %04x "
		        "%04x %04x %04x %04x %04x %04x",
		        base, (unsigned)w[7],
		        (unsigned)w[0], (unsigned)w[1], (unsigned)w[2], (unsigned)w[3],
		        (unsigned)w[4], (unsigned)w[5], (unsigned)w[6], (unsigned)w[7]);
	}
	else if (!memcmp(prev, w, sizeof(prev)))
	{
		// Nothing moved. This is the overwhelmingly common case and is what
		// makes the poll cheap enough to leave enabled.
		return;
	}
	memcpy(prev, w, sizeof(prev));

	unsigned live      =  w[0]        & 0x3f;
	unsigned last      = (w[0] >> 8)  & 0x3f;
	unsigned save_ok   =  w[1]        & 1;
	unsigned save_fail = (w[1] >> 1)  & 1;
	unsigned load_ok   = (w[1] >> 2)  & 1;
	unsigned load_fail = (w[1] >> 3)  & 1;
	unsigned code      = (w[1] >> 4)  & 0x0f;
	unsigned save_busy = (w[1] >> 8)  & 1;
	unsigned load_busy = (w[1] >> 9)  & 1;
	unsigned freeze    = (w[1] >> 10) & 1;
	unsigned rom_scan  = (w[1] >> 11) & 1;
	unsigned seq       =  (unsigned)w[2];
	uint32_t idx       = (uint32_t)w[3] | ((uint32_t)(w[4] & 0xff) << 16);
	unsigned fanout    =  w[5]        & 0xff;
	unsigned ffall     = (w[5] >> 8)  & 0xff;
	unsigned info_code =  w[6]        & 0xff;
	unsigned info_cnt  = (w[6] >> 8)  & 0xff;

	ss_diag("core state=%u(%s) last=%u(%s) seq=%u idx=%u "
	        "save_busy=%u load_busy=%u freeze=%u rom_scan=%u "
	        "save_ok=%u save_fail=%u load_ok=%u load_fail=%u code=%u(%s) "
	        "freeze_falls=%u fanout=%u info_reqs=%u info_last=%u",
	        live, ss_state_name(live), last, ss_state_name(last),
	        seq, (unsigned)idx,
	        save_busy, load_busy, freeze, rom_scan,
	        save_ok, save_fail, load_ok, load_fail, code, ss_fail_name(code),
	        ffall, fanout, info_cnt, info_code);
}
