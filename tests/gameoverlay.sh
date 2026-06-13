#!/bin/bash
# gameoverlay.sh - fake game HUD overlay demo
cd "$(dirname "$0")/.."

./window &
WINPID=$!
PIPE="/tmp/window-${WINPID}.pipe"
w() { ./window "$PIPE" "$@"; }
sleep 0.3

W=800
H=600

w size $W $H
w title "game overlay demo"
w background 20 20 30

# load fonts
w loadfont ui    18 DejaVu Sans
w loadfont big   32 DejaVu Sans Bold
w loadfont small 13 DejaVu Sans

# game state
score=0
health=100
max_health=100
ammo=30
max_ammo=30
wave=1
kills=0
frame=0
compass=0

# enemy dot on minimap
ex=20
ey=20
edx=1
edy=1

# health pulse
pulse=0
pulse_dir=1

while true; do
    frame=$(( frame + 1 ))

    # ── update game state ──────────────────────────────────────
    score=$(( score + 7 ))
    compass=$(( (compass + 2) % 360 ))
    pulse=$(( pulse + pulse_dir ))
    [ $pulse -ge 10 ] && pulse_dir=-1
    [ $pulse -le 0  ] && pulse_dir=1

    # enemy moves on minimap
    ex=$(( ex + edx ))
    ey=$(( ey + edy ))
    [ $ex -ge 58 ] || [ $ex -le 2 ] && edx=$(( -edx ))
    [ $ey -ge 58 ] || [ $ey -le 2 ] && edy=$(( -edy ))

    # health slowly drains then resets
    health=$(( health - 1 ))
    [ $health -le 0 ] && health=100 && kills=$(( kills + 1 )) && wave=$(( kills / 5 + 1 ))

    # ammo decrements
    ammo=$(( ammo - 1 ))
    [ $ammo -le 0 ] && ammo=30

    # ── draw ──────────────────────────────────────────────────
    w clear

    # ── fake game background (grid) ───────────────────────────
    w nofill
    w stroke 30 30 50
    w strokeweight 1
    x=0
    while [ $x -le $W ]; do
        w line $x 0 $x $H
        x=$(( x + 80 ))
    done
    y=0
    while [ $y -le $H ]; do
        w line 0 $y $W $y
        y=$(( y + 80 ))
    done

    # ── crosshair (center) ────────────────────────────────────
    cx=$(( W / 2 ))
    cy=$(( H / 2 ))
    w nofill
    w stroke 255 255 255
    w strokeweight 1
    w line $(( cx - 20 )) $cy $(( cx - 5 )) $cy
    w line $(( cx + 5  )) $cy $(( cx + 20 )) $cy
    w line $cx $(( cy - 20 )) $cx $(( cy - 5 ))
    w line $cx $(( cy + 5  )) $cx $(( cy + 20 ))
    w nofill
    w stroke 255 255 255
    w circle $cx $cy 8

    # ── health bar (bottom left) ──────────────────────────────
    bx=20
    by=$(( H - 60 ))
    bw=200
    bh=18

    # health color: green → yellow → red
    if [ $health -gt 60 ]; then
        hr=50;  hg=220; hb=50
    elif [ $health -gt 30 ]; then
        hr=220; hg=180; hb=20
    else
        # pulse red when low
        hr=$(( 180 + pulse * 7 ))
        hg=20; hb=20
    fi

    # background
    w fill 10 10 10
    w nostroke
    w rect $bx $by $bw $bh

    # health fill
    filled=$(( bw * health / max_health ))
    w fill $hr $hg $hb
    w rect $bx $by $filled $bh

    # border
    w nofill
    w stroke 200 200 200
    w strokeweight 1
    w rect $bx $by $bw $bh

    # label
    w font ui
    w fill 255 255 255
    w text $(( bx + 5 )) $(( by + 14 )) HP: $health / $max_health

    # ── ammo counter (bottom left, below health) ───────────────
    ay_pos=$(( H - 30 ))
    w font ui
    w fill 220 200 80
    w text $bx $ay_pos AMMO:

    # ammo pips
    pip=0
    pipx=$(( bx + 65 ))
    while [ $pip -lt $max_ammo ]; do
        pw=5
        ph=12
        px=$(( pipx + pip * 7 ))
        py=$(( ay_pos - 12 ))
        if [ $pip -lt $ammo ]; then
            w fill 220 200 80
        else
            w fill 40 40 40
        fi
        w nostroke
        w rect $px $py $pw $ph
        pip=$(( pip + 1 ))
    done

    # ── score (top right) ─────────────────────────────────────
    w font big
    w fill 255 220 50
    w text $(( W - 220 )) 45 SCORE: $score

    w font small
    w fill 180 180 180
    w text $(( W - 180 )) 68 WAVE $wave   KILLS: $kills

    # ── wave banner (top center) ──────────────────────────────
    w font ui
    w fill 100 200 255
    w text $(( W/2 - 50 )) 30 WAVE $wave

    # ── minimap (bottom right) ────────────────────────────────
    mx=$(( W - 90 ))
    my=$(( H - 90 ))
    mr=60

    # background circle
    w fill 10 10 20
    w stroke 100 100 150
    w strokeweight 1
    w circle $mx $my $mr

    # grid lines on minimap
    w nofill
    w stroke 30 30 60
    w strokeweight 1
    w line $(( mx - mr )) $my $(( mx + mr )) $my
    w line $mx $(( my - mr )) $mx $(( my + mr ))

    # enemy dot
    esx=$(( mx - 50 + ex ))
    esy=$(( my - 50 + ey ))
    w fill 255 60 60
    w nostroke
    w circle $esx $esy 4

    # player dot (center)
    w fill 80 220 80
    w circle $mx $my 5

    # compass needle using triangle
    rad_cos=$(echo "scale=4; c($compass * 3.14159 / 180)" | bc -l 2>/dev/null || echo "1")
    rad_sin=$(echo "scale=4; s($compass * 3.14159 / 180)" | bc -l 2>/dev/null || echo "0")
    nx=$(echo "$rad_cos * 50 / 1 + $mx" | bc 2>/dev/null || echo $mx)
    ny=$(echo "$rad_sin * 50 / 1 + $my" | bc 2>/dev/null || echo $(( my - 50 )))
    w fill 255 120 0
    w nostroke
    w triangle $mx $my $nx $ny $(( mx + 4 )) $(( my + 4 ))

    # minimap border
    w nofill
    w stroke 150 150 200
    w strokeweight 2
    w circle $mx $my $mr

    # N label
    w font small
    w fill 200 200 255
    w text $(( mx - 4 )) $(( my - mr + 14 )) N

    # ── wave indicator triangles (top left) ───────────────────
    w font small
    w fill 150 220 255
    w text 20 25 WAVE

    t=0
    while [ $t -lt $wave ] && [ $t -lt 10 ]; do
        tx_pos=$(( 65 + t * 16 ))
        w fill 100 180 255
        w nostroke
        w triangle $tx_pos 8  $(( tx_pos + 12 )) 8  $(( tx_pos + 6 )) 22
        t=$(( t + 1 ))
    done

    # ── status effects (right side) ───────────────────────────
    w font small
    w fill 255 100 100
    w text $(( W - 120 )) $(( H/2 - 20 )) !! DANGER !!

    w fill 100 255 100
    w text $(( W - 110 )) $(( H/2 + 10 )) STEALTH

    # ── frame counter ─────────────────────────────────────────
    w font small
    w fill 80 80 80
    w text 5 $(( H - 5 )) frame: $frame

    w flush
    sleep 0.05
done
