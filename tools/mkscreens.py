"""mkscreens.py - the editable ANSI screens: Text/userstatus, Text/wall and a screen per menu.

    python tools/mkscreens.py                      # dist/BBS (what the release ships)
    python tools/mkscreens.py --root C:\\Amiga\\X\\BBS   # a board's own BBS: drawer (its menus + its art)
    python tools/mkscreens.py --force ...          # also replace screens that exist already

Each screen is written as .ans (ANSI colours, CP437, CRLF) and .asc (plain text, LF) in <root>/Text,
to be redrawn in any ANSI editor (PabloDraw, Moebius...).  Values are MCI codes with a width, so the
art stays in line:  |UN$R30 = the user name left-aligned in 30 columns ($L right-aligns, $C centres).

  userstatus   Your account status (main menu A)
  wall         The Wall, the one-liners at logon: |WA..|WL are its 12 lines, oldest first; a line
               written in a colour of its own shows in that colour (set the frame's colour again
               after the code, as these screens do)
  <menu>menu   one per Menus/<menu>.mnu: that menu's banner (its header = screen) and its items in
               the menu's box, drawn as BBSNode draws them.  The .mnu gets "screen = <menu>menu".
               Items a regular caller can't use (level above 10, or an access condition) are left
               out of the picture - they still work for those who may use them.

An existing screen is kept unless --force; delete a screen and BBSNode draws its own built-in one.
"""
import argparse, os, re

HERE = os.path.dirname(os.path.abspath(__file__))
DIST = os.path.normpath(os.path.join(HERE, '..', 'dist', 'BBS'))
W = 76                                   # inside a frame: " |" + 76 + "|" = 79 columns
CALLER_LEVEL = 10                        # menu items above this level stay off the menu screens

BOX = {True: dict(H='\xC4', V='\xB3', TL='\xDA', TR='\xBF', BL='\xC0', BR='\xD9', LT='\xC3', RT='\xB4'),
       False: dict(H='-', V='|', TL='+', TR='+', BL='+', BR='+', LT='+', RT='+')}


PC2ANSI = (0, 4, 2, 6, 1, 5, 3, 7)      # pipe codes count in the PC's order (1 blue, 4 red), ANSI doesn't


def sgr(fg, bg=0):
    """pipe-code colours (fg 0-15, bg 0-7) -> an ANSI SGR sequence that sets both from scratch"""
    bold = '1;' if fg >= 8 else ''
    return '\x1b[0;%s%d;%dm' % (bold, 30 + PC2ANSI[fg % 8], 40 + PC2ANSI[bg % 8])


class Screen:
    def __init__(self, ansi):
        self.ansi = ansi
        self.b = BOX[ansi]

    def c(self, fg, bg=0):
        return sgr(fg, bg) if self.ansi else ''

    def reset(self):
        return '\x1b[0m' if self.ansi else ''

    def top_tab(self, text, frame=9):
        b, t = self.b, len(text) + 4
        l = (W - t) // 2
        return (' ' + self.c(frame) + b['TL'] + b['H'] * l + b['RT'] + ' ' + self.c(15) + text + ' ' +
                self.c(frame) + b['LT'] + b['H'] * (W - t - l) + b['TR'] + self.reset())

    def rule(self, frame=9):
        return ' ' + self.c(frame) + self.b['LT'] + self.b['H'] * W + self.b['RT'] + self.reset()

    def bottom(self, frame=9):
        return ' ' + self.c(frame) + self.b['BL'] + self.b['H'] * W + self.b['BR'] + self.reset()

    def side(self, frame=9):
        return self.c(frame) + self.b['V']

    def join(self, lines):
        nl = '\r\n' if self.ansi else '\n'   # as the other screens: .ans CRLF, .asc LF
        return nl.join(lines) + nl


# ---- Your account status ------------------------------------------------------------------------
STATUS_ROWS = [
    [('Security level', 'LV'), ('Total calls', 'UC')],
    [('User number', 'UI'), ('Calls today', 'CT')],
    [('Messages posted', 'PO'), ('Time left', 'TM')],
    [('Conference', 'CF'), ('Door visits', 'DV')],
    [('Expert mode', 'XP'), ('Terminal', 'TT')],
    [('First call', 'FC'), ('Last call', 'LC')],
    None,
    [('Ratio', 'RT'), ('Downloads left', 'DL')],
    [('Uploaded', 'UK'), ('Files uploaded', 'UP')],
    [('Downloaded', 'DK'), ('Files downloaded', 'DN')],
    [('Credits', 'CD'), ('Protocol', 'PR')],
]


def userstatus(ansi):
    s = Screen(ansi)

    def head(l1, code1, l2, code2):
        w1 = W // 2 - 4 - len(l1)
        w2 = W - W // 2 - 2 - len(l2)
        return (' ' + s.side() + ' ' + s.c(12) + l1 + s.c(8) + ': ' + s.c(14) + '|%s$R%02d' % (code1, w1) + ' ' +
                s.c(12) + l2 + s.c(8) + ': ' + s.c(14) + '|%s$R%02d' % (code2, w2) + s.side() + s.reset())

    def row(pair):
        out = ' ' + s.side()
        for lab, code in pair:
            out += ' ' * (18 - len(lab)) + s.c(3) + lab + s.c(8) + ': ' + s.c(15) + '|%s$R18' % code
        return out + s.side() + s.reset()

    lines = [s.top_tab('User Status'), head('User', 'UN', 'Location', 'UL'), s.rule()]
    for r in STATUS_ROWS:
        lines.append(s.rule() if r is None else row(r))
    lines += [s.rule(), head('Board', 'BN', 'Sysop', 'SN'), s.bottom()]
    return s.join(lines)


# ---- The Wall -----------------------------------------------------------------------------------
def wall(ansi):
    s = Screen(ansi)
    cycle = (11, 13, 15)
    blank = ' ' + s.side() + ' ' * W + s.side() + s.reset()
    lines = [s.top_tab('The Wall'), blank]
    for i in range(12):
        lines.append(' ' + s.side() + ' ' + s.c(cycle[i % 3]) + '|W%s$C%02d' % (chr(65 + i), W - 2) +
                     ' ' + s.side() + s.reset())
    lines += [blank, s.bottom()]
    return s.join(lines)


# ---- menus: what menu.c's menu_draw() draws, at 80 columns -------------------------------------
def read_mnu(path):
    m = dict(title='', header='', screen='', accent=4, frame=8, items=[])
    for raw in open(path, encoding='latin-1'):
        line = raw.strip()
        if not line or line[0] in ';#' or '=' not in line:
            continue
        k, v = [x.strip() for x in line.split('=', 1)]
        k = k.lower()
        if k in ('title', 'header', 'screen'):
            m[k] = v
        elif k == 'accent':
            m['accent'] = int(v) & 7
        elif k == 'frame':
            m['frame'] = int(v) & 15
        elif k == 'item':
            f = [x.strip() for x in v.split('|')]
            if len(f) < 3 or not f[0]:
                continue
            lv = f[1]
            level, acs = (int(lv), '') if lv.isdigit() else (0, lv)
            m['items'].append(dict(key=f[0][0].upper(), level=level, acs=acs, desc=f[4] if len(f) > 4 else ''))
    return m


def art(root, name, ansi):
    """a Text/ screen as BBSNode shows it for this kind of caller (.ans first, or .asc first)"""
    for ext in (('.ans', '.asc', '.txt') if ansi else ('.asc', '.txt', '.ans')):
        p = os.path.join(root, 'Text', name + ext)
        if os.path.isfile(p):
            b = open(p, 'rb').read()
            if b'\x1a' in b:
                b = b[:b.index(b'\x1a')]          # SAUCE / after the DOS EOF isn't shown
            return b.decode('latin-1')
    return None


def menu_screen(root, m, ansi):
    s = Screen(ansi)
    inner, ncol = 80 - 5, 2
    cw = inner // ncol
    dw = cw - 5
    a, fr = m['accent'], m['frame']
    nl = '\r\n' if ansi else '\n'

    def tab(text, wide):
        if ansi:                                   # the "pill": half blocks round the key
            return (sgr(a) + '\xDE' + sgr(15, a) + (' %s ' % text if wide else text) + sgr(a) + '\xDD')
        return ('[ %s ]' if wide else '[%s]') % text

    out = nl
    header = art(root, m['header'], ansi) if m['header'] else None
    if header is not None:
        out += header + nl
    title = None if header is not None or not m['title'] else m['title']
    tlen = len(title) + 4 if title else 0
    if tlen > inner - 2:
        title, tlen = None, 0
    left = (inner + 1 - tlen) // 2
    right = inner + 1 - tlen - left
    out += ' ' + s.c(fr) + s.b['TL'] + s.b['H'] * left
    if title:
        out += tab(title, True) + s.c(fr)
    out += s.b['H'] * right + s.b['TR'] + s.reset() + nl
    col = 0
    shown = [it for it in m['items'] if it['desc'] and it['level'] <= CALLER_LEVEL and not it['acs']]
    for it in shown:
        if col == 0:
            out += ' ' + s.c(fr) + s.b['V'] + ' '
        d = it['desc'][:dw]
        out += tab(it['key'], False) + ' ' + s.c(7) + d + ' ' * (cw - 4 - len(d))
        col += 1
        if col >= ncol:
            out += ' ' * (inner % ncol) + s.c(fr) + s.b['V'] + s.reset() + nl
            col = 0
    if col:
        out += ' ' * (cw * (ncol - col) + inner % ncol) + s.c(fr) + s.b['V'] + s.reset() + nl
    out += ' ' + s.c(fr) + s.b['BL'] + s.b['H'] * (inner + 1) + s.b['BR'] + s.reset() + nl
    return out


def set_screen(mnu_path, screen):
    """add (or change) "screen = <screen>" in a .mnu, just above its title / first setting"""
    text = open(mnu_path, 'rb').read().decode('latin-1')
    nl = '\r\n' if '\r\n' in text else '\n'
    lines = text.split(nl)
    for i, l in enumerate(lines):
        if re.match(r'\s*screen\s*=', l, re.I):
            lines[i] = 'screen = %s' % screen
            break
    else:
        at = next((i for i, l in enumerate(lines) if re.match(r'\s*(title|header)\s*=', l, re.I)), 0)
        lines.insert(at, 'screen = %s' % screen)
    open(mnu_path, 'wb').write(nl.join(lines).encode('latin-1'))


def write(root, name, maker, force):
    for ext, ansi in (('.ans', True), ('.asc', False)):
        p = os.path.join(root, 'Text', name + ext)
        if os.path.exists(p) and not force:
            print('kept   ', p)
            continue
        open(p, 'w', encoding='latin-1', newline='').write(maker(ansi))
        print('wrote  ', p)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=DIST, help="a BBS: drawer (default: the release's dist/BBS)")
    ap.add_argument('--force', action='store_true', help='replace screens that exist')
    o = ap.parse_args()
    root = os.path.normpath(o.root)
    write(root, 'userstatus', userstatus, o.force)
    write(root, 'wall', wall, o.force)
    mdir = os.path.join(root, 'Menus')
    for f in sorted(os.listdir(mdir)):
        if not f.lower().endswith('.mnu'):
            continue
        name = f[:-4]
        path = os.path.join(mdir, f)
        m = read_mnu(path)
        screen = name + 'menu'
        if m['screen'] and m['screen'] != screen:
            print('skipped', path, '(has its own screen = %s)' % m['screen'])
            continue
        write(root, screen, lambda ansi, m=m: menu_screen(root, m, ansi), o.force)
        set_screen(path, screen)


if __name__ == '__main__':
    main()
