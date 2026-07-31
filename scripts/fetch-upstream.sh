#!/usr/bin/env bash
# Clone the three upstreams into upstream/ (gitignored).
# Safe to re-run: existing clones are fetched and fast-forwarded.
#
# NOTE: `master` on kblood's forks tracks plain upstream Main_MiSTer and has
# none of the CD32 work. The CD32/CDTV branch is amiga-console in both repos.
set -euo pipefail

cd "$(dirname "$0")/.."
mkdir -p upstream
cd upstream

clone() {
    local dir="$1" url="$2" branch="$3"
    if [ -d "$dir/.git" ]; then
        echo "== updating $dir ($branch)"
        git -C "$dir" fetch --all --prune
        git -C "$dir" checkout "$branch"
        git -C "$dir" pull --ff-only
    else
        echo "== cloning $dir ($branch)"
        git clone --branch "$branch" "$url" "$dir"
    fi
}

clone physical-disc \
      https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc.git master
clone main-cd32-cdtv \
      https://github.com/kblood/Main_MiSTer-cd32-cdtv.git amiga-console
clone core-cd32-cdtv \
      https://github.com/kblood/Minimig-AGA-cd32-cdtv-native-akiko.git amiga-console

echo
echo "done. see docs/upstream-pins.md for the commits the analysis was written against."
