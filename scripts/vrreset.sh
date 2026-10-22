#!/bin/zsh
# Quit everything (the bottle, orphaned Wine processes, Steam Link on the Quest), then start Steam,
# relaunch Steam Link and connect. BOTTLE selects another bottle (default SteamX64, see bottle.sh).
set -u

source ${0:A:h}/bottle.sh
require_quest
LOGS="$DRIVE/Program Files (x86)/Steam/logs"

adb connect $QUEST >/dev/null 2>&1
adb -s $QUEST shell am force-stop com.valvesoftware.steamlinkvr

WINEPREFIX=$BOTTLE_PATH/$BOTTLE "$WINESERVER" -k 2>/dev/null
sleep 2
pkill -9 -f winedbg 2>/dev/null
pkill -9 -f steamerrorreporter 2>/dev/null
for pid in $(pgrep -f 'C:\\'); do
    kill -9 $pid 2>/dev/null
done
sleep 2
echo "everything stopped"

start_encoder
: > $DRIVE/macquestlink/nvenc_shim.log

# The 32-bit Steam Client Service calls GDI32.D3DKMTSetProcessSchedulingPriorityClass (missing in Wine) when Steam
# raises vrcompositor's GPU priority; it crashes into a hanging winedbg and Steam's main loop waits on it forever.
# With the service disabled the call fails right away and Steam carries on (GPU priority means nothing on Metal).
if ! grep -A9 'Services\\\\Steam Client Service\]' $BOTTLE_PATH/$BOTTLE/system.reg | grep -q '"Start"=dword:00000004'; then
    CX_BOTTLE_PATH=$BOTTLE_PATH "$CX/wine" --bottle $BOTTLE reg add 'HKLM\System\CurrentControlSet\Services\Steam Client Service' \
        /v Start /t REG_DWORD /d 4 /f >/dev/null 2>&1
fi

seen=$(wc -l < "$LOGS/connection_log.txt" 2>/dev/null || echo 0)
CX_BOTTLE_PATH=$BOTTLE_PATH nohup "$CX/wine" --bottle $BOTTLE 'C:\Program Files (x86)\Steam\steam.exe' ${=STEAM_ARGS:-} >/dev/null 2>&1 &
for i in {1..60}; do
    tail -n +$((seen + 1)) "$LOGS/connection_log.txt" 2>/dev/null | grep -q "Logged On.*processing complete" && break
    sleep 2
done
echo "steam logged on"

adb -s $QUEST shell am broadcast -a com.oculus.vrpowermanager.prox_close >/dev/null 2>&1
adb -s $QUEST shell monkey -p com.valvesoftware.steamlinkvr -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
sleep 15
# The computer list is a web view; Enter activates the highlighted PC's Connect.
adb -s $QUEST shell input keyevent KEYCODE_ENTER
echo "connect sent"

for i in {1..40}; do
    pgrep -f vrcompositor.exe >/dev/null && break
    sleep 2
done
pgrep -f vrcompositor.exe >/dev/null && echo "steamvr up" || echo "steamvr did not start"

# Optional: launch a Steam app (e.g. 546560 for Half-Life: Alyx) once the headset is connected.
if [[ -n "${1:-}" ]] && pgrep -f vrcompositor.exe >/dev/null; then
    sleep 10
    CX_BOTTLE_PATH=$BOTTLE_PATH nohup "$CX/wine" --bottle $BOTTLE 'C:\Program Files (x86)\Steam\steam.exe' ${=STEAM_ARGS:-} -applaunch $1 >/dev/null 2>&1 &
    echo "launched app $1"
    [[ $1 == 546560 ]] && activate_window hlvr.exe && echo "activated hlvr.exe"
fi
