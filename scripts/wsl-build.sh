#!/usr/bin/env bash
# Build MiSTer from WSL (or any Linux box) against this checkout.
#
#   ./scripts/wsl-build.sh            full build
#   ./scripts/wsl-build.sh check      syntax-only check of the files we touched
#   ./scripts/wsl-build.sh clean      wipe bin/ and rebuild
#
# The toolchain is arm-none-linux-gnueabihf gcc 10.2. If it is not already on
# PATH, setup_default_toolchain.sh downloads ~150 MB into the repo root (it is
# gitignored via the gcc-* rule).
#
# Replaces the inherited .wsl_build.sh, which hardcoded a path on another
# machine.
set -euo pipefail

REPO="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

# Files this project changes on top of upstream. `check` compiles just these,
# which is seconds rather than minutes -- the useful loop while iterating.
CHANGED=(
    ide.cpp
    ide_cdrom.cpp
    menu.cpp
    support/minimig/akiko_cd32.cpp
    support/minimig/cdtv_cd.cpp
    support/physical_disc/physical_disc.cpp
    support/physical_disc/physical_disc_acoustic.cpp
)

if ! command -v arm-none-linux-gnueabihf-gcc >/dev/null 2>&1; then
    echo "== toolchain not on PATH, sourcing setup_default_toolchain.sh"
    # shellcheck disable=SC1091
    source ./setup_default_toolchain.sh
fi

if ! command -v arm-none-linux-gnueabihf-gcc >/dev/null 2>&1; then
    echo "!! arm-none-linux-gnueabihf-gcc still not found." >&2
    echo "   Check that setup_default_toolchain.sh completed and that" >&2
    echo "   \$REPO/gcc-arm-*/bin is on PATH." >&2
    exit 1
fi

INCLUDES=(
    -I./ -I./lib/libco -I./lib/miniz -I./lib/md5 -I./lib/lzma
    -I./lib/zstd/lib -I./lib/libchdr/include -I./lib/bluetooth
    -I./lib/serial_server/library
)

case "${1:-build}" in
check)
    echo "== syntax-only check of ${#CHANGED[@]} changed file(s)"
    rc=0
    for f in "${CHANGED[@]}"; do
        printf '   %-48s' "$f"
        if arm-none-linux-gnueabihf-g++ -fsyntax-only -std=gnu++14 \
             "${INCLUDES[@]}" "$f" 2>/tmp/amigacd-check.err; then
            echo "ok"
        else
            echo "FAILED"
            sed 's/^/      /' /tmp/amigacd-check.err
            rc=1
        fi
    done
    exit $rc
    ;;
clean)
    rm -rf bin
    ;&
build)
    make
    ls -la bin/MiSTer
    md5sum bin/MiSTer
    ;;
*)
    echo "usage: $0 [build|check|clean]" >&2
    exit 2
    ;;
esac
