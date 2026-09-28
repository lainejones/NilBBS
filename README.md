# NilBBS

A native, multi-node **telnet BBS for AmigaOS 3.1+ (68020+)**, written in C.
Callers telnet in over any `bsdsocket.library` stack (Roadshow, AmiTCP, Miami,
a314bsd, WinUAE's bsdsocket emulation).

## Features

- **Multi-node telnet.** One listener daemon, one `BBSNode` process per caller
  (up to 32 nodes). The accepted socket is handed over with
  `ReleaseSocket`/`ObtainSocket`. Telnet negotiation covers ECHO, SGA,
  BINARY, NAWS (window size) and TTYPE (terminal names).
- **Terminal autodetection.** The BBS sends an ANSI cursor-position request
  and sizes the screen with a second one. A one-glyph UTF-8 probe tells an
  8-bit terminal from a UTF-8 one.
  - **ANSI:** full colour, CP437 art.
  - **VT100:** colour is filtered out, bold/underline/reverse/blink stay, and
    box drawing uses DEC Special Graphics.
  - **ASCII:** escapes are stripped, cursor-forward becomes spaces, and box
    drawing becomes `+-|`.
  - **Character sets:** CP437 (SyncTERM, NetRunner), UTF-8 (PuTTY, xterm,
    macOS/Linux), Amiga (ISO-8859-1: NComm, Term, AmigaTerm), or 7-bit.
    CP437 art is translated for whichever the caller uses, and so is input.
- **IP blocking.**
  - `Config/IPFilter.cfg` has allow and deny rules (CIDR or `1.2.*`
    wildcards) and an optional `default deny` for a private board.
  - Allow rules act as a whitelist, so your own LAN never gets auto-banned.
  - Automatic bans after a **connect flood** (N connects in M seconds) or
    **repeated failed logins**, timed or permanent. Bans are stored in
    `Data/Bans.dat` and survive restarts.
  - Manage bans from the sysop menu, `BBSCtl` or `BBSControl`.
- **Banned handles**: `BBS:Config/BannedNames.cfg` lists handles nobody may sign up as, one per
  line, any case, `*` = anything (`*sysop*`, `evil*`). Edit it in BBSConfig (Banned names), online
  from the sysop menu (`X`: list, add, delete, test a handle) or in any editor. Existing users are
  not affected; refused attempts are logged.
- **Doors.**
  - **Native AmigaDOS CLI doors.** Any CLI program works. Its `Input()` and
    `Output()` are file handles whose "handler" is the node itself, so the
    door gets cooked line input with echo and editing, or raw keys after
    `SetMode(fh,1)`.
  - `WaitForChar()` and `Open("*")` reach the caller too, and Amiga `0x9B`
    CSI output is translated.
  - Drop files: `DOOR.SYS`, `DORINFO1.DEF` and `DOOR32.SYS`, plus
    `$BBSUSER`/`$BBSTIME`/... environment variables.
  - The shipped *Remote Shell* door (`NewShell *`, sysop only) gives a live
    AmigaDOS shell over telnet.
  - **CNet ARexx doors** (`type = cnetrexx`) run unmodified. NilBBS answers
    CNet's ARexx host commands on port `CNETREXX<n>`: TRANSMIT, SENDSTRING,
    QUERY, GETCHAR, MAYGETCHAR, PROMPT, GETUSER, BBSIDENTIFY and more.
    When the caller drops, input commands return `###PANIC` as they do on
    CNet, and after a grace period the script gets a single break so it can
    save and exit. The node is never left hanging.
  - **CNet C doors** (`type = cnetc`). NilBBS becomes the door's CNet port:
    - It builds a `CPort` and `PortData` laid out byte for byte from the CNet 4
      SDK headers (in `src/cnetsdk`; not in the public repo - see below), and checks the offsets at
      compile time.
    - It answers the `CallHost` interface, including PutText, EnterLine,
      OneKey, ReadFile, SetDoing, ReadAccount and FindAccount, and
      translates CNet MCI colour codes.
    - Hang-up works as on CNet: `z->Carrier` drops and `z->Dumped` is set.
    - Doors built on `cnet.library`'s database routines still need a real
      CNet.
  - **AmiExpress XIM doors** (`type = xim`), the biggest library of Amiga
    doors. NilBBS serves `AEDoorPort<n>` with the /X 5 semantics: output,
    hotkeys, prompts, line input, user data (`DT_*`), screens and ZMODEM.
    Tested through the real AEDoor.library 2.8. Loss of carrier returns -1
    as it does under /X.
  - **Last Call BBS JavaScript doors** via `LCBDoor`, a native CLI door
    with an embedded Duktape engine. It implements the Last Call BBS door
    API (`onConnect`/`onUpdate`/`onInput`, `drawText`, the 56x20 screen)
    plus the `_bbs_*` save API, so the free JavaScript remake of **Legend of
    the Red Dragon** runs natively on the Amiga, with the caller's BBS
    handle as their warrior name and one shared game file for everybody.
    See *LORD on the Amiga* below.
  - **Remote doors** over `rlogin` (BBSLink / DoorParty style), `telnet` or
    a raw TCP bridge, e.g. to a Linux box running PC doors under
    dosemu/DOSBox.
- **Messages.**
  - Local areas, private mail and FidoNet echo areas.
  - Reader with threads (`T`), reply-with-quote, and a new-message scan
    across all areas.
  - A **full-screen editor** for ANSI and VT100 callers. It word-wraps and
    supports arrows, Home/End and PgUp/PgDn. `^Z` saves, `^A` aborts, `^Q`
    quotes the message being answered, `^Y` deletes a line, and `Esc` opens
    a menu. ASCII callers, or anyone who picks it in their settings, get the
    line editor with word-wrap instead. Quoted lines wrap rather than being
    cut off. Last-read pointers are kept per user.
- **QWK offline mail.** In the message menu, `W` packs everything new into
  a `.QWK` packet containing CONTROL.DAT, MESSAGES.DAT, `.NDX` indexes and
  DOOR.ID. Private mail is marked private in the packet, and last-read
  pointers only move once the transfer succeeds. `U` takes a `.REP` packet
  back and posts the replies, with access checked per area. The archiver
  is configurable: LhA by default, or Zip for PC readers.
- **FidoNet echomail.** `BBSToss` tosses Type 2+ packets (with dupe
  checking) and exports new posts with MSGID, origin, SEEN-BY and PATH to a
  BinkleyTerm-style outbound. Pair it with a binkp mailer such as binkd.
- **Files.**
  - Areas described by `files.bbs`, with new-file scan and wildcard search.
  - **ZMODEM** download and upload, with batches of up to 10 files, CRC-32,
    error recovery, and full control-character escaping so transfers survive
    non-binary telnet clients.
  - **YMODEM** batch, **XMODEM-1K** and **XMODEM** (CRC or checksum),
    implemented natively for terminals without ZMODEM. Each caller picks a
    protocol in their settings (`K`, then `F`). YMODEM files keep their exact
    size, and XMODEM uploads have the `^Z` padding stripped.
- **Access conditions (ACS).** Menus, menu items, doors, message and file
  areas, bulletins and conferences can take an access condition as well as a
  level. For example, `L20 FA !FB | Gstaff` means "level 20 or more, with
  flag A and without flag B, or anyone in group staff". The sysop sets 26
  access flags (A-Z), a group and download credits per user.
- **Sub-ops.** An area's `subop = <ACS>` lets someone moderate it without
  being sysop: they can delete any message in a message area, and in a file
  area they can move files to `.trash` and write FILE_ID.DIZs.
- **Conferences**, AmiExpress style (`Config/Conferences.cfg`). Areas carry
  `conf = TAG`; callers switch with `J`. The last conference is remembered,
  and `Text/conf_<TAG>.ans` is shown on joining.
- **File ratios and credits.** With `ratio = N`, callers download N KB per
  KB uploaded, after a free allowance. Sysop-granted credits add to that,
  exempt callers are set by ACS, and areas marked `free = yes` never count.
  `R` in the file menu shows your standing.
- **Bulletins** (`Config/Bulletins.cfg`). Bulletins changed since your last
  call are marked NEW and offered at logon.
- **Voting booth.** Topics with up to 10 choices, one vote per user, and
  results shown as bars. Waiting topics are announced at logon. Who may add
  topics is set by ACS.
- **Finger.** `L` looks up a user's profile and the *plan* they write in
  their settings. The daemon also answers real finger on port 79:
  `finger @bbs` lists who's online, and `finger name@bbs` shows a profile.
  Finger lookups don't count toward the connect-flood ban.
- **Teleconference** (`T`): multi-user chat with channels 1-99, `/me`
  actions, `/msg` whispers and `/w` who's here. Lines arrive live above what
  you're typing.
- **Sysop chat.** `P` pages the sysop. If a sysop is logged on, they get the
  page with its reason; otherwise the Amiga's screen beeps and the caller can
  leave a message. From the sysop menu, `H` breaks in on any node for a
  private chat, which ends for both sides when either one leaves.
- **Local logon.** Run `BBSNode LOCAL` from a Shell on the BBS machine for a
  console-window session on a free node, with no network needed. It has the
  same menus, chat and doors as a telnet caller, and shows the plain-text
  screens because the console has no block graphics. File transfers aren't
  offered locally.
- **The rest of the classic set:** one-liners, last callers, who's online,
  user list, node-to-node messages, sysop broadcasts, per-level daily time
  limits, an idle timeout, data-driven menus (`Menus/*.mnu`), and ANSI screens
  with pipe colour codes (`|00`-`|23`) and MCI codes (`|UN`, `|TL`, `|BN`,
  ...).
- **FILE_ID.DIZ.**
  - An archive's own FILE_ID.DIZ is what file listings show. It is
    extracted once with LhA or UnZip and cached in `<area>/.diz`;
    `files.bbs` is the fallback for everything else.
  - When an upload has no DIZ, the caller is offered to create one.
  - The sysop can create or replace any archive's DIZ (sysop menu `Z`).
  - New DIZs use the BBS's house style, `Text/diz.tmpl` (a template
    with `{NAMEVER}`, `{DESC}`, `{BBS}` and more), and are written into
    the archive and verified.
- **ARexx port `NILBBS`** on the daemon: `STATUS`, `WHO`, `NODEUSER n`,
  `KICK n`, `SEND n text`, `BROADCAST text`, `BAN ip [mins [reason]]`,
  `UNBAN ip`, `USERINFO name`, `EVENT tag`, `RELOAD`, `SHUTDOWN` and
  `VERSION`. Results come back in `RESULT`. For example:
  `rx "options results; address NILBBS; 'WHO'; say result"`.
- **Event scheduler** (`Config/Events.cfg`). The daemon runs commands at a
  set time or every N minutes, on chosen days, with output in
  `Logs/Event-<TAG>.log`. An exclusive event (a backup or pack) warns
  callers, refuses new calls, logs everyone off, runs, then reopens.
- **Nightly maintenance** (`BBSMaint`), run by the NIGHTLY event that ships
  switched on (04:00, exclusive):
  - Packs message bases: drops deleted messages, and optionally old ones or
    anything past a per-area count or read private mail past an age. Reply
    links and every user's last-read pointer are renumbered to match, and
    local echomail not yet exported is never dropped.
  - Optionally retires long-inactive callers.
  - Empties old file-area trash, drops stale DIZ cache entries, clears
    leftover node files and rotates logs.
  - Runs each door's own `maint =` command from Doors.cfg. For LORD,
    `LCBDoor LORD.js MAINT` starts the new day and removes warriors who
    haven't played for 30 days.
  - Mails the sysop a report. Everything is also logged in
    `Logs/Maint.log`.
  - Run it now with `rx "address NILBBS 'EVENT NIGHTLY'"` or `BBSMaint`
    from a Shell. With callers online, packing is skipped.
- **Sysop tools:**
  - `BBSConfig`: a Workbench configuration editor, like CNet's config.
    Sections: System, New users, Security, IP rules, Doors, Message areas,
    File areas, Files & DIZ, Conferences, Bulletins, Events, Community/QWK,
    FidoNet, Menus and Users. Users covers access flags, group and credits.
    It edits the config files in place
    and keeps their comments, then tells a running NilBBS to reload.
  - `BBSControl`: a Workbench (GadTools) console with the live node list,
    kick, kick+ban, messaging, the ban list, reload and shutdown.
  - `NilTerm`: an ANSI telnet client, and the sysop's terminal (BBSControl's
    **Logon** button): its own 640x400 16-colour screen (the VGA palette), the
    IBM VGA 8x16 CP437 font, a SyncTERM-style dialing directory, status bar,
    scrollback, capture, clipboard, ZMODEM (auto-start) / YMODEM / XMODEM, and
    Settings (folders). It also ships on its own: `tools/mknilterm.py` builds the
    NilTerm package (installer, AmigaGuide manual, LICENSE). The font is from
    "The Ultimate Oldschool PC Font Pack" by VileR (https://int10h.org/oldschool-pc-fonts/),
    CC BY-SA 4.0.
  - `BBSCtl`: a Shell tool with WHO, BANS, BAN, UNBAN, KICK, SEND, RELOAD
    and SHUTDOWN.
  - A remote sysop menu, reached with `!` from the main menu.

## Install

1. Copy the `BBS` drawer anywhere (e.g. `Work:BBS`).
2. In a Shell: `CD Work:BBS` then `Execute Setup`. This assigns `BBS:`, sets
   the protection bits, and can optionally add NilBBS to `S:User-Startup`.
3. Edit `BBS:Config/NilBBS.cfg` and `BBS:Config/IPFilter.cfg`.
4. With your TCP/IP stack up, run `Run >NIL: BBS:NilBBS` (or
   `BBS:NilBBS PORT=2323`).
5. Telnet in and type `NEW`. **The first account created becomes the sysop.**

From Workbench, the `BBS` drawer has icons for everything:
- **Start NilBBS** and **Stop NilBBS**
- **Local Logon**
- **Run Maintenance**, which opens a window with the report
- **BBSConfig** and **BBSControl**

The four scripts are run by IconX. The icons carry both a classic
4-colour image and an OS 3.5+ colour image, and are regenerated with
`python tools/mkicons.py`.

| Program | What it is |
|---|---|
| `NilBBS` | the listener daemon: `NilBBS [PORT=n] [NODES=n] [QUIET]` |
| `BBSNode` | one caller's session (started by NilBBS, not by you) |
| `BBSCtl` | Shell sysop control |
| `BBSConfig` | Workbench configuration editor (every setting, doors, areas, menus, users) |
| `BBSControl` | Workbench sysop console (live nodes, bans) |
| `NilTerm` | the sysop's ANSI terminal: its own 640x400 screen with the IBM VGA font, logs on to a node (`NilTerm [PORT=n] [HOST=name]`, BBSControl's Logon button, or double-click its icon - Tool Types `PORT=`, `HOST=`, `MODEID=`, `NATIVE`, `SMALL`) |
| `BBSToss` | FidoNet toss/scan: `BBSToss [TOSS] [SCAN]` |
| `BBSMaint` | nightly maintenance: `BBSMaint [NODOORS] [NOPACK]` (the NIGHTLY event runs it) |
| `Doors/Guess/Guess` | sample door, and a template for writing your own |

## Configuration files (`BBS:Config`)

| File | Contents |
|---|---|
| `NilBBS.cfg` | name, sysop, port, nodes, flood/failed-login bans, new-user policy, time limits |
| `IPFilter.cfg` | allow/deny rules (`BBSCtl RELOAD` after editing) |
| `Doors.cfg` | one `[TAG]` per door: type, command, drop file, level, charset |
| `MsgAreas.cfg` | message areas (append new ones at the end: last-read is by position) |
| `FileAreas.cfg` | file areas (directory + levels) |
| `Fido.cfg` | FidoNet address, uplink, inbound/outbound |

Menus live in `BBS:Menus/*.mnu`. Screens (`.ans` for ANSI, `.asc` for plain
text, `.vt` for VT100) live in `BBS:Text`: `connect`, `welcome`, `news`,
`newuser`, `logoff`, `sysinfo`, or any screen a menu names.

### Other languages

Everything the BBS says to callers comes from a plain text language file, so the
user interface can be in any language. `BBS:Text/Language/English.lng` lists every
line (`key = text`); copy it to e.g. `Deutsch.lng` and translate the right-hand
side - keep the `|07` colour codes and every `%s`/`%d` (a line whose `%`-codes
don't match the English is ignored, never crashes a node), and any line you leave
out stays English. `language = Deutsch` in `NilBBS.cfg` (or BBSConfig, System)
makes it the board's default; callers pick their own in *Your settings* (G) and at
signup. A translation may also bring menus (`BBS:Menus/Deutsch/main.mnu`) and
screens (`BBS:Text/Deutsch/`); whatever it doesn't bring - your custom ANSI
screens, door names, area names - is shown to everyone as it is.
`python tools/langcheck.py Deutsch.lng` checks a translation on a PC.

## Writing a door

A door is an ordinary CLI program. Print to `Output()`, read from `Input()`,
and use `SetMode(Input(), 1)` for single keys. When the caller hangs up,
`Read()` returns 0 and the door receives `CTRL-C`. See `src/doors/guess.c`.

## LORD on the Amiga

LORD itself isn't shipped: the JavaScript remake has no licence. To install it:

1. Get `LORD.js` from <https://bitbucket.org/almostsweet/lord/src/main/>.
2. Convert it to ES5 for Duktape. This needs Node.js with `@babel/cli`,
   `@babel/core` and `@babel/preset-env` in `~/babelwork`:
   `src/lcbdoor/transpile.sh LORD.js LORD.es5.js src/lcbdoor/lord-addons.js`.
   The add-on file comes with NilBBS. It adds the **new day** the remake is
   missing: fresh forest and player fights, full hit points and a revival,
   once per calendar day, with 20 forest fights by default. It also adds five
   new places in **(O)ther Places**, written for NilBBS and named after
   classic LORD add-ons:
   - **Forest Outhouse:** a daily gamble for gold, fights, a gem, or embarrassment.
   - **The Gem Trader:** buy and sell gems at a price that changes daily.
   - **Lets Go Fishing:** spend forest fights on casts, up to 10 a day.
   - **Wheel:** bet gold at 2x, 3x or 5x, with gem and fight wedges, 3 spins a day.
   - **The Wise One:** a daily riddle for experience and charm.
   - **Dragon's Claw Tavern:** ale, arm-wrestling Grog, dice, and a carving
     wall on the bar that every player shares.
   - **Castle Coldrake:** a daily raid through the gate, the west tower and
     the barracks to Lord Coldrake, with fights scaled to your character.
   The limits are settings at the top of `lord-addons.js`.
   The script also fixes two upstream bugs: an undeclared variable that the
   file's strict mode rejects, and a stray `$` before player names.
3. Copy `LORD.es5.js` to `BBS:Doors/LORD/LORD.js` next to `LCBDoor`, and
   uncomment the `[LORD]` entry in `Doors.cfg`.

Usage: `LCBDoor <script.js> [USER=name] [DROP=DOOR.SYS] [DATA=file] [LOG=file]`.
- The warrior name comes from `USER=`, or otherwise from line 36 of the
  DOOR.SYS drop file.
- Game data goes in `<script>.dat` by default, shared by every caller.
- The door swaps itself onto a 192K stack, so the door's `stack` setting
  doesn't matter. A carrier drop saves and exits at once.

## Building

This needs the bebbo amiga-gcc toolchain; build under WSL/Linux:

```bash
make            # out/NilBBS out/BBSNode out/BBSCtl out/BBSControl out/BBSToss out/Guess
make dist       # out/BBS - the ready-to-copy install drawer
python tools/mkrelease.py    # out/release/NilBBS (installer package)
python tools/mknilterm.py    # out/release/NilTerm (NilTerm's own package)
```

The CNet door support builds against CNet's own SDK headers, which aren't
published with the source: copy them from a CNet 3 or 4 installation's `sdk`
drawer into `src/cnetsdk` (see the README there). The release `.lha` needs none
of this.

The end-to-end tests (a scripted telnet client driving the BBS in WinUAE with
`bsdsocket_emu=true`: login, detection, doors, IP bans, multi-node use, ZMODEM
against lrzsz, rlogin, FidoNet packets) aren't in the public repo.
