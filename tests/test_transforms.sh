#!/bin/bash
# test_transforms.sh - tests push/pop/translate/rotate/scale
cd "$(dirname "$0")/.."

./window &
WINPID=$!
PIPE="/tmp/window-${WINPID}.pipe"
w() { ./window "$PIPE" "$@"; }
sleep 0.2

w size 500 500
w background 20 20 20
w clear

# baseline rect with no transform
w fill 255 255 255
w rect 10 10 80 40

# translate
w push
w translate 150 0
w fill 255 100 100
w rect 10 10 80 40
w pop

# scale
w push
w translate 0 80
w scale 2 2
w fill 100 255 100
w rect 10 10 80 40
w pop

# rotate
w push
w translate 200 200
w rotate 45
w fill 100 100 255
w rect -40 -20 80 40
w pop

# nested transforms
w push
w translate 350 100
w push
w rotate 30
w scale 1.5 1.5
w fill 255 200 0
w rect -30 -15 60 30
w pop
w pop

w flush
# pause for visual mode
[ "${VISUAL}" = "1" ] && sleep 5
w close
