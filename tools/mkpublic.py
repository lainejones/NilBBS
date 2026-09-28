"""mkpublic.py - export the public GitHub copies of NilBBS and NilTerm.

    python tools/mkpublic.py            -> ../_public/NilBBS and ../_public/NilTerm

The private repo keeps the full history, notes and bench tests.  The public repos get clean snapshots
of what's committed here (git archive HEAD), minus:
  - the working notes (CLAUDE.md, docs/cnet-cutover.md, docs/hackslash-style-brief.md) and the bench tests (test/)
  - src/cnetsdk: CNet's own SDK headers, not ours to redistribute (a README says where to get them)
NilTerm's repo is the handful of files it builds from, with a Makefile of its own.
Both trees must pass the personal-info check (tools/personal.txt), or nothing is written.
Commit + push inside ../_public/<name> afterwards (each is its own git repo).
"""
import io, os, shutil, subprocess, sys, tarfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from mkrelease import ROOT, personal_check

PUB = os.path.normpath(os.path.join(ROOT, '..', '_public'))
DROP_FILES = {'CLAUDE.md', 'docs/cnet-cutover.md', 'docs/hackslash-style-brief.md'}
DROP_DIRS = ('test/', 'src/cnetsdk/')

NILTERM_FILES = [
    'LICENSE', 'VERSION',
    'src/nilterm/nilterm.c', 'src/nilterm/nilfont.h', 'src/nilterm/ntzio.h',
    'src/nilterm/ntzm.c', 'src/nilterm/ntxy.c',
    'src/node/zmodem.c', 'src/node/xymodem.c', 'src/node/zmodem.h', 'src/node/node.h',
    'src/common/bbs.h', 'src/common/cfg.h', 'src/common/util.c', 'src/common/shared.c', 'src/common/cfg.c',
    'install/nilterm/Install_NilTerm', 'install/nilterm/NilTerm.guide', 'install/nilterm/ReadMe',
    'install/nilterm/NilTerm.readme',
]

CNETSDK_README = """# src/cnetsdk - not included

NilBBS runs CNet 3/4 C doors (PFiles), and builds against the CNet SDK headers for that.
Those headers are CNet's own, so they aren't published here.  To build NilBBS, copy the
SDK's include files from a CNet 3 or 4 installation (its `sdk` drawer) into this drawer.

The released NilBBS.lha is complete and needs none of this.
"""

NILTERM_MAKEFILE = """# NilTerm - an ANSI telnet client for the Amiga.  Build under WSL/Linux with the bebbo
# amiga-gcc toolchain on PATH:   make      -> out/NilTerm
# The sources are NilBBS's (github.com/lainejones/NilBBS): NilTerm is its sysop terminal,
# and its file transfers are the BBS's own ZMODEM / X/YMODEM code (src/node).
CC      = m68k-amigaos-gcc
VERSION = $(shell cat VERSION)
VERDATE = $(shell date +%-d.%-m.%Y)
CFLAGS  = -m68020 -O2 -noixemul -Wall -Wno-pointer-sign -Wno-format -fomit-frame-pointer -s -Isrc/common
CFLAGS += -DBBS_VERSION='"$(VERSION)"' -DBBS_VERDATE='"$(VERDATE)"'
SRCS    = src/nilterm/nilterm.c src/nilterm/ntzm.c src/nilterm/ntxy.c \\
          src/common/util.c src/common/shared.c src/common/cfg.c

out/NilTerm: $(SRCS) src/nilterm/*.h src/node/zmodem.c src/node/xymodem.c src/node/*.h src/common/*.h VERSION
\t@mkdir -p out
\t$(CC) $(CFLAGS) -o $@ $(SRCS)

clean:
\trm -rf out

.PHONY: clean
"""

def git_tree():
    """{path: bytes} of every committed file at HEAD"""
    raw = subprocess.run(['git', '-C', ROOT, 'archive', '--format=tar', 'HEAD'],
                         capture_output=True, check=True).stdout
    out = {}
    with tarfile.open(fileobj=io.BytesIO(raw)) as t:
        for m in t.getmembers():
            if m.isfile():
                out[m.name] = t.extractfile(m).read()
    return out

def write_tree(dest, files):
    """replace everything in dest except its .git with files"""
    os.makedirs(dest, exist_ok=True)
    for name in os.listdir(dest):
        if name == '.git':
            continue
        p = os.path.join(dest, name)
        shutil.rmtree(p) if os.path.isdir(p) else os.remove(p)
    for path, data in files.items():
        p = os.path.join(dest, *path.split('/'))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, 'wb').write(data)

def nilterm_readme(tree):
    text = tree['install/nilterm/ReadMe'].decode('latin-1')
    return ("# NilTerm\n\n```\n" + text.rstrip() + "\n```\n\n"
            "## Building\n\n"
            "With the bebbo amiga-gcc toolchain (m68k-amigaos-gcc) on PATH: `make` -> `out/NilTerm`.\n\n"
            "NilTerm is developed as part of [NilBBS](https://github.com/lainejones/NilBBS), whose "
            "ZMODEM / X/YMODEM code it shares (`src/node`); this repo holds just the files it builds "
            "from.  The release `.lha` has the installer, the AmigaGuide manual and the icons.\n\n"
            "## Licence\n\nMIT - see LICENSE.  The built-in IBM VGA 8x16 font (VileR, int10h.org) is "
            "CC BY-SA 4.0.\n").encode('utf-8')

def main():
    tree = git_tree()
    bbs = {p: d for p, d in tree.items()
           if p not in DROP_FILES and not p.startswith(DROP_DIRS)}
    bbs['src/cnetsdk/README.md'] = CNETSDK_README.encode()
    term = {p: tree[p] for p in NILTERM_FILES}
    term['Makefile'] = NILTERM_MAKEFILE.encode()
    term['README.md'] = nilterm_readme(tree)
    term['.gitignore'] = b'out/\n'
    term['.gitattributes'] = tree['.gitattributes']     # *.ans binary: never touch ANSI line ends

    # check before touching anything
    stage = os.path.join(ROOT, 'out', 'public-check')
    ok = True
    for name, files in (('NilBBS', bbs), ('NilTerm', term)):
        d = os.path.join(stage, name)
        shutil.rmtree(d, ignore_errors=True)
        write_tree(d, files)
        for f, w in personal_check(d, allow=(b'github.com/lainejones',)):
            print('PERSONAL INFO (%s): %s mentions %r' % (name, f, w)); ok = False
    shutil.rmtree(stage, ignore_errors=True)
    if not ok:
        sys.exit('public copies NOT clean - nothing written')
    for name, files in (('NilBBS', bbs), ('NilTerm', term)):
        d = os.path.join(PUB, name)
        write_tree(d, files)
        print('%-8s %s  (%d files) - personal-info check passed' % (name, d, len(files)))

if __name__ == '__main__':
    main()
