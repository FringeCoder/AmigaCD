#!/usr/bin/env bash
#
# Cross-check the core's chipset timing against an independent implementation,
# over the serial port, in numbers rather than photographs.
#
# WHY. docs/core-accuracy-todo.md in the core repo opens by saying we cannot
# measure against real hardware, which is what leaves its beam-counter and
# bus-timing items tagged [SUITE]: needing a MiSTer, a screen, and a person
# reading values off it. Copperline's timing-test probes measure a fixed amount
# of CPU and chip-bus work against the CIA-A E-clock and stream the results out
# the SERIAL PORT as ASCII hex -- and our core has a UART. So the same disk can
# run on both and the answer is a diff, not a photograph.
#
# Copperline is not silicon either. Where the two columns disagree that is a
# question, not a verdict; what the harness removes is the manual step, not the
# uncertainty.
#
# THREE MODES, AND START WITH THE FIRST ONE
#
#   ./scripts/timing-probe.sh diagnose
#       Report what the core's UART actually looks like on this MiSTer. Run
#       this FIRST, before anything else -- the whole approach rests on one
#       assumption nobody has tested: that the core's serial output reaches a
#       host device we can read. MiSTer's userspace shells out to `uartmode N`
#       (user_io.cpp SetUARTMode) to bridge the FPGA's UART to the board's
#       physical port, and what is attached to that port afterwards -- a getty,
#       nothing, a different device name -- is not something this script can
#       know in advance. So it asks the machine instead of guessing, and prints
#       what it finds.
#
#   ./scripts/timing-probe.sh reference /path/to/copperline
#       Regenerate docs/reference/timing-probe-copperline.txt from a Copperline
#       checkout. Needs a release build there. Committed, so the compare mode
#       needs no emulator.
#
#   ./scripts/timing-probe.sh adf /path/to/copperline [DEST]
#       Build timing-test.adf from Copperline's committed probe binaries and,
#       given a MiSTer, copy it there. The ADF is 900 KB of generated artifact
#       so it is NOT committed -- it is rebuilt from bytes that are.
#
#   ./scripts/timing-probe.sh compare
#       Capture the probe's output from the MiSTer and diff it against the
#       reference column.
#
# Host comes from ./host (gitignored) or $MISTER_HOST, the same convention
# collect-diag.sh, wsl-build.sh and deploy-from-ci.sh use.

set -uo pipefail
cd "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

REFERENCE="docs/reference/timing-probe-copperline.txt"
ADF_NAME="timing-test.adf"

host() {
    local h="${MISTER_HOST:-}"
    if [ -z "$h" ] && [ -f host ]; then h="$(tr -d '[:space:]' < host)"; fi
    if [ -z "$h" ]; then
        echo "!! No MiSTer address. Put its IP in ./host or set MISTER_HOST." >&2
        return 1
    fi
    printf '%s' "$h"
}

# BatchMode so a missing key fails immediately instead of hanging on a password
# prompt -- the same reason collect-diag.sh uses it.
sshm() {
    ssh -o BatchMode=yes -o ConnectTimeout=10 -o StrictHostKeyChecking=accept-new \
        "root@$1" "$2"
}

# ---------------------------------------------------------------------------
cmd_diagnose() {
    local H; H="$(host)" || return 1
    echo "== MiSTer at $H"
    echo

    echo "-- which core is loaded, and is it ours"
    sshm "$H" "ps aux | grep -c '[M]iSTer_AmigaCD' >/dev/null && echo 'MiSTer_AmigaCD is running' || echo 'MiSTer_AmigaCD is NOT running'; cat /tmp/CORENAME 2>/dev/null | sed 's/^/CORENAME: /'"
    echo

    echo "-- what uartmode the core is in"
    # GetUARTMode() reads these; the file that exists IS the mode.
    sshm "$H" "for n in 1 2 3 4 5 6; do [ -e /tmp/uartmode\$n ] && echo \"uartmode \$n\"; done; [ -e /tmp/UART_SPEED ] && echo \"baud: \$(cat /tmp/UART_SPEED)\"; true"
    echo

    echo "-- serial devices present"
    sshm "$H" "ls -l /dev/ttyS* /dev/ttyUSB* 2>/dev/null; true"
    echo

    echo "-- is anything already holding one (a getty would fight us for bytes)"
    sshm "$H" "command -v fuser >/dev/null && fuser -v /dev/ttyS1 2>&1 | head -5; ps aux | grep -E '[g]etty|[a]getty' | head -5; true"
    echo

    echo "-- the uartmode helper itself"
    sshm "$H" "command -v uartmode && head -30 \$(command -v uartmode) 2>/dev/null; true"
    echo

    cat <<'NOTE'
WHAT TO LOOK FOR

  A serial device (almost certainly /dev/ttyS1) that exists, is not held by a
  getty, and carries the core's output once the core is in a UART mode other
  than None. If a getty has it, stop that getty for the duration -- do not
  fight it for bytes, the interleaving is silent and looks like a timing fault.

  If no mode is set, set one from the core's OSD (the UART MODE menu) and run
  this again. Console mode is the obvious first try; if it turns out to attach
  a login shell to the port, try another mode and record which worked in
  docs/timing-probe.md so the next person does not repeat the experiment.

  If nothing reaches a host device at all, this approach does not work as
  designed and the fallback is the on-screen column: the probe draws all 32
  rows inside the CRT-safe area precisely so it can be photographed. That is
  the manual path we were trying to avoid, but it is not a dead end.
NOTE
}

# ---------------------------------------------------------------------------
cmd_reference() {
    local CANON="${1:-}"
    [ -n "$CANON" ] || { echo "usage: timing-probe.sh reference /path/to/copperline" >&2; return 1; }

    local CL="$CANON/target/release/copperline"
    [ -x "$CL" ] || { echo "!! $CL not found -- build Copperline first (cargo build --release)" >&2; return 1; }

    local WORK; WORK="$(mktemp -d)"
    trap 'rm -rf "$WORK"' RETURN
    build_adf "$CANON" "$WORK/$ADF_NAME" || return 1

    mkdir -p "$(dirname "$REFERENCE")"
    {
        echo "# Copperline's column of the timing-test probe."
        echo "#"
        echo "# Generated by scripts/timing-probe.sh reference. One 8-digit hex value"
        echo "# per test row, in the order the probe emits them. See timing-test/README.md"
        echo "# in the Copperline repo for what each row measures."
        echo "#"
        echo "# This is a SECOND OPINION, not ground truth. Copperline is an emulator,"
        echo "# not a reference machine. A row where our core disagrees is a question."
        echo "#"
        echo "# source   = $("$CL" --version 2>/dev/null | head -1), --model A500 --factory"
        echo "# revision = $(git -C "$CANON" rev-parse --short HEAD 2>/dev/null || echo unknown)"
        echo "# generated= $(date -u +%Y-%m-%d)"
        echo "#"
        # tr -d '\r' is load bearing: the guest's serial output is CRLF, so an
        # anchored grep matches nothing without it and the capture silently
        # comes back empty. The compare path strips it on the MiSTer side for
        # the same reason.
        timeout 300 "$CL" --factory --model A500 --noaudio --serial stdout \
            --insert-disk-after 0 df0 "$WORK/$ADF_NAME" \
            --screenshot-after 25 "$WORK/shot.png" 2>/dev/null \
          | tr -d '\r' | grep -E '^[0-9A-F]{8}$'
    } > "$REFERENCE"

    local n; n="$(grep -cE '^[0-9A-F]{8}$' "$REFERENCE")"
    echo "wrote $REFERENCE ($n rows)"
    [ "$n" -gt 0 ] || { echo "!! no rows captured -- the probe produced no serial output" >&2; return 1; }
}

# ---------------------------------------------------------------------------
# Shared by 'reference' and 'adf': wrap Copperline's committed probe binaries
# into a bootable ADF. build.sh would assemble them first and needs vasm; the
# .bin files are in the repo already and make_adf.py wraps them on its own, so
# this needs no m68k toolchain.
build_adf() {
    local CANON="$1" DEST="$2"
    local TT="$CANON/timing-test"
    [ -f "$TT/boot.bin" ] && [ -f "$TT/test.bin" ] || {
        echo "!! $TT/{boot,test}.bin missing -- is $CANON a Copperline checkout?" >&2
        return 1
    }
    ( cd "$TT" && python3 make_adf.py boot.bin test.bin "$DEST" ) >/dev/null || return 1
}

cmd_adf() {
    local CANON="${1:-}"
    [ -n "$CANON" ] || { echo "usage: timing-probe.sh adf /path/to/copperline [DEST]" >&2; return 1; }
    local DEST="${2:-output/$ADF_NAME}"
    mkdir -p "$(dirname "$DEST")"
    build_adf "$CANON" "$DEST" || return 1
    echo "built $DEST"

    local H; H="$(host 2>/dev/null)" || {
        echo "(no MiSTer address; copy it across yourself)"
        return 0
    }
    local REMOTE="${TIMING_PROBE_REMOTE:-/media/fat/games/Amiga/$ADF_NAME}"
    echo "== copying to $H:$REMOTE"
    if scp -o BatchMode=yes -o ConnectTimeout=10 "$DEST" "root@$H:$REMOTE" 2>&1; then
        echo "copied. Mount it in df0 from the OSD and reset the core."
    else
        echo "!! copy failed -- the directory may not exist on this card." >&2
        echo "   Set TIMING_PROBE_REMOTE to somewhere that does." >&2
        return 1
    fi
}

# ---------------------------------------------------------------------------
cmd_compare() {
    local H; H="$(host)" || return 1
    [ -f "$REFERENCE" ] || { echo "!! $REFERENCE missing -- run 'reference' first" >&2; return 1; }

    local DEV="${TIMING_PROBE_DEV:-/dev/ttyS1}"
    local SECS="${TIMING_PROBE_SECS:-40}"

    echo "== capturing from $H ($DEV, ${SECS}s)"
    echo "   Mount $ADF_NAME in df0 and reset the core now if you have not already."
    local got
    got="$(sshm "$H" "timeout $SECS cat $DEV 2>/dev/null | tr -d '\r'" | grep -oE '^[0-9A-F]{8}$')"

    if [ -z "$got" ]; then
        echo "!! nothing captured. Run 'diagnose' -- the most likely causes are the"
        echo "   core not being in a UART mode, a getty holding $DEV, or the device"
        echo "   being named something else (set TIMING_PROBE_DEV)." >&2
        return 1
    fi

    local want; want="$(grep -E '^[0-9A-F]{8}$' "$REFERENCE")"
    echo
    printf '%-5s %-10s %-10s %s\n' row core copperline ''
    local i=1 differ=0 c r
    while IFS= read -r c; do
        r="$(printf '%s\n' "$want" | sed -n "${i}p")"
        if [ -z "$r" ]; then
            printf '%-5s %-10s %-10s %s\n' "$i" "$c" "--" "(no reference row)"
        elif [ "$c" = "$r" ]; then
            printf '%-5s %-10s %-10s %s\n' "$i" "$c" "$r" "ok"
        else
            printf '%-5s %-10s %-10s %s\n' "$i" "$c" "$r" "DIFFERS"
            differ=$((differ + 1))
        fi
        i=$((i + 1))
    done <<< "$got"

    echo
    echo "$((i - 1)) rows captured, $differ differ"
    echo
    echo "A differing row is a question, not a defect: it says the two"
    echo "implementations time that operation differently and one of them is"
    echo "wrong. timing-test/README.md in the Copperline repo says what each row"
    echo "measures and which of them already have a real-hardware column."
    [ "$differ" -eq 0 ]
}

# ---------------------------------------------------------------------------
case "${1:-}" in
    diagnose)  shift; cmd_diagnose "$@" ;;
    adf)       shift; cmd_adf "$@" ;;
    reference) shift; cmd_reference "$@" ;;
    compare)   shift; cmd_compare "$@" ;;
    *)
        sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'
        exit 1
        ;;
esac
