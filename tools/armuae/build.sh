#!/bin/bash
# armuae: Amiberry's 68k CPU core and 32-bit ARM JIT, on their own, for the
# DE10-Nano's Cortex-A9. PROTOTYPE -- see docs/arm-cpu-plan.md, step 4.
#
#   ./build.sh            # fetch Amiberry, build armuae for the ARM
#   ./build.sh bench      # also build the benchmark ROMs (needs m68k-linux-gnu-gcc)
#
# Amiberry is fetched at v5.7.4 (6d672cd), the last release whose 32-bit ARM
# JIT is switched on: 7.x builds the JIT only for x86-64 and AArch64, and its
# 32-bit ARM JIT no longer compiles. Amiberry is GPL-3; nothing of it is kept
# in this repository, and armuae is a separate program, like armcpu.
#
# What armuae adds around Amiberry's core:
#   shim/        SDL2 and guisan stand-ins: the core includes them, for string
#                helpers, thread types and GUI colours it never uses here
#   override/    a target.h without Amiberry's front end
#   stubs.cpp    the rest of the emulator, as far as the core reaches it
#   armuae.cpp   the memory map, interrupts, the clock and main()
#   patch_newcpu.py  keeps pissoff and natmem_offset within 4 KB of regs, where
#                the JIT's [regs, #imm12] loads reach them
#
# Toolchain: the repo's gcc-arm-10.2 (setup_default_toolchain.sh), as for Main.
# No -DARM_HAS_DIV: the Cortex-A9 has no SDIV/UDIV, and a JIT built with it
# dies on SIGILL at the first 68k divide.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-$HOME/armuae-build}
REPO_ROOT=$(cd "$HERE/../.." && pwd)
TC=${TC:-$(ls -d "$REPO_ROOT"/gcc-arm-10.2-*/bin 2>/dev/null | head -1)}
[ -n "$TC" ] || TC=$(ls -d "$REPO_ROOT"/../AmigaCD/gcc-arm-10.2-*/bin 2>/dev/null | head -1)
T=$TC/arm-none-linux-gnueabihf
AMIBERRY_COMMIT=6d672cd0727df7d0b075418876f49e1efd686f54

mkdir -p "$WORK"
if [ ! -d "$WORK/amiberry" ]; then
	git clone -q https://github.com/BlitterStudio/amiberry.git "$WORK/amiberry"
	git -C "$WORK/amiberry" checkout -q $AMIBERRY_COMMIT
	python3 "$HERE/patch_newcpu.py" "$WORK/amiberry/src/newcpu.cpp"
fi
SRC=$WORK/amiberry/src
OBJ=$WORK/obj
mkdir -p "$OBJ"

CXXFLAGS="-std=c++17 -O3 -w -I$HERE/shim -I$HERE/override -I$SRC/include -I$SRC/threaddep -I$SRC/osdep \
 -I$SRC -I$SRC/jit -DAMIBERRY -D_FILE_OFFSET_BITS=64 -DCPU_arm -DARMV6_ASSEMBLY -DARMV6T2 -DUSE_ARMNEON \
 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -marm"
CORE="newcpu.cpp newcpu_common.cpp readcpu.cpp cpudefs.cpp cpustbl.cpp cpuemu_0.cpp cpuemu_4.cpp cpuemu_11.cpp
 cpuemu_13.cpp cpuemu_40.cpp cpuemu_44.cpp fpp.cpp machdep/support.cpp
 jit/compemu.cpp jit/compstbl.cpp jit/compemu_fpp.cpp jit/compemu_support.cpp"
export SRC OBJ CXXFLAGS
export CXX=$T-g++
echo $CORE | tr ' ' '\n' | xargs -P 8 -I{} bash -c \
	'o=$OBJ/$(echo {} | tr / _ | sed s/.cpp$/.o/); [ $o -nt $SRC/{} ] && exit 0; $CXX $CXXFLAGS -c -o $o $SRC/{} || echo "FAILED {}"'
for f in stubs armuae; do $CXX $CXXFLAGS -c -o "$OBJ/$f.glue.o" "$HERE/$f.cpp"; done
$T-ar rcs "$OBJ/core.a" $(ls "$OBJ"/*.o | grep -v glue)
$CXX -static -rdynamic -o "$WORK/armuae" "$OBJ/stubs.glue.o" "$OBJ/armuae.glue.o" \
	-Wl,--whole-archive "$OBJ/core.a" -Wl,--no-whole-archive -lm
$T-strip -o "$WORK/armuae.stripped" "$WORK/armuae"
echo "built $WORK/armuae (stripped copy: armuae.stripped)"

if [ "$1" = bench ]; then
	cd "$HERE/bench"
	for cpu in 68000 68020; do
		m68k-linux-gnu-gcc -m$cpu -O2 -ffreestanding -fno-builtin -nostdlib -fno-pic -static \
			-Wl,-z,noexecstack -Wl,--build-id=none -T link.ld -o "$WORK/bench$cpu.elf" crt0.S bench.c -lgcc
		m68k-linux-gnu-objcopy -O binary --pad-to 0x1000000 "$WORK/bench$cpu.elf" "$WORK/bench$cpu.rom"
	done
	for mode in 0 1 2 3; do
		m68k-linux-gnu-gcc -m68020 -O2 -ffreestanding -fno-builtin -nostdlib -fno-pic -static -DMODE=$mode \
			-Wl,-z,noexecstack -Wl,--build-id=none -T link.ld -o "$WORK/chip$mode.elf" crt0.S chipbench.c -lgcc
		m68k-linux-gnu-objcopy -O binary --pad-to 0x1000000 "$WORK/chip$mode.elf" "$WORK/chip$mode.rom"
	done
	echo "built $WORK/bench68000.rom, $WORK/bench68020.rom, $WORK/chip{0,1,2,3}.rom (read, write, copy, compute+write)"
fi
