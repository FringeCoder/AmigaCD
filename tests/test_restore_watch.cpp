// Native unit tests for ss_restore_watch(): when the host may hand the drive
// its saved position during a restore.
//
// The bug this pins (measured on hardware 2026-10-23): the host applied the
// save's Akiko block when the restore was TRIGGERED, before the core had
// validated the file. Flink's slot 4 was a 1.3 file saved with fast RAM,
// restored into a machine without any; the core refused it (FAIL_LENGTH) and
// released the Amiga untouched, but the drive had already been told
// base=-1, so the read the game had in flight was gone and every sector
// request after that hit "sec_req but no data read armed" -- 258048 of them
// in 57 s, and the game never ran again.

#include <stdint.h>
#include <stdio.h>

#include <initializer_list>

#include "../support/minimig/minimig_savestate.h"

static int failures = 0;

static void check(const char *name, bool ok)
{
	if (ok) printf("PASS %s\n", name);
	else { printf("FAIL %s\n", name); failures++; }
}

static ss_core_obs_t obs(uint8_t live, int busy, int ok, int fail, uint16_t seq)
{
	ss_core_obs_t o;
	o.live = live;
	o.load_busy = busy;
	o.load_ok = ok;
	o.load_fail = fail;
	o.seq = seq;
	return o;
}

int main(void)
{
	// Before the trigger the core is idle, and load_ok / load_fail still hold
	// the PREVIOUS restore's verdict -- they are sticky until the next arm.
	const uint16_t seq0 = 100;

	// ---- the refusal: nothing may be applied ----------------------------
	{
		ss_restore_watch_t w;
		ss_restore_watch_begin(&w, seq0);
		check("idle before the core arms: wait",
		      ss_restore_watch_step(&w, obs(SS_ST_IDLE, 0, 1, 0, seq0)) == SS_WATCH_WAIT);
		check("validating under the freeze: wait",
		      ss_restore_watch_step(&w, obs(SS_ST_L_FREEZE, 1, 0, 0, seq0 + 3)) == SS_WATCH_WAIT);
		check("payload CRC pass: still wait",
		      ss_restore_watch_step(&w, obs(SS_ST_L_PAY_FEED, 1, 0, 0, seq0 + 9)) == SS_WATCH_WAIT);
		check("refused (load_fail, idle): skip, drive untouched",
		      ss_restore_watch_step(&w, obs(SS_ST_IDLE, 0, 0, 1, seq0 + 12)) == SS_WATCH_SKIP);
	}

	// ---- the success: apply while the machine is still frozen ------------
	{
		ss_restore_watch_t w;
		ss_restore_watch_begin(&w, seq0);
		check("kick check: wait (it can still refuse)",
		      ss_restore_watch_step(&w, obs(SS_ST_L_KICK_CHK, 1, 0, 0, seq0 + 5)) == SS_WATCH_WAIT);
		check("state vector load: committed, apply",
		      ss_restore_watch_step(&w, obs(SS_ST_L_ST_FETCH, 1, 0, 0, seq0 + 6)) == SS_WATCH_APPLY);
	}
	for (uint8_t s : { SS_ST_L_ST_FEED, SS_ST_L_ST_WAIT, SS_ST_L_CH_FETCH, SS_ST_L_CH_ISSUE,
	                   SS_ST_L_CH_WAIT, SS_ST_L_RELEASE, SS_ST_L_FLUSH, SS_ST_L_SH_FETCH,
	                   SS_ST_L_SH_LOAD, SS_ST_L_REPLAY, SS_ST_L_CLUT_FETCH, SS_ST_L_CLUT_LOAD,
	                   SS_ST_L_FA_FETCH, SS_ST_L_FA_LOAD })
	{
		ss_restore_watch_t w;
		ss_restore_watch_begin(&w, seq0);
		char name[64];
		snprintf(name, sizeof(name), "load state %u is committed", (unsigned)s);
		check(name, ss_restore_watch_step(&w, obs(s, 1, 0, 0, seq0 + 1)) == SS_WATCH_APPLY);
	}
	for (uint8_t s : { SS_ST_L_RD_ISSUE, SS_ST_L_RD_WAIT, SS_ST_L_RD_DATA, SS_ST_L_MAGIC,
	                   SS_ST_L_LEN_CAP, SS_ST_L_LEN_CHK, SS_ST_L_CRC_CAP, SS_ST_L_PAY_FETCH,
	                   SS_ST_L_PAY_FEED, SS_ST_L_FREEZE, SS_ST_KICK_ISSUE, SS_ST_KICK_WAIT,
	                   SS_ST_KICK_DRAIN, SS_ST_L_KICK_CHK })
	{
		ss_restore_watch_t w;
		ss_restore_watch_begin(&w, seq0);
		char name[64];
		snprintf(name, sizeof(name), "validation state %u can still refuse", (unsigned)s);
		check(name, ss_restore_watch_step(&w, obs(s, 1, 0, 0, seq0 + 1)) == SS_WATCH_WAIT);
	}

	// ---- the host polled too slowly to see the load at all ---------------
	{
		ss_restore_watch_t w;
		ss_restore_watch_begin(&w, seq0);
		check("finished ok unseen (seq moved): apply late",
		      ss_restore_watch_step(&w, obs(SS_ST_IDLE, 0, 1, 0, seq0 + 40)) == SS_WATCH_APPLY);
	}
	{
		ss_restore_watch_t w;
		ss_restore_watch_begin(&w, seq0);
		check("finished refused unseen (seq moved): skip",
		      ss_restore_watch_step(&w, obs(SS_ST_IDLE, 0, 0, 1, seq0 + 40)) == SS_WATCH_SKIP);
	}
	{
		// A stale load_ok from the last restore, with the core not yet armed,
		// must not read as this restore's success.
		ss_restore_watch_t w;
		ss_restore_watch_begin(&w, seq0);
		check("stale verdict before arm: wait",
		      ss_restore_watch_step(&w, obs(SS_ST_IDLE, 0, 1, 0, seq0)) == SS_WATCH_WAIT);
	}
	{
		// seq is a 16-bit counter and wraps.
		ss_restore_watch_t w;
		ss_restore_watch_begin(&w, 0xFFFF);
		check("seq wrap still counts as moved",
		      ss_restore_watch_step(&w, obs(SS_ST_IDLE, 0, 0, 1, 2)) == SS_WATCH_SKIP);
	}

	printf("%s\n", failures ? "FAILED" : "ALL PASS");
	return failures ? 1 : 0;
}
