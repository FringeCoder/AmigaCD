#!/usr/bin/env bash
#
# Parse every C++ source with the HOST compiler. Seconds, not the minutes the
# ARM cross-build takes, and it needs no toolchain download.
#
# Why this exists: until now the only thing that compiled this tree was CI's
# arm-none-linux-gnueabihf build. That means a typo, a broken static_assert or
# a page whose row guard no longer holds is found by a push and a wait, and not
# at all by anyone without the cross toolchain -- which is the dev machine, a
# Windows box with no WSL. The Menu_MiSTer repo has carried a syntax_check.sh
# for the same reason on its RTL; this is the userspace half.
#
# NOT A BUILD. char is signed here and unsigned on ARM, long is 64-bit here and
# 32-bit there, and this does not link. A clean run means the code parses and
# its compile-time assertions hold. It does not mean it works.
#
#   ./syntax_check.sh              # everything
#   ./syntax_check.sh menu.cpp ... # just these
set -uo pipefail
cd "$(dirname "$0")" || exit 1

CXX=${CXX:-g++}

# Mirrors the Makefile's DFLAGS/CFLAGS. VDATE is normally `date +%y%m%d`; it is
# pinned here so the check does not differ from one day to the next.
FLAGS=(
  -fsyntax-only -std=gnu++14 -Wall -Wextra
  -Wno-class-memaccess -Wno-psabi -Wno-strict-aliasing
  -Wno-stringop-overflow -Wno-stringop-truncation -Wno-format-truncation
  -Wno-restrict
  # Host-vs-target artefacts, not defects: narrowing and the printf length
  # modifiers differ with the word size, and this tree casts between pointers
  # and integers in places that are 32-bit by assumption.
  -Wno-narrowing -Wno-format -Wno-format-security -Wno-int-to-pointer-cast
  -I./ -I./lib/libco -I./lib/miniz -I./lib/md5 -I./lib/lzma -I./lib/zstd/lib
  -I./lib/libchdr/include -I./lib/bluetooth -I./lib/serial_server/library
  -D_7ZIP_ST -DPACKAGE_VERSION='"1.3.3"' -DHAVE_LROUND -DHAVE_STDINT_H
  -DHAVE_STDLIB_H -DHAVE_SYS_PARAM_H -DENABLE_64_BIT_WORDS=0
  -D_FILE_OFFSET_BITS=64 -D_LARGEFILE64_SOURCE -DVDATE='"000000"'
)

# Files the host cannot parse, each for a stated reason. Printed on every run:
# an exclusion nobody sees is cover, and the point of this file is that a thing
# which stops being checked says so.
#
# Adding one means writing the reason here. "It does not compile" is not a
# reason -- that is the check working.
EXCLUDE_WHY=(
  "DiskImage.cpp|casts unsigned char* to unsigned int; a 32-bit target assumption"
  "fpga_io.cpp|casts uint32_t* to uint32_t to mask an address; 32-bit by design"
  "shmem.cpp|prints a pointer as uint32_t; same 32-bit assumption"
  "support/cdi/cdg_unpacker_unittest.cpp|guarded by #ifdef __x86_64__, so the HOST is where its main() appears -- on ARM it is empty. Excluded by construction, not by defect"
)

exclude_reason() {
  local f=$1 e
  for e in "${EXCLUDE_WHY[@]}"; do
    [ "${e%%|*}" = "$f" ] && { printf '%s' "${e#*|}"; return 0; }
  done
  return 1
}

if [ $# -gt 0 ]; then
  FILES=("$@")
else
  mapfile -t FILES < <(ls *.cpp support/*/*.cpp lib/serial_server/library/*.cpp 2>/dev/null)
fi

echo "Excluded from the host parse:"
for e in "${EXCLUDE_WHY[@]}"; do echo "  ${e%%|*}  --  ${e#*|}"; done
echo

checked=0
skipped=0
failed=0
for f in "${FILES[@]}"; do
  if reason=$(exclude_reason "$f"); then skipped=$((skipped + 1)); continue; fi
  if ! "$CXX" "${FLAGS[@]}" "$f"; then
    echo "FAIL $f"
    failed=$((failed + 1))
  fi
  checked=$((checked + 1))
done

echo
if [ "$failed" -gt 0 ]; then
  echo "FAIL  $failed of $checked did not parse ($skipped excluded)"
  exit 1
fi
echo "OK    $checked parsed, $skipped excluded"
