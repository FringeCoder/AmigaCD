#!/usr/bin/env bash
#
# Copy the Menu core to the MiSTer, keep a spare, and notice when update_all
# has taken it back.
#
# WHY THIS EXISTS. menu.rbf is the last file in this project that reaches the
# machine by hand, and it is the one with the nastiest failure mode. Three
# things compound:
#
#   1. Nothing sent it. stage-output.sh puts the build in output/menu.rbf and
#      stops; every remote copy in this repository sends only the userspace
#      binary, and the CI artifact carries just MiSTer and MiSTer.elf.
#
#   2. It cannot be protected from update_all. Downloader filters are
#      category/core level, there is no per-file exclusion, and the boot menu
#      MUST be named menu.rbf, so renaming is not available either.
#
#   3. When it is taken back, nothing says so. The SNAC pad stops answering in
#      the boot menu with a menu.rbf present and a plausible version on screen,
#      which is a rough thing to attribute: it looks like the pad, or the
#      adapter, or snac_psx=1, or SW[1] on the DE10-Nano.
#
# The documented defence has been two commands run by hand after every update.
# This script is those two commands plus the part nobody can do by hand, which
# is remembering to check.
#
# THE TRANSIT NAME MATTERS MORE HERE than it does for a game core. A truncated
# AmigaCD_<date>.rbf is a core that will not load; a truncated menu.rbf is the
# thing that boots. So the copy lands under a name the browser cannot see and
# is renamed into place only once it is on the card -- same shape as
# deploy-core.sh, same reasons, higher stakes.
#
# MODES
#
#   ./scripts/deploy-menu.sh              copy output/menu.rbf into place and
#                                         refresh the spare
#   ./scripts/deploy-menu.sh --check      compare what is live against the
#                                         spare and say whether it was taken
#   ./scripts/deploy-menu.sh --restore    put the spare back, after update_all
#   ./scripts/deploy-menu.sh --dry-run    say what would happen, change nothing
#
# Host comes from ./host (gitignored) or $MISTER_HOST, as elsewhere.

set -uo pipefail
cd "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)" || exit 1

LIVE="/media/fat/menu.rbf"
SPARE="/media/fat/menu_snac.rbf"
SRC="output/menu.rbf"
MODE="deploy"
DRY=0

while [ $# -gt 0 ]; do
    case "$1" in
        --check)   MODE="check";   shift ;;
        --restore) MODE="restore"; shift ;;
        --dry-run) DRY=1;          shift ;;
        -h|--help) sed -n '2,43p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)         echo "!! unknown option: $1" >&2; exit 2 ;;
    esac
done

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
# What --check concluded, given the two md5s and whether each file is there.
#
# Its own function so it can be tested without a MiSTer. The interesting part is
# not the comparison, it is that there are five distinct answers and four of
# them are easy to collapse into "something is wrong" -- which is the state this
# whole script exists to get out of.
#
#   $1  md5 of the live menu.rbf, empty when it is not there
#   $2  md5 of the spare,         empty when it is not there
#
# Prints one of: ok, clobbered, no-spare, no-live, no-either
menu_check_verdict() {
    local live="$1" spare="$2"

    if [ -z "$live" ] && [ -z "$spare" ]; then printf 'no-either';  return 0; fi
    if [ -z "$live" ];                   then printf 'no-live';    return 0; fi
    if [ -z "$spare" ];                  then printf 'no-spare';   return 0; fi
    if [ "$live" = "$spare" ];           then printf 'ok';         return 0; fi
    printf 'clobbered'
    return 0
}

if [ "${DEPLOY_MENU_LIB:-0}" = 1 ]; then return 0 2>/dev/null || exit 0; fi

H="$(host)" || exit 1

md5_of() {
    # Empty output means the file is not there, which the verdict treats as its
    # own answer rather than folding into a mismatch.
    sshm "$H" "md5sum '$1' 2>/dev/null | cut -d' ' -f1"
}

# ---------------------------------------------------------------------------
case "$MODE" in
check)
    LIVE_MD5="$(md5_of "$LIVE")"
    SPARE_MD5="$(md5_of "$SPARE")"
    case "$(menu_check_verdict "$LIVE_MD5" "$SPARE_MD5")" in
        ok)
            echo "== menu.rbf matches the spare ($LIVE_MD5)"
            echo "   The Menu core on the card is the one you deployed."
            ;;
        clobbered)
            echo "!! menu.rbf is NOT the one you deployed."
            echo "   live  $LIVE_MD5"
            echo "   spare $SPARE_MD5"
            echo
            echo "   This is what update_all does: it has no per-file exclusion"
            echo "   and the boot menu must be called menu.rbf. The SNAC pad"
            echo "   will not answer in the boot menu until the spare is back."
            echo
            echo "   ./scripts/deploy-menu.sh --restore"
            exit 1
            ;;
        no-spare)
            echo "!! No spare at $SPARE."
            echo "   Nothing to compare against, so a future update_all would"
            echo "   take the Menu core back with no way to notice or undo it."
            echo "   Deploy once (./scripts/deploy-menu.sh) to create one."
            exit 1
            ;;
        no-live)
            echo "!! No $LIVE on the card at all. The MiSTer will not boot to a"
            echo "   menu. Restore the spare or deploy."
            exit 1
            ;;
        no-either)
            echo "!! Neither $LIVE nor $SPARE exists. Deploy first."
            exit 1
            ;;
    esac
    ;;

restore)
    SPARE_MD5="$(md5_of "$SPARE")"
    if [ -z "$SPARE_MD5" ]; then
        echo "!! No spare at $SPARE to restore from." >&2
        exit 1
    fi
    if [ "$DRY" = 1 ]; then
        echo "-- would copy $SPARE -> $LIVE ($SPARE_MD5)"
        exit 0
    fi
    # Through a transit name for the same reason the deploy is: this is the
    # file the machine boots.
    if ! sshm "$H" "cp -f '$SPARE' '$LIVE.part' && sync && mv -f '$LIVE.part' '$LIVE' && sync"; then
        echo "!! restore failed" >&2
        exit 1
    fi
    echo "-- restored $LIVE from the spare ($SPARE_MD5)"
    ;;

deploy)
    if [ ! -f "$SRC" ]; then
        echo "!! No $SRC. Run scripts/stage-output.sh first." >&2
        exit 1
    fi
    SRC_MD5="$(md5sum "$SRC" | cut -d' ' -f1)"
    echo "== $H:$LIVE  ($(du -h "$SRC" | cut -f1), $SRC_MD5)"

    if [ "$DRY" = 1 ]; then
        echo "-- would copy $SRC -> $LIVE, then refresh $SPARE from it"
        exit 0
    fi

    # Hidden, and not an .rbf: ScanDirectory skips dotfiles before it tests an
    # extension (file_io.cpp:1754) and the extension it would test comes after
    # the last dot. A half-written boot menu is not something to leave visible.
    if ! scp -o BatchMode=yes -q "$SRC" "root@$H:/media/fat/.menu.rbf.part"; then
        echo "!! copy failed. Nothing was renamed, so the card is unchanged." >&2
        exit 1
    fi
    # sync before the rename, not after: the rename is what publishes it, and it
    # should publish bytes that are on the card.
    if ! sshm "$H" "sync && mv -f /media/fat/.menu.rbf.part '$LIVE' && sync"; then
        echo "!! rename failed; the transit file is still there (hidden)." >&2
        exit 1
    fi
    echo "-- published $LIVE"

    # The spare is refreshed from the file just published rather than copied
    # again from here, so the two are the same bytes by construction and
    # --check cannot report a difference that is only a second transfer.
    if ! sshm "$H" "cp -f '$LIVE' '$SPARE' && sync"; then
        echo "!! could not refresh the spare at $SPARE." >&2
        echo "   The deploy stands, but update_all will be able to take it back" >&2
        echo "   with nothing to restore from." >&2
        exit 1
    fi
    echo "-- spare refreshed at $SPARE"
    echo
    echo "After any update_all that touches the Menu core:"
    echo "  ./scripts/deploy-menu.sh --check     # did it take it?"
    echo "  ./scripts/deploy-menu.sh --restore   # put it back"
    ;;
esac
