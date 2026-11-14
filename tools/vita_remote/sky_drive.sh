#!/bin/bash
# sky_drive.sh NAME : from the track's options, launches the race, holds CROSS
# and R2, 6 screenshots out/sky/NAME_n.png and a sheet NAME_sheet.png.
S="$(cd "$(dirname "$0")" && pwd)"
F=ftp://192.168.1.21:1337

log() { local n=$(curl -s $F/ux0:/data/ | awk '{print $NF}' | tr -d '\r' | grep "burnout-0[^ ]*\.log$" | sort | tail -1); curl -s $F/ux0:/data/$n; }
python -I "$S/vc.py" "screen on; press cross; wait 200ms; release cross; wait 4000ms; press cross; wait 200ms; release cross" >/dev/null
for i in $(seq 1 25); do log | grep -q "race data first opened" && break; sleep 3; done
log | grep "\[track\]" | tail -1
sleep 9
python -I "$S/vc.py" "screen on; press cross; press r2" >/dev/null
for i in 1 2 3 4 5 6; do OUT="$S/../../out/sky" bash "$S/step.sh" "" 2 "$1_$i"; done
python -I "$S/vc.py" "release cross; release r2" >/dev/null
cd "$S/../../out/sky"; FF=$(ls -d "$LOCALAPPDATA"/Microsoft/WinGet/Packages/Gyan.FFmpeg*/ffmpeg-*/bin | head -1)
for i in 1 2 3 4 5 6; do echo "file '$1_$i.png'"; done > l.txt
"$FF/ffmpeg.exe" -hide_banner -loglevel error -y -f concat -safe 0 -i l.txt -vf "scale=480:272,tile=3x2" -frames:v 1 "$1_sheet.png"
