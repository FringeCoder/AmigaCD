/*
 * config.h for the vendored libmpeg2 0.5.1 (see README.vendor).
 *
 * Upstream generates this with autoconf. We build only the decoder core, for
 * two targets -- the MiSTer's Cortex-A9 and the CI host -- so the handful of
 * macros those sources test are set by hand here instead.
 */
#ifndef LIBMPEG2_VENDOR_CONFIG_H
#define LIBMPEG2_VENDOR_CONFIG_H

#define ATTRIBUTE_ALIGNED_MAX 64
#define HAVE_BUILTIN_EXPECT   1

/*
 * libmpeg2's only ARM acceleration is motion_comp_arm_s.S, ARMv4 assembler
 * for motion compensation. It is compiled in only when the build asks for it
 * (LIBMPEG2_ARM_ASM, set by tools/fmvbench/Makefile for the ARM target), and
 * it is used only when the caller selects MPEG2_ACCEL_ARM: libmpeg2 does not
 * detect it on its own. fmvbench -a selects it, so one binary times both.
 */
#if defined(__arm__) && defined(LIBMPEG2_ARM_ASM)
#define ARCH_ARM 1
#endif

#endif
