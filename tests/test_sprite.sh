#!/bin/bash

./window &
WINPID=$!
PIPE="/tmp/window-${WINPID}.pipe"
w() { ./window "$PIPE" "$@"; }
sleep 0.3

w title "sprite test"
w size 600 500
w background 60 120 60   # green background

# load the walk strip - 2 frames of 427x496
w load walk strip ./images/walk_sheet.png 0 0 427 496 2

# load the idle strip - 6 frames of 427x496
w load idle strip ./images/idle_sheet.png 0 0 427 496 6

frame=0
anim=walk
count=0

while true; do
    w clear

    # draw current frame scaled down to 150x174
    w drawframe $anim $frame 225 160 150 174

    w flush

    count=$(( count + 1 ))
    if [ $count -ge 3 ]; then
        count=0
        if [ "$anim" = "walk" ]; then
            frame=$(( (frame + 1) % 2 ))
        else
            frame=$(( (frame + 1) % 6 ))
        fi
    fi

    sleep 0.1
done
