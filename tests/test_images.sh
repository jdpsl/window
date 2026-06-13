#!/bin/bash
# test_images.sh - tests image and sprite loading
cd "$(dirname "$0")/.."

./window &
WINPID=$!
PIPE="/tmp/window-${WINPID}.pipe"
w() { ./window "$PIPE" "$@"; }
sleep 0.2

w size 600 300
w background 60 120 60
w clear

w load bg      images/walk_sheet.png
w load walk    strip images/walk_sheet.png 0 0 427 496 2
w load idle    strip images/idle_sheet.png 0 0 427 496 6

# draw full image scaled down
w draw bg 0 0 600 300

# draw individual frames
w drawframe walk 0  10 10 100 120
w drawframe walk 1 120 10 100 120

w drawframe idle 0 240 10 80 96
w drawframe idle 1 330 10 80 96
w drawframe idle 2 420 10 80 96
w drawframe idle 3 510 10 80 96

w flush
# pause for visual mode
[ "${VISUAL}" = "1" ] && sleep 5
w close
