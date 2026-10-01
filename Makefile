# NilBBS - build under WSL with the bebbo amiga-gcc toolchain on PATH:
#   wsl -e bash -lc 'export PATH=/opt/amiga/bin:$PATH; cd /mnt/c/projects/NilBBS && make'
# Output lands in out/ (and the install tree in out/BBS with `make dist`).

CC      = m68k-amigaos-gcc
# -Wno-format: this NDK makes ULONG an unsigned int, so every %lu "mismatches"
CFLAGS  = -m68020 -O2 -noixemul -Wall -Wno-pointer-sign -Wno-format -fomit-frame-pointer -s -Isrc/common -isystem src/cnetsdk
# the version is the one line in VERSION (edit it for a release); the date is the day of the build
VERSION := $(shell cat VERSION)
VERDATE := $(shell date +%-d.%-m.%Y)
CFLAGS  += -DBBS_VERSION='"$(VERSION)"' -DBBS_VERDATE='"$(VERDATE)"'
LIBS    =

COMMON  = src/common/util.c src/common/cfg.c src/common/sha256.c src/common/shared.c \
          src/common/ipfilter.c src/common/userdb.c src/common/msgbase.c src/common/dizcore.c src/common/lists.c src/common/lang.c
NODE    = src/node/node.c src/node/spy.c src/node/telnet.c src/node/charset.c src/node/term.c \
          src/node/login.c src/node/menu.c src/node/door.c src/node/misc.c \
          src/node/sysop.c src/node/msgui.c src/node/fileui.c src/node/zmodem.c src/node/cnetc.c src/node/diz.c \
          src/node/acs.c src/node/community.c src/node/tele.c src/node/xymodem.c src/node/fse.c src/node/qwk.c \
          src/node/serial.c
HDRS    = VERSION src/common/bbs.h src/common/cfg.h src/common/msgbase.h src/common/dizcore.h src/common/lang.h src/node/node.h src/node/zmodem.h

all: out/NilBBS out/BBSNode out/BBSCtl out/BBSControl out/BBSSchedule out/BBSConfig out/DoorCheck out/BBSToss out/BBSMail out/BBSMaint out/Guess out/XIMTest out/XIMProbe out/AEDoor.library out/CNTest out/LCBDoor out/NilTerm

out/NilBBS: src/daemon/nilbbs.c $(COMMON) $(HDRS)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ src/daemon/nilbbs.c $(COMMON) $(LIBS)

out/BBSNode: $(NODE) $(COMMON) $(HDRS)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ $(NODE) $(COMMON) $(LIBS)

out/BBSCtl: src/ctl/bbsctl.c src/config/ini.c src/config/ini.h $(COMMON) $(HDRS)
	@mkdir -p out
	$(CC) $(CFLAGS) -Isrc/config -o $@ src/ctl/bbsctl.c src/config/ini.c $(COMMON) $(LIBS)

out/BBSSchedule: src/ctl/bbsschedule.c $(COMMON) $(HDRS)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ src/ctl/bbsschedule.c $(COMMON) $(LIBS)

out/BBSControl: src/ctl/bbscontrol.c $(COMMON) $(HDRS)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ src/ctl/bbscontrol.c $(COMMON) $(LIBS)

out/BBSConfig: src/config/bbsconfig.c src/config/ini.c src/config/ini.h src/config/doorcheck.c src/config/doorcheck.h $(COMMON) $(HDRS)
	@mkdir -p out
	$(CC) $(CFLAGS) -Isrc/config -o $@ src/config/bbsconfig.c src/config/ini.c src/config/doorcheck.c $(COMMON) $(LIBS)

out/DoorCheck: src/config/dccli.c src/config/doorcheck.c src/config/doorcheck.h src/config/ini.c src/config/ini.h $(COMMON) $(HDRS)
	@mkdir -p out
	$(CC) $(CFLAGS) -Isrc/config -o $@ src/config/dccli.c src/config/doorcheck.c src/config/ini.c $(COMMON) $(LIBS)

# BBSMail: the binkp mailer (calls the uplink, sends BBSToss's outbound, receives into the inbound)
out/BBSMail: src/fido/bbsmail.c src/common/util.c src/common/cfg.c src/common/shared.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ src/fido/bbsmail.c src/common/util.c src/common/cfg.c src/common/shared.c $(LIBS)

out/BBSToss: src/fido/bbstoss.c $(COMMON) $(HDRS)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ src/fido/bbstoss.c $(COMMON) $(LIBS)

out/BBSMaint: src/maint/bbsmaint.c src/maint/import.c src/maint/rexxmaint.c $(COMMON) $(HDRS)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ src/maint/bbsmaint.c src/maint/import.c src/maint/rexxmaint.c $(COMMON) $(LIBS)

# NilTerm: the sysop's ANSI terminal (its own screen, the VGA font); util.c (run_with_stack), shared.c (WATCH=n + the live port), cfg.c (port= fallback); ntzm.c/ntxy.c = the node's ZMODEM/X/YMODEM (src/node) over NilTerm's connection
out/NilTerm: src/nilterm/nilterm.c src/nilterm/nilfont.h src/nilterm/ntzio.h src/nilterm/ntzm.c src/nilterm/ntxy.c src/node/zmodem.c src/node/xymodem.c src/node/zmodem.h src/common/util.c src/common/shared.c src/common/cfg.c $(HDRS)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ src/nilterm/nilterm.c src/nilterm/ntzm.c src/nilterm/ntxy.c src/common/util.c src/common/shared.c src/common/cfg.c $(LIBS)

out/CNTest: src/doors/cntest.c
	@mkdir -p out
	$(CC) $(CFLAGS) -w -o $@ src/doors/cntest.c

out/XIMTest: src/doors/ximtest.c
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ src/doors/ximtest.c

out/XIMProbe: src/doors/ximprobe.c
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ src/doors/ximprobe.c

# our own AEDoor.library (the /X door API), freestanding: exec only
out/AEDoor.library: src/aedoor/aedoor_start.S src/aedoor/aedoor.c
	@mkdir -p out
	$(CC) -m68000 -c -o out/aedoor_start.o src/aedoor/aedoor_start.S
	$(CC) -m68000 -O2 -fomit-frame-pointer -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -Wall -nostdlib -c -o out/aedoor.o src/aedoor/aedoor.c
	$(CC) -m68000 -nostartfiles -nostdlib -s -o $@ out/aedoor_start.o out/aedoor.o

out/Guess: src/doors/guess.c
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ src/doors/guess.c

# LCBDoor: Last Call BBS JavaScript doors (e.g. the LORD remake) on Duktape.
# duktape.c is 3.6 MB and slow to compile, so it's built once into out/duktape.o.
out/duktape.o: src/lcbdoor/duktape/duktape.c
	@mkdir -p out
	$(CC) -m68020 -O2 -noixemul -fomit-frame-pointer -c -o $@ $<

out/LCBDoor: src/lcbdoor/lcbdoor.c out/duktape.o
	$(CC) -m68020 -O2 -noixemul -fomit-frame-pointer -s -Wall -Wno-pointer-sign -Isrc/lcbdoor/duktape -o $@ src/lcbdoor/lcbdoor.c out/duktape.o -lm

# a ready-to-copy install tree: out/BBS  ->  assign BBS: to it on the Amiga
dist: all
	rm -rf out/BBS
	cp -r dist/BBS out/BBS
	cp dist/BBS.info out/
	cp out/NilBBS out/BBSNode out/BBSCtl out/BBSControl out/BBSSchedule out/BBSConfig out/DoorCheck out/BBSToss out/BBSMail out/BBSMaint out/NilTerm out/BBS/
	mkdir -p out/BBS/Doors/Guess out/BBS/Doors/CNTest out/BBS/Doors/XIMTest
	cp out/Guess out/BBS/Doors/Guess/
	cp out/CNTest out/BBS/Doors/CNTest/
	cp out/XIMTest out/BBS/Doors/XIMTest/

clean:
	rm -rf out

.PHONY: all dist clean
