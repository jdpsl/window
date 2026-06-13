#!/bin/bash
# test_cursor.sh - tests cursor modes
cd "$(dirname "$0")/.."

./window &
WINPID=$!
PIPE="/tmp/window-${WINPID}.pipe"
w() { ./window "$PIPE" "$@"; }
sleep 0.2

w size 400 300
w background 30 30 30
w clear
w fill 255 255 255
w loadfont main 20 DejaVu Sans
w font main
w text 20 60 move mouse over window
w text 20 100 cursor changes every second
w flush

w cursor crosshair
w cursor hand
w cursor text
w cursor move
w cursor wait
w cursor none
w cursor default

# pause for visual mode
[ "${VISUAL}" = "1" ] && sleep 5
w close
