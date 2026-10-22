#!/bin/zsh
# Kills the bottle (BOTTLE, default SteamX64) as soon as WindowServer reports a stuck display, before its 40 s watchdog logs the user out.
set -u
source ${0:A:h}/bottle.sh
LOG=${1:-/tmp/displayguard.log}

echo "$(date '+%T') guarding" >> $LOG
/usr/bin/log stream --style compact --predicate 'process == "WindowServer" AND eventMessage CONTAINS[c] "has become stuck"' |
while read -r line; do
    [[ $line == *"has become stuck"* && $line != Filtering* ]] || continue
    echo "$(date '+%T') $line" >> $LOG
    WINEPREFIX=$BOTTLE_PATH/$BOTTLE "$WINESERVER" -k
    for pid in $(pgrep -f 'C:\\'); do
        kill -9 $pid
    done
    echo "$(date '+%T') bottle killed" >> $LOG
done
