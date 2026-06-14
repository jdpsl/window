#!/bin/bash
# demos/bash/sysmon.sh - live system monitor
# reads /proc/stat, /proc/meminfo, /proc/net/dev, /proc/diskstats
cd "$(dirname "$0")/../.."

./window &
WINPID=$!
PIPE="/tmp/window-${WINPID}.pipe"
EVPIPE="/tmp/window-${WINPID}.events"
sleep 0.3

exec 4>"$PIPE"
exec 3<>"$EVPIPE"
w() { echo "$@" >&4; }

W=680
H=460

w size $W $H
w title "sysmon"
w background 10 12 18
w cursor default
w moveevents none
w loadfont ui    14 DejaVu Sans
w loadfont big   20 DejaVu Sans Bold
w loadfont small 11 DejaVu Sans

# ── layout ─────────────────────────────────────────────────────
LX=108     # left edge of all bars
BW=$(( W - LX - 18 ))
BH=20
GAP=12

# ── draw a labeled bar ─────────────────────────────────────────
# draw_bar label x y w h pct r g b
draw_bar() {
    local label="$1" x=$2 y=$3 bw=$4 bh=$5 pct=$6 cr=$7 cg=$8 cb=$9
    [ $pct -lt 0 ] && pct=0; [ $pct -gt 100 ] && pct=100
    local filled=$(( bw * pct / 100 ))
    w fill 20 22 30; w nostroke; w rect $x $y $bw $bh
    [ $filled -gt 0 ] && { w fill $cr $cg $cb; w nostroke; w rect $x $y $filled $bh; }
    w nofill; w stroke 45 50 65; w strokeweight 1; w rect $x $y $bw $bh
    w font small; w fill 120 128 155
    w text $(( x - 100 )) $(( y + bh - 3 )) "$label"
}

# ── format bytes ───────────────────────────────────────────────
fmt() {
    local b=$1
    if   [ $b -ge 1073741824 ]; then echo "$(( b / 1073741824 )).$(( (b % 1073741824) / 107374182 )) GB"
    elif [ $b -ge 1048576    ]; then echo "$(( b / 1048576 )).$(( (b % 1048576) / 104858 )) MB"
    elif [ $b -ge 1024       ]; then echo "$(( b / 1024 )) KB"
    else echo "${b} B"; fi
}

# ── cpu ────────────────────────────────────────────────────────
prev_cpu_active=0
prev_cpu_total=0
declare -a cpu_hist   # last 50 readings

get_cpu() {
    local cpu user nice sys idle iowait irq softirq
    read cpu user nice sys idle iowait irq softirq _ < /proc/stat
    local active=$(( user + nice + sys + irq + softirq ))
    local total=$(( active + idle + iowait ))
    local pct=0
    if [ $prev_cpu_total -gt 0 ]; then
        local da=$(( active - prev_cpu_active ))
        local dt=$(( total  - prev_cpu_total  ))
        [ $dt -gt 0 ] && pct=$(( da * 100 / dt ))
    fi
    prev_cpu_active=$active; prev_cpu_total=$total
    echo $pct
}

# ── memory ─────────────────────────────────────────────────────
get_mem() {
    local total=0 avail=0 key val unit
    while read key val unit; do
        key="${key%:}"
        case $key in MemTotal) total=$val;; MemAvailable) avail=$val;; esac
    done < /proc/meminfo
    echo $total $avail
}

# ── network ────────────────────────────────────────────────────
prev_rx=0; prev_tx=0

# pick first non-loopback interface
NET_IF=$(awk 'NR>2 && !/lo:/ {gsub(/ /,""); sub(/:.*$/,""); print; exit}' /proc/net/dev)

get_net() {
    local rx=0 tx=0
    while IFS=': ' read -r iface r1 r2 r3 r4 r5 r6 r7 r8 t1 rest; do
        iface="${iface//[[:space:]]/}"
        [ "$iface" = "$NET_IF" ] && { rx=$r1; tx=$t1; }
    done < /proc/net/dev
    local drx=$(( rx - prev_rx )); local dtx=$(( tx - prev_tx ))
    [ $drx -lt 0 ] && drx=0; [ $dtx -lt 0 ] && dtx=0
    prev_rx=$rx; prev_tx=$tx
    echo $drx $dtx
}

# ── disk ───────────────────────────────────────────────────────
prev_dr=0; prev_dw=0

get_disk() {
    local sr=0 sw=0
    while read maj min name r_ios r_mg r_sect r_ms w_ios w_mg w_sect rest; do
        [[ "$name" =~ ^(sd[a-z]|vd[a-z]|hd[a-z]|nvme[0-9]n[0-9])$ ]] || continue
        sr=$(( sr + r_sect )); sw=$(( sw + w_sect ))
    done < /proc/diskstats
    local dr=$(( (sr - prev_dr) * 512 ))
    local dw=$(( (sw - prev_dw) * 512 ))
    [ $dr -lt 0 ] && dr=0; [ $dw -lt 0 ] && dw=0
    prev_dr=$sr; prev_dw=$sw
    echo $dr $dw
}

# prime deltas (discard first reading)
get_cpu  > /dev/null
get_net  > /dev/null
get_disk > /dev/null

HOSTNAME=$(hostname)
NET_MAX=10485760    # 10 MB/s = 100%
DISK_MAX=104857600  # 100 MB/s = 100%

# ── main loop (1 Hz) ───────────────────────────────────────────
while true; do

    # ── sample ────────────────────────────────────────────────
    cpu_pct=$(get_cpu)
    read mem_total mem_avail < <(get_mem)
    read net_rx net_tx       < <(get_net)
    read disk_r  disk_w      < <(get_disk)
    read load1 load5 load15 _ < /proc/loadavg
    TIME=$(date +%H:%M:%S)

    # cpu history ring (last 50)
    cpu_hist+=($cpu_pct)
    [ ${#cpu_hist[@]} -gt 50 ] && cpu_hist=("${cpu_hist[@]:1}")

    # derived values
    mem_used=$(( mem_total - mem_avail ))
    mem_pct=$(( mem_used * 100 / mem_total ))

    net_rx_pct=$(( net_rx  * 100 / NET_MAX  )); [ $net_rx_pct  -gt 100 ] && net_rx_pct=100
    net_tx_pct=$(( net_tx  * 100 / NET_MAX  )); [ $net_tx_pct  -gt 100 ] && net_tx_pct=100
    disk_r_pct=$(( disk_r  * 100 / DISK_MAX )); [ $disk_r_pct  -gt 100 ] && disk_r_pct=100
    disk_w_pct=$(( disk_w  * 100 / DISK_MAX )); [ $disk_w_pct  -gt 100 ] && disk_w_pct=100

    # ── draw ──────────────────────────────────────────────────
    w clear

    # header bar
    w fill 16 18 28; w nostroke; w rect 0 0 $W 42
    w nofill; w stroke 38 42 58; w strokeweight 1; w line 0 42 $W 42
    w font big;   w fill 170 180 215; w text 12 29 SYSTEM MONITOR
    w font small; w fill 80 90 120
    w text $(( W - 185 )) 17 $HOSTNAME
    w text $(( W - 185 )) 33 $TIME

    # ── CPU ───────────────────────────────────────────────────
    Y=58

    if   [ $cpu_pct -lt 50 ]; then cr=55;  cg=195; cb=90
    elif [ $cpu_pct -lt 80 ]; then cr=210; cg=170; cb=35
    else                            cr=215; cg=55;  cb=55; fi

    draw_bar "CPU" $LX $Y $BW $BH $cpu_pct $cr $cg $cb
    w font small; w fill 215 215 215
    w text $(( LX + 6 )) $(( Y + BH - 3 )) "${cpu_pct}%   load: $load1  $load5  $load15"

    # cpu sparkline
    SY=$(( Y + BH + 4 ))
    SH=44
    w fill 14 16 24; w nostroke; w rect $LX $SY $BW $SH
    bw=$(( BW / 50 )); [ $bw -lt 1 ] && bw=1
    for i in "${!cpu_hist[@]}"; do
        v=${cpu_hist[$i]}
        bh=$(( SH * v / 100 )); [ $bh -lt 1 ] && [ $v -gt 0 ] && bh=1
        bx=$(( LX + i * bw ))
        by=$(( SY + SH - bh ))
        if   [ $v -lt 50 ]; then w fill 40 155 75
        elif [ $v -lt 80 ]; then w fill 175 145 30
        else                      w fill 175 50  50; fi
        w nostroke; w rect $bx $by $bw $bh
    done
    w nofill; w stroke 38 42 58; w strokeweight 1; w rect $LX $SY $BW $SH
    # 50% guide line
    w stroke 40 45 60; w strokeweight 1
    w line $LX $(( SY + SH/2 )) $(( LX + BW )) $(( SY + SH/2 ))
    w font small; w fill 50 55 75
    w text $(( LX + 3 )) $(( SY + SH/2 - 2 )) 50%

    # ── Memory ────────────────────────────────────────────────
    Y=$(( SY + SH + 14 ))
    draw_bar "MEM" $LX $Y $BW $BH $mem_pct 70 130 210
    w font small; w fill 215 215 215
    w text $(( LX + 6 )) $(( Y + BH - 3 )) \
        "${mem_pct}%   $(fmt $(( mem_used * 1024 ))) / $(fmt $(( mem_total * 1024 )))"

    # ── Network ───────────────────────────────────────────────
    Y=$(( Y + BH + GAP ))
    draw_bar "NET RX" $LX $Y $BW $BH $net_rx_pct 65 195 170
    w font small; w fill 215 215 215
    w text $(( LX + 6 )) $(( Y + BH - 3 )) "$(fmt $net_rx)/s   (${NET_IF})"

    Y=$(( Y + BH + GAP ))
    draw_bar "NET TX" $LX $Y $BW $BH $net_tx_pct 195 155 65
    w font small; w fill 215 215 215
    w text $(( LX + 6 )) $(( Y + BH - 3 )) "$(fmt $net_tx)/s"

    # ── Disk ──────────────────────────────────────────────────
    Y=$(( Y + BH + GAP ))
    draw_bar "DISK R" $LX $Y $BW $BH $disk_r_pct 100 165 245
    w font small; w fill 215 215 215
    w text $(( LX + 6 )) $(( Y + BH - 3 )) "$(fmt $disk_r)/s"

    Y=$(( Y + BH + GAP ))
    draw_bar "DISK W" $LX $Y $BW $BH $disk_w_pct 185 110 245
    w font small; w fill 215 215 215
    w text $(( LX + 6 )) $(( Y + BH - 3 )) "$(fmt $disk_w)/s"

    # footer
    w nofill; w stroke 38 42 58; w strokeweight 1; w line 0 $(( H - 22 )) $W $(( H - 22 ))
    w font small; w fill 55 60 80
    w text 14 $(( H - 7 )) "q: quit   sampling every 1s   net max: 10 MB/s   disk max: 100 MB/s"

    w flush

    # ── wait 1 second (quit on q or close) ────────────────────
    ev=""
    read -t 1 -u 3 ev
    case "$ev" in
        "key down q"*|"close") break ;;
    esac

done

w close
