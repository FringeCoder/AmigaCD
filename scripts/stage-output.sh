#!/usr/bin/env bash
# Collect everything that goes on the MiSTer SD card into one folder.
#
#   ./scripts/stage-output.sh              cores only (fast, no network)
#   ./scripts/stage-output.sh --binary     also fetch the userspace binary from CI
#   ./scripts/stage-output.sh --binary 31218670266    ...from a specific run
#
# Always overwrites. The folder is scratch -- delete it whenever, it rebuilds
# from the two core repos and a CI artifact.
#
# What lands where on the SD card:
#   amigacd.rbf      -> /media/fat/_Computer/  (or wherever you keep cores)
#   menu.rbf         -> /media/fat/            (the boot menu core)
#   MiSTer_AmigaCD   -> /media/fat/            (needs [AmigaCD] main=MiSTer_AmigaCD
#                                               in MiSTer.ini -- see docs/deployment.md)
#   MiSTer           -> /media/fat/            (the SAME build, replacing the stock
#                                               main binary)
#
# The binary is staged under BOTH names on purpose, and they are byte-identical.
# `main=` only redirects the AmigaCD core, so the boot menu still runs whatever
# is at /media/fat/MiSTer -- and the Menu core's PSX SNAC OSD navigation lives in
# userspace, so without replacing that one too the pad does nothing in the menu.
# See docs/psx-snac-hardware.md:59.
set -euo pipefail

cd "$(dirname "$0")/.."
OUT="output"
REPO_SLUG="${AMIGACD_REPO:-FringeCoder/AmigaCD}"

# The AmigaCD core is built as Minimig.rbf because the Quartus project is still
# named Minimig; it is renamed here because that is what it is on the card.
# The core's identity comes from CONF_STR ("AmigaCD"), not the filename --
# docs/deployment.md explains why the filename is irrelevant to the software.
CORE_SRC="upstream/core-cd32-cdtv/output_files/Minimig.rbf"
CORE_DST="$OUT/amigacd.rbf"
MENU_SRC="upstream/core-menu/output_files/menu.rbf"
MENU_DST="$OUT/menu.rbf"

mkdir -p "$OUT"

copy() {
    local src="$1" dst="$2"
    if [ ! -f "$src" ]; then
        echo "MISSING  $src -- build it first" >&2
        return 1
    fi
    cp -f "$src" "$dst"
    printf "  %-18s %9d bytes  built %s\n" \
        "$(basename "$dst")" "$(stat -c%s "$dst")" "$(date -r "$src" '+%Y-%m-%d %H:%M')"
}

echo "staging to $OUT/"
rc=0
copy "$CORE_SRC" "$CORE_DST" || rc=1
copy "$MENU_SRC" "$MENU_DST" || rc=1

if [ "${1:-}" = "--binary" ]; then
    # No ARM cross-toolchain on the dev machine, so the userspace binary always
    # comes from CI -- see docs/deployment.md. Unstripped by default: identical
    # code, ~200 KB larger, and a crash under gdb gives a named backtrace.
    RUN_ID="${2:-}"
    if [ -z "$RUN_ID" ]; then
        RUN_ID=$(gh run list --repo "$REPO_SLUG" --workflow build \
                             --branch "$(git rev-parse --abbrev-ref HEAD)" \
                             --status success --limit 1 --json databaseId \
                             -q '.[0].databaseId')
    fi
    if [ -z "$RUN_ID" ]; then
        echo "MISSING  no successful CI run found for this branch" >&2
        rc=1
    else
        tmp=$(mktemp -d)
        gh run download "$RUN_ID" --repo "$REPO_SLUG" --dir "$tmp" >/dev/null 2>&1 || true
        elf=$(find "$tmp" -name 'MiSTer.elf' -o -name 'MiSTer' | head -1)
        if [ -n "$elf" ]; then
            SHA=$(gh run view "$RUN_ID" --repo "$REPO_SLUG" --json headSha -q '.headSha' | cut -c1-7)
            # Same build under both names -- see the header for why the stock
            # main binary has to be replaced too.
            for name in MiSTer_AmigaCD MiSTer; do
                cp -f "$elf" "$OUT/$name"
                printf "  %-18s %9d bytes  from CI run %s (%s)\n" \
                    "$name" "$(stat -c%s "$OUT/$name")" "$RUN_ID" "$SHA"
            done
        else
            echo "MISSING  no MiSTer binary in run $RUN_ID's artifacts" >&2
            rc=1
        fi
        rm -rf "$tmp"
    fi
else
    echo "  (skipping the binary; pass --binary to fetch it from CI)"
fi

cat > "$OUT/WHERE-THESE-GO.txt" <<'MANIFEST'
Copy to the MiSTer SD card:

  amigacd.rbf      -> /media/fat/_Computer/AmigaCD.rbf   (or wherever you keep cores)
  menu.rbf         -> /media/fat/menu.rbf
  MiSTer_AmigaCD   -> /media/fat/MiSTer_AmigaCD
  MiSTer           -> /media/fat/MiSTer                  (replaces the stock binary)

MiSTer and MiSTer_AmigaCD are the SAME build under two names. Both are needed:
`main=` only redirects the AmigaCD core, so the boot menu keeps running
/media/fat/MiSTer, and the Menu core's PSX SNAC pad navigation lives in
userspace -- without replacing that one the pad does nothing in the menu.

/media/fat/MiSTer.ini needs both of these:

  [AmigaCD]
  main=MiSTer_AmigaCD

  snac_psx=1

The first selects our binary for the AmigaCD core. The second enables the pad in
the boot menu, and is read by whichever binary is running at boot.

Two traps:

- update_all OVERWRITES menu.rbf with the stock one, silently dropping SNAC pad
  support from the boot menu until you copy it back.
- SW[1] on the DE10-Nano must be OFF. It steals three of the user port's pins
  and fails silently.

The binary is unstripped: same code as a stripped build, ~200 KB larger, and a
crash under gdb gives a named backtrace instead of `?? ()`.
MANIFEST
echo "  WHERE-THESE-GO.txt   written"

exit $rc
