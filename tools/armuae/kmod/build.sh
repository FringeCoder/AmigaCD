#!/bin/bash
# Build armwin.ko for the MiSTer (in WSL). The board runs 6.18.38-MiSTer
# (vermagic "6.18.38-MiSTer SMP mod_unload ARMv7 p2v8", no module signing or
# modversions), built with the same gcc 10.2 as Main. A vanilla 6.18.38 tree
# with the board's config and EXTRAVERSION -MiSTer is enough for a module
# that only uses core kernel interfaces:
#
#   curl -LO https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.18.38.tar.xz
#   tar xf linux-6.18.38.tar.xz; cd linux-6.18.38
#   ssh root@mister 'zcat /proc/config.gz' > .config
#   sed -i 's/^EXTRAVERSION =.*/EXTRAVERSION = -MiSTer/' Makefile
#   make ARCH=arm CROSS_COMPILE=arm-none-linux-gnueabihf- olddefconfig modules_prepare
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$HERE/../../.." && pwd)
TC=${TC:-$(ls -d "$REPO_ROOT"/gcc-arm-10.2-*/bin 2>/dev/null | head -1)}
[ -n "$TC" ] || TC=$(ls -d "$REPO_ROOT"/../AmigaCD/gcc-arm-10.2-*/bin 2>/dev/null | head -1)
export PATH="$TC:$PATH"
WORK=${WORK:-/root/kmod/armwin}
mkdir -p "$WORK"
cp "$HERE/armwin.c" "$HERE/Makefile" "$WORK/"
make -C "$WORK" KDIR=${KDIR:-/root/kmod/linux-6.18.38}
echo "built $WORK/armwin.ko"
