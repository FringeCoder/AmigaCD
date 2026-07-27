#pragma once
// Chipset bus trace drain (debug, gfx-trio investigation).
// See rtl/chipset_bus_trace.v and research/docs/chipset-trace-plan.md.

#ifdef __cplusplus
extern "C" {
#endif

// Drain at most ring-depth entries from the chipset trace ring and append
// each decoded entry to /tmp/chipset_trace.csv. Safe to call every poll;
// returns immediately when the ring is empty.
//
// First call opens the CSV in write mode (truncates + writes header).
// Subsequent calls append rows. To start a fresh capture from userspace,
// `rm /tmp/chipset_trace.csv` and the next drain call reopens it.
void chipset_trace_drain(void);

#ifdef __cplusplus
}
#endif
