#!/bin/zsh
# measure.sh [seconds] [label] : one streaming measurement (default 60 s) while a session runs. Prints stream fps,
# encode time and bitrate (helper encoder.log), frame path (shim timing), vrserver/wineserver/hlvr CPU, vrlink Bad link
# events per minute and their ping, and compositor keyed-mutex timeouts.
set -u
source ${0:A:h}/bottle.sh
SECS=${1:-60}
LABEL=${2:-run}
D=$DRIVE/macquestlink
L="$DRIVE/Program Files (x86)/Steam/logs"

lines() { wc -l < "$1" 2>/dev/null || echo 0; }
enc0=$(lines $D/encoder.log)
shim0=$(lines $D/nvenc_shim.log)
link0=$(lines "$L/driver_vrlink.txt")
comp0=$(lines "$L/vrcompositor.txt")

typeset -A cpu
samples=0
end=$((SECONDS + SECS))
while (( SECONDS < end )); do
    ps -Ao %cpu=,command= > /tmp/mql_measure_ps.$$
    for key pat in vrserver 'vrserver.exe' wineserver 'wineserver' hlvr 'hlvr.exe'; do
        cpu[$key]=$(( ${cpu[$key]:-0} + $(grep -F "$pat" /tmp/mql_measure_ps.$$ | grep -v grep | awk '{s += $1} END {print s + 0}') ))
    done
    samples=$((samples + 1))
    sleep 2
done
rm -f /tmp/mql_measure_ps.$$

enc=$(tail -n +$((enc0 + 1)) $D/encoder.log | grep "frames,")
fps=$(echo "$enc" | grep -oE '[0-9.]+ fps' | awk '{s += $1; n++} END {if (n) printf "%.1f", s / n; else printf "?"}')
encms=$(echo "$enc" | grep -oE 'avg [0-9.]+ ms encode' | awk '{s += $2; n++} END {if (n) printf "%.2f", s / n; else printf "?"}')
mbit=$(echo "$enc" | grep -oE '[0-9.]+ Mbit/s' | awk '{s += $1; n++} END {if (n) printf "%.0f", s / n; else printf "?"}')
framepath=$(tail -n +$((shim0 + 1)) $D/nvenc_shim.log | grep -oE 'total [0-9.]+/[0-9.]+' | tail -1)
bad=$(tail -n +$((link0 + 1)) "$L/driver_vrlink.txt" | grep "Bad link event")
nbad=$(echo -n "$bad" | grep -c "Bad link")
ping=$(echo "$bad" | grep -oE 'ping [0-9.]+' | awk '{print $2}' | sort -n | awk '{a[NR] = $1} END {if (NR) printf "median %.1f max %.1f", a[int((NR + 1) / 2)], a[NR]; else printf "-"}')
km=$(tail -n +$((comp0 + 1)) "$L/vrcompositor.txt" | grep -cE "AcquireSync FAILED|WaitForAcquire timed out")

printf '%s %s: %ss | stream %s fps, encode %s ms, %s Mbit/s | path avg/max %s ms | cpu vrserver %.0f%% wineserver %.0f%% hlvr %.0f%% | bad link %.1f/min (ping %s ms) | compositor timeouts %s\n' \
    "$(date '+%F %T')" "$LABEL" "$SECS" "$fps" "$encms" "$mbit" "${framepath#total }" \
    $((cpu[vrserver] * 1.0 / samples)) $((cpu[wineserver] * 1.0 / samples)) $((cpu[hlvr] * 1.0 / samples)) \
    $((nbad * 60.0 / SECS)) "$ping" "$km"
