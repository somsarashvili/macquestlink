#!/bin/zsh
# Restart only SteamVR (Steam keeps running) and reconnect Steam Link. For config/driver changes, not crashes.
# BOTTLE selects another bottle (default SteamX64, see bottle.sh).
set -u
source ${0:A:h}/bottle.sh
require_quest

adb connect $QUEST >/dev/null 2>&1
adb -s $QUEST shell am force-stop com.valvesoftware.steamlinkvr
for pid in $(pgrep -f 'SteamVR\\bin'); do
    kill -9 $pid 2>/dev/null
done
sleep 2
start_encoder
: > $DRIVE/macquestlink/nvenc_shim.log
adb -s $QUEST shell am broadcast -a com.oculus.vrpowermanager.prox_close >/dev/null 2>&1
adb -s $QUEST shell monkey -p com.valvesoftware.steamlinkvr -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
sleep 15
adb -s $QUEST shell input keyevent KEYCODE_ENTER
for i in {1..40}; do
    pgrep -f vrcompositor.exe >/dev/null && break
    sleep 2
done
pgrep -f vrcompositor.exe >/dev/null && echo "steamvr up" || echo "steamvr did not start"
