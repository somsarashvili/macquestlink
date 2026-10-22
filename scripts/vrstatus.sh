#!/bin/zsh
# Snapshot of the streaming chain: processes, encoder throughput, shim activity, crashes, and a headset screenshot.
set -u

source ${0:A:h}/bottle.sh
require_quest
STEAM="$DRIVE/Program Files (x86)/Steam"
OUT=${1:-/tmp/quest.jpg}

echo "== processes"
pgrep -fl "vrserver|vrcompositor|steam.exe|macquestlink-encoder" | sed -E 's/ DXMT.*//' | cut -c1-90
echo "== encoder"
tail -3 $DRIVE/macquestlink/encoder.log
echo "== shim"
grep -E "encoded #|rc=-1|helper" $DRIVE/macquestlink/nvenc_shim.log | tail -3 | cut -c1-160
echo "== recent dumps"
find "$STEAM/dumps" -mmin -3 2>/dev/null | sed 's#.*/##'
echo "== vrlink"
tail -4000 "$STEAM/logs/driver_vrlink.txt" | grep -E "Timed out|Overflow|ping|Exception|reset" | tail -3 | cut -c25-200
echo "== headset"
adb -s $QUEST shell am startservice -n com.oculus.metacam/.capture.CaptureService -a TAKE_SCREENSHOT >/dev/null 2>&1
sleep 3
shot=$(adb -s $QUEST shell ls -t /sdcard/Oculus/Screenshots/ 2>/dev/null | head -1 | tr -d '\r')
if [[ -n "$shot" ]]; then
    adb -s $QUEST pull "/sdcard/Oculus/Screenshots/$shot" /tmp/quest_full.jpg >/dev/null 2>&1
    sips -Z 600 /tmp/quest_full.jpg --out "$OUT" >/dev/null 2>&1
    echo "screenshot: $shot -> $OUT"
fi
