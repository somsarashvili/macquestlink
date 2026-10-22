#!/bin/zsh
# alyxsave.sh [save] : (re)start Half-Life: Alyx with VConsole, load a save (default s6/steamvr_quit) and unpause.
# SteamVR must be up (scripts/vrreset.sh).
set -u
source ${0:A:h}/bottle.sh
SAVE=${1:-s6/steamvr_quit}
G="$DRIVE/Program Files (x86)/Steam/steamapps/common/Half-Life Alyx/game/hlvr"
VC=${0:A:h}/../tools/vconsole.py

pkill -f 'Alyx.game.bin.win64.hlvr.exe'
while pgrep -f 'Alyx.game.bin.win64.hlvr.exe' >/dev/null; do sleep 1; done
sleep 3
[[ -f "$G/console.log" ]] && mv "$G/console.log" "$G/console.prev.log"
CX_BOTTLE_PATH=$BOTTLE_PATH nohup "$CX/wine" --bottle $BOTTLE 'C:\Program Files (x86)\Steam\steam.exe' \
    -applaunch 546560 -condebug -vconsole >/dev/null 2>&1 &
for i in {1..90}; do
    grep -q 'Map: "startup"' "$G/console.log" 2>/dev/null && break
    sleep 2
done
sleep 5
python3 $VC "load $SAVE" >/dev/null
for i in {1..90}; do
    grep -q "paused the game" "$G/console.log" 2>/dev/null && break
    sleep 2
done
# Steam summons its game overlay into the SteamVR dashboard after launch, and Alyx pauses while it is open.
for i in {1..3}; do
    python3 ${0:A:h}/../tools/vrmailbox.py vrwebui_dashboard '{"type":"hide_dashboard_requested","reason":"alyxsave"}'
    sleep 2
    python3 $VC unpause | grep -i pause
    sleep 5
done
activate_window hlvr.exe && echo "activated hlvr.exe"
