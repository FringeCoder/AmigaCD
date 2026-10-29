#ifndef MINIMIG_ARMCPU_H
#define MINIMIG_ARMCPU_H

// The ARM CPU option: a 68020 run on the ARM by tools/armcpu, driving the
// Amiga's bus through the core's 68000 bus proxy. MEASUREMENT BUILD -- see
// docs/arm-cpu-plan.md. Only a core with the proxy can run it, and for now
// that is a core installed under a path containing "lwbridge".

// The core in the FPGA has the bus proxy behind the lightweight bridge.
bool armcpu_core_has_proxy();

// Whether minimig_config asks for the ARM CPU AND the core can run it.
bool armcpu_wanted();

// Start armcpu if it is wanted and not running, stop it if it is running and
// not wanted. Cheap when nothing changes.
void armcpu_sync();

// A machine reset: a running armcpu is restarted, so the 68000 on the ARM
// cold-starts with the chipset (armcpu resets it as it starts).
void armcpu_restart();

// Stop it whatever the config says: before another core is loaded, whose
// FPGA has nothing behind the bridge for armcpu to talk to.
void armcpu_stop();

#endif
