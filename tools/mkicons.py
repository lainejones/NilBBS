#!/usr/bin/env python3
"""
mkicons.py - the Workbench icons for NilBBS (dual: 4-colour classic + OS3.5 colour).

    python tools/mkicons.py [--png]

Writes into dist/:
  BBS.info                          the drawer
  BBS/BBSConfig.info, BBSControl.info,
  BBS/BBSSchedule.info, NilTerm.info the GUI programs (WBTOOL)
  BBS/Start NilBBS.info, Stop NilBBS.info, Local Logon.info,
  BBS/Run Maintenance.info          IconX script icons (WBPROJECT) for the
                                    Shell programs, with IconX ToolTypes
--png also writes previews to test/icons/.
Uses C:/projects/tools/iconlib.py (and makeicon_drawer.py for the drawer).
"""
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TOOLS = r'C:\projects\tools'
sys.path.insert(0, TOOLS)
import iconlib

W, H = 48, 40
PALETTE = [
    (149, 149, 149),  # 0 background (transparent)
    (0, 0, 0),        # 1 black
    (255, 255, 255),  # 2 white
    (102, 136, 187),  # 3 Workbench blue
    (80, 80, 80),     # 4 dark grey
    (200, 200, 200),  # 5 light grey (case)
    (60, 210, 80),    # 6 green
    (225, 50, 40),    # 7 red
    (245, 200, 50),   # 8 amber
    (20, 28, 40),     # 9 screen
    (40, 64, 130),    # 10 dark blue
]
#             0  1  2  3  4  5  6  7  8  9  10
PLANAR_MAP = [0, 1, 2, 3, 1, 2, 2, 3, 2, 1, 3]

def new(): return [[0] * W for _ in range(H)]
def put(g, x, y, c):
    if 0 <= x < W and 0 <= y < H: g[y][x] = c
def rect(g, x0, y0, x1, y1, c):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1): put(g, x, y, c)
def box(g, x0, y0, x1, y1, c):
    for x in range(x0, x1 + 1): put(g, x, y0, c); put(g, x, y1, c)
    for y in range(y0, y1 + 1): put(g, x0, y, c); put(g, x1, y, c)
def line(g, x0, y0, x1, y1, c, r=0):
    dx = abs(x1 - x0); dy = -abs(y1 - y0); sx = 1 if x0 < x1 else -1; sy = 1 if y0 < y1 else -1
    err = dx + dy
    while True:
        for oy in range(-r, r + 1):
            for ox in range(-r, r + 1): put(g, x0 + ox, y0 + oy, c)
        if x0 == x1 and y0 == y1: break
        e2 = 2 * err
        if e2 >= dy: err += dy; x0 += sx
        if e2 <= dx: err += dx; y0 += sy

# ---- building blocks -----------------------------------------------------------
SX0, SY0, SX1, SY1 = 10, 6, 37, 24          # the monitor's screen

def monitor(g, stand=True):
    rect(g, 6, 3, 41, 28, 5); box(g, 6, 3, 41, 28, 1)          # case
    line(g, 7, 4, 40, 4, 2); line(g, 7, 4, 7, 27, 2)           # highlight
    rect(g, SX0, SY0, SX1, SY1, 9); box(g, SX0 - 1, SY0 - 1, SX1 + 1, SY1 + 1, 4)
    put(g, 36, 26, 6)                                          # power LED
    if stand:
        rect(g, 20, 29, 27, 31, 4); rect(g, 13, 32, 34, 34, 5); box(g, 13, 32, 34, 34, 1)

def keyboard(g):
    rect(g, 4, 31, 43, 37, 5); box(g, 4, 31, 43, 37, 1)
    for y in (33, 35):
        for x in range(7, 41, 3): put(g, x, y, 4)

def start_icon():
    g = new(); monitor(g)
    for y in range(8, 23):                                      # a green "play"
        w = (y - 8 if y <= 15 else 22 - y) * 12 // 7
        rect(g, 17, y, 17 + w, y, 6)
    return g

def stop_icon():
    g = new(); monitor(g)
    rect(g, 18, 10, 29, 20, 7); box(g, 18, 10, 29, 20, 1)
    return g

def logon_icon():
    g = new(); monitor(g, stand=False); keyboard(g)
    line(g, 12, 9, 15, 12, 6); line(g, 15, 12, 12, 15, 6)       # ">"
    rect(g, 17, 15, 21, 15, 6)                                  # "_" cursor
    for y, w in ((19, 14), (22, 9)): rect(g, 12, y, 12 + w, y, 3)
    return g

def term_icon():
    g = new(); monitor(g)
    for y in range(SY0, SY1 + 1):                               # an 80x25 ANSI screen:
        rect(g, SX0, y, SX1, y, 9)
    rect(g, SX0 + 2, SY0 + 2, SX1 - 2, SY0 + 3, 3)             # a blue header bar
    for i, (y, w, c) in enumerate(((SY0 + 7, 16, 8), (SY0 + 10, 21, 6), (SY0 + 13, 12, 2))):
        rect(g, SX0 + 2, y, SX0 + 2 + w, y, c)                  # coloured text lines
    rect(g, SX0 + 2, SY1 - 3, SX0 + 5, SY1 - 2, 2)              # the cursor
    return g

def maint_icon():
    g = new(); monitor(g)
    line(g, 14, 21, 27, 11, 8, 1)                               # wrench handle
    rect(g, 26, 8, 31, 13, 8); rect(g, 28, 7, 30, 10, 9)        # its open jaw
    put(g, 26, 8, 9); put(g, 31, 13, 9)
    return g

def control_icon():
    g = new(); monitor(g)
    for i, y in enumerate((8, 11, 14, 17, 20)):
        rect(g, 12, y, 13, y, 8)                                # node number
        rect(g, 15, y, 15 + (18 if i % 2 == 0 else 12), y, 6 if i < 3 else 2)
    return g

def config_icon():
    g = new()
    rect(g, 4, 3, 43, 36, 2); box(g, 4, 3, 43, 36, 1)           # a window
    rect(g, 5, 4, 42, 8, 3); line(g, 5, 9, 42, 9, 1)
    rect(g, 6, 5, 8, 7, 2); box(g, 6, 5, 8, 7, 1)               # close gadget
    for y, k in ((15, 16), (22, 30), (29, 22)):                 # three sliders
        rect(g, 8, y - 2, 11, y + 2, 5)
        line(g, 14, y, 39, y, 4)
        rect(g, k - 2, y - 3, k + 2, y + 3, 10); box(g, k - 2, y - 3, k + 2, y + 3, 1)
    return g

def schedule_icon():
    g = new()
    rect(g, 5, 4, 38, 33, 2); box(g, 5, 4, 38, 33, 1)            # a calendar page
    rect(g, 6, 5, 37, 10, 3); line(g, 6, 11, 37, 11, 1)         # its blue header
    for x in (12, 31): rect(g, x, 2, x + 1, 6, 4)               # the rings
    for r in range(4):                                          # the days
        for c in range(6):
            x, y = 8 + c * 5, 14 + r * 5
            rect(g, x, y, x + 2, y + 2, 7 if (r, c) in ((0, 3), (2, 1)) else 5)
    for y in range(24, 40):                                     # a clock over the corner
        for x in range(28, 44):
            if (x - 35.5) ** 2 + (y - 31.5) ** 2 <= 56: put(g, x, y, 8)
            if 42 <= (x - 35.5) ** 2 + (y - 31.5) ** 2 <= 58: put(g, x, y, 1)
    line(g, 35, 31, 35, 26, 1); line(g, 35, 31, 39, 31, 1)      # its hands
    return g

def write(name, g, **kw):
    path = os.path.join(ROOT, 'dist', name)
    with open(path, 'wb') as f:
        f.write(iconlib.build_info(g, PALETTE, PLANAR_MAP, **kw))
    print('wrote', path)
    if '--png' in sys.argv:
        os.makedirs(os.path.join(ROOT, 'test', 'icons'), exist_ok=True)
        iconlib.write_png(os.path.join(ROOT, 'test', 'icons', os.path.basename(name) + '.png'), g, PALETTE)

write('BBS/BBSConfig.info',  config_icon())
write('BBS/BBSControl.info', control_icon())
write('BBS/BBSSchedule.info', schedule_icon())
write('BBS/NilTerm.info', term_icon())
write('BBS/Start NilBBS.info', start_icon(), icon_type=4, default_tool='C:IconX',
      tool_types=['WINDOW=NIL:'])
write('BBS/Stop NilBBS.info', stop_icon(), icon_type=4, default_tool='C:IconX',
      tool_types=['DELAY=2'])
write('BBS/Local Logon.info', logon_icon(), icon_type=4, default_tool='C:IconX',
      tool_types=['WINDOW=NIL:'])
write('BBS/Run Maintenance.info', maint_icon(), icon_type=4, default_tool='C:IconX',
      tool_types=['WINDOW=CON:0/20/640/220/NilBBS Maintenance/CLOSE/WAIT'])
subprocess.run([sys.executable, os.path.join(TOOLS, 'makeicon_drawer.py'),
                os.path.join(ROOT, 'dist', 'BBS.info')], check=True)
