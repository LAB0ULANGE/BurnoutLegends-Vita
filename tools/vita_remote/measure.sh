#!/bin/bash
# measure.sh NAME [DEPLOY=1]: (deploy,) drive the race, a screenshot mid-race, keep the log as out/remote/NAME.log
S="$(cd "$(dirname "$0")" && pwd)"; O="$S/../../out/remote"; mkdir -p "$O"
F=ftp://192.168.1.21:1337
if [ "${SKIP_LAUNCH:-0}" != 1 ]; then
  python -I "$S/vc.py" "screen on"; python -I "$S/vc.py" destroy; sleep 5
  # Forget the previous session's log, so the boot check reads this one.
  for n in $(curl -s $F/ux0:/data/ | awk '{print $NF}' | grep "burnout-0[^ ]*.log$"); do curl -s -Q "DELE ux0:/data/$n" $F/ -o /dev/null; done
  [ "${2:-1}" = 1 ] && { "$S/deploy.sh" || exit 1; } || python -I "$S/vc.py" "launch BRNG00001"
  # The game's log starts fresh at each launch: wait for it before pressing anything.
  ok=0; for i in $(seq 1 30); do sleep 2; [ $i = 12 ] && python -I "$S/vc.py" "launch BRNG00001" >/dev/null; n=$(curl -s $F/ux0:/data/ | awk '{print $NF}' | grep "burnout-0[^ ]*.log$" | sort | tail -1); curl -s $F/ux0:/data/$n | grep -q "renderer ready" && { ok=1; break; }; done
  [ $ok = 1 ] || { echo "the game did not start"; exit 1; }
fi
DRIVE_S=1 "$S/go_race.sh" || exit 1
python -I "$S/vc.py" "press r2; wait 30s" >/dev/null
OUT="$O" "$S/step.sh" "" 1 "$1"
python -I "$S/vc.py" "wait 40s; release r2" >/dev/null
log=$(curl -s ftp://192.168.1.21:1337/ux0:/data/ | grep -o "burnout-0[^ ]*perf[^ ]*\.log$" | sort | tail -1)
curl -s ftp://192.168.1.21:1337/ux0:/data/$log > "$O/$1.log"; echo "log $log -> $O/$1.log"
