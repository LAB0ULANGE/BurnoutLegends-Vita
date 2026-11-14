#!/bin/bash
# sky_test.sh NAME [SWITCH...] : the cut sky (2026-11-14). Closes the game, leaves
# only the given test switches in ux0:data/burnout (dynres.txt, no_tight.txt,
# no_valid.txt, no_state_cache.txt), relaunches, goes to Silver Lake (Single
# Event > Road Rage, go_silverlake.sh's path), drives (CROSS and R2 held: the
# accelerator is on either in the user's saves) and takes 6 screenshots
# into out/sky/NAME_n.png.
S="$(cd "$(dirname "$0")" && pwd)"
F=ftp://192.168.1.21:1337
NAME=$1; shift
mkdir -p "$S/../../out/sky"
python -I "$S/vc.py" "destroy" >/dev/null; sleep 3
for f in dynres.txt no_tight.txt no_valid.txt no_state_cache.txt; do curl -s -Q "DELE ux0:/data/burnout/$f" $F/ -o /dev/null 2>&1; done
echo x > "$S/../../out/switch.txt"
for f in "$@"; do curl -s -T "$S/../../out/switch.txt" $F/ux0:/data/burnout/$f; done
curl -s $F/ux0:/data/ | awk '{print $NF}' | tr -d '\r' | grep "burnout-0[^ ]*\.log$" | while read n; do curl -s -Q "DELE ux0:/data/$n" $F/ -o /dev/null; done
python -I "$S/vc.py" "launch BRNG00001" >/dev/null; sleep 8
. "$S/boot_to_menu.sh"
p down 2; p cross 4
p down 2; p down 2; p cross 4
p right 2; p down 2; p right 2; p right 2; p right 2
p cross 4; p cross 2
until_log "race data first opened" 25 || { echo "the race did not load"; exit 1; }
sleep 10
python -I "$S/vc.py" "screen on; press cross; press r2" >/dev/null
for i in 1 2 3 4 5 6; do OUT="$S/../../out/sky" bash "$S/step.sh" "" 3 "${NAME}_$i"; done
python -I "$S/vc.py" "release cross; release r2" >/dev/null
log | grep -E "test switch|dynamic resolution: now|state cache" | tail -5
