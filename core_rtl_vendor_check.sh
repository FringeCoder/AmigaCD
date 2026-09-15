#!/usr/bin/env bash
#
# Check the RTL this repository copies from the AmigaCD core, or refresh the
# stamp. The list, the hashes and the provenance live in rtl/core-rtl.vendor.
#
# WHY THERE ARE COPIES AT ALL. The save state modules are built in the core --
# that is the only place they are synthesised -- but their testbenches are here,
# and Icarus needs the sources beside them. So this repository carries a copy of
# each, purely to run benches against. rtl/README.md has the full split.
#
# WHY A STAMP. Because it already went wrong. On 2026-09-15 rtl/ss_ctrl.v here
# was two commits behind the core's: the core had registered its DDR3 read
# return and dropped the rom_scan term from the port mux, and neither change had
# been copied over. CI was green throughout. The bench that the core's own
# source comments cite as proof of its wiring was running against a module the
# core no longer builds, and nothing anywhere said so.
#
# Modes:
#
#   ./core_rtl_vendor_check.sh                 Verify every listed file still
#                                              hashes to what the stamp records.
#                                              This is what CI runs: no second
#                                              checkout needed, and it catches a
#                                              copy edited here, which is the
#                                              common case.
#
#   ./core_rtl_vendor_check.sh /path/to/core   Also diff against a real checkout
#                                              of the core, the only way to
#                                              catch the canonical side moving.
#
#   ./core_rtl_vendor_check.sh --stamp /path   Re-vendor: copy every listed file
#                                              across and rewrite the stamp.
#                                              Run the benches afterwards.
#
# CI runs only the first mode deliberately. A job here cannot read the core
# without a token configured for it, and a check that silently degrades to
# "skipped" when a secret is absent is worse than no check at all.
#
# If the first mode fails, do NOT restamp to make it green. Restamping records
# whatever is here as correct, which is the one thing the check exists to doubt.

set -euo pipefail
cd "$(dirname "$0")"

VENDOR="rtl/core-rtl.vendor"
[ -f "$VENDOR" ] || { echo "error: $VENDOR is missing" >&2; exit 1; }

stamp_field() { sed -n "s/^$1[[:space:]]*=[[:space:]]*//p" "$VENDOR" | head -1; }

WANT_REPO=$(stamp_field repo)
WANT_REPO_FORMER=$(stamp_field repo_former)

# The file table: lines after the "files:" marker, "<path> <sha256> <commit>".
stamp_rows() { sed -n '/^files:/,$p' "$VENDOR" | tail -n +2 | grep -vE '^\s*(#|$)'; }

STAMP=0
if [ "${1:-}" = "--stamp" ]; then STAMP=1; shift; fi
CANON="${1:-}"

# Confirm a checkout really is the repo the stamp names, matching the TAIL of
# the origin URL rather than as a substring -- FringeCoder/AmigaCD is a prefix of
# FringeCoder/AmigaCD_MiSTer, so a substring test would accept this very
# repository as the core.
#
# repo_former is honoured because the core's GitHub rename and this stamp did
# not land on the same day. Drop that line from the stamp once every clone has
# been re-pointed; until then, rejecting a correct checkout would be the more
# annoying failure.
check_origin() {
	local dir="$1" origin slug
	origin=$(git -C "$dir" remote get-url origin 2>/dev/null || echo "")
	if [ -z "$origin" ]; then
		echo "warning: $dir has no origin remote; cannot confirm it is $WANT_REPO" >&2
		return 0
	fi
	slug=${origin%.git}; slug=${slug%/}
	slug=$(printf '%s' "$slug" | sed -e 's#^.*[:/]\([^:/][^:/]*/[^/][^/]*\)$#\1#')
	if [ "${slug,,}" = "${WANT_REPO,,}" ]; then return 0; fi
	if [ -n "$WANT_REPO_FORMER" ] && [ "${slug,,}" = "${WANT_REPO_FORMER,,}" ]; then
		echo "note: $dir is still named $WANT_REPO_FORMER (the core's pre-rename name)" >&2
		return 0
	fi
	echo "error: $dir is not a $WANT_REPO checkout (origin: $origin)." >&2
	echo "       Point this at the repo that OWNS these modules, not at another copy." >&2
	exit 1
}

if [ "$STAMP" = 1 ]; then
	[ -n "$CANON" ] || { echo "error: --stamp needs a path to a $WANT_REPO checkout" >&2; exit 1; }
	check_origin "$CANON"

	tmp=$(mktemp)
	while read -r path _sha _commit; do
		[ -f "$CANON/$path" ] || { echo "error: $CANON/$path not found" >&2; rm -f "$tmp"; exit 1; }
		cp "$CANON/$path" "$path"
		sha=$(sha256sum "$path" | cut -d' ' -f1)
		# The commit that last touched THE FILE, not the checkout's HEAD: HEAD is
		# usually a commit that did not touch this module at all, and the question
		# a stamp answers is which change to diff against.
		commit=$(git -C "$CANON" log -1 --format=%H -- "$path" 2>/dev/null || echo "")
		[ -n "$commit" ] || commit="unknown"
		printf '%-28s %s %s\n' "$path" "$sha" "$commit" >> "$tmp"
	done < <(stamp_rows)

	sed -i "s/^vendored .*/vendored = $(date -u +%Y-%m-%d)/" "$VENDOR"
	sed -i '/^files:/,$d' "$VENDOR"
	{ echo "files:"; cat "$tmp"; } >> "$VENDOR"
	rm -f "$tmp"
	echo "re-vendored from $CANON"
	sed -n '/^files:/,$p' "$VENDOR" | tail -n +2
	echo
	echo "Now run the rtl-sim benches before committing -- the stamp proves the"
	echo "bytes match, not that they still pass."
	exit 0
fi

rc=0
n=0
while read -r path want _commit; do
	n=$((n + 1))
	if [ ! -f "$path" ]; then
		echo "FAIL $path is listed in $VENDOR but missing here"
		rc=1
		continue
	fi
	got=$(sha256sum "$path" | cut -d' ' -f1)
	if [ "$got" != "$want" ]; then
		echo "FAIL $path does not match the stamp"
		echo "       recorded $want"
		echo "       actual   $got"
		rc=1
	fi
done < <(stamp_rows)

if [ "$rc" != 0 ]; then
	echo
	echo "Either a copy was edited here -- don't; these are not canonical, fix them"
	echo "in $WANT_REPO and re-vendor -- or a re-vendor updated files and left"
	echo "$VENDOR behind. Refresh with:"
	echo "  ./core_rtl_vendor_check.sh --stamp /path/to/core-checkout"
	exit 1
fi
echo "OK   $n vendored files match $VENDOR"

if [ -n "$CANON" ]; then
	check_origin "$CANON"
	drift=0
	while read -r path _sha _commit; do
		[ -f "$CANON/$path" ] || { echo "error: $CANON/$path not found" >&2; exit 1; }
		if ! diff -u "$CANON/$path" "$path"; then drift=1; fi
	done < <(stamp_rows)
	if [ "$drift" != 0 ]; then
		echo
		echo "FAIL the canonical copies have moved. Re-vendor:"
		echo "  ./core_rtl_vendor_check.sh --stamp $CANON"
		exit 1
	fi
	echo "OK   identical to the canonical copies in $CANON"
fi
