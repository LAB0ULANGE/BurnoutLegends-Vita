#!/bin/bash
# step.sh "<vitacompanion commands>" WAIT_S NAME : run the commands, wait, take a screenshot into NAME.png
S="$(cd "$(dirname "$0")" && pwd)"
F=ftp://192.168.1.21:1337
[ -n "$1" ] && python -I "$S/vc.py" "$1" >/dev/null
sleep ${2:-2}
before=$(curl -s $F/ux0:/data/burnout-frames/ | awk '{print $NF}' | sort)
echo x > "$S/../../out/shot.txt"; curl -s -T "$S/../../out/shot.txt" $F/ux0:/data/burnout/shot.txt
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do sleep 1; after=$(curl -s $F/ux0:/data/burnout-frames/ | awk '{print $NF}' | sort); new=$(comm -13 <(echo "$before") <(echo "$after") | tail -1); [ -n "$new" ] && break; done
[ -z "$new" ] && { echo "no shot"; exit 1; }
sleep 1; curl -s -o "${OUT:-.}/$3.png" $F/ux0:/data/burnout-frames/$new; curl -s -Q "DELE ux0:/data/burnout-frames/$new" $F/ -o /dev/null; echo "$3.png <- $new"
