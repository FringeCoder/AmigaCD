#!/usr/bin/env bash
#
# Check this repository's half of the snac_state word order against
# docs/snac-state-contract.md.
#
# The Menu core packs 112 bits into snac_state, the framework streams them 16 at
# a time, and snac_psx_poll() below unpacks them. Nothing connects the two but
# the ordering, and getting it wrong makes every button a different button --
# which looks like a broken reader rather than a field order, so it is not
# something the next person debugs quickly.
#
# Neither repository can see the other in CI. So each side is pinned to the table
# in the contract document instead, and a change on either side that is not also
# a change to that table fails the build at the commit that made it. The
# FringeCoder/Menu_MiSTer repository runs the same check against its packing.
#
# If this fails, do NOT delete it as stale. See the contract document.
set -euo pipefail
cd "$(dirname "$0")"

CONTRACT="docs/snac-state-contract.md"
SRC="support/snac/snac_psx.cpp"

# The declared read order, word 0 first. These are the destinations each
# spi_w(0) lands in, and the order is the contract. Kept here rather than parsed
# out of the document: this is the check, and a check that derives its
# expectation from the thing it checks proves nothing.
WANT="status pad[0] axes0_lo axes0_hi pad[1] axes1_lo axes1_hi"

[ -f "$CONTRACT" ] || { echo "error: $CONTRACT is missing" >&2; exit 1; }
[ -f "$SRC" ]      || { echo "error: $SRC is missing" >&2; exit 1; }

# Take the body of snac_psx_poll() up to DisableIO(), keep the lines that assign
# from spi_w(0), and reduce each to its destination.
GOT="$(sed -n '/^void snac_psx_poll/,/DisableIO/p' "$SRC" \
       | grep -E '=[[:space:]]*spi_w\(0\)' \
       | sed -e 's/^[[:space:]]*//' \
             -e 's/^\(uint16_t\|uint32_t\|uint8_t\|int\)[[:space:]]\+//' \
             -e 's/[[:space:]]*=.*//' \
       | tr '\n' ' ' \
       | sed -e 's/ $//')"

if [ -z "$GOT" ]; then
    echo "!! Found no 'x = spi_w(0)' reads in snac_psx_poll() in $SRC." >&2
    echo "   If the unpacking moved or was rewritten, this check has to move" >&2
    echo "   with it -- and so does $CONTRACT, and so does the Menu core side." >&2
    exit 1
fi

if [ "$GOT" != "$WANT" ]; then
    echo "!! snac_state unpack order does not match the contract." >&2
    echo "   want: $WANT" >&2
    echo "   got : $GOT" >&2
    echo >&2
    echo "   Word 0 is the ids, because concatenation in menu.sv puts the first" >&2
    echo "   operand in the high bits and the stream sends the low word first." >&2
    echo "   See $CONTRACT." >&2
    echo >&2
    echo "   Changing this order means changing menu.sv's assign snac_state in" >&2
    echo "   FringeCoder/Menu_MiSTer and the table in $CONTRACT in the same breath." >&2
    exit 1
fi

# The two halves of each 32-bit axes field: low word first, so the reassembly
# has to shift the HIGH one. A swap here reads as sticks stuck at an extreme
# rather than as anything obviously wrong.
if ! grep -qF 'axes[0] = ((uint32_t)axes0_hi << 16) | axes0_lo;' "$SRC" \
|| ! grep -qF 'axes[1] = ((uint32_t)axes1_hi << 16) | axes1_lo;' "$SRC"; then
    echo "!! The axes reassembly is not (hi << 16) | lo -- see $CONTRACT." >&2
    exit 1
fi

# id[0] is the LOW byte of word 0. Getting this backwards swaps the two ports.
if ! grep -qF 'id[0] = status & 0xFF;' "$SRC" \
|| ! grep -qF 'id[1] = status >> 8;' "$SRC"; then
    echo "!! id[0]/id[1] are not the low/high bytes of word 0 -- see $CONTRACT." >&2
    exit 1
fi

echo "OK   snac_psx_poll() unpack order matches $CONTRACT"
echo "     $WANT"
