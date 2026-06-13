#!/bin/bash
# world.sh - interactive night-walk demo using every window feature
cd "$(dirname "$0")/.."

./window &
WINPID=$!
PIPE="/tmp/window-${WINPID}.pipe"
EVPIPE="/tmp/window-${WINPID}.events"
sleep 0.3

# write commands directly to pipe fd - avoids spawning a process per command
exec 4>"$PIPE"
w() { echo "$@" >&4; }

W=800
H=500

w size $W $H
w title "night walk"
w background 8 8 28
w cursor none

# fonts
w loadfont ui    15 DejaVu Sans
w loadfont small 11 DejaVu Sans

# sprites: walk=2 frames, idle=6 frames, each 427x496
SW=72
SH=84
w load walk strip ./images/walk_sheet.png 0 0 427 496 2
w load idle strip ./images/idle_sheet.png 0 0 427 496 6

# character state
px=200
py=310
dest_x=-1
dest_y=-1
moving=0
dx=0
dy=0
speed=4

# animation
atick=0
sframe=0
sanim=idle

# open events FIFO non-blocking
exec 3<>"$EVPIPE"

tick=0
moon_spin=0

while true; do
    tick=$((tick + 1))
    atick=$((atick + 1))
    moon_spin=$((moon_spin + 2))
    [ $moon_spin -ge 360 ] && moon_spin=0

    # ── read one event (33ms timeout ~ 30fps) ─────────────────
    ev=""
    read -t 0.033 -u 3 ev

    case "$ev" in
        "key down q"*)
            break ;;
        "key down Left"*)
            moving=1; dx=-$speed; dy=0; dest_x=-1 ;;
        "key down Right"*)
            moving=1; dx=$speed;  dy=0; dest_x=-1 ;;
        "key down Up"*)
            moving=1; dx=0; dy=-$speed; dest_x=-1 ;;
        "key down Down"*)
            moving=1; dx=0; dy=$speed;  dest_x=-1 ;;
        "key up Left"*|"key up Right"*|"key up Up"*|"key up Down"*)
            [ $dest_x -lt 0 ] && { moving=0; dx=0; dy=0; } ;;
        click\ *)
            dest_x=$(echo "$ev" | cut -d' ' -f2)
            dest_y=$(echo "$ev" | cut -d' ' -f3)
            dest_y=$((dest_y - SH))
            moving=1 ;;
        "close") break ;;
    esac

    # ── move toward click target ───────────────────────────────
    if [ $dest_x -ge 0 ]; then
        ddx=$((dest_x - px))
        ddy=$((dest_y - py))
        if   [ $ddx -gt $speed ];  then px=$((px + speed))
        elif [ $ddx -lt -$speed ]; then px=$((px - speed))
        else px=$dest_x; fi
        if   [ $ddy -gt $speed ];  then py=$((py + speed))
        elif [ $ddy -lt -$speed ]; then py=$((py - speed))
        else py=$dest_y; fi
        [ $px -eq $dest_x ] && [ $py -eq $dest_y ] && { dest_x=-1; moving=0; }
    elif [ $moving -eq 1 ]; then
        px=$((px + dx))
        py=$((py + dy))
    fi

    # clamp to world bounds
    [ $px -lt 0 ]              && px=0
    [ $px -gt $((W - SW)) ]    && px=$((W - SW))
    [ $py -lt 80 ]             && py=80
    [ $py -gt $((H - SH - 40)) ] && py=$((H - SH - 40))

    # animation frame
    if [ $moving -eq 1 ]; then
        sframe=$((atick / 6 % 2))
        sanim=walk
    else
        sframe=$((atick / 10 % 6))
        sanim=idle
    fi

    # ── draw ──────────────────────────────────────────────────
    w identity   # clean matrix at frame start

    w clear

    # sky - gradient via stacked rects
    w nostroke
    w fill 8 8 45
    w rect 0 0 $W 160
    w fill 12 12 52
    w rect 0 160 $W 110
    w fill 16 18 42
    w rect 0 270 $W 80

    # moon halo (arc)
    w nofill
    w stroke 70 68 40
    w strokeweight 1
    w arc 613 45 90 90 0 360

    # moon
    w fill 240 232 185
    w nostroke
    w circle 658 90 38

    # crescent shadow - slightly offset circle in sky color
    w fill 18 12 48
    w circle 672 82 30

    # planet ring around moon (scale to squish a circle into an ellipse)
    w push
    w translate 658 90
    w scale 2 1
    w nofill
    w stroke 55 50 35
    w strokeweight 1
    w circle 0 0 52
    w pop
    w identity

    # spinning sparkle around moon
    w push
    w translate 658 90
    w rotate $moon_spin
    w stroke 255 245 140
    w strokeweight 1
    w nofill
    w line -26 0 26 0
    w line 0 -26 0 26
    w pop
    w push
    w translate 658 90
    w rotate $((moon_spin + 45))
    w stroke 200 195 100
    w strokeweight 1
    w line -16 0 16 0
    w line 0 -16 0 16
    w pop
    w identity

    # tiny stars (point command)
    w stroke 200 200 200
    seed=31337
    i=0
    while [ $i -lt 35 ]; do
        seed=$(( (seed * 1103515245 + 12345) & 0x7fffffff ))
        starx=$((seed % W))
        seed=$(( (seed * 1103515245 + 12345) & 0x7fffffff ))
        stary=$((seed % 220))
        w point $starx $stary
        i=$((i + 1))
    done

    # bigger stars (filled circles)
    w fill 255 255 255
    w nostroke
    w circle 90  38 2
    w circle 310 22 2
    w circle 480 65 2
    w circle 730 30 2
    w circle 150 90 2
    w circle 560 15 2

    # distant hills (polygon)
    w fill 10 38 14
    w nostroke
    w polygon 0 300  80 245  190 265  310 210  430 250  555 195  670 230  800 210  800 305  0 305

    # ground
    w fill 14 52 18
    w nostroke
    w rect 0 305 $W $((H - 305))

    # dirt path - two bezier passes for a wide road look
    w nofill
    w stroke 48 38 18
    w strokeweight 22
    w bezier 150 $H  280 345  520 335  680 $H
    w stroke 62 50 26
    w strokeweight 14
    w bezier 150 $H  280 345  520 335  680 $H

    # trees
    t=0
    while [ $t -lt 6 ]; do
        treex=$((55 + t * 145))
        treeh=$((85 + (t % 3) * 28))
        tbase=305
        # trunk
        w fill 52 30 10
        w nostroke
        w rect $((treex - 5)) $((tbase - 30)) 10 30
        # outer foliage
        w fill 7 55 14
        w triangle $((treex - 34)) $tbase  $((treex + 34)) $tbase  $treex $((tbase - treeh))
        # inner highlight
        w fill 11 75 20
        w triangle $((treex - 18)) $((tbase - treeh / 3))  $((treex + 18)) $((tbase - treeh / 3))  $treex $((tbase - treeh - 18))
        t=$((t + 1))
    done

    # fence - rail line + posts (rects)
    w nostroke
    w fill 85 60 28
    w rect 0 $((H - 58)) $W 5
    f=0
    while [ $f -lt 16 ]; do
        fx=$((f * 52 + 8))
        w rect $fx $((H - 75)) 7 25
        f=$((f + 1))
    done

    # target indicator at click destination
    if [ $dest_x -ge 0 ]; then
        tdraw_y=$((dest_y + SH))
        w nofill
        w stroke 255 228 60
        w strokeweight 2
        w circle $dest_x $tdraw_y 15
        w strokeweight 1
        w circle $dest_x $tdraw_y 6
        w line $((dest_x - 22)) $tdraw_y $((dest_x - 17)) $tdraw_y
        w line $((dest_x + 17)) $tdraw_y $((dest_x + 22)) $tdraw_y
        w line $dest_x $((tdraw_y - 22)) $dest_x $((tdraw_y - 17))
        w line $dest_x $((tdraw_y + 17)) $dest_x $((tdraw_y + 22))
    fi

    # shadow ellipse under character
    w fill 0 20 5
    w nostroke
    w ellipse $((px + SW / 2)) $((py + SH + 4)) 26 6

    # sprite
    w drawframe $sanim $sframe $px $py $SW $SH

    # HUD bar
    w fill 0 0 0
    w nostroke
    w rect 0 0 $W 22

    w font ui
    w fill 190 190 190
    w text 8 16 arrows: move    click: walk to point    q: quit

    # position readout bottom right
    w font small
    w fill 90 90 90
    w text $((W - 155)) $((H - 8)) pos $px,$py  tick $tick

    w flush
done

w close
