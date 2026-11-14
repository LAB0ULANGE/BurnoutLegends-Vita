#!/bin/bash
# Sourced by go_race.sh / go_silverlake.sh: from a fresh launch (renderer
# ready) to the main menu with the profile loaded, synchronised on the
# game's log (2026-11-14): the title movie, the save dialog opening, the
# profile loaded. Defines p() and log().
F=ftp://192.168.1.21:1337
p() { python -I "$S/vc.py" "screen on; press $1; wait 300ms; release $1" >/dev/null; sleep ${2:-2}; }
log() { local n=$(curl -s $F/ux0:/data/ | awk '{print $NF}' | tr -d '\r' | grep "burnout-0[^ ]*\.log$" | sort | tail -1); curl -s $F/ux0:/data/$n; }
until_log() { # until_log PATTERN TRIES [BUTTON]
  for i in $(seq 1 $2); do log | grep -q "$1" && return 0; [ -n "$3" ] && p $3 3 || sleep 3; done; return 1; }
python -I "$S/vc.py" "screen on" >/dev/null   # buttons are ignored while the screen is off
until_log "startB.pmf" 30 cross || { echo "no title screen"; exit 1; }
# Title: START (ignored for its first seconds), then X on "load a profile".
ok=0; for i in $(seq 1 15); do log | grep -q "native dialog (load" && { ok=1; break; }; p start 3; p cross 3; done
[ $ok = 1 ] || log | grep -q "native dialog (load" || { echo "the save dialog did not open"; exit 1; }
until_log "native dialog result" 15 cross || { echo "the profile did not load"; exit 1; }
sleep 6
