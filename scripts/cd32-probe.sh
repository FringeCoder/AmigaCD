#!/usr/bin/env bash
#
# Run Copperline's cd32-probe on our core, so its rows can be compared against
# the same probe on a real CD32 and against Copperline's own column.
#
# WHY THIS IS THE ONE WORTH DOING WITH A REAL CD32. Rows 14-23 are CD seek and
# read costs -- Akiko's behaviour. That is this core's speciality and the single
# area where no test suite exists at all: there is nothing for chunky-to-planar,
# the CD command surface, subcode delivery or the CDTV bridges, and Copperline's
# own HARDWARE-RIG-PLAN.md lists akiko-sector-slot-order-no-ground-truth as an
# open item. A photograph of these rows on a real CD32 would be the first such
# measurement for either project.
#
# It also needs no serial cable. The probe renders its rows on screen as hex,
# deliberately, to be photographed -- the same approach the timing-test disk
# takes. And the disc that carries it is an ISO, which means all three targets
# take the SAME artifact:
#
#   real CD32   burn it (slow, 4x-8x; CD32 drives dislike fast burns)
#   our core    scripts/cd32-probe.sh run <iso>   (mount_cd over MiSTer_cmd)
#   Copperline  copperline --model CD32 --insert-cd-after 0 <iso>
#
# MAKING THE DISC IS COPPERLINE'S JOB, NOT OURS. tools/cd32-probe/ there has a
# prebuilt AmigaOS executable (PROBE, committed) and make-images.py, which
# splices it into a copy of a commercial CD32 disc image in place of SYS:FSCD.
# That is how it sidesteps the trademark problem: a bootable CD32 disc needs a
# .TM block from the Commodore developer kit, which is no more ours to ship than
# a Kickstart, and reusing a real disc's boot structure avoids needing one.
#
# You supply the game image. See tools/cd32-probe/README.md for which variant
# answers which rows -- the data-only padded variant is the one for the CD seek
# rows, and it is the one to burn first.
#
# MODES
#
#   ./scripts/cd32-probe.sh run <iso> [SECONDS]
#       Push the ISO to the card, mount it in the CD bay, let the probe run,
#       screenshot, pull the PNG back.
#
#   ./scripts/cd32-probe.sh reference
#       Print the committed Copperline column with each row's meaning.
#
# Host comes from ./host (gitignored) or $MISTER_HOST, as elsewhere.

set -uo pipefail
cd "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

REFERENCE="docs/reference/cd32-probe-copperline.tsv"
REMOTE_DIR="${CD32_PROBE_REMOTE_DIR:-/media/fat/games/AmigaCD}"

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
cmd_reference() {
    [ -f "$REFERENCE" ] || { echo "!! $REFERENCE missing" >&2; return 1; }
    printf '%-4s %-10s %-22s %s\n' row label copperline meaning
    awk -F'\t' '!/^#/ && NF==4 { printf "%-4s %-10s %-22s %s\n", $1, $2, $3, $4 }' "$REFERENCE"
    echo
    echo "Rows 14-23 are the CD seek and read costs -- the Akiko rows, and the"
    echo "reason a real CD32 is worth the trouble. Rows 24-29 are bus-cycle costs"
    echo "against the CIA E-clock."
    echo
    echo "Copperline's column is what one emulator predicts, not ground truth."
    echo "Rows written as '~139 ms' are approximations in its README; the hex rows"
    echo "are the comparable ones."
}

# ---------------------------------------------------------------------------
cmd_run() {
    local ISO="${1:-}" SECS="${2:-45}"
    [ -f "$ISO" ] || {
        echo "usage: cd32-probe.sh run /path/to/cd32-probe.iso [SECONDS]" >&2
        echo "  Build it with make-images.py in Copperline's tools/cd32-probe/." >&2
        return 1
    }
    local H; H="$(host)" || return 1
    local NAME; NAME="$(basename "$ISO")"

    echo "== pushing $NAME ($(du -h "$ISO" | cut -f1))"
    sshm "$H" "mkdir -p '$REMOTE_DIR'" || return 1
    scp -o BatchMode=yes "$ISO" "root@$H:$REMOTE_DIR/$NAME" || {
        echo "!! copy failed -- a padded probe ISO is ~440 MB, check space on the card" >&2
        return 1
    }

    # mount_cd is our own MiSTer_cmd hook (input.cpp): it puts an image on IDE
    # slot 0, the CD32 CD bay, without OSD navigation. It does not reset the
    # machine, so the probe needs the core to come up with the disc already
    # mounted -- mount first, then reset, not the other way round.
    echo "== mounting in the CD bay"
    sshm "$H" "echo 'mount_cd $REMOTE_DIR/$NAME' > /dev/MiSTer_cmd" || return 1
    sleep 2

    echo "== the core must now be reset so it boots from the disc."
    echo "   Reset it from the OSD, then leave it alone for ${SECS}s."
    echo "   (A cold CD32 pays the drive's spin-up before the first TOC dump --"
    echo "    Copperline measures ~8.8 s from cold, ~3.5 s on a warm change, and"
    echo "    the probe itself runs its seek rows after that.)"
    read -r -p "   Press Return once you have reset it: " _ || true

    sleep "$SECS"
    sshm "$H" "echo 'screenshot cd32probe' > /dev/MiSTer_cmd" || return 1
    sleep 3

    mkdir -p output/cd32-probe
    local SHOT
    SHOT="$(sshm "$H" "ls -1t /media/fat/screenshots/*/*cd32probe* 2>/dev/null | head -1")"
    if [ -z "$SHOT" ]; then
        echo "!! no screenshot appeared -- check the core actually loaded the disc" >&2
        return 1
    fi
    scp -o BatchMode=yes -q "root@$H:$SHOT" "output/cd32-probe/$NAME.png" || return 1
    echo
    echo "captured output/cd32-probe/$NAME.png"
    echo
    echo "Read the 30 hex rows off it and put them beside the real CD32's"
    echo "photograph and the Copperline column:"
    echo "  ./scripts/cd32-probe.sh reference"
    echo
    echo "A row where our core and the real machine disagree is a finding about"
    echo "THIS core. A row where Copperline and the real machine disagree is a"
    echo "finding worth sending them -- their plan says they have never had a"
    echo "machine to check against."
}

# ---------------------------------------------------------------------------
case "${1:-}" in
    run)       shift; cmd_run "$@" ;;
    reference) shift; cmd_reference "$@" ;;
    *)
        sed -n '2,45p' "$0" | sed 's/^# \{0,1\}//'
        exit 1
        ;;
esac
