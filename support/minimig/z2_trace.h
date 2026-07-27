#pragma once
// Z2 fast-RAM hang trace drain (debug, 2026-05-27 Z2-broken investigation).
// See rtl/z2_trace.v + research/docs/handoff-2026-05-27-z2-fix-candidate-built-deployed-untested.md.

// Compile-time gate for the Z2 fast-RAM debug instrumentation (trace ring +
// DDR-peek). 0 = release: no ring drain, no /tmp/z2_trace.csv, no DDR-peek
// logging. Set to 1 to re-enable the 2026-05-27 Z2-hang capture, and pair it
// with the RTL gate by instantiating z2_trace / akiko_ddr_peek with
// CAPTURE_ENABLE(1) (default 0). Shared by akiko_cd32.cpp and z2_trace.cpp.
#define AKIKO_Z2_TRACE 0

#ifdef __cplusplus
extern "C" {
#endif

// Drain at most ring-depth entries from the z2_trace ring and append
// each decoded entry to /tmp/z2_trace.csv. Safe to call every poll;
// returns immediately when the ring is empty.
//
// First call opens the CSV in write mode (truncates + writes header).
// Subsequent calls append rows. To start a fresh capture from userspace,
// `rm /tmp/z2_trace.csv` and the next drain call reopens it.
void z2_trace_drain(void);

#ifdef __cplusplus
}
#endif
