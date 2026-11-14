#!/bin/bash
# From a fresh launch (renderer ready) to World Tour > Compact > Interstate Loop > Course,
# Compact Type 1. Synchronised on the game's log; exits 1 if the race does not load.
S="$(cd "$(dirname "$0")" && pwd)"
. "$S/boot_to_menu.sh"
p down 2; p cross 4                      # Single Event
p down 2; p down 2; p cross 4
p right 2; p down 2; p right 2; p right 2; p right 2
p cross 4; p cross 2
until_log "race data first opened" 25 || { echo "the race did not load"; exit 1; }
sleep 12
python -I "$S/vc.py" "screen on; press cross; wait ${DRIVE_S:-90}s; release cross" >/dev/null
