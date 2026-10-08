"""mkdoors.py - build a separate install package for each of our door games (host side).

    python tools/mkdoors.py              # every game -> out/release/Doors/<Game>/
    python tools/mkdoors.py Dwarfhold    # just one

The BBS package (mkrelease.py) ships only the sample doors.  Each game is its own package:

  <Game>/<Game>/            the game drawer, clean (no players, no saves)
  <Game>/Install_<Game>     an Installer script: NilBBS (copies it into BBS:PFiles and adds the
                            Doors.cfg section), CNet (copies it into PFILES: and says what to enter
                            in CNet's PFile editor), or just copy it somewhere
  <Game>/ReadMe             what it is, installing by hand on NilBBS and on CNet, maintenance
  <Game>/NilBBS-Doors.cfg   the NilBBS Doors.cfg section the installer appends

Run each game's own build first (out/<Game> in its repo).  Every package gets the same personal-info
check as the BBS release: the build FAILS if a file mentions the bench's people, accounts, LAN or domain.
"""
import os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..'))
PROJ = os.path.dirname(ROOT)
OUTROOT = os.path.join(ROOT, 'out', 'release', 'Doors')
TOOLS = os.path.join(PROJ, 'tools')

sys.path.insert(0, HERE)
from mkrelease import copy, copytree, mkdirs, personal_check, package_icons, pkg_icon     # shared helpers + the same check

# ---------------------------------------------------------------- what goes in each game drawer
def c_game(repo, prog, datadirs, extra_dirs=('Art',)):
    def build(dst):
        R = os.path.join(PROJ, repo)
        copy(os.path.join(R, 'out', prog), os.path.join(dst, prog))
        for d in extra_dirs:
            src = os.path.join(R, d)
            if os.path.isdir(src):
                for f in os.listdir(src):
                    if os.path.isfile(os.path.join(src, f)):
                        copy(os.path.join(src, f), os.path.join(dst, d, f))
        mkdirs(os.path.join(dst, 'Data'), datadirs)
    return build

def game_realm(dst):
    R = os.path.join(PROJ, 'Realm')
    for f in ('REALM.rexx', 'REALM_MAINT.rexx', 'REALM_ADMIN.rexx', 'MAKE_DUNGEONS.rexx', 'RealmEdit', 'RealmEdit.info'):
        copy(os.path.join(R, f), os.path.join(dst, f))
    for d in ('ItemDB', os.path.join('Data', 'Screens'), os.path.join('Data', 'Dungeons')):
        copytree(os.path.join(R, d), os.path.join(dst, d))
    maps = os.path.join(R, 'Maps')
    for f in os.listdir(maps):                               # the world + dungeons, not cached town maps
        if f == 'overworld.map' or f.startswith('dungeon'):
            copy(os.path.join(maps, f), os.path.join(dst, 'Maps', f))
    towns = os.path.join(R, 'Data', 'Towns')
    for f in os.listdir(towns):                              # names + street maps, not places.dat
        if f.endswith('.map') or f in ('towns.dat', 'places.dat'):   # places.dat: where each town / mouth is (80x50 world)
            copy(os.path.join(towns, f), os.path.join(dst, 'Data', 'Towns', f))
    mkdirs(os.path.join(dst, 'Data'), ['Players', 'Inventory', 'Vault', 'Bank', 'Castles', 'Drops', 'Market',
                                       'Shops', 'PvP', 'Raids', 'Messages', 'Quests', 'Lottery'])
    mkdirs(dst, ['Logs'])

def game_spacebounty(dst):
    S = os.path.join(PROJ, 'SpaceBounty')
    for f in ('SpaceBounty.rexx', 'SpaceBountyMaint.rexx'):
        copy(os.path.join(S, f), os.path.join(dst, f))
    copytree(os.path.join(S, 'ansi'), os.path.join(dst, 'ansi'))
    tracked = subprocess.run(['git', '-C', S, 'ls-files', 'data'], capture_output=True, text=True).stdout.split()
    for f in tracked:                                        # the static world (sectors, ports, ships, planets)
        copy(os.path.join(S, f), os.path.join(dst, f))
    for f in ('market.dat', 'events.dat', 'npc_positions.dat'):   # starting state, no players in them
        copy(os.path.join(S, 'data', f), os.path.join(dst, 'data', f))
    with open(os.path.join(dst, 'data', 'leaderboard.dat'), 'w', newline='\n') as fh:
        fh.write('# Space Bounty Leaderboard\n# FORMAT: USERNAME|CREDITS|WINS|LOSSES|TURNS\n')
    mkdirs(os.path.join(dst, 'data'), ['players', 'floating', 'notices'])

def game_emberwyrm(dst):
    E = os.path.join(PROJ, 'EmberWyrm')
    c_game('EmberWyrm', 'EmberWyrm', ['Heroes', 'Mail', 'IGM'], ('Art', 'IGM'))(dst)
    with open(os.path.join(dst, 'Data', 'igm.cfg'), 'w', newline='\n') as fh:
        fh.write('; Other Places: ARexx add-ons ("sub-doors").  One a line:\n'
                 ';   TAG | Name on the menu | script | min level | description | daily maintenance script (optional)\n'
                 'WELL    | The Old Well           | PFILES:EmberWyrm/IGM/well.rexx    | 1 | a coin a day, and a wish\n'
                 'GEMS    | Nell\'s gem stall       | PFILES:EmberWyrm/IGM/gems.rexx    | 1 | buy and sell gems | PFILES:EmberWyrm/IGM/gemsmaint.rexx\n'
                 'COTTAGE | Widow Hale\'s cottage   | PFILES:EmberWyrm/IGM/cottage.rexx | 1 | a safe bed for the night\n')

# kind: 'c' = a CNet C PFile (NilBBS type cnetc), 'rexx' = a CNet ARexx PFile (NilBBS type cnetrexx)
# update: what an update copies over an existing install (programs + art, never Data/players/saves)
DOORS = {
    'Dwarfhold': dict(title='Dwarfhold Depths', tag='DWARF', kind='c', main='Dwarfhold', build=c_game('Dwarfhold', 'Dwarfhold', ['Dwarves', 'Reports']),
        update_files=['Dwarfhold'], update_dirs=['Art'], exe=['Dwarfhold'],
        blurb='A dwarf hold under the mountain: the Proving Pits, the Deep Mines, forging, runes, duels, clans, '
              'the tavern and a lake under the mountain.'),
    'Grimhold': dict(title='Blades of Grimhold', tag='GRIM', kind='c', main='Grimhold', build=c_game('Grimhold', 'Grimhold', ['Heroes', 'Reports']),
        update_files=['Grimhold'], update_dirs=['Art'], exe=['Grimhold'],
        blurb='A frontier town: the Blood Arena, the Old Keep\'s ten floors, jousting, gangs, the Mage Tower, homes and burglary.'),
    'SunkenIsles': dict(title='Sunken Isles', tag='ISLES', kind='c', main='SunkenIsles', build=c_game('SunkenIsles', 'SunkenIsles', ['Captains', 'Reports']),
        update_files=['SunkenIsles'], update_dirs=['Art'], exe=['SunkenIsles'],
        blurb='A pirate captain among eight island ports: trade, chase sails, broadsides and boarding, the law, fleets, '
              'treasure maps and the Kraken.'),
    'Shadowguild': dict(title='Shadowguild', tag='SHADOW', kind='c', main='Shadowguild', build=c_game('Shadowguild', 'Shadowguild', ['Guilds', 'Reports']),
        update_files=['Shadowguild'], update_dirs=['Art'], exe=['Shadowguild'],
        blurb='Run a thieves\' guild: recruit, steal, and raid other guilds.'),
    'EmberWyrm': dict(title='Legend of the Ember Wyrm', tag='EMBER', kind='c', main='EmberWyrm', build=game_emberwyrm,
        update_files=['EmberWyrm'], update_dirs=['Art'], exe=['EmberWyrm'],
        blurb='The classic forest-and-dragon door game: fight in the Ashwood, train with the masters, court Rowan '
              'or Corin at the inn, slaughter sleeping rivals - and climb Mount Morrow to face the Ember Wyrm. '
              'Other Places are ARexx add-ons (see IGM/ and Data/igm.cfg).'),
    'Realm': dict(title='Realm of the Overworld', tag='REALM', kind='rexx', main='REALM.rexx', build=game_realm,
        update_files=['#?.rexx', 'RealmEdit', 'RealmEdit.info'], update_dirs=['Data/Screens'], exe=['RealmEdit'],
        update_new=['ItemDB'],      # new item lists (legends, herbs) arrive; ones the sysop edited in RealmEdit stay
        update_world=['Maps/overworld.map', 'Data/Towns/places.dat'],
        maint='REALM_MAINT.rexx',
        blurb='Fantasy RPG on an 80x50 overworld that reveals as you explore: fight, trade, build castles and raid other players\' castles.'),
    'SpaceBounty': dict(title='Space Bounty', tag='BOUNTY', kind='rexx', main='SpaceBounty.rexx', build=game_spacebounty,
        update_files=['#?.rexx'], update_dirs=['ansi'], exe=[], maint='SpaceBountyMaint.rexx',
        blurb='Trade between star ports, hunt pirates, found corporations and planets.'),
}

def doors_cfg(game, d):
    lines = ['; %s - added by Install_%s' % (d['title'], game), '[%s]' % d['tag'], 'name     = %s' % d['title']]
    if d['kind'] == 'c':
        lines += ['type     = cnetc', 'command  = PFILES:%s/%s' % (game, d['main']), 'assign   = PFILES: BBS:PFiles',
                  'charset  = cp437']
    else:
        lines += ['type     = cnetrexx', 'command  = PFILES:%s/%s' % (game, d['main']), 'assign   = PFILES: BBS:PFiles']
        if d.get('maint'):
            lines.append('maint    = rx PFILES:%s/%s' % (game, d['maint']))
    if d.get('single'):
        lines.append('single   = yes           ; one caller at a time')
    lines.append('level    = 10')
    return '\n' + '\n'.join(lines) + '\n'

def q(s):
    return s.replace('\\', '\\\\').replace('"', '\\"')

def installer(game, d):
    kind = 'CNet C PFile (a program)' if d['kind'] == 'c' else 'CNet ARexx PFile (a script)'
    upd = []
    for f in d['update_files']:
        if '#?' in f:
            upd.append('        (copyfiles (source "%s") (dest pfdir) (pattern "%s"))' % (game, f))
        else:
            upd.append('        (copyfiles (source "%s") (dest pfdir) (choices "%s"))' % (game, f))
    for sub in d['update_dirs']:
        upd.append('        (copyfiles (source "%s/%s") (dest (tackon pfdir "%s")) (all))' % (game, sub, sub))
    for sub in d.get('update_new', []):
        # only files the install doesn't have yet: an update must not overwrite what the sysop edited
        upd.append('        (if (not (exists (tackon pfdir "%s"))) (makedir (tackon pfdir "%s")))' % (sub, sub))
        upd.append('        (foreach "%s/%s" "#?" (if (not (exists (tackon (tackon pfdir "%s") @each-name)))'
                   ' (copyfiles (source (tackon "%s/%s" @each-name)) (dest (tackon pfdir "%s")) (nogauge))))'
                   % (game, sub, sub, game, sub, sub))
    if d.get('update_world'):
        # the world itself (Realm: 60x40 -> 80x50): only when the installed map isn't this one's size, and only
        # if the sysop says so - a map edited with RealmEdit is theirs.  The old files are kept as .old.
        first = d['update_world'][0]
        upd.append('        (if (<> (getsize (tackon pfdir "%s")) (getsize "%s/%s"))' % (first, game, first))
        upd.append('          (if (askbool (prompt "This version comes with a different world map.\\n\\n'
                   'Replace your world map and town list with it? Players, castles and saves keep their places; '
                   'your old map and town list are kept as .old files.")')
        upd.append('                       (help "Choose Keep mine if you changed the map with RealmEdit and want to keep '
                   'your changes. The new map has the same land where the old one was, with new ground and new '
                   'towns added around it.")')
        upd.append('                       (choices "Replace" "Keep mine") (default 0))')
        upd.append('            (')
        for w in d['update_world']:
            wd, wf = os.path.split(w)
            upd.append('              (if (exists (tackon pfdir "%s")) (copyfiles (source (tackon pfdir "%s")) (dest (tackon pfdir "%s")) '
                       '(newname "%s.old") (nogauge)))' % (w, w, wd, wf))
            upd.append('              (copyfiles (source "%s/%s") (dest (tackon pfdir "%s")) (choices "%s") (nogauge))'
                       % (game, wd, wd, wf))
        upd.append('            )))')
    exe = '\n'.join('(run (cat "Protect \\"" (tackon pfdir "%s") "\\" +e") (safe))' % e for e in d['exe'])
    maint_nuz = ('\\n\\nIts nightly maintenance (%s) is in the Doors.cfg entry - NilBBS runs it with the nightly '
                 'maintenance.' % d['maint']) if d.get('maint') else ''
    maint_cnet = ('\\n\\nAlso add a nightly CNet event that runs:\\n  rx PFILES:%s/%s' % (game, d['maint'])) if d.get('maint') else ''
    multi = 'several callers at once' if not d.get('single') else 'one caller at a time'
    return f'''; -------------------------------------------------------------------------
; {d["title"]} - Installer script
; Installs the door on NilBBS (and adds it to Doors.cfg), or copies it into
; CNet's PFILES: drawer, or just copies it where you say.  Run it again over an
; existing install to update the program: players and saves are kept.
; -------------------------------------------------------------------------
(set @app-name "{q(d["title"])}")
(complete 0)
(if (not (exists "{game}/{d["main"]}"))
    (abort "Can't find the {game} drawer next to this script.\\n\\n"
           "Run the installer from the drawer it came in."))

(message "{q(d["title"])}\\n\\n{q(d["blurb"])}\\n\\n"
         "A door game for NilBBS or CNet ({kind}). It is made for {multi}.")

(set bbs
    (askchoice
        (prompt "Which BBS is it for?")
        (help "NilBBS: the game goes into the BBS drawer's PFiles and is added to Config/Doors.cfg.\\n\\n"
              "CNet: the game goes into CNet's PFILES: drawer; you then add it in CNet's PFile editor "
              "(the installer tells you what to enter).\\n\\n"
              "Somewhere else: the drawer is just copied - see the ReadMe to set it up by hand.")
        (choices "NilBBS" "CNet" "Somewhere else (copy only)")
        (default 0)))

(if (= bbs 0)
  (
    (set base (askdir (prompt "Where is your NilBBS drawer?\\n(the BBS drawer: it holds NilBBS, Config and PFiles)")
                      (help "The drawer the NilBBS installer made, for example Work:BBS. If BBS: is assigned, it's that.")
                      (default (if (exists "BBS:Config/Doors.cfg" (noreq)) "BBS:" "Work:BBS"))))
    (while (not (exists (tackon base "Config/Doors.cfg") (noreq)))
        (set base (askdir (prompt "That drawer has no Config/Doors.cfg.\\n\\nPick the NilBBS BBS drawer itself:")
                          (help "It holds the programs NilBBS and BBSNode and the drawers Config and PFiles.")
                          (default base))))
    (makedir (tackon base "PFiles") (safe))
    (set pfdir (tackon base "PFiles/{game}"))
  ))
(if (= bbs 1)
  (
    (set base (askdir (prompt "Where is CNet's PFILES: drawer?")
                      (help "CNet keeps its door programs in the drawer assigned as PFILES:.")
                      (default (if (exists "PFILES:" (noreq)) "PFILES:" "Work:"))))
    (set pfdir (tackon base "{game}"))
  ))
(if (= bbs 2)
  (
    (set base (askdir (prompt "Where should the {game} drawer go?") (help "Any drawer.") (default "Work:")))
    (set pfdir (tackon base "{game}"))
  ))
(set @default-dest pfdir)
(complete 30)

(if (exists (tackon pfdir "{d["main"]}"))
  (
    (if (= 0 (askbool (prompt "{q(d["title"])} is already in\\n" pfdir "\\n\\nUpdate it? Players and saves are kept.")
                      (help "Update replaces the program and its screens only.")
                      (choices "Update" "Cancel") (default 1)))
        (exit "Nothing was changed." (quiet)))
{chr(10).join(upd)}
  )
  (copyfiles (prompt "Copying {q(d["title"])} to " pfdir) (help @copyfiles-help) (source "{game}") (dest pfdir) (all)))
{exe}
(complete 70)

(if (= bbs 0)
  (
    (set cfg (tackon base "Config/Doors.cfg"))
    (if (<> 0 (run (cat "Search >NIL: \\"" cfg "\\" \\"[{d["tag"]}]\\"") (safe)))
        (run (cat "Type \\"NilBBS-Doors.cfg\\" >>\\"" cfg "\\"") (safe)))
    (message "{q(d["title"])} is installed in\\n" pfdir "\\n\\n"
             "It's on the doors list as [{d["tag"]}] (level 10) - callers pick it from the doors menu. "
             "Change its name or level in BBSConfig's Doors page.{maint_nuz}")
  ))
(if (= bbs 1)
    (message "{q(d["title"])} is copied to\\n" pfdir "\\n\\n"
             "Now add it in CNet's PFile editor (as sysop, in the PFiles area):\\n"
             "  Name:  {q(d["title"])}\\n"
             "  Path:  PFILES:{game}/{d["main"]}\\n"
             "  Type:  {kind}\\n"
             "  Multi-user: {'yes' if not d.get('single') else 'NO - one caller at a time'}\\n"
             "Give it the access groups you want.{maint_cnet}\\n\\nThe ReadMe has the details."))
(if (= bbs 2)
    (message "{q(d["title"])} is copied to\\n" pfdir "\\n\\nSee the ReadMe for setting it up on your BBS."))
(complete 100)
(exit (quiet))
'''

def readme(game, d):
    kind = 'a CNet C PFile (a compiled program)' if d['kind'] == 'c' else 'a CNet ARexx PFile (a script)'
    nuz_type = 'cnetc' if d['kind'] == 'c' else 'cnetrexx'
    maint = ''
    if d.get('maint'):
        maint = f'''
NIGHTLY MAINTENANCE
  {d["maint"]} resets the daily turns and cleans up.  On NilBBS the Doors.cfg
  entry's "maint =" line has the nightly maintenance run it.  On CNet add a
  nightly event that runs:   rx PFILES:{game}/{d["maint"]}
'''
    multi = ('Made for several callers at once: every change to the shared files is\n'
             '  locked, so two nodes playing together is safe.') if not d.get('single') else \
            ('One caller at a time: NilBBS keeps others out while someone plays\n'
             '  ("single = yes"); on CNet set the PFile to single-user.')
    return f'''{d["title"]}
{"=" * len(d["title"])}

{d["blurb"]}

It is {kind} for NilBBS or CNet on the Amiga.
{multi}

INSTALLING WITH THE INSTALLER
  Double-click Install_{game}.  Pick NilBBS, CNet, or "somewhere else".
    NilBBS: point it at the BBS drawer.  The game goes into PFiles/{game} and is
            added to Config/Doors.cfg as [{d["tag"]}] at level 10.
    CNet:   point it at your PFILES: drawer; then add the PFile in CNet (below).
  Run it again later to update the program - players and saves are kept.

INSTALLING BY HAND - NilBBS
  1. Copy the {game} drawer into BBS:PFiles/ (make PFiles if it isn't there).
  2. Add the section in NilBBS-Doors.cfg to the end of BBS:Config/Doors.cfg
     (or add it in BBSConfig: Doors > New, then fill in the same fields):
       type    = {nuz_type}
       command = PFILES:{game}/{d["main"]}
       assign  = PFILES: BBS:PFiles
  3. Callers find it on the doors menu (X on the main menu).

INSTALLING BY HAND - CNet
  1. Copy the {game} drawer into your PFILES: drawer.
  2. Log on as sysop, go to the PFiles area and add an item in CNet's PFile
     editor:   path  PFILES:{game}/{d["main"]}
               name  {d["title"]}
               type  {"C program" if d["kind"] == "c" else "ARexx"}
     then set the access groups you want.  (Your CNet manual, "PFiles",
     covers the editor.)
{maint}
UPDATING BY HAND
  Copy only the program{"s" if d["kind"] == "rexx" else ""} and the screen drawer over the old ones.  Never copy
  the Data drawer over an installed game - it holds the players.
'''

def build(game):
    d = DOORS[game]
    out = os.path.join(OUTROOT, game)
    if os.path.exists(out):
        shutil.rmtree(out)
    d['build'](os.path.join(out, game))
    open(os.path.join(out, 'NilBBS-Doors.cfg'), 'w', newline='\n').write(doors_cfg(game, d))
    open(os.path.join(out, 'Install_' + game), 'w', newline='\n', encoding='latin-1').write(installer(game, d))
    open(os.path.join(out, 'ReadMe'), 'w', newline='\n', encoding='latin-1').write(readme(game, d))
    copy(os.path.join(PROJ, d.get('repo', game), 'LICENSE'), os.path.join(out, 'LICENSE'))   # MIT, the game's own
    open(os.path.join(out, 'Install_%s.info' % game), 'wb').write(pkg_icon('Install_' + game))
    package_icons(out)
    bad = personal_check(out)
    if bad:
        for f, w in bad:
            print('PERSONAL INFO: %s mentions %r' % (f, w))
        sys.exit('%s package NOT clean - fix the above' % game)
    n = sum(len(fs) for _, _, fs in os.walk(out))
    size = sum(os.path.getsize(os.path.join(b, f)) for b, _, fs in os.walk(out) for f in fs)
    print('%-12s %s  (%d files, %d KB) - personal-info check passed' % (game, out, n, size // 1024))

if __name__ == '__main__':
    for g in (sys.argv[1:] or list(DOORS)):
        build(g)
