#!/bin/bash
# From a fresh launch (renderer ready) to World Tour > Compact > Interstate Loop > Course,
# Compact Type 1. Synchronised on the game's log; exits 1 if the race does not load.
S="$(cd "$(dirname "$0")" && pwd)"
. "$S/boot_to_menu.sh"
p cross 5        # World Tour
p cross 5        # Compact
# (no arrows: the game keeps the last region, event and car, so every run replays the same one)
p cross 4        # region
p cross 12       # Course
p cross 2        # car
until_log "race data first opened" 20 || { echo "the race did not load"; exit 1; }
sleep 12
# Accelerate (the user's save maps it to the right trigger: "r2" here).
python -I "$S/vc.py" "screen on; press cross; wait ${DRIVE_S:-90}s; release cross" >/dev/null
