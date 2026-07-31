#!/usr/bin/env bash
# Fetch a CI build and install it on the MiSTer.
#
#   ./scripts/deploy-from-ci.sh                 latest successful build
#   ./scripts/deploy-from-ci.sh 30616420260     a specific run
#
# This is the counterpart to wsl-build.sh's `deploy` mode. That one builds
# locally; this one pulls the artifact from GitHub Actions, which is what you
# want on a machine with no cross-toolchain -- see docs/deployment.md for why
# there isn't one.
#
# Host comes from ./host (gitignored) or $MISTER_HOST, the same convention
# wsl-build.sh and build.sh use.
#
# Deploys the UNSTRIPPED MiSTer.elf by default. Identical code to the stripped
# MiSTer, ~200 KB larger, and the difference is that a crash under gdb gives a
# named backtrace instead of `?? ()` -- which is precisely what turned the
# "OSD locks up" report into a one-line fix. Set AMIGACD_STRIPPED=1 when
# bring-up is over.
set -euo pipefail

REPO_SLUG="${AMIGACD_REPO:-FringeCoder/AmigaCD}"
BINARY_NAME="${AMIGACD_BINARY_NAME:-MiSTer_AmigaCD}"

if [ -n "${AMIGACD_STRIPPED:-}" ]; then
    ARTIFACT_FILE="MiSTer"
else
    ARTIFACT_FILE="MiSTer.elf"
fi

cd "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

command -v gh >/dev/null 2>&1 || { echo "!! gh CLI not found." >&2; exit 1; }

HOST="${MISTER_HOST:-}"
if [ -z "$HOST" ] && [ -f host ]; then HOST="$(tr -d '[:space:]' < host)"; fi
if [ -z "$HOST" ]; then
    echo "!! No MiSTer address. Put its IP in ./host or set MISTER_HOST." >&2
    exit 1
fi

RUN_ID="${1:-}"
if [ -z "$RUN_ID" ]; then
    RUN_ID="$(gh run list --repo "$REPO_SLUG" --workflow build --status success \
              --limit 1 --json databaseId --jq '.[0].databaseId')"
    if [ -z "$RUN_ID" ]; then
        echo "!! No successful build found on $REPO_SLUG." >&2
        exit 1
    fi
fi

# mktemp rather than a fixed path under /tmp: `gh run download` refuses to
# write into a directory that already exists, so a leftover from an earlier
# deploy makes the next one fail. Trap cleans up on any exit path.
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "== run $RUN_ID"
gh run download "$RUN_ID" --repo "$REPO_SLUG" --name MiSTer_AmigaCD --dir "$WORK"
ls -l "$WORK/$ARTIFACT_FILE"

echo "== deploying $ARTIFACT_FILE to $HOST as /media/fat/$BINARY_NAME"
# Copy under a temp name and rename: rename over a running binary is atomic and
# avoids ETXTBSY if the core happens to be loaded right now.
scp "$WORK/$ARTIFACT_FILE" "root@$HOST:/media/fat/$BINARY_NAME.new"
ssh "root@$HOST" \
    "mv -f /media/fat/$BINARY_NAME.new /media/fat/$BINARY_NAME && \
     chmod +x /media/fat/$BINARY_NAME && sync && \
     echo -n 'deployed md5: ' && md5sum /media/fat/$BINARY_NAME"

echo "== done. Reload the core to pick it up."
