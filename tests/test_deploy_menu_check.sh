#!/usr/bin/env bash
#
# What scripts/deploy-menu.sh --check concludes.
#
# WHY THIS EXISTS. The whole point of --check is that update_all taking the Menu
# core back is otherwise INVISIBLE: the SNAC pad stops answering in the boot
# menu with a menu.rbf present and a plausible version on screen. A check that
# gets its verdict wrong puts that invisibility back, and worse, wearing a
# reassuring "ok".
#
# There are five states and four of them are easy to collapse into "something is
# wrong", which is the state this script exists to get out of. Each is worth
# distinguishing: no spare means a future update_all is unrecoverable; no live
# means the machine will not boot to a menu; a mismatch means restore; and both
# missing means nothing has ever been deployed.
#
# It needs no MiSTer. deploy-menu.sh returns immediately when sourced with
# DEPLOY_MENU_LIB=1, before it reads an argument or opens a connection.
#
# FALSIFIED. Each of these mutations was applied to menu_check_verdict() and the
# named case was confirmed to fail:
#
#   drop the no-spare arm        a spare that is gone reads as clobbered
#   drop the no-live arm         a missing menu.rbf reads as clobbered
#   = becomes != in the compare  a matching pair reads as clobbered, and a
#                                CLOBBERED PAIR READS AS OK, which is the one
#                                failure that matters
#   no-either arm removed        both missing reads as no-live
#
# Run: bash tests/test_deploy_menu_check.sh

set -uo pipefail
cd "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)" || exit 1

# shellcheck disable=SC1091
DEPLOY_MENU_LIB=1 . scripts/deploy-menu.sh

if ! declare -F menu_check_verdict >/dev/null; then
    echo "!! sourcing scripts/deploy-menu.sh did not define menu_check_verdict."
    echo "   Either the DEPLOY_MENU_LIB guard moved above the function or it was"
    echo "   renamed. Nothing below tested anything."
    exit 1
fi

PASS=0
FAIL=0

check() {
    local desc="$1" live="$2" spare="$3" want="$4" got
    got="$(menu_check_verdict "$live" "$spare")"
    if [ "$got" = "$want" ]; then
        PASS=$((PASS + 1)); printf 'ok   %s\n' "$desc"
    else
        FAIL=$((FAIL + 1))
        printf 'FAIL %s\n       live=%q spare=%q\n       expected %s, got %s\n' \
               "$desc" "$live" "$spare" "$want" "$got"
    fi
}

A=d41d8cd98f00b204e9800998ecf8427e
B=5d41402abc4b2a76b9719d911017c592

check "the deployed core is still live"        "$A" "$A" ok
check "update_all took it"                     "$B" "$A" clobbered
# The reverse pair. Which of the two changed is not knowable from md5s alone and
# the answer is the same either way: the live file is not the spare.
check "and the mismatch is symmetric"          "$A" "$B" clobbered
check "no spare to compare against"            "$A" ""   no-spare
check "no menu.rbf on the card"                ""   "$A" no-live
check "nothing deployed yet"                   ""   ""   no-either

# One-character differences, because md5s are compared as strings and a
# truncated or whitespace-mangled capture is the realistic corruption.
check "a one character difference is a mismatch" \
      "5d41402abc4b2a76b9719d911017c592" "5d41402abc4b2a76b9719d911017c593" clobbered

echo
if [ "$FAIL" = 0 ]; then echo "RUN: PASS ($PASS checks)"; exit 0; fi
echo "RUN: FAIL ($FAIL of $((PASS + FAIL)) checks)"
exit 1
