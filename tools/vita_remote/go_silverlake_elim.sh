#!/bin/bash
# From a fresh launch to World Tour > Compact > Silver Lake > Eliminateur
# (2026-11-14, the 60 fps test race: the heaviest GPU load seen), then holds
# accelerate (X in the user's save while the tests run) DRIVE_S seconds.
S="$(cd "$(dirname "$0")" && pwd)"
. "$S/boot_to_menu.sh"
p cross 5        # World Tour
p cross 5        # Compact
p cross 4        # the region under the cursor (Airport after a launch): its events
p triangle 3     # back to the map
p right 3        # Silver Lake
p cross 4        # its events
p down 2         # Eliminateur
p cross 12       # car select
p cross 2        # the car
until_log "race data first opened" 25 || { echo "the race did not load"; exit 1; }
sleep 12
python -I "$S/vc.py" "screen on; press cross; wait ${DRIVE_S:-90}s; release cross" >/dev/null
