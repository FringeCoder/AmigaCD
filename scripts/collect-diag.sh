#!/usr/bin/env bash
# Pull the MiSTer's diagnostic state into one tarball.
#
#   ./scripts/collect-diag.sh                  the logs, the config, and what is
#                                              installed, as text
#   ./scripts/collect-diag.sh --with-images    also the save state thumbnails and
#                                              the newest screenshots
#   ./scripts/collect-diag.sh --clear          truncate the device logs FIRST, so
#                                              what comes back is only the run you
#                                              are about to do
#   ./scripts/collect-diag.sh --full           whole logs instead of the last
#                                              5000 lines of each
#
# Why this exists: every question this project has left open is answered by a
# file on that machine rather than by looking at the screen -- whether the
# restore bar drew or was dropped by the menustate gate, what the real per-sector
# period is, whether a save state toast reached the OSD, what a thumbnail
# actually looks like. Collecting them by hand means remembering five paths and
# which of them only exist if that subsystem ran at all.
#
# Read-only on the device unless --clear is given, and that only truncates logs
# under /tmp.
#
# Host comes from ./host (gitignored) or $MISTER_HOST, the same convention
# deploy-from-ci.sh, wsl-build.sh and build.sh use.
set -euo pipefail

TAIL_LINES=5000
WITH_IMAGES=0
CLEAR_FIRST=0

while [ $# -gt 0 ]; do
    case "$1" in
        --with-images) WITH_IMAGES=1 ;;
        --clear)       CLEAR_FIRST=1 ;;
        --full)        TAIL_LINES=0 ;;
        -h|--help)     sed -n '2,27p' "$0"; exit 0 ;;
        *) echo "!! unknown option: $1" >&2; exit 1 ;;
    esac
    shift
done

cd "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

HOST="${MISTER_HOST:-}"
if [ -z "$HOST" ] && [ -f host ]; then HOST="$(tr -d '[:space:]' < host)"; fi
if [ -z "$HOST" ]; then
    echo "!! No MiSTer address. Put its IP in ./host or set MISTER_HOST." >&2
    exit 1
fi

SSH="ssh -o BatchMode=yes -o ConnectTimeout=10 root@$HOST"

# Fail here rather than half way through, with the reason the user can act on.
if ! $SSH true 2>/dev/null; then
    echo "!! Cannot ssh to root@$HOST." >&2
    echo "   Check the address, and that a key is installed -- BatchMode is on," >&2
    echo "   so this never sits waiting for a password prompt you cannot see." >&2
    exit 1
fi

STAMP="$(date +%Y%m%d-%H%M%S)"
OUT_NAME="amigacd-diag-${HOST//[^A-Za-z0-9._-]/_}-$STAMP"
OUT_DIR="diag/$OUT_NAME"
mkdir -p "$OUT_DIR"

if [ "$CLEAR_FIRST" = 1 ]; then
    echo "== clearing device logs"
    # Truncate rather than delete: the writers hold these open with fopen(...,"a")
    # and a deleted inode would keep filling a file nothing can read.
    $SSH ': > /tmp/akiko_dbg.log; : > /tmp/cdtv_dbg.log; : > /tmp/ss_dbg.log; \
           : > /tmp/physical_disc_stats.log' 2>/dev/null || true
    echo "   done -- reproduce the problem now, then run this again without --clear."
    exit 0
fi

echo "== collecting from $HOST"

# ---------------------------------------------------------------------------
# One round trip for everything textual. Each section is labelled so the file
# reads top to bottom without needing this script open beside it.
# ---------------------------------------------------------------------------
$SSH "bash -s" > "$OUT_DIR/report.txt" 2>&1 <<REMOTE
set +e
say() { echo; echo "===== \$* ====="; }

say uname
uname -a
say uptime
uptime
say "disk (/media/fat)"
df -h /media/fat /tmp 2>&1
say memory
free -m 2>&1

say "running MiSTer binaries"
ps aux | grep -i '[M]iSTer' 2>&1

say "installed binaries and cores"
ls -l /media/fat/MiSTer /media/fat/MiSTer_AmigaCD /media/fat/_Test/*.rbf 2>&1
md5sum /media/fat/MiSTer_AmigaCD /media/fat/_Test/*.rbf 2>&1

say "MiSTer.ini"
cat /media/fat/MiSTer.ini 2>&1

say "save states and thumbnails"
ls -l /media/fat/savestates/AmigaCD/ /media/fat/savestates/Minimig/ 2>&1

say "screenshots (newest 10)"
ls -lt /media/fat/screenshots/ 2>&1 | head -11

say "log sizes"
ls -l /tmp/akiko_dbg.log /tmp/cdtv_dbg.log /tmp/ss_dbg.log \
      /tmp/physical_disc_stats.log /tmp/debug.txt 2>&1

say "dmesg (tail)"
dmesg 2>&1 | tail -80
REMOTE

echo "   report.txt"

# ---------------------------------------------------------------------------
# The logs themselves. Each is optional: a log only exists if that subsystem
# ran, so a missing one is information rather than a failure.
# ---------------------------------------------------------------------------
for f in akiko_dbg.log cdtv_dbg.log ss_dbg.log physical_disc_stats.log debug.txt; do
    if [ "$TAIL_LINES" = 0 ]; then
        cmd="cat /tmp/$f"
    else
        cmd="tail -n $TAIL_LINES /tmp/$f"
    fi

    if $SSH "test -f /tmp/$f" 2>/dev/null; then
        $SSH "$cmd" > "$OUT_DIR/$f" 2>/dev/null || true
        echo "   $f ($(wc -l < "$OUT_DIR/$f") lines)"
    else
        echo "   $f -- not present on the device"
    fi
done

# ---------------------------------------------------------------------------
# Pictures. Off by default: they are the largest thing here and are only wanted
# when the question is about what something LOOKED like.
# ---------------------------------------------------------------------------
if [ "$WITH_IMAGES" = 1 ]; then
    echo "== images"
    mkdir -p "$OUT_DIR/images"

    # Save state thumbnails: small, few, and the whole point of the preview
    # feature, so take all of them.
    scp -q "root@$HOST:/media/fat/savestates/AmigaCD/*.png" "$OUT_DIR/images/" 2>/dev/null \
        && echo "   savestate thumbnails" \
        || echo "   no savestate thumbnails (none saved since the feature landed?)"

    # Screenshots: newest three only. The directory can hold a lot of them and
    # the recent ones are the ones tied to whatever is being investigated.
    newest="$($SSH "ls -t /media/fat/screenshots/*.png /media/fat/screenshots/*.jpg 2>/dev/null | head -3" || true)"
    if [ -n "$newest" ]; then
        while IFS= read -r s; do
            [ -n "$s" ] || continue
            scp -q "root@$HOST:$s" "$OUT_DIR/images/" 2>/dev/null || true
        done <<< "$newest"
        echo "   newest screenshots"
    else
        echo "   no screenshots"
    fi
fi

TARBALL="diag/$OUT_NAME.tar.gz"
tar -czf "$TARBALL" -C diag "$OUT_NAME"
rm -rf "$OUT_DIR"

echo
echo "== $TARBALL"
echo "   tar -xzf $TARBALL -C diag   to unpack, or hand the tarball over as is."
