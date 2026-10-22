#!/bin/zsh
# fpsrecord.sh <seconds> : stream fps, bitrate, frame path and compositor keyed-mutex timeouts every 10 s
set -u
source ${0:A:h}/bottle.sh
D=$DRIVE/macquestlink
C="$DRIVE/Program Files (x86)/Steam/logs/vrcompositor.txt"
fails() { grep -cE "AcquireSync FAILED|WaitForAcquire timed out" "$C" }
start=$(fails)
for i in $(seq 1 $(( $1 / 10 ))); do
    sleep 10
    f=$(grep "frames," $D/encoder.log | tail -1 | awk '{print $4, $10}')
    s=$(grep timing $D/nvenc_shim.log | tail -1 | grep -oE "total [0-9./]+")
    echo "$(date +%T) fps/mbit $f | $s | compositor fails $(( $(fails) - start ))"
done
