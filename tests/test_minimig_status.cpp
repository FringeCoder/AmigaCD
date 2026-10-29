// minimig_status_compose(): what reaches the AmigaCD core in status[63:32].
//
// Bits 19 and up of the extended config are not this core's status bits.
// Upstream e836aea put the drive sound volume there, at ext bits 20:19, which
// on this core are status[52:51] -- the save state slot and the save REQUEST.
// Sent as they are, picking "Quiet" would take a save state. These checks pin
// where each bit goes.
#include "../support/minimig/minimig_status.h"
#include <stdio.h>

static int fails = 0;

static void check(const char *what, unsigned int got, unsigned int want)
{
	if (got == want) printf("PASS %s\n", what);
	else { printf("FAIL %s: got %08x want %08x\n", what, got, want); fails++; }
}

// Status bit n (n >= 32) as a bit of the status[63:32] word.
#define ST(n) (1u << ((n) - 32))

int main()
{
	// The extended config's own bits pass through, cur_status's are kept.
	check("ext bits 18:0 land on status[50:32]",
	      minimig_status_compose(0, 0x0007FFFFu), 0x0007FFFFu);
	check("cur_status keeps the save state, test pattern and A2065 bits",
	      minimig_status_compose(ST(51) | ST(53) | ST(55) | ST(57), 0),
	      ST(51) | ST(53) | ST(55) | ST(57));
	// The CD sector rate (akiko_send_rate(), status[62:61]) is cur_status's.
	check("cur_status keeps the CD sector rate (status[62:61])",
	      minimig_status_compose(ST(61) | ST(62), 0), ST(61) | ST(62));
	check("the extended config cannot set the CD sector rate",
	      minimig_status_compose(0, ST(61) | ST(62)), 0);
	check("cur_status cannot set an extended config bit",
	      minimig_status_compose(0x0007FFFFu, 0), 0);

	// The drive sound volume, each level.
	for (unsigned int v = 0; v < 4; v++) {
		char what[64];
		snprintf(what, sizeof(what), "volume %u goes to status[60:59] and nowhere else", v);
		// ST(59), not MINIMIG_DSND_STATUS_SHIFT: the place is fixed by what
		// AmigaCD.sv reads (status[60:59]), and must not move with the macro.
		check(what, minimig_status_compose(0, v << 19), v * ST(59));
	}
	check("Quiet does not request a save state (status[51])",
	      minimig_status_compose(0, 1u << 19) & ST(51), 0);
	check("Normal does not move the save slot (status[52])",
	      minimig_status_compose(0, 2u << 19) & ST(52), 0);
	check("cur_status cannot set the volume",
	      minimig_status_compose(ST(59) | ST(60), 0), 0);

	// The cartridge type (upstream b57370d, ext bit 23). Upstream's core reads
	// it at status[55], this core's test pattern: picking the Action Replay
	// must not turn the test pattern on, and must reach status[63].
	check("the Action Replay goes to status[63] and nowhere else",
	      minimig_status_compose(0, 1u << 23), ST(63));
	check("the Action Replay does not turn on the test pattern (status[55])",
	      minimig_status_compose(0, 1u << 23) & ST(55), 0);
	check("cur_status cannot set the cartridge type",
	      minimig_status_compose(ST(63), 0), 0);
	check("the test pattern stays cur_status's beside the cartridge",
	      minimig_status_compose(ST(55), 1u << 23), ST(55) | ST(63));

	if (fails) { printf("\n%d minimig_status check(s) failed\n", fails); return 1; }
	printf("\nall minimig_status checks passed\n");
	return 0;
}
