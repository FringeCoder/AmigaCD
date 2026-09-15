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
# What lands where on the SD card (see the generated WHERE-THESE-GO.txt):
#   AmigaCD_<date>.rbf -> /media/fat/_Computer/  (or wherever you keep cores)
#   menu.rbf         -> /media/fat/            (the boot menu core)
#   MiSTer_AmigaCD   -> /media/fat/            (needs TWO MiSTer.ini sections)
#
# The stock /media/fat/MiSTer is deliberately left alone. `main=` matches a core's
# CONF_STR name, and the boot menu core calls itself MENU, so [MENU] main=
# MiSTer_AmigaCD gets the SNAC pad into the boot menu without replacing the binary
# that boots everything -- which also means update_all can keep updating it.
set -euo pipefail

cd "$(dirname "$0")/.."
OUT="output"
REPO_SLUG="${AMIGACD_REPO:-FringeCoder/AmigaCD}"

# The core's identity comes from CONF_STR ("AmigaCD"), not the filename --
# docs/deployment.md explains why the filename is irrelevant to the software.
# What the filename DOES carry is the date the menu shows; see below.
#
# Minimig.rbf is accepted as a fallback. The Quartus project was named Minimig
# until 2026-09-15, so a core tree that has not been re-fitted since the rename,
# or a checkout of kblood's amiga-console branch, still writes the old name.
# Preferring the new one and falling back is the difference between staging a
# stale bitstream knowingly and doing it silently: the fallback says so.
CORE_DIR="upstream/core-cd32-cdtv/output_files"
CORE_SRC="$CORE_DIR/AmigaCD.rbf"
if [ ! -f "$CORE_SRC" ] && [ -f "$CORE_DIR/Minimig.rbf" ]; then
    echo "note: no AmigaCD.rbf; using Minimig.rbf (core tree predates the rename)" >&2
    CORE_SRC="$CORE_DIR/Minimig.rbf"
fi
# Dated filename, because that is the only place the MiSTer menu gets a core's
# date from. file_io.cpp's get_display_name() looks for "_20" in the .rbf name
# and needs at least 6 characters after it; failing that it writes "------" into
# the datecode, which menu.cpp renders next to the name as "--.--.--". That is
# the dash row you see on an undated core, and it is not a missing build date
# inside the bitstream -- there is nowhere in a .rbf for one to live.
#
# The date is the fit's mtime, so it means "when this bitstream was built"
# rather than when it was staged. Restaging an old fit does not relabel it.
#
# Safe for everything that resolves the core by name: MiSTer.ini sections match
# CONF_STR, not the filename (docs/deployment.md), and bootcore already matches
# <core>_YYYYMMDD.rbf and picks the newest -- see matchesCore_yyyyMMdd_rbf() in
# bootcore.cpp, which exists for exactly this convention.
#
# Staged under the name it gets ON THE CARD, rather than staged as amigacd.rbf
# and renamed by hand on the way. The manifest used to say "copy amigacd.rbf to
# _Computer/AmigaCD.rbf", and a rename a human performs is a rename a human
# eventually performs differently.
CORE_BASE="AmigaCD"
CORE_DATE="$(date -r "$CORE_SRC" +%Y%m%d 2>/dev/null || echo "")"
if [ -n "$CORE_DATE" ]; then
    CORE_DST="$OUT/${CORE_BASE}_$CORE_DATE.rbf"
else
    # No mtime means no honest date. An undated name loses the stamp; a made-up
    # one is worse, because the menu would then show a date that is not the
    # build's.
    echo "note: cannot read $CORE_SRC mtime; staging undated" >&2
    CORE_DST="$OUT/$CORE_BASE.rbf"
fi
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

# Dated names accumulate. Clear the staging directory's own older cores first,
# so `output/` always holds exactly the one that was just staged -- otherwise a
# copy of the whole directory to the card installs every build ever made, and
# the menu shows a column of near-identical entries with only the date to tell
# them apart.
for old in "$OUT"/amigacd.rbf "$OUT"/amigacd_*.rbf \
           "$OUT/$CORE_BASE.rbf" "$OUT/${CORE_BASE}_"*.rbf; do
    [ -e "$old" ] || continue
    [ "$old" = "$CORE_DST" ] && continue
    echo "  removing stale $(basename "$old")"
    rm -f "$old"
done

rc=0
copy "$CORE_SRC" "$CORE_DST" || rc=1
copy "$MENU_SRC" "$MENU_DST" || rc=1

# The card is not ours to clean -- it is over ssh and the core directory is the
# user's -- but a leftover undated amigacd.rbf there sits beside the new one and
# both load, so say it once rather than leaving it to be noticed.
if [ -f "$CORE_DST" ] && [ -n "$CORE_DATE" ]; then
    echo "  note: delete any older AmigaCD.rbf / AmigaCD_*.rbf from the card --"
    echo "        the dated file does not replace them, and all of them show in"
    echo "        the menu as separate entries."
fi

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
        # The checked-out branch may have no CI of its own (or be unrelated work).
        # develop carries the merged userspace, which is what ships.
        RUN_ID=$(gh run list --repo "$REPO_SLUG" --workflow build --branch develop                              --status success --limit 1 --json databaseId                              -q '.[0].databaseId')
        [ -n "$RUN_ID" ] && echo "  (no CI on this branch; using latest successful develop build)"
    fi
    if [ -z "$RUN_ID" ]; then
        echo "MISSING  no successful CI run found on this branch or develop" >&2
        rc=1
    else
        tmp=$(mktemp -d)
        gh run download "$RUN_ID" --repo "$REPO_SLUG" --dir "$tmp" >/dev/null 2>&1 || true
        elf=$(find "$tmp" -name 'MiSTer.elf' -o -name 'MiSTer' | head -1)
        if [ -n "$elf" ]; then
            SHA=$(gh run view "$RUN_ID" --repo "$REPO_SLUG" --json headSha -q '.headSha' | cut -c1-7)
            # One name only. The stock /media/fat/MiSTer stays put -- see the
            # header, and WHERE-THESE-GO.txt for the two .ini sections that make
            # this binary serve both the boot menu and the AmigaCD core.
            cp -f "$elf" "$OUT/MiSTer_AmigaCD"
            printf "  %-18s %9d bytes  from CI run %s (%s)\n" \
                "MiSTer_AmigaCD" "$(stat -c%s "$OUT/MiSTer_AmigaCD")" "$RUN_ID" "$SHA"
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

  AmigaCD_<date>.rbf -> /media/fat/_Computer/   (or wherever you keep cores)
  menu.rbf           -> /media/fat/menu.rbf
  MiSTer_AmigaCD     -> /media/fat/MiSTer_AmigaCD

Copy the core under the name it already has -- do not drop the date. The MiSTer
menu reads a core's date from its FILENAME (it looks for "_20" and takes the six
characters after it); a core named without one shows "--.--.--" in the date
column. Delete the older AmigaCD*.rbf from the card when you copy a new one:
nothing replaces them and each one is its own menu entry.

Do NOT overwrite /media/fat/MiSTer. Leave the stock binary alone -- see below.

/media/fat/MiSTer.ini:

  [MENU]
  main=MiSTer_AmigaCD

  [AmigaCD]
  main=MiSTer_AmigaCD

  snac_psx=1

Two sections, one binary. `main=` matches the core's CONF_STR name, and the boot
menu core calls itself MENU -- so pointing [MENU] at our binary gets the SNAC pad
working in the boot menu WITHOUT replacing the stock main binary. update_all can
then update /media/fat/MiSTer as often as it likes; MiSTer_AmigaCD is in no
Downloader database, so nothing touches it.

That is also the safe way round: if our binary ever misbehaves, delete one .ini
line and you are back on stock, rather than having overwritten the binary that
boots everything. No restart loop -- once running as MiSTer_AmigaCD, the app name
matches main= and the check falls through.

snac_psx=1 enables the pad in the boot menu. AmigaCD's own SNAC settings are NOT
here -- they live in AmigaCD.cfg via the OSD, so they can differ per saved config.

update_all AND menu.rbf: there is no way to protect it. Downloader filters are
category/core level only (filter = console !snes, in downloader.ini under
[mister]); there is no per-file exclusion, and the boot menu must be named
menu.rbf so renaming is out. Keep a spare copy under a name Downloader does not
know and restore it after any update that touches the Menu core:

  cp /media/fat/menu.rbf /media/fat/menu_snac.rbf      # once, now
  cp /media/fat/menu_snac.rbf /media/fat/menu.rbf      # after each update_all

SW[1] on the DE10-Nano must be OFF. It hands three of the user port's pins to
HDMI audio and the pad simply never answers, with no error.

The binary is unstripped: same code as a stripped build, ~200 KB larger, and a
crash under gdb gives a named backtrace instead of `?? ()`.
MANIFEST
echo "  WHERE-THESE-GO.txt   written"

exit $rc
