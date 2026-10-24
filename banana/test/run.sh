#!/bin/sh
# The BananaOS port, headless, on the fixture ROMs (build_fixture_roms.sh):
# scripted taps and buttons, both panels written as PNGs into banana/build/shots,
# and the checks that matter for a player's data - a save from another
# firmware copied in, never moved; its bytes kept exactly; a backup before the
# game starts; a state saved and loaded with F + R / F + L; SIGTERM writes and
# leaves cleanly; the screen mode kept across runs; --probe for the fallback.
#   run.sh [pokedaisy binary]
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
B=${1:-$HERE/../build/pokedaisy}
FX=$HERE/../build/fixtures
OUT=$HERE/../build/shots
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
[ -x "$B" ] || { echo "no $B: make -C banana first"; exit 1; }
[ -f "$FX/fixture-firered_vanilla.gba" ] || sh "$HERE/build_fixture_roms.sh" "$FX"
rm -rf "$OUT"; mkdir -p "$OUT"
fail=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi; }
card() {
    export POKEDAISY_DATA=$W/$1/pokedaisy POKEDAISY_SAVES=$W/$1/saves POKEDAISY_STATES=$W/$1/states/gba \
        POKEDAISY_BACKUPS=$W/$1/pokedaisy/backups
}
run() { "$B" --headless "$OUT" "$@" 2>>"$W/log"; }

# Tab chips' centres (the companion's 320x240): y 13, PARTY 27, BAG 78, MAP 129, GUIDE 180,
# DEX / BATTLE 231, fast-forward 273, SETTINGS 303. SETTINGS' SCREEN row is at y 150; its
# pick-list's first choice (PIXEL) at y 83.
card a
run --script "wait:30 shot:rgds-party tap:78,13 wait:5 shot:rgds-bag tap:129,13 wait:5 shot:rgds-map \
    tap:180,13 wait:5 shot:rgds-guide-notice tap:245,227 wait:5 shot:rgds-guide tap:231,13 wait:5 shot:rgds-dex \
    tap:303,13 wait:5 shot:rgds-settings" "$FX/fixture-firered_vanilla.gba"
check "FireRed: seven shots" '[ $(ls "$OUT"/rgds-*.png | wc -l) = 7 ]'
check "the GUIDE notice, once accepted, is kept" 'grep -q "^guide=[1-9]" "$W/a/pokedaisy/settings.ini"'
check "FireRed: a save made for it" '[ -f "$W/a/saves/fixture-firered_vanilla.srm" ]'

card b
mkdir -p "$W/roms"
cp "$FX/fixture-emerald_vanilla.gba" "$W/roms/Emerald Test.gba"
head -c 131072 /dev/urandom > "$W/roms/Emerald Test.sav"
orig=$(sha1sum < "$W/roms/Emerald Test.sav")
run --size 1024x768 --script "wait:20 shot:plus-party hold:F+R:3 wait:5 tap:303,13 wait:5 shot:plus-settings \
    hold:F+L:3 wait:5 hold:F+L:3 wait:5 hold:F+L:3 wait:5 term wait:30" "$W/roms/Emerald Test.gba"
check "another firmware's save left where it was" '[ "$(sha1sum < "$W/roms/Emerald Test.sav")" = "$orig" ]'
check "...copied in, byte for byte" '[ "$(sha1sum < "$W/b/saves/Emerald Test.srm")" = "$orig" ]'
check "...at its size" '[ $(wc -c < "$W/b/saves/Emerald Test.srm") = 131072 ]'
check "a backup before the game" '[ "$(cat "$W"/b/pokedaisy/backups/"Emerald Test"-*.sav | sha1sum)" = "$orig" ]'
check "...and only one: repeated state loads add no copies of the same save" '[ $(ls "$W"/b/pokedaisy/backups | wc -l) = 1 ]'
check "F + R saved a state" '[ -s "$W/b/states/gba/Emerald Test.pd1" ]'
check "...loaded with F + L" 'grep -q "state loaded" "$W/log"'
check "SIGTERM wrote the save and left" 'grep -q "signal 15: writing the save" "$W/log"'
check "no half-written files left" '[ -z "$(find "$W/b" -name "*.part")" ]'

card c
run --script "wait:20 shot:battle press:F wait:3 tap:60,150 wait:3 shot:pick tap:60,83 wait:5 shot:pixel" \
    "$FX/fixture-emerald_de_battle.gba"
check "SCREEN picked and kept" 'grep -q "^screen=0" "$W/c/pokedaisy/settings.ini"'
run --script "wait:5 shot:pixel-next-run" "$FX/fixture-emerald_de_battle.gba"

cp "$FX/fixture-firered_vanilla.gba" "$W/other.gba"
python3 "$HERE/mkheader.py" "$W/other.gba" AXYZ 0
check "--probe: a game it knows" '"$B" --probe "$FX/fixture-firered_vanilla.gba" >/dev/null'
check "--probe: another GBA game (2)" '"$B" --probe "$W/other.gba" >/dev/null; [ $? = 2 ]'

[ $fail = 0 ] && echo "all passed; shots in $OUT" || { echo "--- log"; cat "$W/log"; exit 1; }
