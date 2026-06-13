#!/bin/bash
# basic proof of concept test

# run server in background, derive pipe path from its PID
./window &
WINPID=$!
PIPE="/tmp/window-${WINPID}.pipe"
w() { ./window "$PIPE" "$@"; }

# wait for server to create the pipe and enter its loop
sleep 0.3

w title "window poc"
w size 600 400
w background 30 30 120   # dark blue - clear color

# bouncing circle demo
x=300
y=200
dx=3
dy=2
r=30

while true; do
    # logic in bash
    x=$(( x + dx ))
    y=$(( y + dy ))

    if [ $x -ge $(( 600 - r )) ] || [ $x -le $r ]; then dx=$(( -dx )); fi
    if [ $y -ge $(( 400 - r )) ] || [ $y -le $r ]; then dy=$(( -dy )); fi

    # draw
    w clear

    # a visible rectangle
    w fill 80 80 200
    w nostroke
    w rect 10 10 120 40

    # bouncing circle
    w fill 255 80 80
    w stroke 255 255 255
    w strokeweight 2
    w circle $x $y $r

    # trail rings
    w nofill
    w stroke 255 80 80
    w strokeweight 1
    w circle $x $y $(( r + 10 ))
    w circle $x $y $(( r + 20 ))

    w flush
    sleep 0.016
done

w close
