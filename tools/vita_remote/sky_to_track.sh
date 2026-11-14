#!/bin/bash
# sky_to_track.sh [START] : from the main menu (cursor on T. DU MONDE; START=1:
# already on COURSE SIMPLE) to Single Event > Road Rage > USA > Silver Lake
# Normal options; screenshot out/sky/track.png. One vitacompanion call per press.
S="$(cd "$(dirname "$0")" && pwd)"
p() { python -I "$S/vc.py" "screen on; press $1; wait 200ms; release $1" >/dev/null; sleep ${2:-1.2}; }
[ "$1" = 1 ] || p down 1.5
p cross 3; p down; p down 1.5; p cross 3
p right; p down; p right 1; p right 1; p right 1
OUT="$S/../../out/sky" bash "$S/step.sh" "" 1 track
