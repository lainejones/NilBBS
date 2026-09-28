"""putrelease.py - copy out/release/NilBBS onto the bench (NetHarness) for an installer test.

    python tools/putrelease.py [dest] [--pkg NAME]   # default WorkBench:NuzPkg/NilBBS
    --pkg NilTerm puts out/release/NilTerm (tools/mknilterm.py) instead
"""
import os, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
args = sys.argv[1:]
PKG = 'NilBBS'
if '--pkg' in args:
    i = args.index('--pkg'); PKG = args[i + 1]; del args[i:i + 2]
SRC = os.path.join(HERE, '..', 'out', 'release', PKG)
DEST = args[0] if args else 'WorkBench:NuzPkg/' + PKG
NH = [sys.executable, r'C:\projects\NetHarness\host\nhctl.py', '--host', '127.0.0.1']

def nh(*a):
    return subprocess.run(NH + list(a), capture_output=True, text=True).stdout.strip()

nh('EXEC', 'Delete >NIL: "%s" ALL QUIET FORCE' % DEST)
made, n = set(), 0
for base, dirs, files in os.walk(SRC):
    rel = os.path.relpath(base, SRC).replace(os.sep, '/')
    d = DEST if rel == '.' else DEST + '/' + rel
    if d not in made:
        nh('EXEC', 'MakeDir >NIL: "%s" ALL' % d); made.add(d)
    for f in files:
        r = nh('PUTFILE', os.path.join(base, f), d + '/' + f)
        if 'OK' not in r: print('FAILED', f, r)
        n += 1
print('put', n, 'files in', DEST)
