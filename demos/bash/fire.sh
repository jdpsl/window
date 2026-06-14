#!/bin/bash
# demos/bash/fire.sh - cellular automaton fire effect (Doom-style)
cd "$(dirname "$0")/../.."

./window &
WINPID=$!
PIPE="/tmp/window-${WINPID}.pipe"
EVPIPE="/tmp/window-${WINPID}.events"
sleep 0.3

exec 4>"$PIPE"
exec 3<>"$EVPIPE"
w() { echo "$@" >&4; }

COLS=40; ROWS=25; CW=16; CH=16
W=$(( COLS * CW )); H=$(( ROWS * CH ))
COLS1=$(( COLS - 1 ))
ROWS1=$(( ROWS - 1 ))

w size $W $H
w title "fire"
w background 0 0 0
w moveevents none
w cursor none

# palette: black -> deep red -> orange -> yellow -> white
declare -a PR PG PB
i=0
while [ $i -lt 256 ]; do
    r=$(( i * 3 ));      [ $r -gt 255 ] && r=255
    g=$(( i * 2 - 128 )); [ $g -lt 0 ] && g=0; [ $g -gt 255 ] && g=255
    b=$(( (i - 210) * 6 )); [ $b -lt 0 ] && b=0; [ $b -gt 255 ] && b=255
    PR[$i]=$r; PG[$i]=$g; PB[$i]=$b
    i=$(( i + 1 ))
done

# fire grid - flat array, index = y*COLS + x  (y=0 top, y=ROWS-1 bottom)
declare -a F
i=0
while [ $i -lt $(( ROWS * COLS )) ]; do
    F[$i]=0
    i=$(( i + 1 ))
done

w nostroke

while true; do
    # seed bottom row with flickering fuel
    x=0
    while [ $x -lt $COLS ]; do
        F[$(( ROWS1 * COLS + x ))]=$(( 220 + RANDOM % 36 ))
        x=$(( x + 1 ))
    done

    # propagate fire upward with cooling and drift
    y=0
    while [ $y -lt $ROWS1 ]; do
        yw=$(( y * COLS ))
        yw1=$(( (y + 1) * COLS ))
        x=0
        while [ $x -lt $COLS ]; do
            dx=$(( RANDOM % 3 - 1 ))
            sx=$(( x + dx ))
            [ $sx -lt 0 ] && sx=0
            [ $sx -gt $COLS1 ] && sx=$COLS1
            val=$(( ${F[$(( yw1 + sx ))]} - RANDOM % 3 ))
            [ $val -lt 0 ] && val=0
            F[$(( yw + x ))]=$val
            x=$(( x + 1 ))
        done
        y=$(( y + 1 ))
    done

    # draw
    w clear
    y=0
    while [ $y -lt $ROWS ]; do
        yw=$(( y * COLS ))
        py=$(( y * CH ))
        x=0
        while [ $x -lt $COLS ]; do
            v=${F[$(( yw + x ))]}
            if [ $v -gt 4 ]; then
                w fill ${PR[$v]} ${PG[$v]} ${PB[$v]}
                w rect $(( x * CW )) $py $CW $CH
            fi
            x=$(( x + 1 ))
        done
        y=$(( y + 1 ))
    done
    w flush

    ev=""
    read -t 0.033 -u 3 ev
    case "$ev" in
        "key down q"*|"close") break ;;
    esac
done

w close
