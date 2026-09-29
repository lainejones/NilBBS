"""mklha.py - pack a release package into an .lha on the bench (real LhA, keeps protection bits).

    python tools/mklha.py NilTerm 1.2          # out/release/NilTerm (+ NilTerm.info) -> out/release/NilTerm-1.2.lha
    python tools/mklha.py NilBBS 1.2
    python tools/mklha.py Doors/Dwarfhold 1.1  # a door package (tools/mkdoors.py)

The archive holds the package drawer AND its icon (<Pkg>.info beside it): without the icon the
unpacked drawer is invisible on Workbench and only a Shell reaches Install_<Pkg>.  Stages the files
with putrelease.py, runs LhA with the current directory at the staging drawer (else LhA stores full
paths), fetches the .lha back and checks its listing has the icon and the installer.
"""
import os, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..'))
NH = [sys.executable, r'C:\projects\NetHarness\host\nhctl.py', '--host', '127.0.0.1']

def nh(*a):
    return subprocess.run(NH + list(a), capture_output=True, text=True).stdout.strip()

def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    rel, ver = sys.argv[1].replace('\\', '/'), sys.argv[2]
    name = rel.split('/')[-1]
    src = os.path.join(ROOT, 'out', 'release', *rel.split('/'))
    if not os.path.isdir(src) or not os.path.isfile(src + '.info'):
        sys.exit('%s or its .info is missing - run the package builder first' % src)
    lha = '%s-%s.lha' % (name, ver)
    stage = 'RAM:LhaStage'
    nh('EXEC', 'Delete >NIL: %s ALL QUIET FORCE' % stage)
    nh('EXEC', 'Delete >NIL: RAM:%s QUIET FORCE' % lha)
    subprocess.check_call([sys.executable, os.path.join(HERE, 'putrelease.py'), '%s/%s' % (stage, name),
                           '--pkg', rel])
    script = os.path.join(ROOT, 'out', 'mklha.script')
    open(script, 'w', newline='\n').write('CD %s\nLhA >NIL: -r -q a RAM:%s %s %s.info\n' % (stage, lha, name, name))
    nh('PUTFILE', script, 'RAM:mklha.script')
    print(nh('EXEC', 'Execute RAM:mklha.script'))
    dst = os.path.join(os.path.dirname(src), lha)
    print(nh('GETFILE', 'RAM:%s' % lha, dst))
    wsl = '/mnt/' + dst[0].lower() + dst[2:].replace(os.sep, '/')
    listing = subprocess.run(['wsl', '-e', 'lha', 'l', wsl], capture_output=True, text=True).stdout
    names = [l.split()[-1] for l in listing.splitlines() if l.startswith('[')]
    for want in ('%s.info' % name, '%s/Install_%s' % (name, name), '%s/Install_%s.info' % (name, name)):
        if want not in names:
            print(listing)
            sys.exit('the archive has no ' + want)
    nh('EXEC', 'Delete >NIL: %s ALL QUIET FORCE' % stage)
    print('%s: %s (with %s.info and Install_%s)' % (name, dst, name, name))

if __name__ == '__main__':
    main()
