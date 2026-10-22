# Sourced by the scripts. Settings come from the environment or from local.env in the repo root (not tracked):
#   BOTTLE       CrossOver bottle name (default SteamX64) or absolute path
#   BOTTLE_PATH  directory holding the bottles (default ~/CXPBottles, CrossOver Preview's default)
#   CROSSOVER    CrossOver app (default /Applications/CrossOver Preview.app)
#   QUEST        the headset's adb address for wireless debugging, e.g. 192.168.1.50:5555 (scripts that use adb)
[[ -f ${0:A:h:h}/local.env ]] && source ${0:A:h:h}/local.env
BOTTLE_PATH=${BOTTLE_PATH:-$HOME/CXPBottles}
BOTTLE=${BOTTLE:-SteamX64}
if [[ $BOTTLE == /* ]]; then
    BOTTLE_PATH=${BOTTLE:h}
    BOTTLE=${BOTTLE:t}
fi
CROSSOVER=${CROSSOVER:-/Applications/CrossOver Preview.app}
CX="$CROSSOVER/Contents/SharedSupport/CrossOver/bin"
DRIVE=$BOTTLE_PATH/$BOTTLE/drive_c

if [[ ! -f $BOTTLE_PATH/$BOTTLE/cxbottle.conf ]]; then
    echo "no bottle $BOTTLE_PATH/$BOTTLE" >&2
    exit 1
fi
# The wine script picks the wineserver by WineArch: arm64 bottles run wineserver-arm64, win64 ones wineserver-x86.
if grep -q '^"WineArch" = "arm64"' $BOTTLE_PATH/$BOTTLE/cxbottle.conf; then
    WINESERVER="$CX/wineserver-arm64"
else
    WINESERVER="$CX/wineserver-x86"
fi

require_quest() {
    if [[ -z "${QUEST:-}" ]]; then
        echo "set QUEST to the headset's adb address (e.g. QUEST=192.168.1.50:5555), in the environment or local.env" >&2
        exit 1
    fi
}

# Starts the bottle's encoder helper on its frames.bin, replacing a helper that serves another bottle (port 47800 is shared)
# or runs an older build.
start_encoder() {
    local encoder=$DRIVE/macquestlink/macquestlink-encoder pid
    for pid in $(pgrep -f '/drive_c/macquestlink/macquestlink-encoder'); do
        if ps -o command= -p $pid | grep -qF "$encoder"; then
            local started=$(LC_ALL=C date -j -f "%a %b %d %T %Y" "$(LC_ALL=C ps -o lstart= -p $pid | xargs)" +%s 2>/dev/null)
            [[ -n $started && $started -ge $(stat -f %m $encoder) ]] && continue
        fi
        kill $pid
        for i in {1..20}; do
            kill -0 $pid 2>/dev/null || break
            sleep 0.25
        done
    done
    if ! pgrep -f macquestlink-encoder >/dev/null; then
        nohup $encoder $DRIVE/macquestlink/frames.bin >/dev/null 2>&1 &
    fi
}

# macOS keeps a GUI process that was launched in the background and never activated at a low scheduling role;
# Alyx then runs at half rate (~40 fps). Activating its window once fixes that for the life of the process.
activate_window() {
    for i in {1..30}; do
        osascript -e "tell application \"System Events\" to set frontmost of process \"$1\" to true" >/dev/null 2>&1
        sleep 1
        lsappinfo info -only name "$(lsappinfo front)" 2>/dev/null | grep -qF "\"$1\"" && return 0
    done
    return 1
}
