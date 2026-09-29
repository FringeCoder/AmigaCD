#!/usr/bin/env bash
# Build MiSTer from WSL (or any Linux box) against this checkout.
#
#   ./scripts/wsl-build.sh            full build
#   ./scripts/wsl-build.sh check      syntax-only check of the files we touched
#   ./scripts/wsl-build.sh clean      wipe bin/ and rebuild
#   ./scripts/wsl-build.sh deploy     build, then copy to the MiSTer as
#                                     /media/fat/MiSTer_AmigaCD
#
# The toolchain is arm-none-linux-gnueabihf gcc 10.2. If it is not already on
# PATH, setup_default_toolchain.sh downloads ~150 MB into the repo root (it is
# gitignored via the gcc-* rule).
#
# For deploy, put the MiSTer's IP in a file named `host` in the repo root (the
# same convention build.sh uses), or set MISTER_HOST. See docs/deployment.md
# for the MiSTer.ini side.
#
# Replaces the inherited .wsl_build.sh, which hardcoded a path on another
# machine.
set -euo pipefail

REPO="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

# Must match the `main=` value in the [AmigaCD] section of MiSTer.ini.
BINARY_NAME="${AMIGACD_BINARY_NAME:-MiSTer_AmigaCD}"

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

need_toolchain() {
    if ! command -v arm-none-linux-gnueabihf-gcc >/dev/null 2>&1; then
        echo "== toolchain not on PATH, sourcing setup_default_toolchain.sh"
        # setup_default_toolchain.sh is upstream's and tests its three
        # override variables with [ -z "${VAR}" ] rather than "${VAR:-}", so
        # under the set -u above it dies on the first one with
        # "MISTER_GCC_INSTALL_DIR: unbound variable" -- before downloading
        # anything. That made this script unusable on exactly the machines it
        # exists for: any checkout without the toolchain already on PATH.
        # Relaxed around the source rather than patched upstream, so a sync
        # does not silently undo the fix.
        set +u
        # shellcheck disable=SC1091
        source ./setup_default_toolchain.sh
        set -u
    fi
    if ! command -v arm-none-linux-gnueabihf-gcc >/dev/null 2>&1; then
        echo "!! arm-none-linux-gnueabihf-gcc still not found." >&2
        echo "   Check that setup_default_toolchain.sh completed and that" >&2
        echo "   $REPO/gcc-arm-*/bin is on PATH." >&2
        exit 1
    fi
}

INCLUDES=(
    -I./ -I./lib/libco -I./lib/miniz -I./lib/md5 -I./lib/lzma
    -I./lib/zstd/lib -I./lib/libchdr/include -I./lib/bluetooth
    -I./lib/serial_server/library
)

# Must track DFLAGS in the Makefile. Without these, `check` compiles a
# different program than `build` does -- _FILE_OFFSET_BITS in particular
# changes off_t and every file_io.h signature that depends on it, so a clean
# `check` would not mean the real build compiles.
DEFINES=(
    -D_7ZIP_ST -DPACKAGE_VERSION='"1.3.3"' -DHAVE_LROUND -DHAVE_STDINT_H
    -DHAVE_STDLIB_H -DHAVE_SYS_PARAM_H -DENABLE_64_BIT_WORDS=0
    -D_FILE_OFFSET_BITS=64 -D_LARGEFILE64_SOURCE
    -DVDATE="\"$(date +%y%m%d)\""
)

case "${1:-build}" in
check)
    need_toolchain
    echo "== syntax-only check of ${#CHANGED[@]} changed file(s)"
    rc=0
    for f in "${CHANGED[@]}"; do
        printf '   %-48s' "$f"
        if arm-none-linux-gnueabihf-g++ -fsyntax-only -std=gnu++14 \
             -Wall -Wextra -Wno-strict-aliasing -Wno-psabi -Wno-class-memaccess \
             "${DEFINES[@]}" "${INCLUDES[@]}" "$f" 2>/tmp/amigacd-check.err; then
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
    need_toolchain
    rm -rf bin
    make
    ls -la bin/MiSTer
    md5sum bin/MiSTer
    ;;
build)
    need_toolchain
    make
    ls -la bin/MiSTer
    md5sum bin/MiSTer
    ;;
deploy)
    need_toolchain
    make

    HOST="${MISTER_HOST:-}"
    [ -z "$HOST" ] && [ -f host ] && HOST="$(tr -d '[:space:]' < host)"
    if [ -z "$HOST" ]; then
        echo "!! No MiSTer address. Put its IP in ./host or set MISTER_HOST." >&2
        exit 1
    fi

    echo "== deploying to $HOST as /media/fat/$BINARY_NAME"
    # Copy to a temp name and rename: rename over a running binary is atomic
    # and avoids ETXTBSY if the AmigaCD core happens to be loaded right now.
    scp bin/MiSTer "root@$HOST:/media/fat/$BINARY_NAME.new"
    ssh "root@$HOST" \
        "mv -f /media/fat/$BINARY_NAME.new /media/fat/$BINARY_NAME && \
         chmod +x /media/fat/$BINARY_NAME && sync"
    echo "== done. Reload the AmigaCD core to pick it up."
    ;;
*)
    echo "usage: $0 [build|check|clean|deploy]" >&2
    exit 2
    ;;
esac
