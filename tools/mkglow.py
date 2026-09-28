#!/usr/bin/env python3
"""
mkglow.py - GlowIcons-style Workbench icons for NilBBS and NilTerm.

    python tools/mkglow.py [--png]

Each icon is drawn in true colour at 4x (supersampled), downsampled with anti-aliased
edges (no black outlines), given a soft drop shadow, and emitted as a DUAL icon
(mkicons.py's hand-drawn 4-colour image for OS 3.0/3.1 + the OS 3.5+ colour image) with its own
selected image: the gold GlowIcons halo plus a lit-up variant of the drawing.
The pipeline is C:/projects/tools/makeicon_glow.py (the AmiTools icons); this file
only has NilBBS's drawings.  Writes the program icons into dist/BBS/ (replacing
mkicons.py's classic ones); --png also writes previews and a contact sheet to test/icons/.
"""
import math, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, r'C:\projects\tools')
import iconlib
from makeicon_glow import (W, H, SS, hcanvas, hset, lerp, V, hrect, hvgrad, hdisc, hline,
                           dk, lt, downsample, compose, quantise, planar_of)

# ---- extra drawing helpers (virtual 48x40 coordinates, drawn at SS x) --------------------
def inside_round(x, y, x0, y0, x1, y1, r):
    cx = min(max(x, x0 + r), x1 + 1 - r)
    cy = min(max(y, y0 + r), y1 + 1 - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r

def hround(cv, x0, y0, x1, y1, r, top, bot, rim=None, hi=0.45):
    """rounded panel: vertical gradient, light top edge, soft darker rim"""
    for hy in range(V(y0), V(y1 + 1)):
        t = (hy - V(y0)) / max(1, V(y1 + 1) - V(y0) - 1)
        base = lerp(top, bot, t)
        for hx in range(V(x0), V(x1 + 1)):
            vx, vy = (hx + 0.5) / SS, (hy + 0.5) / SS
            if not inside_round(vx, vy, x0, y0, x1, y1, r):
                continue
            edge = not inside_round(vx, vy, x0 + 0.6, y0 + 0.6, x1 - 0.6, y1 - 0.6, max(0.1, r - 0.6))
            if edge:
                col = rim if rim else dk(bot, 0.7)
                if vy < y0 + r + 0.2 and vy < (y0 + y1) / 2:
                    col = lt(top, hi)            # the lit top rim
            else:
                col = base
            hset(cv, hx, hy, col)

def hpoly(cv, pts, col):
    """filled polygon (virtual coords); col = rgb or fn(vx, vy) -> rgb"""
    xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
    for hy in range(V(min(ys)) - 1, V(max(ys)) + 2):
        vy = (hy + 0.5) / SS
        for hx in range(V(min(xs)) - 1, V(max(xs)) + 2):
            vx = (hx + 0.5) / SS
            inside = False
            j = len(pts) - 1
            for i in range(len(pts)):
                xi, yi = pts[i]; xj, yj = pts[j]
                if (yi > vy) != (yj > vy) and vx < (xj - xi) * (vy - yi) / (yj - yi) + xi:
                    inside = not inside
                j = i
            if inside:
                hset(cv, hx, hy, col(vx, vy) if callable(col) else col)

def ball(c0, c1, cx, cy, r):
    """a shaded sphere-ish disc: light top-left, dark bottom-right"""
    def f(dx, dy, d):
        t = min(1.0, max(0.0, 0.5 + (dx + dy) / (2.6 * r)))
        c = lerp(c0, c1, t)
        if d > r - 0.7: c = dk(c1, 0.8)                     # rim
        return c
    return f

def gear(cv, cx, cy, r_out, r_in, teeth, rot, c0, c1, hole):
    pts = []
    for i in range(teeth * 4):
        a = rot + i * (2 * math.pi / (teeth * 4))
        rr = r_out if (i % 4) in (1, 2) else r_in
        pts.append((cx + rr * math.cos(a), cy + rr * math.sin(a)))
    hpoly(cv, pts, lambda vx, vy: lerp(c0, c1, min(1, max(0, 0.5 + ((vx - cx) + (vy - cy)) / (2.4 * r_out)))))
    hdisc(cv, cx, cy, hole, lambda dx, dy, d: (70, 74, 84) if d > hole - 0.8 else (46, 50, 58))

# ---- colours ------------------------------------------------------------------------------
CASE0 = (238, 234, 224); CASE1 = (178, 172, 160)
MET0 = (226, 230, 236); MET1 = (128, 132, 142)
SCR0 = (30, 40, 70); SCR1 = (10, 14, 30)
SCRLIT0 = (46, 66, 120); SCRLIT1 = (18, 26, 56)
CYAN = (90, 224, 236); YEL = (250, 222, 96); MAG = (232, 116, 222); GRN = (112, 232, 120)
RED = (236, 88, 76); BLUE = (92, 152, 250); WHITE = (250, 250, 250); GREY = (150, 156, 168)
GOLD0 = (255, 226, 120); GOLD1 = (196, 140, 36)
GREEN0 = (140, 236, 120); GREEN1 = (40, 150, 56)
RED0 = (255, 140, 120); RED1 = (170, 40, 34)
PAPER0 = (248, 249, 252); PAPER1 = (206, 210, 222)

def crt(cv, x0, y0, x1, y1, lit):
    """a beige CRT monitor body with a dark screen; returns the screen box"""
    hround(cv, x0, y0, x1, y1, 3, CASE0, CASE1)
    hround(cv, x0 + 2, y0 + 2, x1 - 2, y1 - 4, 2, dk(CASE1, 0.75), dk(CASE0, 0.8))   # bezel
    sx0, sy0, sx1, sy1 = x0 + 3, y0 + 3, x1 - 3, y1 - 5
    hround(cv, sx0, sy0, sx1, sy1, 1.5, SCRLIT0 if lit else SCR0, SCRLIT1 if lit else SCR1)
    hdisc(cv, x1 - 3, y1 - 2, 0.9, lambda dx, dy, d: GRN if lit else (70, 170, 80))   # power LED
    return sx0, sy0, sx1, sy1

# ---- the icons ----------------------------------------------------------------------------
def nilterm(cv, sel):
    sx0, sy0, sx1, sy1 = crt(cv, 5, 2, 42, 29, sel)
    # ANSI on the screen: a colour bar, text lines, a block cursor
    x = sx0 + 2
    for c in (CYAN, YEL, MAG, GRN, RED, BLUE):
        hrect(cv, x, sy0 + 2, x + 3, sy0 + 3, lt(c, 0.25) if sel else c); x += 4
    rows = [(GRN, 14), (WHITE, 18), (MAG, 10), (CYAN, 16)]
    for i, (c, n) in enumerate(rows):
        hrect(cv, sx0 + 2, sy0 + 6 + i * 3, sx0 + 2 + n, sy0 + 6 + i * 3, lt(c, 0.3) if sel else dk(c, 0.85))
    hrect(cv, sx0 + 2, sy1 - 3, sx0 + 3, sy1 - 2, WHITE if sel else (200, 204, 214))
    hrect(cv, 20, 30, 27, 31, dk(CASE1, 0.8))                  # neck
    hround(cv, 14, 32, 33, 34, 1, MET0, MET1)                  # foot

def bbscontrol(cv, sel):
    hround(cv, 4, 5, 43, 33, 3, MET0, MET1)
    hround(cv, 7, 8, 24, 20, 1.5, SCRLIT0 if sel else SCR0, SCRLIT1 if sel else SCR1)   # status screen
    for i, n in enumerate((12, 8, 14, 6)):
        hrect(cv, 9, 10 + i * 2.5, 9 + n, 10 + i * 2.5, lt(GRN, 0.3) if sel else dk(GRN, 0.85))
    leds = [GRN, GRN, YEL, None] if not sel else [GRN, GRN, GRN, YEL]
    for i, c in enumerate(leds):
        y = 9 + i * 5
        hround(cv, 27, y - 1, 40, y + 2, 1, dk(MET1, 0.7), dk(MET1, 0.55))   # a node's slot
        col = c if c else (60, 64, 72)
        hdisc(cv, 29.2, y + 0.5, 1.4, ball(lt(col, 0.5), dk(col, 0.7), 0, 0, 1.4))
        hrect(cv, 32, y, 32 + (6 if c else 2), y + 0.8, lt(col, 0.2) if (c and sel) else (104, 108, 118))
    for i in range(5):                                        # vents
        hrect(cv, 8 + i * 3, 24, 9 + i * 3, 30, dk(MET1, 0.72))

def bbsconfig(cv, sel):
    hround(cv, 4, 4, 30, 34, 2, PAPER0, PAPER1)
    hrect(cv, 4, 4, 30, 8, (104, 140, 200)); hrect(cv, 6, 6, 8, 7, WHITE)   # title bar
    knobs = (18, 10, 22) if not sel else (12, 20, 16)
    for i, kx in enumerate(knobs):
        y = 13 + i * 7
        hrect(cv, 8, y, 26, y + 0.7, (128, 132, 142))
        hround(cv, kx - 1.5, y - 2.2, kx + 1.5, y + 2.8, 0.8, lt(BLUE, 0.3), dk(BLUE, 0.7))
    gear(cv, 33, 26, 10, 7.2, 8, math.radians(22.5 if sel else 0), MET0, MET1, 3)

def bbsschedule(cv, sel):
    hround(cv, 3, 6, 31, 34, 2, PAPER0, PAPER1)
    hround(cv, 3, 6, 31, 12, 2, lt(RED, 0.2), dk(RED, 0.8))                # the red header
    for x in (9, 25):                                          # rings
        hround(cv, x - 1, 3, x + 1, 9, 0.8, MET0, MET1)
    red = (2, 1) if not sel else (3, 2)
    for r in range(4):
        for c in range(5):
            x, y = 6 + c * 5, 15 + r * 4.5
            hrect(cv, x, y, x + 3, y + 2.5, dk(RED, 0.9) if (c, r) == red else (212, 216, 226))
    hdisc(cv, 34, 26, 10, ball(GOLD0, GOLD1, 0, 0, 10))       # the clock
    hdisc(cv, 34, 26, 7.6, lambda dx, dy, d: lerp(WHITE, (218, 222, 232), min(1, d / 7.6)))
    for i in range(12):
        a = i * math.pi / 6
        hdisc(cv, 34 + 6.3 * math.cos(a), 26 + 6.3 * math.sin(a), 0.45, lambda dx, dy, d: (90, 96, 110))
    h, m = (math.radians(-60), math.radians(-150)) if not sel else (math.radians(30), math.radians(-90))
    hline(cv, 34, 26, 34 + 4 * math.cos(h), 26 + 4 * math.sin(h), (50, 54, 64), 1.3)
    hline(cv, 34, 26, 34 + 6 * math.cos(m), 26 + 6 * math.sin(m), (50, 54, 64), 0.9)
    hdisc(cv, 34, 26, 0.9, lambda dx, dy, d: RED)

def tower(cv, sel):
    hround(cv, 12, 2, 35, 34, 3, MET0, MET1)
    for y in (6, 10):                                          # drive bays
        hround(cv, 15, y, 32, y + 2.5, 0.8, dk(MET1, 0.75), dk(MET1, 0.6))
        hrect(cv, 29, y + 1, 30, y + 1.5, GRN if sel else (80, 170, 90))
    hrect(cv, 15, 31, 32, 31.5, dk(MET1, 0.7))

def start(cv, sel):
    tower(cv, sel)
    c0, c1 = (lt(GREEN0, 0.25), GREEN1) if sel else (GREEN0, dk(GREEN1, 0.85))
    hdisc(cv, 23.5, 22, 7, ball(c0, c1, 0, 0, 7))
    hpoly(cv, [(21, 18.3), (21, 25.7), (27.4, 22)], WHITE)

def stop(cv, sel):
    tower(cv, sel)
    c0, c1 = (lt(RED0, 0.25), RED1) if sel else (RED0, dk(RED1, 0.85))
    oct_ = [(23.5 + 7 * math.cos(math.radians(22.5 + 45 * i)), 22 + 7 * math.sin(math.radians(22.5 + 45 * i))) for i in range(8)]
    hpoly(cv, oct_, lambda vx, vy: lerp(c0, c1, min(1, max(0, 0.5 + ((vx - 23.5) + (vy - 22)) / 16))))
    hrect(cv, 20.5, 19, 26.5, 25, WHITE)

def logon(cv, sel):
    sx0, sy0, sx1, sy1 = crt(cv, 10, 1, 38, 21, sel)
    hrect(cv, sx0 + 2, sy0 + 2, sx0 + 3, sy0 + 2.5, GRN)     # a prompt: >_
    hline(cv, sx0 + 2, sy0 + 2, sx0 + 4, sy0 + 3.5, lt(GRN, 0.3) if sel else GRN, 0.8)
    hline(cv, sx0 + 4, sy0 + 3.5, sx0 + 2, sy0 + 5, lt(GRN, 0.3) if sel else GRN, 0.8)
    hrect(cv, sx0 + 6, sy0 + 5, sx0 + 9, sy0 + 5.6, WHITE if sel else (200, 204, 214))
    hround(cv, 3, 25, 41, 34, 2, MET0, MET1)                   # keyboard
    for r in range(3):
        for c in range(11):
            hrect(cv, 5.5 + c * 3.2, 27 + r * 2.3, 7.3 + c * 3.2, 28.3 + r * 2.3, lt(MET1, 0.55))
    # a gold key over the keyboard's corner
    hdisc(cv, 39, 25, 3.6, ball(GOLD0, GOLD1, 0, 0, 3.6))
    hdisc(cv, 39, 25, 1.3, lambda dx, dy, d: dk(GOLD1, 0.7))   # the key's hole
    hline(cv, 36, 27.5, 29, 33, GOLD1 if not sel else GOLD0, 1.6)
    hline(cv, 31, 31.5, 32.5, 33.5, GOLD1, 1.0); hline(cv, 33, 30, 34.5, 32, GOLD1, 1.0)

def maint(cv, sel):
    hround(cv, 3, 17, 34, 33, 2.5, MET0, MET1)                 # a hard drive
    hround(cv, 6, 20, 24, 25, 1, (242, 244, 248), (196, 200, 210))   # its label
    for i in range(3):
        hrect(cv, 8, 21.3 + i * 1.3, 20 - i * 3, 21.6 + i * 1.3, (140, 146, 160))
    hdisc(cv, 30, 29.5, 1.1, lambda dx, dy, d: GRN if sel else (70, 120, 80))
    # a gold wrench across it: a round-ended bar, then the head with its jaw cut open
    g0 = lt(GOLD0, 0.2) if sel else GOLD0
    x0, y0, x1, y1 = 14, 9, 40, 31
    for i in range(61):
        t = i / 60
        hdisc(cv, x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, 2.0, ball(g0, GOLD1, 0, 0, 2.0))
    hdisc(cv, x0, y0, 5.2, ball(g0, GOLD1, 0, 0, 5.2))
    for hy in range(V(y0 - 4), V(y0 + 4)):                     # the open jaw (transparent)
        for hx in range(V(x0 - 7), V(x0 - 0.8)):
            vx, vy = (hx + .5) / SS, (hy + .5) / SS
            if abs(vy - y0) < 1.7 + (x0 - 0.8 - vx) * 0.25:
                hset(cv, hx, hy, (0, 0, 0), 0.0)

import mkicons                               # its drawings = the classic images
CLASSIC = {'NilTerm': mkicons.term_icon, 'BBSControl': mkicons.control_icon, 'BBSConfig': mkicons.config_icon,
           'BBSSchedule': mkicons.schedule_icon, 'Start NilBBS': mkicons.start_icon, 'Stop NilBBS': mkicons.stop_icon,
           'Local Logon': mkicons.logon_icon, 'Run Maintenance': mkicons.maint_icon}

GLYPH = {'NilTerm': nilterm, 'BBSControl': bbscontrol, 'BBSConfig': bbsconfig, 'BBSSchedule': bbsschedule,
         'Start NilBBS': start, 'Stop NilBBS': stop, 'Local Logon': logon, 'Run Maintenance': maint}
PROJECT = {   # IconX script icons keep their tool types
    'Start NilBBS': ['WINDOW=NIL:'], 'Stop NilBBS': ['DELAY=2'], 'Local Logon': ['WINDOW=NIL:'],
    'Run Maintenance': ['WINDOW=CON:0/20/640/220/NilBBS Maintenance/CLOSE/WAIT']}

def render(fn, sel):
    cv = hcanvas(); fn(cv, sel)
    return compose(downsample(cv), glow=sel)

def build(name):
    rn, tn = render(GLYPH[name], False); rs, ts = render(GLYPH[name], True)
    cn, pal = quantise(rn, tn)
    def qk(v): return int(round(v / 6.0) * 6)
    idx = {c: i for i, c in enumerate(pal)}
    cs = [[0] * W for _ in range(H)]
    for y in range(H):
        for x in range(W):
            if ts[y][x]: continue
            k = (qk(rs[y][x][0]), qk(rs[y][x][1]), qk(rs[y][x][2]))
            if k not in idx: idx[k] = len(pal); pal.append(k)
            cs[y][x] = idx[k]
    kw = {}
    if name in PROJECT: kw = dict(icon_type=4, default_tool='C:IconX', tool_types=PROJECT[name])
    # the 4-colour (OS 3.0/3.1) image is mkicons.py's hand-drawn one: glow shading doesn't survive 4 pens
    return iconlib.build_info(cn, pal, planar_of(pal), W, H, 0, cidx_sel=cs,
                              classic=CLASSIC[name](), classic_map=mkicons.PLANAR_MAP, **kw), cn, cs, pal

def main():
    png = '--png' in sys.argv
    sheet = []
    for name in GLYPH:
        data, cn, cs, pal = build(name)
        path = os.path.join(ROOT, 'dist', 'BBS', name + '.info')
        open(path, 'wb').write(data)
        print('wrote %s (%d bytes, %d colours)' % (path, len(data), len(pal)))
        if png: sheet.append((name, cn, cs, pal))
    if png:
        from PIL import Image, ImageDraw
        S, cw = 4, 48 * 4 + 16
        img = Image.new('RGB', (cw * len(sheet), 40 * S * 2 + 40), (149, 149, 149))
        d = ImageDraw.Draw(img)
        for k, (name, cn, cs, pal) in enumerate(sheet):
            for row, grid in enumerate((cn, cs)):
                for y in range(H):
                    for x in range(W):
                        c = pal[grid[y][x]] if grid[y][x] else (149, 149, 149)
                        d.rectangle([k * cw + 8 + x * S, 4 + row * (40 * S + 16) + y * S,
                                     k * cw + 8 + x * S + S - 1, 4 + row * (40 * S + 16) + y * S + S - 1], fill=tuple(c))
            d.text((k * cw + 8, 40 * S * 2 + 24), name, fill=(0, 0, 0))
        os.makedirs(os.path.join(ROOT, 'test', 'icons'), exist_ok=True)
        out = os.path.join(ROOT, 'test', 'icons', 'glow_sheet.png')
        img.save(out); print('sheet', out)

if __name__ == '__main__':
    main()
