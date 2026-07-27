#!/usr/bin/env bash
# Clone the three upstreams into upstream/ (gitignored).
# Re-run with --update to fetch instead of clone.
set -euo pipefail

cd "$(dirname "$0")/.."
mkdir -p upstream
cd upstream

clone() {
    local dir="$1" url="$2" branch="$3"
    if [ -d "$dir/.git" ]; then
        echo "== updating $dir"
        git -C "$dir" fetch --all --prune
        git -C "$dir" checkout "$branch"
        git -C "$dir" pull --ff-only
    else
        echo "== cloning $dir"
        git clone --branch "$branch" "$url" "$dir"
    fi
}

clone physical-disc \
      https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc.git master
clone main-cd32-cdtv \
      https://github.com/kblood/Main_MiSTer-cd32-cdtv.git master
clone core-cd32-cdtv \
      https://github.com/kblood/Minimig-AGA-cd32-cdtv-native-akiko.git cd32-native-mvp

echo
echo "done. see docs/upstream-pins.md for the commits the analysis was written against."
