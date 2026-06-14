#!/bin/bash
# demos/bash/pixelrain.sh - matrix-style pixel rain
cd "$(dirname "$0")/../.."

./window &
WINPID=$!
PIPE="/tmp/window-${WINPID}.pipe"
EVPIPE="/tmp/window-${WINPID}.events"
sleep 0.3

exec 4>"$PIPE"
exec 3<>"$EVPIPE"
w() { echo "$@" >&4; }

w size 640 400
w background 0 0 0
w cursor none
w moveevents none

COLS=40

declare -a CY CSPEED
for i in $(seq 0 $((COLS - 1))); do
    CY[$i]=$(( RANDOM % 400 ))
    CSPEED[$i]=$(( RANDOM % 3 + 1 ))
done

while true; do
    w clear
    for i in $(seq 0 $((COLS - 1))); do
        x=$(( i * 16 + 8 ))
        y=${CY[$i]}
        t=0
        while [ $t -lt 14 ]; do
            bright=$(( 200 - t * 14 ))
            [ $bright -lt 0 ] && break
            w stroke 0 $bright 0
            w point $x $(( y - t ))
            t=$(( t + 1 ))
        done
        w stroke 180 255 180
        w point $x $y
        CY[$i]=$(( (y + CSPEED[$i]) % 400 ))
    done
    w flush

    ev=""
    read -t 0.033 -u 3 ev
    case "$ev" in
        "key down q"*|"close") break ;;
    esac
done

w close
