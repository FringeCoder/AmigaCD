#!/usr/bin/env bash
#
# Copy a staged core to the MiSTer without it appearing in the menu half-written,
# and prune the ones it supersedes.
#
# WHY THE TEMP NAME. An .rbf is a few megabytes over the network, and for the
# duration of the copy a partially written file sits in a directory the core
# browser scans. Selecting it loads a truncated bitstream. So the copy lands
# under a name the browser cannot see and is renamed into place once complete --
# the same shape wsl-build.sh's deploy mode already uses for the binary, and for
# the same reason.
#
# The transit name is BOTH hidden and not an .rbf:
#
#   .AmigaCD_20260916.rbf.part
#   ^                      ^
#   |                      +-- ScanDirectory matches the extension after the
#   |                          LAST dot (file_io.cpp ~1787), so this is "part"
#   +-- hidden files are skipped outright before the extension test even runs
#       (file_io.cpp:1754, `if (!strncasecmp(de->d_name, ".", 1)) continue;`)
#
# Either alone would do. Both cost nothing and the file is invisible to anything
# else poking at the card too.
#
# WHY IT DOES NOT DELETE EVERYTHING BUT THE NEWEST. docs/deployment.md says the
# previous core is the way back when a new one misbehaves, and that dated names
# make that happen by itself. Pruning to one core throws that away on the very
# deploy most likely to need it. So the default keeps the new core AND the one
# before it; --keep changes that, and --keep 1 will say what it is about to lose.
#
# WHAT IT WILL NEVER TOUCH. Only files matching AmigaCD_<8 digits>.rbf exactly --
# the names this project's own staging produces. An undated AmigaCD.rbf, a core
# you renamed by hand, a core from somewhere else: reported, never removed,
# unless you pass --include-undated for the undated one specifically.
#
# USAGE
#   ./scripts/deploy-core.sh [options] [RBF]
#
#     RBF                the core to deploy; default is the newest
#                        output/AmigaCD_*.rbf that stage-output.sh produced
#     --keep N           dated cores to leave on the card, newest first (default 2)
#     --include-undated  also remove an undated AmigaCD.rbf / amigacd.rbf
#     --dry-run          say what would happen, change nothing
#     --dest DIR         where cores live on the card; default is wherever the
#                        existing AmigaCD*.rbf are, else /media/fat/_Test
#
# Host comes from ./host (gitignored) or $MISTER_HOST, as elsewhere.

set -uo pipefail
cd "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)" || exit 1

host() {
    local h="${MISTER_HOST:-}"
    if [ -z "$h" ] && [ -f host ]; then h="$(tr -d '[:space:]' < host)"; fi
    if [ -z "$h" ]; then
        echo "!! No MiSTer address. Put its IP in ./host or set MISTER_HOST." >&2
        return 1
    fi
    printf '%s' "$h"
}

sshm() {
    ssh -o BatchMode=yes -o ConnectTimeout=10 -o StrictHostKeyChecking=accept-new \
        "root@$1" "$2"
}

# ---------------------------------------------------------------------------
# Which cores to remove.
#
# Its own function, defined before this script does anything, so it can be
# tested without a MiSTer: this is the part that DELETES THINGS, and "I read it
# carefully" is not a test. tests/test_deploy_core_prune.sh sources this file
# with DEPLOY_CORE_LIB=1 -- which returns just below, before any argument
# parsing or ssh -- feeds this function synthetic listings, and checks exactly
# which lines come back.
#
#   stdin  paths, one per line, newest first
#   $1     how many dated cores to keep
#   $2     basename of the core just deployed, which is never pruned even if the
#          count would say otherwise -- deploying and then deleting what you
#          just deployed is the one outcome with no defensible reading
#   stdout the paths to remove, in the order given
#
# Only the dated form counts towards $1. A directory holding one dated core and
# six hand-named ones prunes nothing at --keep 2, because six of those seven are
# not ours to count or to remove.
prune_list() {
    local keep="$1" keepname="$2" f b n=0
    while IFS= read -r f; do
        [ -n "$f" ] || continue
        b="${f##*/}"
        # Only the exact dated form this project stages. Anything else --
        # hand-named, from elsewhere, undated -- is not ours to remove.
        case "$b" in
            AmigaCD_[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9].rbf) ;;
            *) continue ;;
        esac
        n=$((n + 1))
        [ "$n" -gt "$keep" ] && [ "$b" != "$keepname" ] && printf '%s\n' "$f"
    done
    return 0
}

# Sourced by the test: everything above is definitions, everything below talks
# to a MiSTer.
if [ "${DEPLOY_CORE_LIB:-0}" = 1 ]; then return 0 2>/dev/null || exit 0; fi

KEEP=2
INCLUDE_UNDATED=0
DRY=0
DEST="${CORE_DEST:-}"
RBF=""

while [ $# -gt 0 ]; do
    case "$1" in
        --keep)            KEEP="${2:?--keep needs a number}"; shift 2 ;;
        --include-undated) INCLUDE_UNDATED=1; shift ;;
        --dry-run)         DRY=1; shift ;;
        --dest)            DEST="${2:?--dest needs a directory}"; shift 2 ;;
        -h|--help)         sed -n '2,47p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*)                echo "!! unknown option: $1" >&2; exit 2 ;;
        *)                 RBF="$1"; shift ;;
    esac
done

case "$KEEP" in
    ''|*[!0-9]*) echo "!! --keep needs a non-negative integer, got '$KEEP'" >&2; exit 2 ;;
esac

# ---------------------------------------------------------------------------
# What to send.
if [ -z "$RBF" ]; then
    # Newest by the DATE IN THE NAME, not by mtime: a restaged older fit has a
    # fresh mtime and an old date, and the date is what the thing actually is.
    RBF="$(ls -1 output/AmigaCD_*.rbf 2>/dev/null | sort | tail -1)"
fi
if [ -z "$RBF" ] || [ ! -f "$RBF" ]; then
    echo "!! No core to deploy. Run scripts/stage-output.sh first, or name one." >&2
    exit 1
fi
NAME="$(basename "$RBF")"

case "$NAME" in
    AmigaCD_[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9].rbf) ;;
    *)
        # Not fatal -- you may be deploying something hand-named on purpose --
        # but the pruning below only recognises the dated form, so say so rather
        # than leaving you wondering why nothing was cleaned up.
        echo "note: '$NAME' is not AmigaCD_<date>.rbf, so it will be copied but"
        echo "      will not participate in the dated-core pruning."
        ;;
esac

H="$(host)" || exit 1

# ---------------------------------------------------------------------------
# Where cores live. Ask the card rather than assuming: the directory is the
# user's choice and docs/deployment.md is explicit that getting it wrong is
# silent -- the copy succeeds and the machine keeps running the core it had.
if [ -z "$DEST" ]; then
    DEST="$(sshm "$H" "ls -1d /media/fat/_Test /media/fat/_Computer 2>/dev/null | while read -r d; do ls \"\$d\"/AmigaCD*.rbf >/dev/null 2>&1 && echo \"\$d\" && break; done")"
fi
if [ -z "$DEST" ]; then
    DEST="/media/fat/_Test"
    echo "note: no existing AmigaCD*.rbf found on the card; defaulting to $DEST"
fi
echo "== $H:$DEST"

BEFORE="$(sshm "$H" "ls -1 '$DEST'/AmigaCD*.rbf '$DEST'/amigacd*.rbf 2>/dev/null | sort")"
if [ -n "$BEFORE" ]; then
    echo "-- already there:"
    printf '%s\n' "$BEFORE" | sed 's|.*/|     |'
fi

# ---------------------------------------------------------------------------
# Copy under a name the browser cannot see, then rename into place.
TMP=".$NAME.part"
if [ "$DRY" = 1 ]; then
    echo "-- would copy $RBF -> $DEST/$TMP, then rename to $NAME"
else
    echo "-- copying as $TMP ($(du -h "$RBF" | cut -f1))"
    if ! scp -o BatchMode=yes -q "$RBF" "root@$H:$DEST/$TMP"; then
        echo "!! copy failed. Nothing was renamed, so the card is unchanged and" >&2
        echo "   the leftover $TMP is hidden from the menu -- remove it when convenient." >&2
        exit 1
    fi
    # sync before the rename, not after: the rename is what publishes the file
    # to the menu, and it should publish bytes that are actually on the card.
    if ! sshm "$H" "sync && mv -f '$DEST/$TMP' '$DEST/$NAME' && sync"; then
        echo "!! rename failed; $TMP is still on the card (hidden)." >&2
        exit 1
    fi
    echo "-- published $NAME"
fi

# ---------------------------------------------------------------------------
# Prune. On a dry run the just-copied core is not on the card yet, so add it to
# the listing by hand -- otherwise --dry-run would report pruning one more core
# than the real run does, which is the one way a dry run can mislead.
DATED="$(sshm "$H" "ls -1 '$DEST'/AmigaCD_*.rbf 2>/dev/null")"
if [ "$DRY" = 1 ]; then
    DATED="$(printf '%s\n%s\n' "$DATED" "$DEST/$NAME")"
fi
DATED="$(printf '%s\n' "$DATED" | grep -v '^$' | sort -u -r)"
PRUNE="$(printf '%s\n' "$DATED" | prune_list "$KEEP" "$NAME")"

if [ -n "$PRUNE" ]; then
    echo "-- pruning to --keep $KEEP:"
    printf '%s\n' "$PRUNE" | sed 's|.*/|     |'
    if [ "$KEEP" -lt 2 ]; then
        echo "   NOTE: --keep $KEEP leaves no previous core. docs/deployment.md"
        echo "   treats that one as the way back if this build misbehaves."
    fi
    if [ "$DRY" = 1 ]; then
        echo "   (dry run, nothing removed)"
    else
        printf '%s\n' "$PRUNE" | while IFS= read -r f; do
            [ -n "$f" ] && sshm "$H" "rm -f '$f'"
        done
        sshm "$H" "sync"
    fi
else
    echo "-- nothing to prune"
fi

# ---------------------------------------------------------------------------
# The undated one: reported by default, removed only on request. It is the most
# likely cause of a duplicate menu entry, and also the most likely thing to have
# been put there deliberately, so this asks rather than decides.
UNDATED="$(sshm "$H" "ls -1 '$DEST'/AmigaCD.rbf '$DEST'/amigacd.rbf 2>/dev/null")"
if [ -n "$UNDATED" ]; then
    if [ "$INCLUDE_UNDATED" = 1 ]; then
        echo "-- removing undated core(s):"
        printf '%s\n' "$UNDATED" | sed 's|.*/|     |'
        [ "$DRY" = 1 ] || { printf '%s\n' "$UNDATED" | while IFS= read -r f; do
            [ -n "$f" ] && sshm "$H" "rm -f '$f'"; done; sshm "$H" "sync"; }
    else
        echo "-- an UNDATED core is also present:"
        printf '%s\n' "$UNDATED" | sed 's|.*/|     |'
        echo "   It shows in the menu as a separate entry with '--.--.--' for its"
        echo "   date, because the menu reads the date from the filename. Left"
        echo "   alone in case you put it there on purpose; --include-undated"
        echo "   removes it."
    fi
fi

echo
echo "-- on the card now:"
sshm "$H" "ls -1 '$DEST'/AmigaCD*.rbf '$DEST'/amigacd*.rbf 2>/dev/null | sort" | sed 's|.*/|     |'
