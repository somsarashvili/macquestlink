#!/bin/zsh
# Headless smoke test with SteamVR's null driver (no headset; vrlink, the NVENC shim and audio are not exercised).
#   scripts/nulltest.sh start [appid]  : stop the bottle, switch steamvr.vrsettings to the null driver (backup kept),
#                                        start Steam and SteamVR, then optionally a Steam app (546560 = Half-Life: Alyx)
#   scripts/nulltest.sh stop           : stop the bottle and restore steamvr.vrsettings
set -u
source ${0:A:h}/bottle.sh
STEAM_DIR="$DRIVE/Program Files (x86)/Steam"
LOGS="$STEAM_DIR/logs"
VRSETTINGS="$STEAM_DIR/config/steamvr.vrsettings"
BACKUP="$VRSETTINGS.nulltest-backup"

stop_bottle() {
    WINEPREFIX=$BOTTLE_PATH/$BOTTLE "$WINESERVER" -k 2>/dev/null
    sleep 2
    pkill -9 -f winedbg 2>/dev/null
    for pid in $(pgrep -f 'C:\\'); do
        kill -9 $pid 2>/dev/null
    done
    sleep 2
}

case "${1:-}" in
start)
    stop_bottle
    [[ -f $BACKUP ]] || cp "$VRSETTINGS" "$BACKUP"
    python3 - "$BACKUP" "$VRSETTINGS" <<'EOF'
import json, sys
s = json.load(open(sys.argv[1]))
s.setdefault("driver_null", {})["enable"] = True
s.setdefault("steamvr", {})["forcedDriver"] = "null"
json.dump(s, open(sys.argv[2], "w"), indent=3)
EOF
    start_encoder
    if ! grep -A9 'Services\\\\Steam Client Service\]' $BOTTLE_PATH/$BOTTLE/system.reg | grep -q '"Start"=dword:00000004'; then
        CX_BOTTLE_PATH=$BOTTLE_PATH "$CX/wine" --bottle $BOTTLE reg add 'HKLM\System\CurrentControlSet\Services\Steam Client Service' \
            /v Start /t REG_DWORD /d 4 /f >/dev/null 2>&1
    fi
    seen=$(wc -l < "$LOGS/connection_log.txt" 2>/dev/null || echo 0)
    CX_BOTTLE_PATH=$BOTTLE_PATH nohup "$CX/wine" --bottle $BOTTLE 'C:\Program Files (x86)\Steam\steam.exe' >/dev/null 2>&1 &
    for i in {1..60}; do
        tail -n +$((seen + 1)) "$LOGS/connection_log.txt" 2>/dev/null | grep -q "Logged On.*processing complete" && break
        sleep 2
    done
    echo "steam logged on"
    CX_BOTTLE_PATH=$BOTTLE_PATH nohup "$CX/wine" --bottle $BOTTLE 'C:\Program Files (x86)\Steam\steam.exe' -applaunch 250820 \
        >/dev/null 2>&1 &
    for i in {1..40}; do
        pgrep -f 'C:.*vrcompositor' >/dev/null && break
        sleep 2
    done
    pgrep -f 'C:.*vrcompositor' >/dev/null && echo "steamvr up (null driver)" || { echo "steamvr did not start"; exit 1; }
    # The null HMD has no play area, so vrmonitor opens Room Setup, which takes the scene-app slot.
    for i in {1..20}; do
        pkill -f 'steamvr_room_setup.exe' && echo "closed room setup"
        sleep 1
    done
    if [[ -n "${2:-}" ]]; then
        sleep 10
        CX_BOTTLE_PATH=$BOTTLE_PATH nohup "$CX/wine" --bottle $BOTTLE 'C:\Program Files (x86)\Steam\steam.exe' -applaunch $2 \
            >/dev/null 2>&1 &
        echo "launched app $2"
    fi
    ;;
stop)
    stop_bottle
    if [[ -f $BACKUP ]]; then
        mv "$BACKUP" "$VRSETTINGS"
        echo "steamvr.vrsettings restored"
    fi
    echo "bottle stopped"
    ;;
*)
    echo "usage: $0 start [appid] | stop" >&2
    exit 1
    ;;
esac
