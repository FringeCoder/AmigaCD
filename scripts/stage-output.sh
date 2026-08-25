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
#   amigacd.rbf      -> /media/fat/_Computer/  (or wherever you keep cores)
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

  amigacd.rbf      -> /media/fat/_Computer/AmigaCD.rbf   (or wherever you keep cores)
  menu.rbf         -> /media/fat/menu.rbf
  MiSTer_AmigaCD   -> /media/fat/MiSTer_AmigaCD

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
