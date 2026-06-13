#!/bin/bash
# test_text.sh - tests font loading and text rendering
cd "$(dirname "$0")/.."

./window &
WINPID=$!
PIPE="/tmp/window-${WINPID}.pipe"
w() { ./window "$PIPE" "$@"; }
sleep 0.2

w size 500 300
w background 20 20 40
w clear

w loadfont small 14 DejaVu Sans
w loadfont main  24 DejaVu Sans
w loadfont big   48 DejaVu Sans
w loadfont mono  20 DejaVu Sans Mono

w font small
w fill 180 180 255
w text 20 50 small 14pt text

w font main
w fill 255 255 255
w text 20 110 medium 24pt text

w font big
w fill 255 200 0
w text 20 190 big 48pt

w font mono
w fill 100 255 100
w text 20 260 monospace font

w flush
# pause for visual mode
[ "${VISUAL}" = "1" ] && sleep 5
w close
