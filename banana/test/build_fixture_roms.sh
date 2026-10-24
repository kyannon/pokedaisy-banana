#!/bin/sh
# The 3DS port's fixture test ROMs (3ds/test/fixture_rom.c: a few lines of GBA
# homebrew that copy one of the repo's RAM fixtures into the GBA and idle),
# built with clang and lld instead of devkitARM, into banana/build/fixtures/.
# No game code or data from any ROM: only the save state the fixtures hold.
#   build_fixture_roms.sh [out dir]
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
OUT=${1:-$REPO/banana/build/fixtures}
FIXTURES=$REPO/app/src/test/resources/fixtures
CLANG=${CLANG:-clang}
LLD=${LLD:-ld.lld}
OBJCOPY=${OBJCOPY:-llvm-objcopy}
mkdir -p "$OUT"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

ARM="--target=armv4t-none-eabi -mcpu=arm7tdmi -ffreestanding -fno-builtin -nostdlib"
"$CLANG" $ARM -marm -c "$HERE/crt0.s" -o "$T/crt0.o"
"$CLANG" $ARM -mthumb -O2 -c "$REPO/3ds/test/fixture_rom.c" -o "$T/fixture_rom.o"

# fixture key, game code, revision (as the 3DS build's)
for spec in "firered_vanilla BPRE 1" "emerald_vanilla BPEE 0" "emerald_de_battle BPEE 0" ${EXTRA_FIXTURES:-}; do
    set -- $spec
    src=$FIXTURES/$1
    [ -f "$src/ewram.bin" ] || src=$REPO/3ds/build/fixtures/$1
    [ -f "$src/ewram.bin" ] || { echo "no fixture $1, skipped"; continue; }
    "$CLANG" $ARM -c -x assembler-with-cpp \
        -DFIXTURE_EWRAM="\"$src/ewram.bin\"" -DFIXTURE_IWRAM="\"$src/iwram.bin\"" \
        "$REPO/3ds/test/fixture_data.s" -o "$T/data-$1.o"
    "$LLD" -T "$HERE/gba.ld" "$T/crt0.o" "$T/fixture_rom.o" "$T/data-$1.o" -o "$T/$1.elf"
    "$OBJCOPY" -O binary "$T/$1.elf" "$OUT/fixture-$1.gba"
    python3 "$HERE/mkheader.py" "$OUT/fixture-$1.gba" "$2" "$3"
    echo "$OUT/fixture-$1.gba ($2 rev $3)"
done
