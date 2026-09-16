#!/usr/bin/env bash
#
# What scripts/deploy-core.sh will and will not delete.
#
# WHY THIS EXISTS. prune_list() is the only part of this project that removes
# files from the user's SD card, over ssh, without asking. Everything else there
# copies or reports. A rule that is one character wrong -- `-ge` for `-gt`, a
# glob that matches one digit too few -- deletes a working core, and the first
# sign of it is a machine that will not boot the thing you were about to test.
# Reading the function carefully is not a test of it; this is.
#
# It needs no MiSTer. deploy-core.sh returns immediately when sourced with
# DEPLOY_CORE_LIB=1, before it parses an argument or opens a connection, so what
# runs below is the shipped function and nothing else.
#
# FALSIFIED, which is the only reason to believe it (AGENTS.md: "a bench that
# has never failed has not been shown to test anything"). Five mutations were
# applied to the shipped prune_list(), one at a time, and each was caught by the
# check named for it -- listed with the total so a future mutation that trips
# fewer can be recognised as having found a gap:
#
#   -gt -> -ge              keep 2 of 4                              5/18 fail
#   drop the keepname test  re-deploying an older core ...           8/18 fail
#   8 digits -> 7           a 7-digit name is not ours               9/18 fail
#   drop the case filter    only the dated form counts towards ...   8/18 fail
#   ${f##*/} -> $f          a full path is returned whole            8/18 fail
#
# The harness itself failed first, before any of that, and the way it failed is
# the reason to distrust a green test that has never been red: `check` read the
# heredoc with two command substitutions, the first swallowed the lot, and every
# expectation came out empty. It reported 10 of 18 ok -- exactly the 10 that
# expect no output. See the note on `all="$(cat)"` below.
#
# Run: bash tests/test_deploy_core_prune.sh

set -uo pipefail
cd "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)" || exit 1

# shellcheck disable=SC1091
DEPLOY_CORE_LIB=1 . scripts/deploy-core.sh

if ! declare -F prune_list >/dev/null; then
    echo "!! sourcing scripts/deploy-core.sh did not define prune_list."
    echo "   Either the DEPLOY_CORE_LIB guard moved above the function, or the"
    echo "   function was renamed. Nothing below tested anything."
    exit 1
fi

PASS=0
FAIL=0

# check <description> <keep> <deployed-basename> <<< listing, expected on stdin
# after a line reading '--'.
check() {
    local desc="$1" keep="$2" keepname="$3"
    local all input expected got

    # Read stdin ONCE. Two command substitutions reading the same heredoc do not
    # split it between them -- the first consumes the lot and the second gets
    # nothing, which silently makes every expectation empty. That version of this
    # harness "passed" precisely the cases expecting no output.
    all="$(cat)"
    input="$(printf '%s\n' "$all" | sed -n '1,/^--$/p' | sed '$d')"
    expected="$(printf '%s\n' "$all" | sed -n '/^--$/,$p' | sed '1d')"
    got="$(printf '%s\n' "$input" | prune_list "$keep" "$keepname")"

    if [ "$got" = "$expected" ]; then
        PASS=$((PASS + 1))
        printf 'ok   %s\n' "$desc"
    else
        FAIL=$((FAIL + 1))
        printf 'FAIL %s\n' "$desc"
        printf '       keep=%s deployed=%s\n' "$keep" "$keepname"
        printf '       expected: %s\n' "$(printf '%s' "$expected" | tr '\n' ' ')"
        printf '       got:      %s\n' "$(printf '%s' "$got" | tr '\n' ' ')"
    fi
}

D=/media/fat/_Test

# --- the ordinary deploy ---------------------------------------------------
# Newest first, as `ls | sort -r` hands them over. Default --keep 2 leaves the
# new core and the one before it, which is what docs/deployment.md calls the way
# back.
check "keep 2 of 4" 2 AmigaCD_20260916.rbf <<EOF
$D/AmigaCD_20260916.rbf
$D/AmigaCD_20260910.rbf
$D/AmigaCD_20260901.rbf
$D/AmigaCD_20260815.rbf
--
$D/AmigaCD_20260901.rbf
$D/AmigaCD_20260815.rbf
EOF

check "keep 2 of exactly 2 prunes nothing" 2 AmigaCD_20260916.rbf <<EOF
$D/AmigaCD_20260916.rbf
$D/AmigaCD_20260910.rbf
--
EOF

check "keep 2 of 1 prunes nothing" 2 AmigaCD_20260916.rbf <<EOF
$D/AmigaCD_20260916.rbf
--
EOF

check "first deploy to an empty directory" 2 AmigaCD_20260916.rbf <<EOF

--
EOF

# --- the just-deployed core is never a candidate ---------------------------
# Re-deploying an older build to reproduce something: it is now the oldest name
# on the card, so the count selects it. Deleting what you just spent a minute
# copying is the one outcome with no reading under which it is what you wanted.
check "re-deploying an older core does not delete it" 1 AmigaCD_20260815.rbf <<EOF
$D/AmigaCD_20260916.rbf
$D/AmigaCD_20260910.rbf
$D/AmigaCD_20260815.rbf
--
$D/AmigaCD_20260910.rbf
EOF

check "keep 0 still spares the deployed core" 0 AmigaCD_20260910.rbf <<EOF
$D/AmigaCD_20260916.rbf
$D/AmigaCD_20260910.rbf
$D/AmigaCD_20260901.rbf
--
$D/AmigaCD_20260916.rbf
$D/AmigaCD_20260901.rbf
EOF

# Nothing is sacred if it is not on the card: the caller passes a basename, and
# a basename that does not appear simply spares nothing.
check "keep 0 with the deployed core absent" 0 AmigaCD_20260916.rbf <<EOF
$D/AmigaCD_20260910.rbf
$D/AmigaCD_20260901.rbf
--
$D/AmigaCD_20260910.rbf
$D/AmigaCD_20260901.rbf
EOF

# --- names that are not ours -----------------------------------------------
# The header of deploy-core.sh promises this explicitly, and it is the promise
# most worth holding it to: a core someone put there by hand, or one belonging
# to another project, is not this script's to remove at any --keep.
check "an undated core is never pruned" 0 AmigaCD_20260916.rbf <<EOF
$D/AmigaCD_20260916.rbf
$D/AmigaCD.rbf
$D/amigacd.rbf
--
EOF

check "hand-named and foreign cores are never pruned" 0 AmigaCD_20260916.rbf <<EOF
$D/AmigaCD_20260916.rbf
$D/AmigaCD_known_good.rbf
$D/AmigaCD_20260910_hybris.rbf
$D/Minimig_20260910.rbf
$D/MegaCD_20260910.rbf
--
EOF

check "a 7-digit name is not ours" 0 AmigaCD_20260916.rbf <<EOF
$D/AmigaCD_20260916.rbf
$D/AmigaCD_2026091.rbf
--
EOF

check "a 9-digit name is not ours" 0 AmigaCD_20260916.rbf <<EOF
$D/AmigaCD_20260916.rbf
$D/AmigaCD_202609160.rbf
--
EOF

# The transit file this script itself writes, and the backups people make. If
# the copy failed halfway, .AmigaCD_20260916.rbf.part is sitting there; removing
# it silently on the next run would destroy the evidence of what went wrong.
check "the transit file and .bak copies are not ours" 0 AmigaCD_20260916.rbf <<EOF
$D/AmigaCD_20260916.rbf
$D/.AmigaCD_20260910.rbf.part
$D/AmigaCD_20260910.rbf.bak
$D/AmigaCD_20260910.rbf.old
--
EOF

# Case matters, and should: the browser is case-insensitive about the extension
# but this is a delete, and "close enough" is not a standard to delete on.
check "a lowercased name is not ours" 0 AmigaCD_20260916.rbf <<EOF
$D/AmigaCD_20260916.rbf
$D/amigacd_20260910.rbf
--
EOF

# --- interaction between the two rules -------------------------------------
# The count is over OUR cores only. A directory with one dated core and five
# hand-named ones prunes nothing at --keep 2: five of those six do not count and
# cannot be removed, so there is nothing for the sixth to be sixth of.
check "only the dated form counts towards --keep" 2 AmigaCD_20260916.rbf <<EOF
$D/AmigaCD_20260916.rbf
$D/AmigaCD_known_good.rbf
$D/AmigaCD_hybris_test.rbf
$D/AmigaCD.rbf
$D/Minimig_20260910.rbf
$D/AmigaCD_20260910.rbf
--
EOF

# Same listing plus one more dated core: now there are three of ours, and the
# third goes -- and only the third.
check "junk interleaved does not shift the count" 2 AmigaCD_20260916.rbf <<EOF
$D/AmigaCD_20260916.rbf
$D/AmigaCD_known_good.rbf
$D/AmigaCD_20260910.rbf
$D/AmigaCD.rbf
$D/AmigaCD_20260901.rbf
$D/Minimig_20260910.rbf
--
$D/AmigaCD_20260901.rbf
EOF

# --- what comes back -------------------------------------------------------
# The caller feeds these straight to `rm -f` on the card, so a basename here
# would delete relative to whatever directory the ssh lands in -- which is not
# $DEST. It must be the whole path.
check "a full path is returned whole" 1 AmigaCD_20260916.rbf <<EOF
/media/fat/_Computer/AmigaCD_20260916.rbf
/media/fat/_Computer/AmigaCD_20260910.rbf
--
/media/fat/_Computer/AmigaCD_20260910.rbf
EOF

# Cores live wherever the user put them, and _Test is only this project's
# convention. A space in the path must survive the read.
check "a path with spaces survives" 1 AmigaCD_20260916.rbf <<EOF
/media/fat/_My Cores/AmigaCD_20260916.rbf
/media/fat/_My Cores/AmigaCD_20260910.rbf
--
/media/fat/_My Cores/AmigaCD_20260910.rbf
EOF

# Blank lines are what an empty `ls` becomes on the way through `printf '%s\n'`.
check "blank lines are ignored" 1 AmigaCD_20260916.rbf <<EOF

$D/AmigaCD_20260916.rbf

$D/AmigaCD_20260910.rbf

--
$D/AmigaCD_20260910.rbf
EOF

echo
if [ "$FAIL" = 0 ]; then
    echo "RUN: PASS ($PASS checks)"
    exit 0
fi
echo "RUN: FAIL ($FAIL of $((PASS + FAIL)) checks)"
exit 1
