#!/bin/bash
# demos/bash/paint.sh - mouse painting canvas with color palette
cd "$(dirname "$0")/../.."

./window &
WINPID=$!
PIPE="/tmp/window-${WINPID}.pipe"
EVPIPE="/tmp/window-${WINPID}.events"
sleep 0.3

exec 4>"$PIPE"
exec 3<>"$EVPIPE"
w() { echo "$@" >&4; }

W=800; H=560
CANVY=492          # canvas ends here, palette bar below

w size $W $H
w title "paint"
w background 245 245 245
w moveevents drag
w cursor crosshair
w loadfont ui 13 DejaVu Sans

# ── color palette (16 colors as separate R G B arrays) ─────────
PAL_R=( 0   255 190  80  220 240 220  50  30  30  130 220 140  20  20  255 )
PAL_G=( 0   255 190  80   40 120 200 180 160 130  60  50  80  80  40  160 )
PAL_B=( 0   255 190  80   40  30  30  50 100 220 200 160  40  40 120   60 )
NCOLORS=16
SW=32; SH=40
SX=8;  SY=$(( CANVY + 14 ))

# ── brush sizes ─────────────────────────────────────────────────
# layout: 8 + 16*32 + 12 gap = 532 start, 5*(30+4)=170 wide → ends at 702
SIZES=( 2 5 10 16 24 )
NSIZES=5
BSX=$(( SX + NCOLORS * SW + 12 ))
BSW=30; BSH=40; BSG=4

# ── clear button ────────────────────────────────────────────────
CLX=$(( BSX + NSIZES * (BSW + BSG) + 6 ))
CLY=$(( CANVY + 18 ))
CLW=$(( W - CLX - 8 )); CLH=34

# ── state ───────────────────────────────────────────────────────
sel_ci=0
sel_si=1
last_x=-1; last_y=-1
drawing=0

# ── draw palette bar (no flush - caller decides when to flush) ──
draw_palette() {
    w fill 22 24 32; w nostroke
    w rect 0 $CANVY $W $(( H - CANVY ))
    w stroke 45 50 68; w strokeweight 1; w nofill
    w line 0 $CANVY $W $CANVY

    # color swatches
    local i=0
    while [ $i -lt $NCOLORS ]; do
        local sx=$(( SX + i * SW ))
        w fill ${PAL_R[$i]} ${PAL_G[$i]} ${PAL_B[$i]}; w nostroke
        w rect $sx $SY $(( SW - 4 )) $SH
        if [ $i -eq $sel_ci ]; then
            w nofill; w stroke 255 255 255; w strokeweight 2
            w rect $sx $SY $(( SW - 4 )) $SH
        fi
        i=$(( i + 1 ))
    done

    # brush size buttons
    local i=0
    while [ $i -lt $NSIZES ]; do
        local bx=$(( BSX + i * (BSW + BSG) ))
        local sz=${SIZES[$i]}
        if [ $i -eq $sel_si ]; then
            w fill 50 60 95
        else
            w fill 28 31 42
        fi
        w nostroke; w rect $bx $SY $BSW $BSH
        w fill ${PAL_R[$sel_ci]} ${PAL_G[$sel_ci]} ${PAL_B[$sel_ci]}
        w nostroke; w circle $(( bx + BSW/2 )) $(( SY + BSH/2 )) $sz
        if [ $i -eq $sel_si ]; then
            w nofill; w stroke 100 130 200; w strokeweight 1
            w rect $bx $SY $BSW $BSH
        fi
        i=$(( i + 1 ))
    done

    # clear button
    w fill 30 18 18; w nostroke; w rect $CLX $CLY $CLW $CLH
    w nofill; w stroke 110 40 40; w strokeweight 1
    w rect $CLX $CLY $CLW $CLH
    w font ui; w fill 190 70 70
    w text $(( CLX + 16 )) $(( CLY + 23 )) CLEAR
}

# ── initial state ───────────────────────────────────────────────
w clear
draw_palette
w flush

# ── main loop ───────────────────────────────────────────────────
while true; do
    ev=""
    read -t 0.05 -u 3 ev

    case "$ev" in
        "key down q"*|"close") break ;;

        click\ *)
            cx=$(echo "$ev" | cut -d' ' -f2)
            cy=$(echo "$ev" | cut -d' ' -f3)

            if [ $cy -lt $CANVY ]; then
                # paint a dot on canvas
                drawing=1; last_x=$cx; last_y=$cy
                sz=${SIZES[$sel_si]}
                w fill ${PAL_R[$sel_ci]} ${PAL_G[$sel_ci]} ${PAL_B[$sel_ci]}
                w nostroke; w circle $cx $cy $sz
                w flush
            else
                drawing=0
                # color swatch hit?
                if [ $cy -ge $SY ] && [ $cy -lt $(( SY + SH )) ]; then
                    i=0
                    while [ $i -lt $NCOLORS ]; do
                        sx=$(( SX + i * SW ))
                        if [ $cx -ge $sx ] && [ $cx -lt $(( sx + SW - 4 )) ]; then
                            sel_ci=$i; break
                        fi
                        i=$(( i + 1 ))
                    done
                    # brush size hit? (same y band, different x range)
                    i=0
                    while [ $i -lt $NSIZES ]; do
                        bx=$(( BSX + i * (BSW + BSG) ))
                        if [ $cx -ge $bx ] && [ $cx -lt $(( bx + BSW )) ]; then
                            sel_si=$i; break
                        fi
                        i=$(( i + 1 ))
                    done
                fi
                # clear button hit?
                if [ $cx -ge $CLX ] && [ $cx -lt $(( CLX + CLW )) ] && \
                   [ $cy -ge $CLY ] && [ $cy -lt $(( CLY + CLH )) ]; then
                    w clear
                fi
                draw_palette; w flush
            fi
            ;;

        release\ *)
            drawing=0; last_x=-1; last_y=-1
            ;;

        move\ *)
            if [ $drawing -eq 1 ]; then
                cx=$(echo "$ev" | cut -d' ' -f2)
                cy=$(echo "$ev" | cut -d' ' -f3)
                [ $cy -ge $CANVY ] && { last_x=$cx; last_y=$cy; continue; }
                sz=${SIZES[$sel_si]}
                cr=${PAL_R[$sel_ci]}; cg=${PAL_G[$sel_ci]}; cb=${PAL_B[$sel_ci]}
                # line from last point to current (fills gap between move events)
                if [ $last_x -ge 0 ]; then
                    w stroke $cr $cg $cb
                    w strokeweight $(( sz * 2 ))
                    w nofill
                    w line $last_x $last_y $cx $cy
                fi
                # circle cap at current point
                w fill $cr $cg $cb; w nostroke
                w circle $cx $cy $sz
                w flush
                last_x=$cx; last_y=$cy
            fi
            ;;
    esac
done

w close
