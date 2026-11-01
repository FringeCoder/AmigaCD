# fmvbench: MEASUREMENT ONLY, see fmvbench.c and docs/fmv-plan.md phase 1.
#
#   make          ARM build for the MiSTer: same toolchain as Main
#                 (setup_default_toolchain.sh), static so it runs on any MiSTer
#                 image, with libmpeg2's ARM motion compensation compiled in
#                 (choose at run time with -a c|arm).
#   make host     the same source for the build machine, for the CI test
#                 (test_fmvbench.py). Plain C, no ARM assembler.
BASE   ?= arm-none-linux-gnueabihf
CC      = $(BASE)-gcc
HOSTCC ?= gcc

LM2     = ../../lib/libmpeg2
KJ      = ../../lib/kjmp2
LM2_SRC = $(addprefix $(LM2)/libmpeg2/,alloc.c cpu_accel.c cpu_state.c decode.c \
          header.c idct.c motion_comp.c slice.c motion_comp_arm.c)
SRC     = fmvbench.c $(LM2_SRC) $(KJ)/kjmp2.c
INC     = -I$(LM2) -I$(LM2)/include -I$(KJ)

# -marm: the assembler is ARM-state code, and the Cortex-A9 runs either.
# -mfpu=neon lets GCC vectorise the C paths where it can; the A9 has NEON.
ARMFLAGS = -O3 -marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -DLIBMPEG2_ARM_ASM
# libmpeg2 0.5.1 predates several warnings; they are not ours to fix.
CFLAGS   = -std=gnu99 -Wall -Wno-unused-parameter -Wno-sign-compare \
           -Wno-pointer-sign -Wno-unused-but-set-variable $(INC)

fmvbench: $(SRC) $(LM2)/libmpeg2/motion_comp_arm_s.S fmvbench.c
	$(CC) $(CFLAGS) $(ARMFLAGS) -static -o $@ $(SRC) $(LM2)/libmpeg2/motion_comp_arm_s.S -lm

host: fmvbench-host
fmvbench-host: $(SRC)
	$(HOSTCC) $(CFLAGS) -O2 -o $@ $(SRC) -lm

clean:
	rm -f fmvbench fmvbench-host

.PHONY: clean host
