#!/bin/bash
# Quit Burnout, replace only its eboot.bin, relaunch it.
S="$(cd "$(dirname "$0")" && pwd)"
EB="$S/../../out/vita/profiles/burnout/eboot_gl.bin"
python -I "$S/vc.py" "destroy" ; sleep 3
curl -s --max-time 120 -T "$EB" ftp://192.168.1.21:1337/ux0:/app/BRNG00001/eboot.bin || { echo upload failed; exit 1; }
remote=$(curl -s ftp://192.168.1.21:1337/ux0:/app/BRNG00001/ | grep " eboot.bin" | awk '{print $5}')
echo "eboot local $(stat -c %s "$EB") remote $remote"
python -I "$S/vc.py" "launch BRNG00001"
