#!/usr/bin/env python3
"""
mkpkgicons.py - the release packages' icons, written once into dist/icons/ and committed.

    python tools/mkpkgicons.py

Run it only when an icon should change or a door is added (it makes one installer icon per
door in mkdoors.DOORS). The release scripts (mkrelease, mkdoors, mknilterm) just copy the
committed files, so building a release needs no icon writer; this script does - the author's
iconlib.py and makeicon_* in C:/projects/tools.

  drawer.info                   a package drawer (beside the drawer in the archive)
  doc.info                      ReadMe / LICENSE / .readme / .guide
  Install_<Pkg>.info            the Installer project icon, APPNAME/SCRIPT tooltypes per package
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from mkrelease import ROOT, TOOLS            # noqa: E402
sys.path.insert(0, TOOLS)
import iconlib                               # noqa: E402
import makeicon_doc as doc                   # noqa: E402
import makeicon_drawer as drw                # noqa: E402
import makeicon_install as mi                # noqa: E402
from mkdoors import DOORS                    # noqa: E402

DEST = os.path.join(ROOT, 'dist', 'icons')


def install_icon(app, script, minuser='AVERAGE'):
    return iconlib.build_info(mi.build_cidx(), mi.PALETTE, mi.PLANAR_MAP, mi.W, mi.H, mi.TRANSPARENT,
                              icon_type=4, default_tool='Installer',
                              tool_types=['APPNAME=' + app, 'SCRIPT=' + script, 'DEFUSER=AVERAGE',
                                          'MINUSER=' + minuser, 'LOG=FALSE'])


def main():
    os.makedirs(DEST, exist_ok=True)
    icons = {
        'drawer': iconlib.build_info(drw.build_cidx(), drw.PALETTE, drw.PLANAR_MAP, drw.W, drw.H,
                                     drw.TRANSPARENT, drawer=True),
        'doc': iconlib.build_info(doc.build_cidx(), doc.PALETTE, doc.PLANAR_MAP, doc.W, doc.H,
                                  doc.TRANSPARENT, icon_type=4, default_tool=doc.DEFAULT_TOOL),
        'Install_NilBBS': install_icon('NilBBS', 'Install_NilBBS'),
        'Install_NilTerm': install_icon('NilTerm', 'Install_NilTerm', 'NOVICE'),
    }
    for game, d in DOORS.items():
        icons['Install_' + game] = install_icon(d['title'], 'Install_' + game)
    for name, data in icons.items():
        open(os.path.join(DEST, name + '.info'), 'wb').write(data)
    print('%d icons in %s' % (len(icons), DEST))


if __name__ == '__main__':
    main()
