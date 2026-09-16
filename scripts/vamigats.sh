#!/usr/bin/env bash
#
# Run vAmigaTS tests on the core and compare against references photographed on
# real Amiga hardware.
#
# WHY THIS ONE MATTERS MORE THAN THE OTHERS. Every verification route we have
# compares us against another model: our benches assert what we wrote into them,
# and the Copperline work compares us against a second emulator. vAmigaTS is
# different -- 2,967 of its reference images are PHOTOGRAPHS OF A REAL AMIGA 500
# (18 more of a real A1200). That is silicon, and docs/core-accuracy-todo.md in
# the core repo opens by saying we do not have any.
#
# It is also what T7 was written against. Agnus/Registers/VPOS/ contains
# probe1..probe13, vhpos1..5, vprobe1..4, ersy1, ersy2, lof1, lof2, cycle01v and
# cycleD9v -- and T7's text cites "ersy1 is correct on only 14 of 16" and "304
# probe rows" by those names. The suite upstream measured against has been
# sitting there all along.
#
# THE SUITE IS NOT VENDORED. It is 3.6 GB. Clone it wherever you keep such
# things and pass the path:
#
#   git clone https://github.com/dirkwhoffmann/vAmigaTS
#
# A KICKSTART IS REQUIRED and is not ours to ship -- it is Cloanto's, still sold
# as Amiga Forever, and the core's own README says to dump your machine's ROM or
# buy that. This is not an extra hurdle: the core cannot run without one, so if
# the core runs, the ROM is already on the card and this uses whatever is
# configured.
#
# BUT WHICH ONE MATTERS, and it is not obvious. vAmigaTS's references were taken
# with KICKSTART 1.3 -- every .retrosh in the suite says "regression setup
# A500_OCS_1MB /tmp/kick13.rom". The VPOS probes take the machine over
# completely (interrupts and DMA off, no OS calls), so they SHOULD be
# ROM-independent, and Copperline's own timing-test README says its display
# probes were verified byte-identical between AROS and a Kickstart 1.3 boot.
# "Should" is not "were". If a probe disagrees with the reference, the ROM
# version is the first variable to rule out, not the last -- run the kickstart
# mode below and compare.
#
# MODES
#
#   ./scripts/vamigats.sh kickstart
#       Report which Kickstart the core is configured with, and which ROM files
#       are on the card. Worth running once before the first test -- see the
#       note on 1.3 below.
#
#   ./scripts/vamigats.sh inventory <vamigats-path>
#       What is in the suite and how much of it has a hardware reference.
#
#   ./scripts/vamigats.sh run <vamigats-path> <test-dir> [SECONDS]
#       Push the test's ADF to the MiSTer, build an MGL that loads the core with
#       it mounted in df0, run it, take a screenshot, and pull the PNG back.
#       SECONDS is how long to let it settle (default 12) -- the reference
#       scripts use 9 for most tests.
#
#   ./scripts/vamigats.sh compare <vamigats-path> <test-dir>
#       Print where our capture and the hardware photograph are, side by side.
#
# THE COMPARISON IS NOT AUTOMATIC, AND CANNOT BE. The reference is a photograph
# of a CRT: different geometry, different colour, JPEG artefacts, a visible
# curve. Pixel-diffing it against our framebuffer would be measuring the camera.
# What IS comparable is the numbers the probes print on screen -- which is why
# they print them in large text inside the CRT-safe area. Read them off both and
# record them in docs/reference/vamigats-groundtruth.tsv, which is the durable
# artifact here: once a probe's true values are written down, a bench can check
# the RTL against them forever without the photograph, the disk or the machine.
#
# Host comes from ./host (gitignored) or $MISTER_HOST, as elsewhere.

set -uo pipefail
cd "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

GROUNDTRUTH="docs/reference/vamigats-groundtruth.tsv"

# Where things land on the card. The core path is what the MGL names, so it has
# to match a core that is actually there -- dated names mean this needs saying
# rather than guessing.
REMOTE_DIR="${VAMIGATS_REMOTE_DIR:-/media/fat/games/Amiga}"
REMOTE_MGL="${VAMIGATS_REMOTE_MGL:-/media/fat/_Test/vamigats.mgl}"
CORE_RBF="${VAMIGATS_CORE:-}"

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
cmd_kickstart() {
    local H; H="$(host)" || return 1
    echo "== MiSTer at $H"
    echo
    echo "-- Kickstart the core is configured with"
    # minimig_config.kickstart[] is persisted in the core's .cfg; the config
    # name follows user_io_get_core_name(), so AmigaCD.cfg is the slot-0 one.
    # The path is stored as text inside a binary blob, so pull printable runs
    # out of it rather than pretending to parse the struct.
    sshm "$H" "for f in /media/fat/config/AmigaCD*.cfg; do [ -e \"\$f\" ] || continue; echo \"\$f:\"; strings -n 4 \"\$f\" 2>/dev/null | grep -iE '\\.rom\$|kick|aros' | head -4; done; true"
    echo
    echo "-- Amiga Forever key file"
    # UploadKickstart() looks in HomeDir() then the card root. HomeDir() for
    # this core resolves to "Amiga": user_io_get_core_path maps both "minimig"
    # and "amigacd" to it (user_io.cpp:206), so Minimig and AmigaCD share ROMs.
    sshm "$H" "ls -l /media/fat/games/Amiga/ROM.KEY /media/fat/games/Amiga/rom.key /media/fat/ROM.KEY /media/fat/rom.key 2>/dev/null || echo '  (none found -- only needed for ENCRYPTED Amiga Forever ROMs)'"
    echo

    echo "-- ROM files, sized against what the core accepts"
    # The core identifies a ROM BY SIZE (minimig_config.cpp UploadKickstart),
    # so the size is not trivia -- it is the whole detection mechanism, and an
    # unrecognised one fails with nothing but "Unsupported ROM file size!".
    sshm "$H" "ls -l /media/fat/*.rom /media/fat/games/Amiga/*.rom /media/fat/Amiga*/*.rom 2>/dev/null | head -12; true" \
      | awk '
          { print }
          /\.[rR][oO][mM]$/ {
            n = $5
            # if/else rather than a ternary chain: mawk rejects a chain split
            # across lines, and this has to survive whatever awk is to hand.
            if      (n == 262144)  t = "256K plain (KS 1.2/1.3)"
            else if (n == 262155)  t = "256K Amiga Forever encrypted (needs ROM.KEY)"
            else if (n == 524288)  t = "512K plain (KS 2.x/3.x)"
            else if (n == 524299)  t = "512K Amiga Forever encrypted (needs ROM.KEY)"
            else if (n == 1048576) t = "1MB plain"
            else if (n == 8192)    t = "A1000 boot ROM"
            else if (n == 8203)    t = "A1000 boot ROM, encrypted"
            else                   t = "SIZE NOT RECOGNISED -- this one will be refused"
            print "       ^ " t
          }'
    echo
    cat <<'NOTE'
WHAT TO LOOK FOR

  vAmigaTS's reference images were taken with KICKSTART 1.3, which is a 256 KB
  ROM. The VPOS probes take the machine over completely, so they should not
  care -- but that is an expectation, not a measurement. If a probe's values
  disagree with the photograph, check the ROM version before concluding
  anything about the RTL.

AMIGA FOREVER ROMS NEED NO DECRYPTION

  Copy them across as they are, together with rom.key. The core loads the key
  (from games/Amiga/ or the card root) and decrypts on upload -- the encrypted
  forms are recognised by their size, which is the plain size plus 11 bytes.

  ONE GAP, worth knowing before it wastes an evening: there is a branch for
  encrypted 256 KB and 512 KB ROMs but NOT for an encrypted 1 MB one. A 1 MB
  Amiga Forever ROM (1048587 bytes) falls through to "Unsupported ROM file
  size!". Use a decrypted 1 MB image, or a 512 KB ROM, for that case.
NOTE
}

# ---------------------------------------------------------------------------
cmd_inventory() {
    local TS="${1:-}"
    [ -d "$TS" ] || { echo "usage: vamigats.sh inventory /path/to/vAmigaTS" >&2; return 1; }

    echo "== $TS"
    echo
    printf '%-12s %8s %8s\n' area tests refs
    local d
    for d in Agnus CIA CPU Denise FPU Mainboard Memory Misc Paula; do
        [ -d "$TS/$d" ] || continue
        printf '%-12s %8s %8s\n' "$d" \
            "$(find "$TS/$d" -name '*.adf' 2>/dev/null | wc -l)" \
            "$(find "$TS/$d" \( -iname '*_A500_*' -o -iname '*_A1200_*' \) 2>/dev/null | wc -l)"
    done
    echo
    echo "hardware references by machine:"
    find "$TS" \( -iname '*_A500_*' -o -iname '*_A1200_*' -o -iname '*_A2000_*' \) 2>/dev/null \
      | sed 's/.*_\(A[0-9]*\)_.*/  \1/' | sort | uniq -c
    echo
    echo "the VPOS probes T7 names:"
    ls "$TS/Agnus/Registers/VPOS/" 2>/dev/null | grep -vE '^(Makefile|README|.*\.i)$' | tr '\n' ' '
    echo
}

# ---------------------------------------------------------------------------
resolve_core() {
    local H="$1"
    if [ -n "$CORE_RBF" ]; then printf '%s' "$CORE_RBF"; return 0; fi
    # Dated core names mean the path is not a constant any more. Ask the card
    # for the newest AmigaCD*.rbf rather than baking in a name that goes stale
    # the next time a fit is staged.
    local found
    found="$(sshm "$H" "ls -1t /media/fat/_Test/AmigaCD*.rbf /media/fat/_Computer/AmigaCD*.rbf 2>/dev/null | head -1")"
    if [ -z "$found" ]; then
        echo "!! No AmigaCD*.rbf found in _Test or _Computer. Set VAMIGATS_CORE" >&2
        echo "   to the core's path on the card." >&2
        return 1
    fi
    # The MGL names the core WITHOUT /media/fat and without the extension.
    printf '%s' "$(printf '%s' "$found" | sed -e 's|^/media/fat/||' -e 's|\.rbf$||')"
}

cmd_run() {
    local TS="${1:-}" TEST="${2:-}" SECS="${3:-12}"
    [ -d "$TS" ] && [ -n "$TEST" ] || {
        echo "usage: vamigats.sh run /path/to/vAmigaTS Agnus/Registers/VPOS/probe6 [SECONDS]" >&2
        return 1
    }
    local DIR="$TS/$TEST"
    [ -d "$DIR" ] || { echo "!! no such test: $DIR" >&2; return 1; }

    local ADF; ADF="$(find "$DIR" -maxdepth 1 -name '*.adf' | head -1)"
    [ -n "$ADF" ] || { echo "!! no .adf in $DIR" >&2; return 1; }
    local NAME; NAME="$(basename "$ADF" .adf)"

    local H; H="$(host)" || return 1
    local CORE; CORE="$(resolve_core "$H")" || return 1
    echo "== core: $CORE"

    echo "== pushing $NAME.adf"
    sshm "$H" "mkdir -p '$REMOTE_DIR' '$(dirname "$REMOTE_MGL")'" || return 1
    scp -o BatchMode=yes -q "$ADF" "root@$H:$REMOTE_DIR/$NAME.adf" || {
        echo "!! copy failed" >&2; return 1; }

    # An MGL loads a core and mounts media in one step. type="f" index="0" is
    # df0 -- support/minimig/minimig_config.cpp parses exactly that.
    local MGL
    MGL="<mistergamedescription>
    <rbf>$CORE</rbf>
    <file type=\"f\" index=\"0\" path=\"${REMOTE_DIR#/media/fat/}/$NAME.adf\"/>
</mistergamedescription>"
    printf '%s\n' "$MGL" | sshm "$H" "cat > '$REMOTE_MGL'" || return 1

    echo "== loading, settling ${SECS}s"
    sshm "$H" "echo 'load_core $REMOTE_MGL' > /dev/MiSTer_cmd" || return 1
    sleep "$SECS"
    sshm "$H" "echo 'screenshot vamigats-$NAME' > /dev/MiSTer_cmd" || return 1
    sleep 3

    mkdir -p output/vamigats
    # The screenshot lands under screenshots/<CONF_STR name>/<datecode>-<name>.png
    # (file_io.cpp). Pull the newest match rather than reconstructing the name:
    # the datecode is the core's idea of now, not ours.
    local REMOTE_SHOT
    REMOTE_SHOT="$(sshm "$H" "ls -1t /media/fat/screenshots/*/*vamigats-$NAME* 2>/dev/null | head -1")"
    if [ -z "$REMOTE_SHOT" ]; then
        echo "!! no screenshot appeared. The core may not have been loaded, or the" >&2
        echo "   scaler may not support capture on this build -- do_screenshot()" >&2
        echo "   prints 'problem with scaler' in that case; check the core's log." >&2
        return 1
    fi
    scp -o BatchMode=yes -q "root@$H:$REMOTE_SHOT" "output/vamigats/$NAME.png" || return 1
    echo "captured output/vamigats/$NAME.png"
    echo
    cmd_compare "$TS" "$TEST"
}

# ---------------------------------------------------------------------------
cmd_compare() {
    local TS="${1:-}" TEST="${2:-}"
    [ -d "$TS" ] && [ -n "$TEST" ] || {
        echo "usage: vamigats.sh compare /path/to/vAmigaTS Agnus/Registers/VPOS/probe6" >&2
        return 1
    }
    local DIR="$TS/$TEST"
    local NAME; NAME="$(basename "$(find "$DIR" -maxdepth 1 -name '*.adf' | head -1)" .adf)"

    echo "ours       : output/vamigats/$NAME.png"
    local ref
    for ref in "$DIR"/*_A500_OCS.JPG "$DIR"/*_A500_ECS.JPG "$DIR"/*_A1200*.JPG; do
        [ -e "$ref" ] && echo "hardware   : $ref"
    done
    for ref in "$DIR"/*.tiff; do
        [ -e "$ref" ] && echo "vAmiga     : $ref"
    done
    echo
    echo "Compare by READING THE NUMBERS off both, not by diffing the images: the"
    echo "reference is a photograph of a CRT and a pixel diff would be measuring"
    echo "the camera. Record what the hardware says in $GROUNDTRUTH --"
    echo "once a probe's true values are written down, a bench can hold the RTL to"
    echo "them forever without the photo, the disk, or the machine."
}

# ---------------------------------------------------------------------------
case "${1:-}" in
    kickstart) shift; cmd_kickstart "$@" ;;
    inventory) shift; cmd_inventory "$@" ;;
    run)       shift; cmd_run "$@" ;;
    compare)   shift; cmd_compare "$@" ;;
    *)
        sed -n '2,50p' "$0" | sed 's/^# \{0,1\}//'
        exit 1
        ;;
esac
