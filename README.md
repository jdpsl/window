# window

A scriptable X11 display server. Spawn a window, get a pipe, send drawing commands from any language.

```bash
./window &
PIPE="/tmp/window-$!.pipe"
exec 4>"$PIPE"
w() { echo "$@" >&4; }
sleep 0.3

w size 400 300
w title "hello"
w background 20 20 40
w fill 255 200 0
w circle 200 150 80
w flush
```

No SDK. No bindings. Just a pipe.

## how it works

`window` is a single binary with two modes:

- **Server** (`./window`) — opens an X11 window and creates two named FIFOs:
  - `/tmp/window-<pid>.pipe` — you write commands to this
  - `/tmp/window-<pid>.events` — window writes events to this
- **Client** (`./window $PIPE <command>`) — sends one command to a running window

The logic stays in your script. `window` only handles rendering.

## fast animation loops

Each `./window $PIPE cmd` call forks a new process. Fine for one-off commands, too slow for animation. Open the pipe once instead:

```bash
exec 4>"$PIPE"
w() { echo "$@" >&4; }

while true; do
    w clear
    w circle 200 150 $radius
    w flush
    sleep 0.016
done
```

## reading events

```bash
# block until one event arrives
EVENT=$(./window $PIPE wait)

# non-blocking game loop (open events pipe directly)
exec 3<>"/tmp/window-$!.events"
read -t 0.033 -u 3 event
```

Events: `click x y button`, `release x y button`, `scroll x y up|down`, `move x y [button]`, `key down/up name [modifiers]`, `enter x y`, `leave x y`, `resize w h`, `close`

## commands

**Window** — `title`, `size`, `position`, `fullscreen`, `windowed`, `close`

**Buffer** — `buffer=true|false`, `clear`, `flush`

**Style** — `background r g b`, `fill r g b`, `stroke r g b`, `strokeweight n`, `nofill`, `nostroke`
Colors also accept hex: `fill #ff8800`

**Shapes** — `rect`, `circle`, `ellipse`, `arc`, `line`, `point`, `triangle`, `bezier`, `polygon`

**Transforms** — `push`, `pop`, `translate x y`, `rotate deg`, `scale x y`, `identity`

**Text** — `loadfont name size Family Name`, `font name`, `text x y message`

**Images** — `load name file.png`, `draw name x y [w h]`

**Sprites** — `load name strip file sx sy fw fh count`, `drawframe name frame x y [w h]`

**Cursor** — `cursor default|none|crosshair|hand|text|move|wait|image|anim`

**Capture** — `grab x y w h [file.png]` — saves backbuffer region, returns path over events pipe

## multiple windows

Just spawn more than one:

```bash
./window &; PIPE1="/tmp/window-$!.pipe"
./window &; PIPE2="/tmp/window-$!.pipe"
```

Each gets its own PID, pipe, and event stream.

## install

**1. Install dependencies (Debian/Ubuntu/WSL)**
```bash
sudo apt install \
    libx11-dev libxrender-dev libxft-dev libxcursor-dev \
    libfreetype6-dev pkg-config gcc make
```

**2. Build and install**
```bash
make
sudo make install   # copies binary to /usr/local/bin/window
```

**3. Verify**
```bash
window --help
```

## full reference

```bash
window --help
```
