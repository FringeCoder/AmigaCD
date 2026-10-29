#ifndef AKIKO_PACE_H
#define AKIKO_PACE_H

#include <stdint.h>
#include <stdbool.h>

// Sector delivery pacing for the Akiko (CD32) bridge.
//
// Why a rate limit exists at all
// ------------------------------
// Enabling Z2 fast RAM with a CD mounted used to boot the CD32 core to a black
// screen (#1277 here, core #218). The controlling variable is elapsed time per
// sector: below roughly 0.4 ms it fails every time, at 1.6 ms it passes every
// time. We shipped that time as 64-word chunks with a usleep(10) between them,
// which spends it as a side effect of the transport shape.
//
// That was a mitigation for a core-side defect, now identified: ddram_ctrl's
// state 1 dropped a DDR3 read return that arrived while waitrequest was high,
// which wedges master 0 permanently -- a 68k stalled on a fast-RAM fetch. Z2
// fast RAM lives in DDR3, and Akiko's sector DMA is what makes waitrequest
// coincide with a return often enough to matter. See T24 in the core repo's
// docs/core-accuracy-todo.md.
//
// Why this module exists
// ----------------------
// The chunk gaps were never a rate limit anyone could read: usleep(10) costs a
// scheduler wakeup (~92 us on the MiSTer), so 18 of them spent ~1.7 ms while
// reading as 180 us. That became a stated floor in microseconds here, slept by
// the Main, and on 2026-10-28 a rate the core keeps -- below.
//
// What is left in the Main is the drive's mechanics (the seek and the spindle
// settle, waited out by declining requests rather than sleeping) and the
// choice of rate. Kept free of MiSTer headers so it can be unit-tested with
// the host compiler.

// The sector RATE is the core's to keep now. akiko.v starts at most one PBX
// ship per period, at the rate the Main sends on status[62:61]; the Main pushes
// each sector the moment Akiko asks and does not sleep. Before 2026-10-28 it
// slept here, start to start, in the binary's one main loop -- up to 6.7 ms a
// sector at 2x and 13.3 ms at 1x on the default Drive setting, stalling input,
// the OSD and every other bridge for most of every period, and widening the
// window in which a new READ can overtake a sector already fetched for the old
// one (the core's Test H2). Upstream moved pacing into the core in
// Minimig-AGA ce1f386; see that commit's port in AmigaCD_MiSTer for why the
// Main, not the core, decides the rate.
//
// The codes are what akiko.v decodes, not an ordering. 0 is 8x so that an
// older Main, which leaves the bits clear and paces by sleeping, is not slowed
// down by a newer core.
#define AKIKO_RATE_8X  0   // 600 sectors/s, 1667 us
#define AKIKO_RATE_1X  1   //  75 sectors/s, 13333 us
#define AKIKO_RATE_2X  2   // 150 sectors/s, 6667 us
#define AKIKO_RATE_4X  3   // 300 sectors/s, 3333 us

// The rate to send for an OSD setting (mm_cdSpeed: 0 fast, 1 2x, 2 drive) and
// the speed the guest last commanded (1 or 2, from bit 6 of cmd[8]).
//
// Fast is 8x: the 1.7 ms floor this file used to keep, which sits just above
// the measured passing side (1.64 ms) of the Z2-fast-RAM-plus-CD boot knee --
// below ~0.4 ms that boot failed every time. 8x is 1.667 ms, still above it.
// The fault behind the knee (ddram_ctrl dropping a DDR3 read return, T24 in
// the core's docs) has since been fixed, so the margin is now insurance.
//
// An unknown mode is Fast, as the OSD shows it.
int akiko_cd_rate_bits(int mode, int commanded_speed);

// The CDTV drive, which the core does not pace: its bridge has a FIFO and a
// credit, not PBX slots, so the Main still spaces the sectors of a READ -- but
// from the poll loop, one sector per poll when it is due, never by sleeping.
//
// The CR-511 is a 1x drive: 75 sectors/s, 13333 us, which is Drive (the
// default). CD32 2x gives it 2x. Fast keeps what the synchronous loop did: a
// 1 ms gap that the 3.3 ms SPI push of a sector dwarfs, i.e. as fast as the
// push goes. An unknown mode is Fast, as the OSD shows it.
int64_t cdtv_sector_period_us(int mode);

// CDTV SEEK: the drive takes no command for half a second and then raises a
// status change, as WinUAE has it (upstream Main_MiSTer 0437012). Drive mode
// only, like the CD32 seek model: the other two are for loading over realism.
#define CDTV_SEEK_US 500000

// Drive mechanics: the head has to get there, and it is not instant.
//
// We deliver the first sector of a new read immediately, from anywhere to
// anywhere. No drive does that. A 2x unit of 1993 quotes random access around
// 400 ms and a full stroke around half a second, and the cost scales with how
// far the head travels -- which is why a sequential read costs nothing and a
// jump from the directory to the far end of the disc costs the most.
//
// These numbers are MODELLED from published typical figures for drives of that
// class, not measured from a CD32, and they are deliberately on the low side:
// the shape (proportional to distance, free when sequential) is what matters
// here, and an over-long seek would be indistinguishable from the stalls this
// core spent weeks chasing. Applied only in mmcd_drive, the authentic mode.
#define AKIKO_SEEK_BASE_US    120000   // head settle plus rotational latency
#define AKIKO_SEEK_FULL_US    500000   // lead-in to lead-out

// Changing between 1x and 2x is a spindle change, not a register write.
#define AKIKO_SPEED_SETTLE_US 100000

// How long the head takes to move from `from_lba` to `to_lba` on a disc whose
// last sector is `span_lba`, in microseconds.
//
// Zero when the read continues where the last one stopped, because that is the
// case the drive is built for and the one every streaming title relies on. A
// negative `from_lba` means the head position is unknown -- after a spin-up it
// is at the lead-in, so that is what is assumed.
int64_t akiko_seek_us(int32_t from_lba, int32_t to_lba, uint32_t span_lba);

// Whether a sector request should be served at all.
//
// Two reasons to decline, and they are different failures. No armed read
// (data_lba_base < 0) means the engine is asking for data nobody requested.
// Paused means the guest asked the drive to stop, and a real drive delivers
// nothing until it is told to resume -- this one used to deliver one more
// sector into a PBX slot, which the next read then parses as its own.
bool akiko_sec_should_serve(int32_t data_lba_base, int paused);

// A pause that outlives a read is a hang, not a pause.
//
// Declining while paused is correct; declining for ever because cd_paused was
// set and never cleared is a dead machine. `since_ms` is when the declining
// started, 0 when it is not. True once it has gone on longer than `limit_ms`,
// which is the caller's cue to say so rather than to keep quiet.
bool akiko_pause_stuck(uint32_t since_ms, uint32_t now_ms, uint32_t limit_ms);

#endif
