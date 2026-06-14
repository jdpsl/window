#!/bin/bash
# demos/bash/particles.sh - particle system in pure bash
# click to burst, q to quit
cd "$(dirname "$0")/../.."

./window &
WINPID=$!
PIPE="/tmp/window-${WINPID}.pipe"
EVPIPE="/tmp/window-${WINPID}.events"
sleep 0.3

exec 4>"$PIPE"
exec 3<>"$EVPIPE"
w() { echo "$@" >&4; }

W=800
H=550

w size $W $H
w title "particles (bash)  |  click to burst  |  q to quit"
w cursor crosshair
w background 8 8 18
w loadfont ui 13 DejaVu Sans
w moveevents drag

# ── particle pool (fixed slots, PLIFE=0 means dead) ───────────
MAX=30

declare -a PX PY VX VY PR PC_R PC_G PC_B PLIFE PAGE

# color palette
COLORS=(
    "255 80  80"
    "255 160 40"
    "255 220 40"
    "80  255 80"
    "40  200 255"
    "120 100 255"
    "220 80  255"
    "255 80  180"
)
NC=${#COLORS[@]}

# init all slots as dead
for i in $(seq 0 $((MAX-1))); do PLIFE[$i]=0; done

find_slot() {
    for i in $(seq 0 $((MAX-1))); do
        [ "${PLIFE[$i]:-0}" -eq 0 ] && echo $i && return
    done
    echo -1
}

add_particle() {
    local x=$1 y=$2 vx=$3 vy=$4
    local i; i=$(find_slot)
    [ "$i" -eq -1 ] && return
    local col=(${COLORS[$((RANDOM % NC))]})
    PX[$i]=$x;         PY[$i]=$y
    VX[$i]=$vx;        VY[$i]=$vy
    PR[$i]=$(( RANDOM % 8 + 4 ))
    PC_R[$i]=${col[0]}; PC_G[$i]=${col[1]}; PC_B[$i]=${col[2]}
    PLIFE[$i]=$(( RANDOM % 250 + 150 ))
    PAGE[$i]=0
}

burst() {
    local x=$1 y=$2
    for _ in $(seq 1 15); do
        local vx=$(( (RANDOM % 17) - 8 ))
        local vy=$(( (RANDOM % 11) - 9 ))
        add_particle "$x" "$y" "$vx" "$vy"
    done
}

# seed initial particles
for _ in $(seq 1 20); do
    px=$(( RANDOM % (W - 100) + 50 ))
    py=$(( RANDOM % (H / 3)  + 50 ))
    vx=$(( (RANDOM % 8) - 4 ))
    vy=$(( (RANDOM % 6) - 4 ))
    add_particle $px $py $vx $vy
done

tick=0
clicks=0

while true; do
    tick=$(( tick + 1 ))

    # ── read event (33ms timeout = frame pacing) ──────────────
    # ignore move events - they flood the pipe and cause speed spikes
    ev=""
    read -t 0.033 -u 3 ev
    case "$ev" in
        "key down q"*) break ;;
        click\ *)
            cx=$(echo "$ev" | cut -d' ' -f2)
            cy=$(echo "$ev" | cut -d' ' -f3)
            burst "$cx" "$cy"
            clicks=$(( clicks + 1 ))
            ;;
        "close") break ;;
    esac

    # ── update particles ───────────────────────────────────────
    alive=0
    for i in $(seq 0 $((MAX-1))); do
        [ "${PLIFE[$i]:-0}" -eq 0 ] && continue

        PAGE[$i]=$(( PAGE[$i] + 1 ))
        [ "${PAGE[$i]}" -ge "${PLIFE[$i]}" ] && { PLIFE[$i]=0; continue; }

        # gravity
        VY[$i]=$(( VY[$i] + 1 ))

        px=$(( PX[$i] + VX[$i] ))
        py=$(( PY[$i] + VY[$i] ))
        r=${PR[$i]}

        # bounce off walls with damping
        if [ $px -lt $r ]; then
            px=$r
            VX[$i]=$(( -VX[$i] * 8 / 10 ))
        fi
        if [ $px -gt $(( W - r )) ]; then
            px=$(( W - r ))
            VX[$i]=$(( -VX[$i] * 8 / 10 ))
        fi
        if [ $py -lt $r ]; then
            py=$r
            VY[$i]=$(( -VY[$i] * 8 / 10 ))
        fi
        if [ $py -gt $(( H - 40 - r )) ]; then
            py=$(( H - 40 - r ))
            VY[$i]=$(( -VY[$i] * 8 / 10 ))
        fi

        PX[$i]=$px
        PY[$i]=$py
        alive=$(( alive + 1 ))
    done

    # top up if too few alive
    while [ $alive -lt 12 ]; do
        px=$(( RANDOM % (W - 100) + 50 ))
        py=$(( RANDOM % (H / 3)  + 50 ))
        vx=$(( (RANDOM % 8) - 4 ))
        vy=$(( (RANDOM % 8) - 6 ))
        add_particle $px $py $vx $vy
        alive=$(( alive + 1 ))
    done

    # ── draw ──────────────────────────────────────────────────
    w clear

    # floor line
    w nofill
    w stroke 40 40 60
    w strokeweight 1
    w line 0 $(( H - 40 )) $W $(( H - 40 ))

    # particles
    for i in $(seq 0 $((MAX-1))); do
        [ "${PLIFE[$i]:-0}" -eq 0 ] && continue
        age=${PAGE[$i]}
        life=${PLIFE[$i]}
        # fade color by remaining life
        cr=$(( PC_R[$i] * (life - age) / life ))
        cg=$(( PC_G[$i] * (life - age) / life ))
        cb=$(( PC_B[$i] * (life - age) / life ))
        w fill $cr $cg $cb
        w nostroke
        w circle ${PX[$i]} ${PY[$i]} ${PR[$i]}
    done

    # HUD
    w font ui
    w fill 100 100 120
    w text 8 $(( H - 14 )) particles: $alive  clicks: $clicks  tick: $tick    click to burst  \|  q to quit

    w flush
done

w close
