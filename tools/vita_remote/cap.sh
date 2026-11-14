#!/bin/bash
# cap.sh NOM : image de l'écran de la Vita par USB (udcd_uvc) dans out/remote/NOM.png, sans toucher au jeu.
FF=$(ls -d "$LOCALAPPDATA"/Microsoft/WinGet/Packages/Gyan.FFmpeg*/ffmpeg-*/bin 2>/dev/null | head -1)
O="$(cd "$(dirname "$0")" && pwd)/../../out/remote"; mkdir -p "$O"
"$FF/ffmpeg.exe" -hide_banner -loglevel error -y -f dshow -video_size 960x544 -framerate 30 -i video="PSVita" -frames:v 1 "$O/$1.png" && echo "$O/$1.png"
