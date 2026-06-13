#!/usr/bin/env python3
"""
demo_particles.py - particle system using window pipe
shows rapid prototyping in Python: real physics, 50+ objects, interactive

  python3 demo_particles.py

click to burst particles, q to quit
"""

import subprocess, os, time, math, random, select

# ── spawn window ───────────────────────────────────────────────
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
proc = subprocess.Popen([os.path.join(ROOT, 'window')])
pid  = proc.pid
pipe_path   = f'/tmp/window-{pid}.pipe'
events_path = f'/tmp/window-{pid}.events'
time.sleep(0.4)

# open command pipe (write), events pipe (read, non-blocking)
cmd  = open(pipe_path, 'w', buffering=1)
evfd = os.open(events_path, os.O_RDONLY | os.O_NONBLOCK)

def w(*args):
    cmd.write(' '.join(str(a) for a in args) + '\n')

def flush():
    cmd.flush()

# ── setup ──────────────────────────────────────────────────────
W, H = 800, 550

w('size', W, H)
w('title', 'particles  |  click to burst  |  q to quit')
w('cursor', 'crosshair')
w('background', 8, 8, 18)
w('loadfont', 'ui',    13, 'DejaVu Sans')
w('loadfont', 'big',   28, 'DejaVu Sans Bold')
flush()

# ── particle ───────────────────────────────────────────────────

def hsv_to_rgb(h, s=1.0, v=1.0):
    h6 = (h % 1.0) * 6
    i  = int(h6)
    f  = h6 - i
    q  = v * (1 - f)
    t  = v * f
    rgb = [(v,t,0),(q,v,0),(0,v,t),(0,q,v),(t,0,v),(v,0,q)][i % 6]
    return int(rgb[0]*255), int(rgb[1]*255), int(rgb[2]*255)

class Particle:
    def __init__(self, x, y, vx=None, vy=None, radius=None):
        self.x  = float(x)
        self.y  = float(y)
        self.vx = vx if vx is not None else random.uniform(-4, 4)
        self.vy = vy if vy is not None else random.uniform(-6, 2)
        self.r  = radius or random.randint(4, 14)
        self.hue = random.random()
        self.life = random.randint(180, 500)
        self.age  = 0

    def update(self):
        self.vy  += 0.12          # gravity
        self.vx  *= 0.999         # air resistance
        self.x   += self.vx
        self.y   += self.vy
        self.age += 1

        # bounce off walls
        if self.x - self.r < 0:
            self.x  = float(self.r);   self.vx =  abs(self.vx) * 0.8
        if self.x + self.r > W:
            self.x  = float(W - self.r); self.vx = -abs(self.vx) * 0.8
        if self.y - self.r < 0:
            self.y  = float(self.r);   self.vy =  abs(self.vy) * 0.8
        if self.y + self.r > H - 40:
            self.y  = float(H - 40 - self.r); self.vy = -abs(self.vy) * 0.75

    def alive(self):
        return self.age < self.life

    def draw(self):
        alpha  = 1.0 - self.age / self.life
        speed  = math.hypot(self.vx, self.vy)
        # shift hue toward red as speed increases
        hue    = (self.hue + speed * 0.02) % 1.0
        cr, cg, cb = hsv_to_rgb(hue, 1.0, alpha)
        w('fill', cr, cg, cb)
        w('nostroke')
        w('circle', int(self.x), int(self.y), max(1, int(self.r * alpha)))


def burst(x, y, count=30):
    for _ in range(count):
        speed = random.uniform(3, 12)
        angle = random.uniform(0, math.pi * 2)
        r     = random.randint(3, 10)
        particles.append(Particle(
            x, y,
            vx=math.cos(angle) * speed,
            vy=math.sin(angle) * speed,
            radius=r
        ))


# ── initial particles ──────────────────────────────────────────
particles = []
for _ in range(50):
    particles.append(Particle(
        random.randint(50, W-50),
        random.randint(50, H//2)
    ))

# ── main loop ──────────────────────────────────────────────────
tick      = 0
clicks    = 0
running   = True
event_buf = ''
frame_t   = time.monotonic()

while running:
    tick += 1

    # ── events (non-blocking read from fd) ────────────────────
    try:
        event_buf += os.read(evfd, 4096).decode()
    except BlockingIOError:
        pass

    while '\n' in event_buf:
        line, event_buf = event_buf.split('\n', 1)
        line = line.strip()
        if not line:
            continue
        parts = line.split()
        if parts[0] == 'click':
            burst(int(parts[1]), int(parts[2]), 40)
            clicks += 1
        elif parts[:2] == ['key', 'down'] and parts[2] == 'q':
            running = False
        elif parts[0] == 'close':
            running = False

    # ── physics ───────────────────────────────────────────────
    particles = [p for p in particles if p.alive()]
    # keep a minimum count
    while len(particles) < 30:
        particles.append(Particle(
            random.randint(100, W-100),
            random.randint(50, H//3)
        ))
    for p in particles:
        p.update()

    # ── connect nearby particles with lines ───────────────────
    # only check a subset each frame to keep it fast
    connections = []
    sample = particles[:40]
    for i, a in enumerate(sample):
        for b in sample[i+1:]:
            d = math.hypot(a.x - b.x, a.y - b.y)
            if d < 80:
                connections.append((a, b, d))

    # ── draw ──────────────────────────────────────────────────
    w('clear')

    # connecting lines (dim, based on distance)
    for a, b, d in connections:
        alpha = int((1 - d / 80) * 60)
        w('stroke', alpha, alpha, alpha + 20)
        w('strokeweight', 1)
        w('nofill')
        w('line', int(a.x), int(a.y), int(b.x), int(b.y))

    # particles
    for p in particles:
        p.draw()

    # floor line
    w('stroke', 40, 40, 60)
    w('strokeweight', 1)
    w('nofill')
    w('line', 0, H-40, W, H-40)

    # fps
    now    = time.monotonic()
    dt     = now - frame_t
    frame_t = now
    fps    = int(1.0 / dt) if dt > 0 else 0

    # HUD
    w('font', 'ui')
    w('fill', 100, 100, 120)
    w('text', 8, H-14,
      f'particles: {len(particles)}   connections: {len(connections)}'
      f'   clicks: {clicks}   fps: {fps}   tick: {tick}'
      f'     click to burst  |  q to quit')

    w('flush')
    flush()

    # frame cap ~60fps
    elapsed = time.monotonic() - now
    sleep   = max(0.0, 0.016 - elapsed)
    if sleep:
        time.sleep(sleep)

# ── cleanup ───────────────────────────────────────────────────
w('close')
flush()
cmd.close()
os.close(evfd)
