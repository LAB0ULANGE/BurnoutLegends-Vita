#!/bin/bash
# sky_boot.sh [SWITCH...] : closes the game, leaves only the given test switches
# (dynres.txt, no_tight.txt, no_valid.txt, no_state_cache.txt), relaunches and
# loads the profile (boot_to_menu.sh); then screenshot out/sky/boot.png.
S="$(cd "$(dirname "$0")" && pwd)"
F=ftp://192.168.1.21:1337
python -I "$S/vc.py" "destroy" >/dev/null; sleep 3
for f in dynres.txt no_tight.txt no_valid.txt no_state_cache.txt; do curl -s -Q "DELE ux0:/data/burnout/$f" $F/ -o /dev/null 2>&1; done
echo x > "$S/../../out/switch.txt"
for f in "$@"; do curl -s -T "$S/../../out/switch.txt" $F/ux0:/data/burnout/$f; done
curl -s $F/ux0:/data/ | awk '{print $NF}' | tr -d '\r' | grep "burnout-0[^ ]*\.log$" | while read n; do curl -s -Q "DELE ux0:/data/$n" $F/ -o /dev/null; done
python -I "$S/vc.py" "launch BRNG00001" >/dev/null; sleep 8
. "$S/boot_to_menu.sh"
OUT="$S/../../out/sky" bash "$S/step.sh" "" 1 boot
log | grep "test switch"
