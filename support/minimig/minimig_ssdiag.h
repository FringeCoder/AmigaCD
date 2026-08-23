#ifndef MINIMIG_SSDIAG_H
#define MINIMIG_SSDIAG_H

// Save state diagnostics — read the core's 0xF600 UIO status window and log it.
//
// A debugging aid, not a feature. See minimig_ssdiag.cpp for the window layout,
// the log format and what it can and cannot tell apart.

// Poll the window. Cheap and rate limited internally; call it from the Minimig
// branch of user_io_poll() as often as you like. Logs only when something
// changed. A no-op on non-Minimig cores and on cores whose hps_ext.v predates
// the window.
void minimig_ss_diag_poll(void);

// Append a line to the same log, on the same timeline as the polled state.
// user_io.cpp uses this to record what the OSD toast path actually did, which
// is the half of the question the core-side window cannot answer. A no-op on
// non-Minimig cores.
void minimig_ss_diag_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
