"""mknilterm.py - build NilTerm's own package (NilTerm also ships inside the NilBBS release).

    make dist                      (WSL: builds out/NilTerm and its icon in out/BBS)
    python tools/mknilterm.py      -> out/release/NilTerm/

  NilTerm/                  the drawer that gets installed
    NilTerm, NilTerm.info     the program (WBTOOL icon from mkicons.py)
    NilTerm.guide(.info)      the manual (AmigaGuide; opens in MultiView)
    ReadMe(.info), LICENSE(.info)
  NilTerm.info              that drawer's icon
  Install_NilTerm(.info)    the Installer script
  ReadMe(.info)             the same ReadMe, next to the installer
  NilTerm.readme            Aminet-style description (for an upload; not in the drawer)

Then the .lha is made on the bench so protection bits survive:
    python tools/putrelease.py WorkBench:NuzPkg/NilTerm --pkg NilTerm
    (bench) cd WorkBench:NuzPkg  +  LhA -r -e -x a NilTerm.lha NilTerm NilTerm/#?
Same personal-info check as the BBS release.
"""
import os, shutil, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from mkrelease import ROOT, TOOLS, copy, personal_check

OUT = os.path.join(ROOT, 'out', 'release', 'NilTerm')
SRC = os.path.join(ROOT, 'install', 'nilterm')

def main():
    prog = os.path.join(ROOT, 'out', 'NilTerm')
    icon = os.path.join(ROOT, 'out', 'BBS', 'NilTerm.info')
    if not (os.path.isfile(prog) and os.path.isfile(icon)):
        sys.exit('run `make dist` first (out/NilTerm or out/BBS/NilTerm.info is missing)')
    if os.path.exists(OUT):
        shutil.rmtree(OUT)
    d = os.path.join(OUT, 'NilTerm')
    copy(prog, os.path.join(d, 'NilTerm'))
    copy(icon, os.path.join(d, 'NilTerm.info'))
    for f in ('NilTerm.guide', 'ReadMe'):
        copy(os.path.join(SRC, f), os.path.join(d, f))
    copy(os.path.join(ROOT, 'LICENSE'), os.path.join(d, 'LICENSE'))
    copy(os.path.join(SRC, 'ReadMe'), os.path.join(OUT, 'ReadMe'))
    copy(os.path.join(SRC, 'Install_NilTerm'), os.path.join(OUT, 'Install_NilTerm'))
    copy(os.path.join(SRC, 'NilTerm.readme'), os.path.join(OUT, 'NilTerm.readme'))

    sys.path.insert(0, TOOLS)
    import iconlib, makeicon_doc as doc, makeicon_drawer as drw, makeicon_install as mi
    docicon = iconlib.build_info(doc.build_cidx(), doc.PALETTE, doc.PLANAR_MAP, doc.W, doc.H, doc.TRANSPARENT,
                                 icon_type=4, default_tool=doc.DEFAULT_TOOL)
    for f in ('NilTerm/NilTerm.guide', 'NilTerm/ReadMe', 'NilTerm/LICENSE', 'ReadMe'):
        open(os.path.join(OUT, f + '.info'), 'wb').write(docicon)
    open(os.path.join(OUT, 'NilTerm.info'), 'wb').write(
        iconlib.build_info(drw.build_cidx(), drw.PALETTE, drw.PLANAR_MAP, drw.W, drw.H, drw.TRANSPARENT, drawer=True))
    open(os.path.join(OUT, 'Install_NilTerm.info'), 'wb').write(
        iconlib.build_info(mi.build_cidx(), mi.PALETTE, mi.PLANAR_MAP, mi.W, mi.H, mi.TRANSPARENT,
                           icon_type=4, default_tool='Installer',
                           tool_types=['APPNAME=NilTerm', 'SCRIPT=Install_NilTerm', 'DEFUSER=AVERAGE',
                                       'MINUSER=NOVICE', 'LOG=FALSE']))

    bad = personal_check(OUT)
    if bad:
        for f, w in bad:
            print('PERSONAL INFO: %s mentions %r' % (f, w))
        sys.exit('NilTerm package NOT clean - fix the above')
    n = sum(len(fs) for _, _, fs in os.walk(OUT))
    size = sum(os.path.getsize(os.path.join(b, f)) for b, _, fs in os.walk(OUT) for f in fs)
    print('NilTerm package: %s  (%d files, %d KB) - personal-info check passed' % (OUT, n, size // 1024))

if __name__ == '__main__':
    main()
