#!/bin/zsh
# Mirrors the Quest's full display (passthrough, home and VR apps) and reconnects whenever scrcpy drops.
[[ -f ${0:A:h:h}/local.env ]] && source ${0:A:h:h}/local.env
: ${QUEST:?set QUEST to the headset adb address, e.g. QUEST=192.168.1.50:5555}
while true; do
    adb connect $QUEST >/dev/null 2>&1
    adb -s $QUEST shell "svc power stayon true; am broadcast -a com.oculus.vrpowermanager.prox_close" >/dev/null 2>&1
    scrcpy -s $QUEST --no-audio --max-size=1600 --video-bit-rate=8M --window-title="Quest 3S"
    sleep 2
done
