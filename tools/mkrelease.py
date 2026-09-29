"""mkrelease.py - build the NilBBS install package (host side).

    python tools/mkrelease.py            # -> out/release/NilBBS/  (+ a personal-info check)

Run `make dist` first (the WSL build) so out/BBS holds fresh programs.  The package:

  NilBBS/Install_NilBBS(.info)   the Installer script (Commodore Installer, OS 3.1+)
  NilBBS/ReadMe                  what's in it, how to install by hand
  NilBBS/WBStartup/BBSControl    a project icon the installer can drop in SYS:WBStartup
  NilBBS/BBS/                    the BBS drawer: programs + default config, NO users/logs/messages
  NilBBS/BBS/Doors/              the sample doors (one of each kind) - the door GAMES are separate
                                 packages: tools/mkdoors.py

Personal info is stripped by construction (only listed files are copied, player
data never) and then checked: the build FAILS if any file mentions the bench's
people, accounts, LAN or domain.
"""
import os, re, shutil, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..'))
PROJ = os.path.dirname(ROOT)
OUT = os.path.join(ROOT, 'out', 'release', 'NilBBS')
TOOLS = os.path.join(PROJ, 'tools')

# anything that must never ship (checked case-insensitively in every file): one word per line in
# tools/personal.txt - kept out of git, so the list itself isn't published.  Without it the check
# only knows the generic private-network prefixes.
def _personal():
    p = os.path.join(HERE, 'personal.txt')
    words = [b'192.168.', b'10.0.0.']
    if os.path.isfile(p):
        words = [l.strip().lower().encode() for l in open(p, encoding='utf-8')
                 if l.strip() and not l.startswith('#')]
    return words
PERSONAL = _personal()
# the bench test accounts are also monster names (Realm has golems and kobolds), so
# for them only a FILE named after one fails the check (that would be a save)
ACCOUNT_FILES = ('golem', 'kobold')

def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)

def copytree(src, dst, skip=()):
    for base, dirs, files in os.walk(src):
        dirs[:] = [d for d in dirs if d not in ('.git', '__pycache__')]
        for f in files:
            if f in skip:
                continue
            s = os.path.join(base, f)
            copy(s, os.path.join(dst, os.path.relpath(s, src)))

def mkdirs(base, names):
    for n in names:
        p = os.path.join(base, n)
        os.makedirs(p, exist_ok=True)
        open(os.path.join(p, '.keep'), 'w').close()      # an empty drawer survives .lha/.zip

def package_icons(out):
    """out = out/release/.../<Pkg>: give the package drawer its own icon (<Pkg>.info BESIDE it - the
    .lha must carry it, or the unpacked drawer is invisible on Workbench and only a Shell reaches the
    installer), and a document icon on each top-level ReadMe / LICENSE / .readme / .guide"""
    if TOOLS not in sys.path:
        sys.path.insert(0, TOOLS)
    import iconlib, makeicon_doc as doc, makeicon_drawer as drw
    open(out.rstrip('\\/') + '.info', 'wb').write(
        iconlib.build_info(drw.build_cidx(), drw.PALETTE, drw.PLANAR_MAP, drw.W, drw.H, drw.TRANSPARENT, drawer=True))
    docicon = iconlib.build_info(doc.build_cidx(), doc.PALETTE, doc.PLANAR_MAP, doc.W, doc.H, doc.TRANSPARENT,
                                 icon_type=4, default_tool=doc.DEFAULT_TOOL)
    for f in os.listdir(out):
        p = os.path.join(out, f)
        if (os.path.isfile(p) and not os.path.exists(p + '.info') and
                (f in ('ReadMe', 'LICENSE') or f.endswith('.readme') or f.endswith('.guide'))):
            open(p + '.info', 'wb').write(docicon)

# ---------------------------------------------------------------- the check
def wbstartup_icon(tool_info, dst_dir):
    """BBSControl's tool icon -> a WBStartup project icon (default tool BBS:BBSControl - the installer
    sets the real path - and DONOTWAIT), plus the empty file it belongs to"""
    import struct
    b = open(tool_info, 'rb').read()
    form = b.find(b'FORM')
    classic, colour = (b[:form], b[form:]) if form > 0 else (b, b'')
    assert classic[:2] == b'\xe3\x10' and classic[50:58] == b'\0' * 8, 'BBSControl.info: not a plain tool icon'
    hdr = bytearray(classic[:78])
    hdr[48] = 4                                             # WBPROJECT
    hdr[50:54] = hdr[54:58] = struct.pack('>I', 0x12345678)  # "has a default tool", "has tooltypes"
    def s(x):
        x = x.encode('latin-1') + b'\0'
        return struct.pack('>I', len(x)) + x
    tools = ['DONOTWAIT']
    os.makedirs(dst_dir, exist_ok=True)
    open(os.path.join(dst_dir, 'BBSControl.info'), 'wb').write(
        bytes(hdr) + classic[78:] + s('BBS:BBSControl') + struct.pack('>I', (len(tools) + 1) * 4) +
        b''.join(s(t) for t in tools) + colour)
    open(os.path.join(dst_dir, 'BBSControl'), 'wb').close()

def personal_check(top, allow=()):
    bad = []
    for base, dirs, files in os.walk(top):
        for f in files:
            p = os.path.join(base, f)
            if os.path.splitext(f)[0].lower() in ACCOUNT_FILES:
                bad.append((os.path.relpath(p, top), 'a test account save'))
            data = open(p, 'rb').read().lower()
            if f.upper().startswith('LICENSE'):     # the author's copyright line is meant to be there
                data = bytes([10]).join(l for l in data.split(bytes([10])) if not l.startswith(b'copyright (c)'))
            for a in allow:                         # e.g. the public GitHub account in a link
                data = data.replace(a, b'')
            for w in PERSONAL:
                if w in data:
                    bad.append((os.path.relpath(p, top), w.decode()))
    return bad

def main():
    dist = os.path.join(ROOT, 'out', 'BBS')
    if not os.path.isfile(os.path.join(dist, 'NilBBS')):
        sys.exit('run `make dist` first (out/BBS is missing)')
    import subprocess                       # the language template must match the program's text
    if subprocess.call([sys.executable, os.path.join(ROOT, 'tools', 'mklang.py'), '--check']):
        sys.exit('dist English.lng is stale: run tools/mklang.py, then make dist')
    if not os.path.isfile(os.path.join(dist, 'Text', 'Language', 'English.lng')):
        sys.exit('out/BBS has no Text/Language/English.lng: run make dist again')
    if os.path.exists(OUT):
        shutil.rmtree(OUT)
    copytree(dist, os.path.join(OUT, 'BBS'))
    copy(os.path.join(ROOT, 'dist', 'BBS.info'), os.path.join(OUT, 'BBS.info'))
    wbstartup_icon(os.path.join(OUT, 'BBS', 'BBSControl.info'), os.path.join(OUT, 'WBStartup'))
    copy(os.path.join(ROOT, 'install', 'Install_NilBBS'), os.path.join(OUT, 'Install_NilBBS'))
    copy(os.path.join(ROOT, 'install', 'ReadMe'), os.path.join(OUT, 'ReadMe'))
    copy(os.path.join(ROOT, 'LICENSE'), os.path.join(OUT, 'LICENSE'))
    icon = os.path.join(OUT, 'Install_NilBBS.info')
    sys.path.insert(0, TOOLS)
    import iconlib, makeicon_install as mi
    data = iconlib.build_info(mi.build_cidx(), mi.PALETTE, mi.PLANAR_MAP, mi.W, mi.H, mi.TRANSPARENT,
                              icon_type=4, default_tool='Installer',
                              tool_types=['APPNAME=NilBBS', 'SCRIPT=Install_NilBBS', 'DEFUSER=AVERAGE',
                                          'MINUSER=AVERAGE', 'LOG=FALSE'])
    open(icon, 'wb').write(data)
    package_icons(OUT)

    bad = personal_check(OUT)
    if bad:
        for f, w in bad:
            print('PERSONAL INFO: %s mentions %r' % (f, w))
        sys.exit('release NOT clean - fix the above')
    n = sum(len(fs) for _, _, fs in os.walk(OUT))
    size = sum(os.path.getsize(os.path.join(b, f)) for b, _, fs in os.walk(OUT) for f in fs)
    print('release: %s  (%d files, %d KB) - personal-info check passed' % (OUT, n, size // 1024))

if __name__ == '__main__':
    main()
