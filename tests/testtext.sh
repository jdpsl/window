#!/bin/bash

./window &
WINPID=$!
PIPE="/tmp/window-${WINPID}.pipe"
w() { ./window "$PIPE" "$@"; }
sleep 0.3

w size 600 500
w background 40 40 80
w clear

w loadfont small 16 DejaVu Sans
w loadfont main  24 DejaVu Sans
w loadfont big   48 DejaVu Sans

w font small
w fill 180 180 255
w text 20 80 small 16pt text

w font main
w fill 255 255 255
w text 20 180 medium 24pt text

w font big
w fill 255 200 0
w text 20 280 big 48pt text

w fill 100 255 100
w text 20 380 also big green

w flush

# keep window open
sleep 10
w close
