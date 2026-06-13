#!/bin/bash
# test_shapes.sh - tests all shape and style commands
cd "$(dirname "$0")/.."

./window &
WINPID=$!
PIPE="/tmp/window-${WINPID}.pipe"
w() { ./window "$PIPE" "$@"; }
sleep 0.2

w size 400 400
w background 30 30 30
w clear

w fill 255 0 0
w rect 10 10 100 50

w fill 0 255 0
w stroke 255 255 255
w strokeweight 2
w circle 200 80 40

w fill 0 0 255
w ellipse 300 80 80 40

w nofill
w stroke 255 255 0
w strokeweight 1
w line 10 150 390 150

w stroke 255 0 255
w arc 10 160 100 100 0 270

w fill 255 128 0
w nostroke
w rect 200 160 100 100 10

w fill 255 0 255
w stroke 255 255 255
w strokeweight 2
w triangle 50 380  150 250  250 380

w fill 255 255 255
w point 50 300
w point 51 300
w point 52 300

w flush
# pause for visual mode
[ "${VISUAL}" = "1" ] && sleep 5
w close
