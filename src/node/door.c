/*
 * door.c - running external programs ("doors") for the caller.
 *
 * Doors are defined in BBS:Config/Doors.cfg, one [TAG] section each:
 *
 *   [GUESS]
 *   name     = Guess the Number
 *   type     = cli             ; cli | tcp | rlogin | telnet
 *   command  = BBS:Doors/Guess/Guess %f
 *   dir      = BBS:Doors/Guess ; current directory for the door
 *   dropfile = door.sys        ; door.sys dorinfo1.def door32.sys all none
 *   level    = 10              ; minimum security level
 *   acs      = FD              ; optional extra condition (see acs.c)
 *   charset  = cp437           ; what the door prints: cp437 | amiga
 *   amigacsi = yes             ; 0x9B in the door's output is a CSI (Amiga console style)
 *   arrows8  = no              ; send arrow keys as Amiga 0x9B sequences
 *   rawout   = no              ; don't filter/translate output (binary doors)
 *   stack    = 16384
 *   single   = no              ; yes = only one node at a time
 *   host     = bbs.example.org ; tcp/rlogin/telnet
 *   port     = 513
 *   rlogin_user = %h           ; tcp/rlogin: the name sent to the server
 *   rlogin_term = ansi-bbs/115200
 *
 * Macros in command/dir/rlogin_*:  %n node  %f drop file  %d drop dir
 *   %u handle  %r real name  %l level  %t minutes left  %i user number
 *   %p caller IP  %T terminal (ANSI/VT100/ASCII)
 *
 * CLI doors: the door's Input()/Output() are file handles whose "handler" is
 * a message port in this node process.  We answer the DOS packets ourselves -
 * ACTION_READ from the caller's keyboard (cooked line editing or SetMode()
 * raw), ACTION_WRITE to the caller's screen through the terminal filter, and
 * ACTION_WAIT_CHAR / SCREEN_MODE / FINDINPUT for Open("*").  Any AmigaDOS CLI
 * program therefore works as a door without knowing it is on a BBS.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/filio.h>
#include <netinet/in.h>
#include <netdb.h>
#include <proto/bsdsocket.h>
#include <errno.h>
#include <rexx/storage.h>
#include <rexx/rxslib.h>
#include <proto/rexxsyslib.h>

#include "node.h"
#include "zmodem.h"

extern struct Library *SocketBase;

/* ---- door definition ------------------------------------------------------ */

#define HUP_RULES 12
struct Door {
    char tag[NAMELEN];
    char name[LONGNAME];
    char type[12];
    char command[PATHLEN];
    char dir[PATHLEN];
    char dropfile[24];
    char host[80];
    char ruser[NAMELEN * 2];
    char rterm[40];
    UWORD port;
    UBYTE level;
    char  acs[64];
    UBYTE srccs;
    BOOL amigacsi, arrows8, rawout, single, qquit, txnewline;
    BOOL debug;                     /* debug = yes: log every /X command the door sends */
    BOOL hup_error;                 /* hangup_read = error: after a hang-up reads fail (-1), not EOF */
    char hupkeys[160];              /* hangup_keys: typed into the door when the caller drops */
    LONG huprounds, hupgrace;       /* how many times, and for how long (seconds), before EOF */
    char stalelock[64];             /* stale_lock: the door's own "in use" file, removed at launch */
    char hupon[HUP_RULES][120];     /* hangup_1..12: "text on screen | keys to type" */
    LONG cnetver;
    BOOL oldmci;                    /* old_mci: CNet 1.x/2.x \c1-style codes in the text */
    LONG panicgrace;
    char assign[PATHLEN];
    LONG stack;
};

static struct Cfg *load_doors(void)
{
    return cfg_load("BBS:Config/Doors.cfg");
}

static BOOL door_get(struct Cfg *dc, const char *tag, struct Door *d)
{
    const char *cmd, *host;
    memset(d, 0, sizeof(*d));
    cmd = cfg_sget(dc, tag, "command", "");
    host = cfg_sget(dc, tag, "host", "");
    if (!cmd[0] && !host[0]) return FALSE;
    str_copy(d->tag, tag, sizeof(d->tag));
    str_copy(d->name, cfg_sget(dc, tag, "name", tag), sizeof(d->name));
    str_copy(d->type, cfg_sget(dc, tag, "type", "cli"), sizeof(d->type));
    str_copy(d->command, cmd, sizeof(d->command));
    str_copy(d->dir, cfg_sget(dc, tag, "dir", ""), sizeof(d->dir));
    str_copy(d->dropfile, cfg_sget(dc, tag, "dropfile", "door.sys"), sizeof(d->dropfile));
    str_copy(d->host, host, sizeof(d->host));
    str_copy(d->ruser, cfg_sget(dc, tag, "rlogin_user", "%h"), sizeof(d->ruser));
    str_copy(d->rterm, cfg_sget(dc, tag, "rlogin_term", "ansi-bbs/115200"), sizeof(d->rterm));
    d->level    = (UBYTE)cfg_sint(dc, tag, "level", 0);
    str_copy(d->acs, cfg_sget(dc, tag, "acs", ""), sizeof(d->acs));
    {   /* "amiga" and "latin1" are the same thing: ISO-8859-1 */
        const char *cs = cfg_sget(dc, tag, "charset", !str_icmp(d->type, "xim") ? "amiga" : "cp437");
        d->srccs = (!str_icmp(cs, "amiga") || !str_icmp(cs, "latin1")) ? CS_LATIN1 : CS_CP437;
    }
    d->amigacsi = cfg_sbool(dc, tag, "amigacsi", TRUE);
    d->arrows8  = cfg_sbool(dc, tag, "arrows8", FALSE);
    d->rawout   = cfg_sbool(dc, tag, "rawout", FALSE);
    d->single   = cfg_sbool(dc, tag, "single", FALSE);
    d->qquit    = cfg_sbool(dc, tag, "qquit", FALSE);
    d->debug    = cfg_sbool(dc, tag, "debug", FALSE);
    /* old DOS doors that loop on EOF often give up on a read error - what a dropped modem gave them */
    d->hup_error = !str_icmp(cfg_sget(dc, tag, "hangup_read", "eof"), "error");
    /* a door that never notices the caller has gone (no EOF / CTRL-C check - most 1990s DOS doors):
     * after a drop NilBBS walks it out like a caller would.  Each time it goes quiet and wants input,
     * the first hangup_<n> rule ("text on screen | keys") whose text is on screen is typed, else
     * hangup_keys.  In keys: \r Enter, \b backspace, \e Esc, \n, \t, \\, ~ = wait a second,
     * %u = the caller's handle.  At most hangup_rounds steps (default 30) and hangup_grace seconds
     * (default 90), then EOF as for any door (feed_choose / feed_ready). */
    str_copy(d->hupkeys, cfg_sget(dc, tag, "hangup_keys", ""), sizeof(d->hupkeys));
    d->huprounds = cfg_sint(dc, tag, "hangup_rounds", 30);
    {   /* hangup_1 .. hangup_12 = text on screen | keys to type (feed_choose) */
        int i;
        char k[12];
        for (i = 0; i < HUP_RULES; i++) {
            sprintf(k, "hangup_%d", i + 1);
            str_copy(d->hupon[i], cfg_sget(dc, tag, k, ""), sizeof(d->hupon[i]));
        }
    }
    d->hupgrace = cfg_sint(dc, tag, "hangup_grace", 90);
    /* a single-player door's own "someone is playing" file (DSE: SERunning), left behind when the Amiga
     * went down mid-game - with single = yes nobody else can be in the door, so at launch it's stale */
    str_copy(d->stalelock, cfg_sget(dc, tag, "stale_lock", ""), sizeof(d->stalelock));
    /* CNet 3.05 ends every TRANSMIT with a newline: a CNet capture shows
     * "Q.Quit" + newline + ">>" where the script sent no CRLF, and Realm builds
     * its screens from one TRANSMIT per line.  (Once set FALSE here by mistake.) */
    d->txnewline = cfg_sbool(dc, tag, "transmit_newline", TRUE);
    d->panicgrace = cfg_sint(dc, tag, "panic_grace", 10);
    d->cnetver = cfg_sint(dc, tag, "cnet_version", 4) >= 4 ? 40 : 30;
    d->oldmci   = cfg_sbool(dc, tag, "old_mci", FALSE);
    str_copy(d->assign, cfg_sget(dc, tag, "assign", ""), sizeof(d->assign));
    d->stack    = cfg_sint(dc, tag, "stack", 16384);
    d->port     = (UWORD)cfg_sint(dc, tag, "port", !str_icmp(d->type, "rlogin") ? 513 : 23);
    if (d->stack < 4096) d->stack = 4096;
    return TRUE;
}

/* ---- macros ------------------------------------------------------------------ */

static void node_dir(char *buf)
{
    sprintf(buf, "BBS:Nodes/Node%d", N.node);
}

static void expand(const char *src, char *dst, LONG size, const char *dropfile)
{
    char tmp[64], *end = dst + size - 1;
    while (*src && dst < end) {
        const char *ins = NULL;
        if (*src != '%' || !src[1]) { *dst++ = *src++; continue; }
        src++;
        switch (*src++) {
        case 'n': sprintf(tmp, "%d", N.node); ins = tmp; break;
        case 'f': ins = dropfile; break;
        case 'd': node_dir(tmp); ins = tmp; break;
        /* caller-typed text goes onto a Shell command line: neutralise it */
        case 'u': case 'h': shell_safe(tmp, N.user.name, sizeof(tmp)); ins = tmp; break;
        case 'r': shell_safe(tmp, N.user.realname, sizeof(tmp)); ins = tmp; break;
        case 'l': sprintf(tmp, "%d", (int)N.user.level); ins = tmp; break;
        case 't': { LONG t = time_left_mins(); sprintf(tmp, "%ld", t < 0 ? 999L : t); ins = tmp; break; }
        case 'i': sprintf(tmp, "%lu", N.user.id); ins = tmp; break;
        case 'p': ins = N.ipstr; break;
        case 'T': ins = term_name(N.term); break;
        case '%': ins = "%"; break;
        default:  ins = ""; break;
        }
        while (ins && *ins && dst < end) *dst++ = *ins++;
    }
    *dst = 0;
}

/* ---- drop files ------------------------------------------------------------------ */

static void fput(BPTR fh, const char *s) { FPuts(fh, (STRPTR)s); FPuts(fh, (STRPTR)"\r\n"); }

static void mmddyy(ULONG t, char *buf)
{
    char d[16];
    static const char *mon = "JanFebMarAprMayJunJulAugSepOctNovDec";
    int m;
    bbs_datestr(t, d);                          /* "22-Sep-26" */
    for (m = 0; m < 12; m++) if (!strncmp(mon + m * 3, d + 3, 3)) break;
    sprintf(buf, "%02d/%c%c/%c%c", m + 1, d[0], d[1], d[7], d[8]);
}

static void split_name(const char *full, char *first, char *last)
{
    const char *sp = strchr(full, ' ');
    if (sp) {
        LONG n = sp - full;
        if (n > NAMELEN - 1) n = NAMELEN - 1;
        memcpy(first, full, n); first[n] = 0;
        str_copy(last, sp + 1, NAMELEN);
    } else {
        str_copy(first, full, NAMELEN);
        last[0] = 0;
    }
}

static void write_dropfiles(struct Door *d, char *mainfile)
{
    char dir[PATHLEN], path[PATHLEN], buf[80];
    const char *df = d->dropfile;
    BOOL all = !str_icmp(df, "all");
    LONG left = time_left_mins();
    BPTR fh;
    if (left < 0) left = 999;

    node_dir(dir);
    mainfile[0] = 0;
    if (!str_icmp(df, "none")) return;

    if (all || !str_icmp(df, "door.sys")) {
        sprintf(path, "%s/DOOR.SYS", dir);
        if (!mainfile[0]) strcpy(mainfile, path);
        if ((fh = Open((STRPTR)path, MODE_NEWFILE))) {
            fput(fh, "COM0:");
            fput(fh, "38400");
            fput(fh, "8");
            sprintf(buf, "%d", N.node); fput(fh, buf);
            fput(fh, "38400");
            fput(fh, "Y"); fput(fh, "N"); fput(fh, "Y"); fput(fh, "Y");
            fput(fh, N.user.realname[0] ? N.user.realname : N.user.name);
            fput(fh, N.user.location);
            fput(fh, "000-000-0000"); fput(fh, "000-000-0000");
            fput(fh, "PASSWORD");
            sprintf(buf, "%d", (int)N.user.level); fput(fh, buf);
            sprintf(buf, "%lu", N.user.calls); fput(fh, buf);
            mmddyy(N.user.lastcall, buf); fput(fh, buf);
            sprintf(buf, "%ld", left * 60); fput(fh, buf);
            sprintf(buf, "%ld", left); fput(fh, buf);
            fput(fh, N.term == TT_ANSI ? "GR" : N.term == TT_VT100 ? "NG" : "7E");
            sprintf(buf, "%d", (int)N.rows); fput(fh, buf);
            fput(fh, (N.user.flags & UF_EXPERT) ? "Y" : "N");
            fput(fh, "1"); fput(fh, "1");
            fput(fh, "12/31/99");
            sprintf(buf, "%lu", N.user.id); fput(fh, buf);
            fput(fh, "Z");
            sprintf(buf, "%lu", N.user.uploads); fput(fh, buf);
            sprintf(buf, "%lu", N.user.downloads); fput(fh, buf);
            fput(fh, "0"); fput(fh, "999999");
            fput(fh, "01/01/70");
            fput(fh, "BBS:"); fput(fh, "BBS:");
            fput(fh, cfg_str(N.cfg, "sysop_name", "Sysop"));
            fput(fh, N.user.name);
            fput(fh, "00:00");
            fput(fh, "Y");
            fput(fh, N.term == TT_ASCII ? "N" : "Y");
            fput(fh, "Y");
            fput(fh, "7");
            fput(fh, "0");
            mmddyy(N.user.lastcall, buf); fput(fh, buf);
            bbs_timestr(N.logon, buf); fput(fh, buf);
            bbs_timestr(N.user.lastcall, buf); fput(fh, buf);
            fput(fh, "999"); fput(fh, "0");
            sprintf(buf, "%lu", N.user.ulkb); fput(fh, buf);
            sprintf(buf, "%lu", N.user.dlkb); fput(fh, buf);
            fput(fh, "");
            sprintf(buf, "%lu", N.user.doors); fput(fh, buf);
            sprintf(buf, "%lu", N.user.posts); fput(fh, buf);
            Close(fh);
        }
    }
    if (all || !str_icmp(df, "dorinfo1.def")) {
        char f1[NAMELEN], l1[NAMELEN];
        sprintf(path, "%s/DORINFO1.DEF", dir);
        if (!mainfile[0]) strcpy(mainfile, path);
        if ((fh = Open((STRPTR)path, MODE_NEWFILE))) {
            fput(fh, cfg_str(N.cfg, "bbs_name", "NilBBS"));
            split_name(cfg_str(N.cfg, "sysop_name", "Sysop"), f1, l1);
            fput(fh, f1); fput(fh, l1);
            fput(fh, "COM0");
            fput(fh, "38400 BAUD,N,8,1");
            fput(fh, "0");
            split_name(N.user.name, f1, l1);
            fput(fh, f1); fput(fh, l1);
            fput(fh, N.user.location);
            fput(fh, N.term == TT_ASCII ? "0" : "1");
            sprintf(buf, "%d", (int)N.user.level); fput(fh, buf);
            sprintf(buf, "%ld", left); fput(fh, buf);
            fput(fh, "-1");
            Close(fh);
        }
    }
    if (all || !str_icmp(df, "door32.sys")) {
        sprintf(path, "%s/DOOR32.SYS", dir);
        if (!mainfile[0]) strcpy(mainfile, path);
        if ((fh = Open((STRPTR)path, MODE_NEWFILE))) {
            fput(fh, "0");                      /* local: I/O is stdin/stdout */
            fput(fh, "0");
            fput(fh, "38400");
            fput(fh, "NilBBS " BBS_VERSION);
            sprintf(buf, "%lu", N.user.id); fput(fh, buf);
            fput(fh, N.user.realname[0] ? N.user.realname : N.user.name);
            fput(fh, N.user.name);
            sprintf(buf, "%d", (int)N.user.level); fput(fh, buf);
            sprintf(buf, "%ld", left); fput(fh, buf);
            fput(fh, N.term == TT_ASCII ? "0" : "1");
            sprintf(buf, "%d", N.node); fput(fh, buf);
            Close(fh);
        }
    }
}

/* ---- the fake console handler ---------------------------------------------------- */

#define H_IN   1
#define H_OUT  2

struct DoorIO {
    struct MsgPort *port;
    struct Task *door;              /* whoever sends us packets */
    struct Door *d;
    BOOL  raw;
    BOOL  in_open, out_open;        /* the two handles System() owns */
    LONG  extra_open;               /* Open("*") handles still open */
    LONG  next_id;
    BOOL  hungup;
    struct DosPacket *readq[8];     /* pending ACTION_READs, oldest first */
    int   nread;
    struct DosPacket *waitpkt;      /* pending ACTION_WAIT_CHAR */
    ULONG wait_deadline;            /* in ticks (DateStamp) */
    UBYTE line[256];                /* cooked: the line being edited */
    int   linelen;
    UBYTE ready[512];               /* cooked: finished lines not yet read */
    int   readylen;
    BOOL  eof_pending;              /* cooked: ^\ on an empty line */
    BOOL  feeding, fed;             /* typing the door's hangup_keys (the caller has gone) / done */
    int   fpos, frounds;
    ULONG fpause;                   /* ticks: the next key not before this */
    char  fexp[200];                /* the keys being typed (%u filled in) */
    char  hbuf[200];                /* what the door showed since the last keys (no escape codes) */
    int   hlen;
    BOOL  hesc;
    ULONG hlast;                    /* ticks: its last output */
};

static ULONG ticks_now(void)
{
    struct DateStamp ds;
    DateStamp(&ds);
    return (ULONG)ds.ds_Days * 4320000UL + (ULONG)ds.ds_Minute * 3000UL + ds.ds_Tick;
}

/* one byte from the caller, converted to the door's charset; -1 = none */
/* ---- the hang-up keys: walking a door out after the caller has gone ----------------------------
 * The door's output is kept (escape codes left out) instead of sent; each time it goes quiet and wants
 * input, the first hangup_<n> rule whose text is on screen says what to type ("text | keys"), else
 * hangup_keys does.  In the keys: \r Enter, \b backspace, \e Esc, \n, \t, \\, ~ = wait a second,
 * %u = the caller's handle. */

static void hup_capture(struct DoorIO *io, const UBYTE *buf, LONG len)
{
    LONG i;
    for (i = 0; i < len; i++) {
        UBYTE c = buf[i];
        if (io->hesc) { if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) io->hesc = FALSE; continue; }
        if (c == 27 || c == 0x9B) { io->hesc = TRUE; continue; }
        if (c == '\r' || c == '\n') c = ' ';
        if (c < 32) continue;
        if (io->hlen >= (int)sizeof(io->hbuf) - 1) {
            memmove(io->hbuf, io->hbuf + 50, io->hlen - 50);
            io->hlen -= 50;
        }
        io->hbuf[io->hlen++] = (char)c;
    }
    io->hlast = ticks_now();
}

/* pick what to type next: the first rule whose text is on screen, else hangup_keys */
static void feed_choose(struct DoorIO *io)
{
    const char *keys = io->d->hupkeys, *s;
    char *o = io->fexp;
    int i;
    io->hbuf[io->hlen] = 0;
    for (i = 0; i < HUP_RULES; i++) {
        const char *r = io->d->hupon[i], *bar = strchr(r, '|');
        char pat[80];
        int n;
        if (!r[0] || !bar) continue;
        n = bar - r;
        if (n > (int)sizeof(pat) - 1) n = sizeof(pat) - 1;
        memcpy(pat, r, n); pat[n] = 0;
        str_trim(pat);
        if (pat[0] && str_istr(io->hbuf, pat)) {
            keys = bar + 1;
            while (*keys == ' ') keys++;
            break;
        }
    }
    for (s = keys; *s && o < io->fexp + sizeof(io->fexp) - NAMELEN - 1; s++) {
        if (s[0] == '%' && s[1] == 'u') { o += sprintf(o, "%s", N.user.name); s++; }
        else *o++ = *s;
    }
    *o = 0;
    io->fpos = 0;
    io->hlen = 0;                               /* what comes next is the answer to this */
    io->frounds++;
}

/* is a key ready to type now?  (chooses the next keys when the door has gone quiet) */
static BOOL feed_ready(struct DoorIO *io)
{
    for (;;) {
        if (io->fed || ticks_now() < io->fpause) return FALSE;
        if (!io->fexp[io->fpos]) {
            if (io->frounds >= io->d->huprounds) { io->fed = TRUE; return FALSE; }
            if (io->frounds && ticks_now() - io->hlast < 25) return FALSE;     /* still printing */
            feed_choose(io);
            if (!io->fexp[0]) { io->fpause = ticks_now() + 50; return FALSE; }
            continue;
        }
        if (io->fexp[io->fpos] == '~') { io->fpos++; io->fpause = ticks_now() + 50; continue; }
        return TRUE;
    }
}

static LONG feed_byte(struct DoorIO *io)
{
    char c;
    if (!feed_ready(io)) return -1;
    c = io->fexp[io->fpos++];
    if (c == '\\' && io->fexp[io->fpos]) {
        char e = io->fexp[io->fpos++];
        switch (e) {
        case 'r': return 13;
        case 'n': return 10;
        case 'b': return 8;
        case 'e': return 27;
        case 't': return 9;
        default:  return (UBYTE)e;
        }
    }
    return (UBYTE)c;
}

static LONG door_inbyte(struct DoorIO *io)
{
    LONG c;
    if (io->feeding) return feed_byte(io);
    c = in_get();
    if (c < 0) return -1;
    if (c == 0x7F) return 8;
    if (c >= 0x80 && N.charset == CS_UTF8) {
        UWORD u;
        int more;
        if ((c & 0xE0) == 0xC0)      { u = c & 0x1F; more = 1; }
        else if ((c & 0xF0) == 0xE0) { u = c & 0x0F; more = 2; }
        else return '?';
        while (more-- > 0) {
            LONG d = in_get();
            if (d < 0 || (d & 0xC0) != 0x80) break;
            u = (UWORD)((u << 6) | (d & 0x3F));
        }
        if (io->d->srccs == CS_LATIN1) return u < 256 ? (LONG)u : '?';
        return uni_to_cp437(u);
    }
    if (c >= 0x80) {
        /* 8-bit caller: CP437 or Latin-1 -> the door's charset */
        UWORD u = (N.charset == CS_LATIN1) ? (UWORD)c : cp437_to_uni((UBYTE)c);
        if (io->d->srccs == CS_LATIN1) return u < 256 ? (LONG)u : '?';
        return uni_to_cp437(u);
    }
    if (c == 27 && io->d->arrows8 && in_avail() && io->raw) {
        LONG n = in_get();
        if (n == '[') return 0x9B;                  /* ESC [ -> Amiga CSI */
        if (n >= 0) in_unget((UBYTE)n);
    }
    return c;
}

static void reply(struct DosPacket *p, LONG r1, LONG r2)
{
    ReplyPkt(p, r1, r2);
}

/* the door's output, through the terminal filter */
static const char *time_up_text(void)
{
    return L("door.time_up", "\r\n\r\n*** Your time is up ***\r\n");
}

static void hup_capture(struct DoorIO *io, const UBYTE *buf, LONG len);
static void door_write(struct DoorIO *io, const UBYTE *buf, LONG len)
{
    hup_capture(io, buf, len);                  /* the screen so far: what the hang-up rules read */
    if (io->feeding || io->hungup || !N.online) return;
    if (io->d->rawout) tn_raw(buf, len);
    else tputraw(buf, len, io->d->srccs);
}

/* cooked mode: run the line editor over whatever the caller typed */
static void cooked_input(struct DoorIO *io)
{
    LONG c;
    while (io->readylen < (int)sizeof(io->ready) - 2 && (c = door_inbyte(io)) >= 0) {
        if (c == '\r' || c == '\n') {
            if (io->readylen + io->linelen + 1 > (int)sizeof(io->ready)) break;
            memcpy(io->ready + io->readylen, io->line, io->linelen);
            io->readylen += io->linelen;
            io->ready[io->readylen++] = '\n';
            io->linelen = 0;
            tn_raw((const UBYTE *)"\r\n", 2);
        } else if (c == 8) {
            if (io->linelen > 0) { io->linelen--; tn_raw((const UBYTE *)"\b \b", 3); }
        } else if (c == 21 || c == 24) {            /* ^U ^X kill line */
            while (io->linelen > 0) { io->linelen--; tn_raw((const UBYTE *)"\b \b", 3); }
        } else if (c == 3) {                        /* ^C: break signal */
            if (io->door) Signal(io->door, SIGBREAKF_CTRL_C);
            tn_raw((const UBYTE *)"^C", 2);
        } else if (c == 4) {                        /* ^D */
            if (io->door) Signal(io->door, SIGBREAKF_CTRL_D);
        } else if (c == 28) {                       /* ^\ = end of file */
            if (io->linelen == 0) io->eof_pending = TRUE;
        } else if (c >= 32 && io->linelen < (int)sizeof(io->line) - 1) {
            UBYTE b = (UBYTE)c;
            io->line[io->linelen++] = b;
            tputraw(&b, 1, io->d->srccs);           /* echo */
        }
    }
}

static BOOL input_ready(struct DoorIO *io)
{
    if (io->hungup) return TRUE;                    /* reads return EOF */
    if (io->feeding) return feed_ready(io);
    if (io->raw) return in_avail() > 0;
    return io->readylen > 0 || io->eof_pending;
}

/* try to satisfy the oldest queued read */
static void hup_read(struct DoorIO *io, struct DosPacket *p);
static BOOL serve_read(struct DoorIO *io, struct DosPacket *p)
{
    UBYTE *buf = (UBYTE *)p->dp_Arg2;
    LONG want = p->dp_Arg3, got = 0;

    if (io->hungup) { hup_read(io, p); return TRUE; }
    if (want <= 0) { reply(p, 0, 0); return TRUE; }
    if (io->raw) {
        LONG c;
        while (got < want && (c = door_inbyte(io)) >= 0) buf[got++] = (UBYTE)c;
        if (!got) return FALSE;
    } else {
        if (io->readylen == 0) {
            if (io->eof_pending) { io->eof_pending = FALSE; reply(p, 0, 0); return TRUE; }
            return FALSE;
        }
        got = io->readylen < want ? io->readylen : want;
        memcpy(buf, io->ready, got);
        memmove(io->ready, io->ready + got, io->readylen - got);
        io->readylen -= got;
    }
    reply(p, got, 0);
    return TRUE;
}

static void service_queues(struct DoorIO *io)
{
    if (!io->raw && !io->hungup) cooked_input(io);
    while (io->nread > 0 && serve_read(io, io->readq[0])) {
        memmove(io->readq, io->readq + 1, (io->nread - 1) * sizeof(io->readq[0]));
        io->nread--;
    }
    if (io->waitpkt) {
        if (input_ready(io)) {
            reply(io->waitpkt, DOSTRUE, 0);
            io->waitpkt = NULL;
        } else if (ticks_now() >= io->wait_deadline) {
            reply(io->waitpkt, DOSFALSE, 0);
            io->waitpkt = NULL;
        }
    }
}

static void handle_packet(struct DoorIO *io, struct DosPacket *p)
{
    if (p->dp_Port && p->dp_Port->mp_SigTask) io->door = (struct Task *)p->dp_Port->mp_SigTask;

    switch (p->dp_Type) {
    case ACTION_READ:
        if (io->nread < 8) io->readq[io->nread++] = p;
        else reply(p, -1, ERROR_NO_FREE_STORE);
        break;

    case ACTION_WRITE:
        door_write(io, (const UBYTE *)p->dp_Arg2, p->dp_Arg3);
        reply(p, p->dp_Arg3, 0);
        break;

    case ACTION_SCREEN_MODE:                /* SetMode(): nonzero = raw */
        if (!p->dp_Arg1 && io->raw) {
            /* back to cooked: typeahead becomes the start of the next line */
            io->linelen = 0;
        }
        io->raw = p->dp_Arg1 != 0;
        reply(p, DOSTRUE, 0);
        break;

    case ACTION_WAIT_CHAR: {
        ULONG usecs = (ULONG)p->dp_Arg1;
        if (!io->raw) cooked_input(io);
        if (input_ready(io)) reply(p, DOSTRUE, 0);
        else if (usecs == 0) reply(p, DOSFALSE, 0);
        else {
            if (io->waitpkt) reply(io->waitpkt, DOSFALSE, 0);
            io->waitpkt = p;
            io->wait_deadline = ticks_now() + usecs / 20000 + 1;
        }
        break;
    }

    case ACTION_FINDINPUT:                  /* Open("*") / Open("CONSOLE:") */
    case ACTION_FINDOUTPUT:
    case ACTION_FINDUPDATE: {
        struct FileHandle *fh = (struct FileHandle *)BADDR(p->dp_Arg1);
        fh->fh_Port = (struct MsgPort *)DOSTRUE;    /* interactive */
        fh->fh_Args = io->next_id++;
        io->extra_open++;
        reply(p, DOSTRUE, 0);
        break;
    }

    case ACTION_END:
        if (p->dp_Arg1 == H_IN) io->in_open = FALSE;
        else if (p->dp_Arg1 == H_OUT) io->out_open = FALSE;
        else if (io->extra_open > 0) io->extra_open--;
        reply(p, DOSTRUE, 0);
        break;

    case ACTION_CHANGE_SIGNAL:
        reply(p, DOSTRUE, 0);
        break;

    case ACTION_FLUSH:
        reply(p, DOSTRUE, 0);
        break;

    case ACTION_IS_FILESYSTEM:
        reply(p, DOSFALSE, 0);
        break;

    case ACTION_LOCATE_OBJECT:
        reply(p, 0, ERROR_OBJECT_NOT_FOUND);
        break;

    default:                                /* DISK_INFO, SEEK, EXAMINE_FH, ... */
        reply(p, DOSFALSE, ERROR_ACTION_NOT_KNOWN);
        break;
    }
}

static void drain_port(struct DoorIO *io)
{
    struct Message *m;
    while ((m = GetMsg(io->port)))
        handle_packet(io, (struct DosPacket *)m->mn_Node.ln_Name);
}

static BPTR make_handle(struct DoorIO *io, LONG id)
{
    struct FileHandle *fh = AllocDosObject(DOS_FILEHANDLE, NULL);
    if (!fh) return 0;
    fh->fh_Type = io->port;
    fh->fh_Port = (struct MsgPort *)DOSTRUE;
    fh->fh_Args = id;
    return MKBADDR(fh);
}

/* a read after the caller has gone: EOF, or (hangup_read = error) a failed read */
static void hup_read(struct DoorIO *io, struct DosPacket *p)
{
    /* not a disk error: ERROR_DEVICE_NOT_MOUNTED and friends make AmigaOS put up a
     * "Please replace volume" requester on the BBS machine (seen with DSE) */
    if (io->d && io->d->hup_error) reply(p, -1, ERROR_OBJECT_NOT_FOUND);
    else reply(p, 0, 0);
}

static void flush_queues_eof(struct DoorIO *io)
{
    while (io->nread > 0) hup_read(io, io->readq[--io->nread]);
    if (io->waitpkt) { reply(io->waitpkt, DOSTRUE, 0); io->waitpkt = NULL; }
}

/*
 * The door is started from a helper process, not from the node itself:
 * while SystemTags() sets up the new Shell, that Shell already sends DOS
 * packets (mode, console queries) to our handle port and waits for the
 * replies.  If the node were the one blocked inside SystemTags() nobody would
 * answer them - a deadlock (seen on the first test run).  So the node keeps
 * servicing its port while the launcher waits for System() to return.
 */
static struct {
    char   cmd[PATHLEN + 16];
    BPTR   in, out;
    struct MsgPort *port;
    LONG   stack;
    LONG   rc;
    struct Task *parent;
    ULONG  donesig;
    BOOL   sync;                /* XIM: run to completion, NIL: for I/O */
    char   dir[PATHLEN];        /* sync: current directory for the door */
    volatile BOOL done;
} LN;

static void launcher(void)
{
    if (LN.sync) {
        BPTR in = Open((STRPTR)"NIL:", MODE_OLDFILE), out = Open((STRPTR)"NIL:", MODE_NEWFILE);
        BPTR dl = LN.dir[0] ? Lock((STRPTR)LN.dir, ACCESS_READ) : 0, od = 0;
        if (dl) od = CurrentDir(dl);            /* the door's shell inherits it */
        LN.rc = SystemTags((STRPTR)LN.cmd,
                          SYS_Input, in, SYS_Output, out,
                          NP_StackSize, LN.stack, NP_Priority, 0,
                          TAG_END);
        if (dl) { CurrentDir(od); UnLock(dl); }
        if (in) Close(in);
        if (out) Close(out);
        Forbid();
        LN.done = TRUE;
        Signal(LN.parent, LN.donesig);
        return;
    }
    LN.rc = SystemTags((STRPTR)LN.cmd,
                      SYS_Input,  LN.in,
                      SYS_Output, LN.out,
                      SYS_Asynch, TRUE,
                      NP_ConsoleTask, (ULONG)LN.port,
                      NP_StackSize, LN.stack,
                      NP_Priority, 0,
                      TAG_END);
    /* exit inside Forbid(): once the node sees `done` it may unload us */
    Forbid();
    LN.done = TRUE;
    Signal(LN.parent, LN.donesig);
}

static void run_cli(struct Door *d)
{
    struct DoorIO io;
    char dir[PATHLEN], drop[PATHLEN], script[PATHLEN], cmd[PATHLEN * 2], line[PATHLEN * 2];
    BPTR in, out, fh;
    LONG rc;
    ULONG portsig, hung_at = 0;
    BOOL leak = FALSE;

    memset(&io, 0, sizeof(io));
    io.d = d;
    io.next_id = 3;

    node_dir(dir);
    write_dropfiles(d, drop);

    /* the door runs from a small script: environment, directory, command */
    sprintf(script, "%s/Door.run", dir);
    if (!(fh = Open((STRPTR)script, MODE_NEWFILE))) {
        /* a door left behind by a RESET or a hang-up may still be running
         * Door.run (its shell keeps the script open): take the next free name */
        int i;
        for (i = 1; i <= 9 && !fh; i++) {
            sprintf(script, "%s/Door.%d.run", dir, i);
            fh = Open((STRPTR)script, MODE_NEWFILE);
        }
    }
    if (!fh) {
        tputs(L("door.run_cli.cant_write_the", "|12Can't write the door script.|07\n"));
        return;
    }
    sprintf(line, "Failat 21\nStack %ld\n", d->stack); FPuts(fh, (STRPTR)line);
    sprintf(line, "Set BBSNODE %d\n", N.node); FPuts(fh, (STRPTR)line);
    { char safe[NAMELEN]; shell_safe(safe, N.user.name, sizeof(safe));
      sprintf(line, "Set BBSUSER \"%s\"\n", safe); FPuts(fh, (STRPTR)line); }
    sprintf(line, "Set BBSLEVEL %d\n", (int)N.user.level); FPuts(fh, (STRPTR)line);
    { LONG t = time_left_mins(); sprintf(line, "Set BBSTIME %ld\n", t < 0 ? 999L : t); }
    FPuts(fh, (STRPTR)line);
    sprintf(line, "Set BBSTERM %s\n", term_name(N.term)); FPuts(fh, (STRPTR)line);
    sprintf(line, "Set BBSDROP \"%s\"\n", drop); FPuts(fh, (STRPTR)line);
    if (d->dir[0]) {
        expand(d->dir, cmd, sizeof(cmd), drop);
        sprintf(line, "CD \"%s\"\n", cmd); FPuts(fh, (STRPTR)line);
    }
    expand(d->command, cmd, sizeof(cmd), drop);
    FPuts(fh, (STRPTR)cmd);
    FPuts(fh, (STRPTR)"\n");
    Close(fh);

    if (!(io.port = CreateMsgPort())) { tputs(L("door.run_cli.out_of_memory", "|12Out of memory.|07\n")); return; }
    portsig = 1UL << io.port->mp_SigBit;
    in  = make_handle(&io, H_IN);
    out = make_handle(&io, H_OUT);
    if (!in || !out) {
        if (in) FreeDosObject(DOS_FILEHANDLE, BADDR(in));
        if (out) FreeDosObject(DOS_FILEHANDLE, BADDR(out));
        DeleteMsgPort(io.port);
        tputs(L("door.run_cli.out_of_memory", "|12Out of memory.|07\n"));
        return;
    }
    io.in_open = io.out_open = TRUE;
    N.door_csi8 = d->amigacsi;

    {
        BYTE sb = AllocSignal(-1);
        struct Process *lp;
        if (sb < 0) goto nostart;
        memset(&LN, 0, sizeof(LN));
        sprintf(LN.cmd, "Execute \"%s\"", script);
        LN.in = in; LN.out = out; LN.port = io.port; LN.stack = d->stack;
        LN.parent = FindTask(NULL);
        LN.donesig = 1UL << sb;
        lp = CreateNewProcTags(NP_Entry, (ULONG)launcher,
                               NP_Name, (ULONG)"NilBBS door launcher",
                               NP_StackSize, 8192,
                               NP_Priority, 0,
                               TAG_END);
        if (!lp) { FreeSignal(sb); goto nostart; }
        portsig |= LN.donesig;

        /* ---- serve the door until System() has returned and every handle
         * on our port is closed: the two System() owns, plus any the door
         * opened itself with Open("*") - that is what keeps e.g.
         * "NewShell *" (a remote shell door) attached ---- */
        N.wait_new = 1;
        while (!LN.done || (LN.rc != -1 && (io.in_open || io.out_open || io.extra_open > 0))) {
            if (N.online) {
                ULONG got;
                ULONG ms = io.waitpkt ? 60 : 1000;
                tn_wait(ms, portsig, &got);
            if (N.msg_waiting) N.msg_waiting = FALSE;   /* shown after the door */
            if (N.loggedin && time_left_mins() == 0) {
                door_write(&io, (const UBYTE *)time_up_text(), strlen(time_up_text()));
                tn_flush();
                node_hangup("time limit in door");
            }
        } else {
            Delay(5);
        }
        drain_port(&io);

        if (!N.online && !io.hungup && !io.feeding && !io.fed && (d->hupkeys[0] || d->hupon[0][0])) {
            /* caller gone from a door that won't notice: walk it out with its hang-up keys */
            io.feeding = TRUE;
            io.fpos = io.frounds = 0; io.fpause = 0;
            io.fexp[0] = 0; io.hlast = 0;           /* hbuf keeps what the caller last saw */
            hung_at = bbs_now();
            bbs_log(BBS_SYSLOG, "node %d: caller dropped in door %s - typing its hang-up keys", N.node, d->tag);
        }
        if (io.feeding && (io.fed || bbs_now() - hung_at > (ULONG)d->hupgrace)) {
            io.feeding = FALSE;                     /* it didn't quit: on to EOF + CTRL-C */
            io.fed = TRUE;
            bbs_log(BBS_SYSLOG, "node %d: door %s is still running after its hang-up keys", N.node, d->tag);
        }
        if (!N.online && !io.hungup && !io.feeding) {
            /* caller gone: make the door give up */
            io.hungup = TRUE;
            hung_at = bbs_now();
            flush_queues_eof(&io);
            if (io.door) Signal(io.door, SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_D |
                                        SIGBREAKF_CTRL_E | SIGBREAKF_CTRL_F);
        }
        if (io.hungup) {
            ULONG since = bbs_now() - hung_at;
            flush_queues_eof(&io);
            if (since && since % 10 == 0 && io.door) Signal(io.door, SIGBREAKF_CTRL_C);
            /* sysop RESET: give the node back now, but keep answering the door
             * (EOF) off the node table until it ends - a door abandoned mid-I/O
             * would wait on our port for ever.  The 300 s cut-off still applies. */
            if (node_reset_wanted()) node_release_slot();
            if (since > 300) {              /* it won't quit: abandon it */
                leak = TRUE;
                break;
            }
        } else {
            service_queues(&io);
        }
        }
        while (!LN.done) {                   /* launcher must be gone before we return */
            Delay(5);
            if (node_reset_wanted()) node_release_slot();
        }
        FreeSignal(sb);
        rc = LN.rc;
    }
    if (rc == -1) {
nostart:
        FreeDosObject(DOS_FILEHANDLE, BADDR(in));
        FreeDosObject(DOS_FILEHANDLE, BADDR(out));
        DeleteMsgPort(io.port);
        N.wait_new = 0;
        N.door_csi8 = 0;
        tputs(L("door.run_cli.the_door_failed", "|12The door failed to start.|07\n"));
        bbs_log(BBS_SYSLOG, "node %d: door %s failed to start", N.node, d->tag);
        return;
    }

    /* The door may have left a background process holding one of our
     * handles.  Keep answering briefly, then detach the port for good. */
    if (!leak && io.extra_open > 0) {
        int i;
        for (i = 0; i < 20 && io.extra_open > 0; i++) {
            Delay(5);
            drain_port(&io);
            flush_queues_eof(&io);
        }
        if (io.extra_open > 0) leak = TRUE;
    }
    N.wait_new = 0;
    flush_queues_eof(&io);
    if (leak) {
        /* nobody may Signal() us through this port after we exit: make it
         * a silent sink and let it go (a small, deliberate leak) */
        Forbid();
        io.port->mp_Flags = PA_IGNORE;
        Permit();
        bbs_log(BBS_SYSLOG, "node %d: door %s left handles open - port abandoned", N.node, d->tag);
    } else {
        DeleteMsgPort(io.port);
    }
    N.door_csi8 = 0;
    tcolor(7);
}


/* ---- shared helpers for other door engines (cnetc.c) ------------------------- */

/* Start `cmd` from the launcher process, synchronously, with NIL: I/O.
 * Returns the signal mask that fires when it finishes, 0 on failure. */
ULONG door_launch_sync(const char *cmd, const char *dir, LONG stack)
{
    BYTE sb = AllocSignal(-1);
    if (sb < 0) return 0;
    memset(&LN, 0, sizeof(LN));
    str_copy(LN.cmd, cmd, sizeof(LN.cmd));
    if (dir) str_copy(LN.dir, dir, sizeof(LN.dir));
    LN.sync = TRUE;
    LN.stack = stack < 4096 ? 4096 : stack;
    LN.parent = FindTask(NULL);
    LN.donesig = 1UL << sb;
    if (!CreateNewProcTags(NP_Entry, (ULONG)launcher, NP_Name, (ULONG)"NilBBS door launcher",
                           NP_StackSize, 8192, NP_Priority, 0, TAG_END)) {
        FreeSignal(sb);
        return 0;
    }
    return LN.donesig;
}
BOOL door_launch_done(void) { return LN.done; }
LONG door_launch_finish(void)
{
    while (!LN.done) Delay(5);
    { ULONG m = LN.donesig; BYTE b = 0; while (m > 1) { m >>= 1; b++; } FreeSignal(b); }
    return LN.rc;
}

/*
 * CNet MCI -> ANSI.  CNet text carries commands as ^Q<cmd><args>} (shown
 * as {Cn}) or ^Y<cmd><one char>.  Colours use ANSI order (1 = red); 8-f
 * are the intense ones.  Supported: C/Z colour, F0 home, F1 clear, N/B/H
 * repeat newline/bell/backspace, O bold, R reverse, U underline, cursor
 * ^ ! > < and . (tab to column), Q1 reset.  Anything else is dropped.
 */
static const UBYTE ansi2pc[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

static int hexval(UBYTE c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void mci_command(UBYTE cmd, const char *arg)
{
    char s[24];
    int n = atoi(arg), i;
    int h = arg[0] ? hexval((UBYTE)arg[0]) : -1;
    /* a ^Y code's one argument character is a hex digit: ^Y>f = 15 right, ^Yna = 10 new lines
     * (Acey Deucey places its cards with them) - only ^Q {...} codes carry decimal numbers */
    if (arg[0] && !arg[1] && h >= 0) n = h;
    switch (cmd | 0x20) {           /* letters case-insensitive */
    case 'c': if (h >= 0) tcolor(ansi2pc[h & 7] | (h & 8)); break;
    case 'z': if (h >= 0) tcolor(16 + ansi2pc[h & 7]); break;
    case 'f': if (n == 1) tcls(); else tn_raw((const UBYTE *)"\x1b[H", 3); break;
    case 'n': for (i = 0; i < (n ? n : 1) && i < 40; i++) tnl(); break;
    case 'b': for (i = 0; i < (n ? n : 1) && i < 5; i++) tn_raw((const UBYTE *)"\x07", 1); break;
    case 'h': for (i = 0; i < n && i < 80; i++) tn_raw((const UBYTE *)"\b", 1); break;
    case 'o': if (N.term != TT_ASCII) tn_raw((const UBYTE *)(n ? "\x1b[1m" : "\x1b[22m"), n ? 4 : 5); break;
    case 'r': if (N.term != TT_ASCII) tn_raw((const UBYTE *)(n ? "\x1b[7m" : "\x1b[27m"), n ? 4 : 5); break;
    case 'u': if (N.term != TT_ASCII) tn_raw((const UBYTE *)(n ? "\x1b[4m" : "\x1b[24m"), n ? 4 : 5); break;
    case 'q': if (n == 1 || n == 0) tcolor(7); break;
    default:
        if (N.term == TT_ASCII) break;
        switch (cmd) {
        case '^': sprintf(s, "\x1b[%dA", n ? n : 1); tn_raw((UBYTE *)s, strlen(s)); break;
        case '!': sprintf(s, "\x1b[%dB", n ? n : 1); tn_raw((UBYTE *)s, strlen(s)); break;
        case '>': sprintf(s, "\x1b[%dC", n ? n : 1); tn_raw((UBYTE *)s, strlen(s)); break;
        case '<': sprintf(s, "\x1b[%dD", n ? n : 1); tn_raw((UBYTE *)s, strlen(s)); break;
        case '.': sprintf(s, "\x1b[%dG", n ? n : 1); tn_raw((UBYTE *)s, strlen(s)); break;
        }
    }
}

void cnet_mci_write(const UBYTE *s, LONG len, UBYTE srccs)
{
    const UBYTE *run = s, *end = s + len;
    while (s < end) {
        if (*s == 0x11 && s + 1 < end) {            /* ^Q cmd args } */
            char arg[32];
            int n = 0;
            UBYTE cmd;
            if (s > run) tputraw(run, s - run, srccs);
            cmd = s[1];
            s += 2;
            while (s < end && *s != '}' && n < 31) arg[n++] = *s++;
            arg[n] = 0;
            if (s < end && *s == '}') s++;
            mci_command(cmd, arg);
            run = s;
        } else if (*s == 0x19 && s + 2 < end) {     /* ^Y cmd arg */
            char arg[2];
            if (s > run) tputraw(run, s - run, srccs);
            arg[0] = s[2]; arg[1] = 0;
            mci_command(s[1], arg);
            s += 3;
            run = s;
        } else s++;
    }
    if (s > run) tputraw(run, s - run, srccs);
}
/* ---- CNet-compatible ARexx doors ---------------------------------------------------
 *
 * type = cnetrexx  runs an ARexx script written for CNet's PFile ARexx
 * interface.  The node publishes an ARexx host port and starts the script
 * through RexxMast with that port as its default host, so the script's bare
 * commands come to us:
 *   TRANSMIT text / SENDSTRING text    output, exactly as given (no newline)
 *   GETCHAR                            one key, no echo -> RESULT
 *   MAYGETCHAR                         a key if one is waiting, else ''
 *   QUERY prompt                       prompt, then an edited line -> RESULT
 *   GETUSER n                          user data (1 handle, 7 time, 23 port...)
 * The script's own SAY/PULL use the same caller-connected handles as CLI
 * doors.  Unknown commands succeed with an empty RESULT and are logged once,
 * so the gaps show up in System.log.
 */
struct RexxArgHdr { LONG size; UWORD length; UBYTE flags, hash; };
struct XimState;
struct RexxHost;
static void aim_command(struct DoorIO *io, struct RexxHost *h, struct RexxMsg *m,
                        const char *word, const UBYTE *arg, LONG arglen);
static void xim_retcmd(struct XimState *x, struct Door *d);
static struct XimState *aim_state(void);
static LONG xim_timeout(struct XimState *x);

#define MCI_DEPTH   6
#define MCI_FILEMAX (256L * 1024)
enum { MW_NONE, MW_KEY, MW_LINE, MW_YESNO, MW_TIME, MW_SUB };

struct RexxHost {
    struct MsgPort *port;           /* public: the script's host */
    struct MsgPort *reply;          /* our launch message comes back here */
    char   name[24];
    struct RexxMsg *launch;
    struct RexxMsg *waiting;        /* GETCHAR / QUERY blocked on input */
    UBYTE  wait_kind;               /* 1 = GETCHAR, 2 = QUERY / PROMPT line, 3 = PROMPT YESNO/NOYES */
    UBYTE  qline[256];
    int    qlen;
    int    qmax;                    /* PROMPT <len> */
    BOOL   qhide;                   /* PROMPT HIDE: echo * */
    UBYTE  qdef;                    /* YESNO 'Y' / NOYES 'N': Enter's answer */
    char   object[88];              /* SETOBJECT: the value the next PUTUSER stores */
    struct UserRec *scr;            /* LOADSCRATCH: another account, for GETSCRATCH / PUTSCRATCH */
    BOOL   inscr;                   /* getuser() reading the scratch record: no PUTUSER overrides */
    BOOL   xproto_set;              /* SETPROTOCOL: the protocol for XDN / XUP (else the caller's) */
    int    xproto;
    char   uov[64][24];             /* PUTUSER n (n < 64): what GETUSER n reads for the rest of the door */
    BOOL   uovset[64];
    struct XimState *aim;           /* type = aim: an AmiExpress ARexx door (AERexxControl<n>) */
    struct Editor *ed;              /* CNet's editor buffer (CLEAREDITOR / CALLEDITOR ...) */
    char   mailsubj[64];            /* SETMAILSUBJ */
    ULONG  wait_since;              /* AIM: when the input request was parked (the /X timeout) */
    struct Task *script;
    char   unknown[8][16];
    int    nunknown;
    /* output that can stop half-way: TRANSMIT / SENDSTRING / SENDFILE text runs as an MCI
     * stream, and {G} {I} {?} {W} {#0} pause it until the caller answers (or the time is up,
     * or the sub-pfile ends); the script's command is answered when its stream is done.
     * {* file} pushes the file as a stream of its own. */
    struct MciStream {
        struct RexxMsg *msg;        /* the command answered when the text is out (NULL for a {*} file) */
        UBYTE *buf;
        LONG   len, pos;
        UBYTE  wait;                /* what it waits for: MW_KEY/LINE/YESNO/TIME/SUB */
        struct RexxMsg *sub;        /* {#0 file}: the sub-pfile it waits for */
        BOOL   paged;               /* a file ({*}, SENDFILE) shown with -- More -- pauses */
    } st[MCI_DEPTH];
    int    nst;
    UBYTE  mreg;                    /* {Gn}: the key goes to string register n */
    UBYTE  mdef;                    /* {?n}: the answer to plain Enter */
    UWORD  mflags;                  /* {In m}: input options */
    int    mmax;                    /*          and length */
    ULONG  muntil;                  /* {Wn}: ticks */
    int    nsubs;
    int    postdrop;                /* commands answered since the caller dropped */
    char   sreg[5][84];             /* MCI string registers (GETUSER 70-74) */
    LONG   nreg[10];                /* MCI numeric registers (GETUSER 60-69) */
    char   spawn[PATHLEN];          /* SPAWN: the script that takes over when this one ends */
    BPTR   in, out;                 /* the script's handles, for sub-pfiles and SPAWN */
    UBYTE  srccs;
    BOOL   oldmci;                  /* \c1-style codes (door option old_mci) */
};

/* only the top stream runs; a {#0} sub-pfile's own text goes on top of the one waiting for it */
#define TOPW(h)     ((h)->nst ? (h)->st[(h)->nst - 1].wait : MW_NONE)
#define SETW(h, v)  ((h)->st[(h)->nst - 1].wait = (v))

static struct Library *RexxSysBase_door;
#define RexxSysBase RexxSysBase_door

static LONG argstr_len(STRPTR a)
{
    return a ? ((struct RexxArgHdr *)(a - sizeof(struct RexxArgHdr)))->length : 0;
}

static void rx_reply(struct RexxMsg *m, LONG rc, const char *res, LONG len)
{
    m->rm_Result1 = rc;
    m->rm_Result2 = 0;
    if (rc == 0 && (m->rm_Action & RXFF_RESULT) && res)
        m->rm_Result2 = (LONG)CreateArgstring((STRPTR)res, len);
    ReplyMsg((struct Message *)m);
}

static const char *const wdays[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

LONG msg_unread_mail(void);                 /* msgui.c */
const char *msg_area_name(void);
static void getuser(struct RexxHost *h, LONG n, char *out)
{
    LONG t = time_left_mins();
    BOOL sy = h->inscr ? N.user.level >= cfg_int(N.cfg, "sysop_level", 255) : N.sysop;   /* GETSCRATCH: that account's */
    if (t < 0) t = 999;
    out[0] = 0;
    if (n >= 1000000) { cnet_getuser_raw(n, out, 80); return; }   /* raw PortData: 1200032 */
    if (n >= 60 && n <= 69) { sprintf(out, "%ld", (long)h->nreg[n - 60]); return; }
    if (!h->inscr && n > 0 && n < 64 && h->uovset[n]) { strcpy(out, h->uov[n]); return; }   /* PUTUSER'd */
    if (n >= 70 && n <= 74) { strcpy(out, h->sreg[n - 70]); return; }
    switch (n) {                    /* numbers from the CNet ARexx command list */
    case 1:  strcpy(out, N.user.name); break;
    case 2:  strcpy(out, "********"); break;                /* password: never */
    case 3:  strcpy(out, N.user.realname); break;
    case 4:  strcpy(out, N.user.location); break;
    case 7:  sprintf(out, "%ld", t * 10); break;            /* time left, TENTHS of minutes (PortData.TimeLeft) */
    case 11: bbs_datestr(N.user.lastcall, out); break;
    case 12:                                                /* "Wed 29-Sep-26 10:15" */
        strcpy(out, wdays[((bbs_now() / 86400) + 0) % 7]);   /* 1-Jan-1978 was a Sunday */
        out[3] = ' ';
        bbs_datetimestr(bbs_now(), out + 4);
        break;
    case 15: sprintf(out, "%d", sy ? 31 : (int)(N.user.level * 31 / 255)); break;   /* access group 0-31, the sysop 31 */
    case 16: strcpy(out, sy ? "SysOp" : "Member"); break;
    case 17: strcpy(out, sy ? "1" : "0"); break;
    case 18: sprintf(out, "%lu", ((bbs_now() - N.logon) / 60 + N.user.mins_today) * 10); break;   /* TENTHS, like 7 */
    case 19: strcpy(out, "12"); break;
    case 22: sprintf(out, "%lu", N.user.calls); break;
    case 23: sprintf(out, "%d", N.node - 1); break;         /* CNet ports count from 0 */
    case 24: strcpy(out, "11520"); break;
    case 27: sprintf(out, "%d", (int)N.cols); break;
    case 28: strcpy(out, N.term == TT_ASCII ? "0" : "2"); break;   /* 0 dumb, 2 ANSI */
    case 29: sprintf(out, "%d", N.user.proto == PROTO_Y ? 89 : N.user.proto == PROTO_X ? 88 : N.user.proto == PROTO_X1K ? 75 : 90); break;   /* protocol as its ASCII code: 90 = Z */
    case 30: sprintf(out, "%lu", N.user.ulkb); break;
    case 31: sprintf(out, "%lu", N.user.uploads); break;
    case 32: sprintf(out, "%lu", N.user.dlkb); break;
    case 33: sprintf(out, "%lu", N.user.downloads); break;
    case 36: sprintf(out, "%lu", N.user.posts); break;
    case 40: sprintf(out, "%lu", N.user.id); break;         /* account number */
    case 39: sprintf(out, "%lu", N.user.id); break;         /* the mail folder: MAIL:users/<39>/ */
    case 34: case 35: sprintf(out, "%lu", (ULONG)N.user.credits); break;   /* byte / file credits */
    case 21: sprintf(out, "%ld", (long)N.user.gamepoints); break;   /* game points (ADDPOINTS) */
    case 41: sprintf(out, "%lu", N.user.id); break;         /* unique id */
    /* the rest of CNet 4's list (gu40pack GU40_Shorts): NilBBS's own data where it has it,
     * else the empty / 0 a CNet account without that field gives */
    case 5: case 6: case 8: case 9: case 10: case 38: case 56: case 58: case 59: break;   /* zip, address,
                                                   comment, phones, WHO banner, country, birthday, org */
    case 13: sprintf(out, "%ld", (long)msg_unread_mail()); break;   /* private mail waiting */
    case 14: sprintf(out, "%ld", (long)msg_unread_mail()); break;   /* NEW mail waiting */
    case 20: case 25: case 26: case 37: case 42: case 43: case 44: case 45: strcpy(out, "0"); break;
    case 46: {                                              /* Morning / Afternoon / Evening */
        int hr = (int)((bbs_now() % 86400) / 3600);
        strcpy(out, hr < 12 ? "Morning" : hr < 18 ? "Afternoon" : "Evening");
        break;
    }
    case 48: sprintf(out, "%d", (int)N.cur_msgarea + 1); break;
    case 49: str_copy(out, msg_area_name(), 80); break;
    case 51: case 52: case 53: case 54: strcpy(out, "1"); break;    /* the door's own area access */
    case 55: strcpy(out, (N.user.flags & UF_EXPERT) ? "2" : "1"); break;   /* help level: expert / intermediate */
    case 57:                                                /* logon time, CNet's long form */
        strcpy(out, wdays[(N.logon / 86400) % 7]);
        out[3] = ' ';
        bbs_datetimestr(N.logon, out + 4);
        break;
    default: break;
    }
}
/* ---- MCI streams (see struct RexxHost) ---------------------------------------------- */

/* start an ARexx script on our host port (a {#0} sub-pfile, or SPAWN's next script) */
static struct RexxMsg *rx_launch(struct RexxHost *h, const char *path)
{
    struct RexxMsg *m;
    struct MsgPort *rexx;
    if (!(m = CreateRexxMsg(h->reply, (STRPTR)"rexx", (STRPTR)h->name))) return NULL;
    m->rm_Args[0] = CreateArgstring((STRPTR)path, strlen(path));
    m->rm_Action = RXCOMM;
    m->rm_Stdin = h->in;
    m->rm_Stdout = h->out;
    Forbid();
    if ((rexx = FindPort((STRPTR)"REXX"))) PutMsg(rexx, (struct Message *)m);
    Permit();
    if (!rexx) {
        DeleteArgstring(m->rm_Args[0]);
        DeleteRexxMsg(m);
        return NULL;
    }
    return m;
}

/* a text file for SENDFILE / {*}: up to ^Z (CP/M-style end of text), capped */
static UBYTE *load_text(const char *path, LONG *len)
{
    BPTR f = Open((STRPTR)path, MODE_OLDFILE);
    UBYTE *buf, *z;
    LONG n;
    *len = 0;
    if (!f) return NULL;
    if (!(buf = AllocVec(MCI_FILEMAX, MEMF_ANY))) { Close(f); return NULL; }
    n = Read(f, buf, MCI_FILEMAX);
    Close(f);
    if (n < 0) n = 0;
    if ((z = memchr(buf, 0x1A, n))) n = z - buf;
    *len = n;
    return buf;
}

/* the script's command is answered when its text is done */
static void mci_pop(struct RexxHost *h)
{
    struct MciStream *s = &h->st[--h->nst];
    if (s->paged) { tpage_end(); s->paged = FALSE; }
    if (s->msg) rx_reply(s->msg, 0, "", 0);
    if (s->buf) FreeVec(s->buf);
    s->msg = NULL; s->buf = NULL;
}

/* old_mci: CNet 1.x/2.x screen codes are a backslash, not ^Y -
 *   backslash + letter/@ + digit   -> ^Y letter digit      (colour, reverse, new line, clear...)
 *   backslash * n file / $ n cmd   -> ^Q * n file }         (to the end of the line)
 *   two backslashes                -> one backslash
 * Returns a new buffer (the old one is freed) and its length in *len. */
static UBYTE *mci_oldcodes(UBYTE *buf, LONG *len)
{
    LONG i, o = 0, n = *len;
    UBYTE *out = AllocVec(n * 2 + 8, MEMF_ANY);
    if (!out) return buf;
    for (i = 0; i < n; i++) {
        UBYTE c = buf[i];
        if (c == 0x5C && i + 1 < n && buf[i + 1] == 0x5C) { out[o++] = 0x5C; i++; continue; }
        if (c == 0x5C && i + 2 < n && (buf[i + 1] == '*' || buf[i + 1] == '$') && isdigit(buf[i + 2])) {
            out[o++] = 0x11; out[o++] = buf[i + 1];
            for (i += 2; i < n && buf[i] != '\n' && buf[i] != '\r'; i++) out[o++] = buf[i];
            out[o++] = '}';
            i--;
            continue;
        }
        if (c == 0x5C && i + 2 < n && (isalpha(buf[i + 1]) || buf[i + 1] == '@') && isalnum(buf[i + 2])) {
            out[o++] = 0x19; continue;
        }
        out[o++] = c;
    }
    FreeVec(buf);
    *len = o;
    return out;
}

/* a QUERY / PROMPT prompt, with old_mci's codes turned into ^Y ones first */
static void host_mci_write(BOOL oldmci, const UBYTE *s, LONG len, UBYTE srccs)
{
    UBYTE *b;
    if (!oldmci || len <= 0 || !(b = AllocVec(len + 1, MEMF_ANY))) { cnet_mci_write(s, len, srccs); return; }
    memcpy(b, s, len);
    b = mci_oldcodes(b, &len);
    cnet_mci_write(b, len, srccs);
    FreeVec(b);
}

/* buf becomes the stream's (freed when done); msg (may be NULL) is answered then */
static void mci_push(struct RexxHost *h, struct RexxMsg *msg, UBYTE *buf, LONG len)
{
    struct MciStream *s;
    if (h->nst >= MCI_DEPTH) {                  /* {*} files showing each other: stop there */
        if (buf) FreeVec(buf);
        if (msg) rx_reply(msg, 0, "", 0);
        return;
    }
    if (buf && h->oldmci) buf = mci_oldcodes(buf, &len);
    s = &h->st[h->nst++];
    s->msg = msg; s->buf = buf; s->len = buf ? len : 0; s->pos = 0;
    s->wait = MW_NONE; s->sub = NULL; s->paged = FALSE;
}

/* a text file ({*}, SENDFILE) as a stream with -- More -- pauses, unless something around it is
 * already paging or the caller turned More off; the paging ends with the file (mci_pop) */
static void mci_push_file(struct RexxHost *h, struct RexxMsg *msg, UBYTE *buf, LONG len)
{
    int depth = h->nst;
    BOOL start = !N.paging;
    mci_push(h, msg, buf, len);
    if (h->nst > depth && start) {
        tpage_start();
        h->st[h->nst - 1].paged = N.paging;         /* FALSE when the caller has More off */
    }
}

/* tshowpath with -- More -- pauses (AIM SHOWFILE, /X DISPLAY_FILE) */
static BOOL show_paged(const char *path, UBYTE srccs)
{
    BOOL r, start = !N.paging;
    if (start) tpage_start();
    r = tshowpath(path, srccs);
    if (start) tpage_end();
    return r;
}

/* drop every stream (the caller has gone, or the door ended) */
static void mci_reset(struct RexxHost *h)
{
    while (h->nst > 0) mci_pop(h);
}

/* the file part of "{*0 file}" / "{#0file}" / "{$ cmd}" */
static const char *mci_path(const char *arg)
{
    while (*arg >= '0' && *arg <= '9') arg++;
    while (*arg == ' ') arg++;
    return arg;
}

/* one MCI command in the top stream; the ones that wait say so in its .wait */
static void mci_act(struct DoorIO *io, struct RexxHost *h, UBYTE cmd, const char *arg)
{
    char out[88];
    switch (cmd | 0x20) {
    case 'g':                                   /* {Gn} a key into register 70+n */
        h->mreg = (UBYTE)(atoi(arg) % 5);
        SETW(h, MW_KEY);
        break;
    case 'i': {                                 /* {In m} a line into register 70 */
        const char *m = strchr(arg, ' ');
        h->mflags = (UWORD)atoi(arg);
        h->mmax = m ? atoi(m + 1) : 40;
        if (h->mmax <= 0 || h->mmax > 80) h->mmax = 40;
        h->qlen = 0;
        SETW(h, MW_LINE);
        break;
    }
    case 'w':                                   /* {Wn} n seconds */
        if (atoi(arg) > 0) {
            h->muntil = ticks_now() + (ULONG)(atoi(arg) > 30 ? 30 : atoi(arg)) * 50;
            SETW(h, MW_TIME);
        }
        break;
    case 'v':                                   /* {Vn} a GETUSER value */
        getuser(h, atol(arg), out);
        tputraw((const UBYTE *)out, strlen(out), h->srccs);
        break;
    case 'l': {                                 /* {Ln m} set a register: m = #literal or a GETUSER */
        LONG n = atol(arg);
        const char *m = strchr(arg, ' ');
        if (!m) break;
        m++;
        if (*m == '#' || *m == '\'') str_copy(out, m + 1, sizeof(out));
        else getuser(h, atol(m), out);
        if (n >= 60 && n <= 69) h->nreg[n - 60] = atol(out);
        else if (n >= 70 && n <= 74) str_copy(h->sreg[n - 70], out, sizeof(h->sreg[0]));
        break;
    }
    case '+':                                   /* {+ text} to the log */
        bbs_log(BBS_SYSLOG, "node %d: [%s] %s", N.node, io->d->tag, mci_path(arg));
        break;
    case '$':                                   /* {$ cmd} an AmigaDOS command */
        if (arg[0]) {
            BPTR nin = Open((STRPTR)"NIL:", MODE_OLDFILE), nout = Open((STRPTR)"NIL:", MODE_NEWFILE);
            if (nin && nout) SystemTags((STRPTR)mci_path(arg), SYS_Input, nin, SYS_Output, nout, TAG_END);
            if (nin) Close(nin);
            if (nout) Close(nout);
        }
        break;
    default:
        switch (cmd) {
        case '?':                               /* {?n} yes/no, "1"/"0" into register 70 */
            h->mdef = (UBYTE)(atoi(arg) ? 1 : 0);
            SETW(h, MW_YESNO);
            break;
        case '*': {                             /* {*n file} show a file */
            LONG len;
            UBYTE *b = load_text(mci_path(arg), &len);
            if (b) mci_push_file(h, NULL, b, len);
            break;
        }
        case '#':                               /* {#n file} run a pfile: 0/1 ARexx, on our port */
            if (arg[0] == '0' || arg[0] == '1') {
                struct RexxMsg *sm = rx_launch(h, mci_path(arg));
                if (sm) { h->st[h->nst - 1].sub = sm; h->nsubs++; SETW(h, MW_SUB); }
            } else bbs_log(BBS_SYSLOG, "node %d: MCI {#%c} (C/DOS/Paragon pfile) not emulated (door %s)",
                           N.node, arg[0], io->d->tag);
            break;
        case '@': case ':': case '&': case '=': case '%': case '-':
            break;                              /* MCI modes, BBS commands, prompt defaults */
        default:
            if ((cmd | 0x20) == 's' || (cmd | 0x20) == 'k' || (cmd | 0x20) == 'p' ||
                (cmd | 0x20) == 't' || (cmd | 0x20) == 'j' || (cmd | 0x20) == 'x' ||
                (cmd | 0x20) == 'm' || (cmd | 0x20) == 'a' || (cmd | 0x20) == 'd' ||
                (cmd | 0x20) == 'e')
                break;                          /* speed, kolorific, direction, tests, maths */
            mci_command(cmd, arg);
        }
    }
}

/* write the top stream until it ends (its command is answered) or something waits */
static void mci_run(struct DoorIO *io, struct RexxHost *h)
{
    while (h->nst > 0 && TOPW(h) == MW_NONE) {
        int depth = h->nst;
        struct MciStream *s = &h->st[depth - 1];
        const UBYTE *p = s->buf + s->pos, *end = s->buf + s->len, *run = p;
        if (!N.online || io->hungup) { mci_reset(h); return; }
        if (s->paged && N.page_abort) { mci_pop(h); continue; }     /* "n" at -- More --: the rest goes */
        while (p < end) {
            if (*p == 0x11 && p + 1 < end) {            /* ^Q cmd args } */
                char arg[200];
                int n = 0;
                UBYTE cmd;
                if (p > run) tputraw(run, p - run, h->srccs);
                cmd = p[1];
                p += 2;
                while (p < end && *p != '}' && n < (int)sizeof(arg) - 1) arg[n++] = *p++;
                arg[n] = 0;
                if (p < end && *p == '}') p++;
                s->pos = p - s->buf;
                if (s->paged && N.page_abort) { run = p = end; break; }    /* skipped at -- More -- */
                mci_act(io, h, cmd, arg);
                run = p;
                if (h->nst != depth || s->wait != MW_NONE) break;
            } else if (*p == 0x19 && p + 2 < end) {     /* ^Y cmd arg */
                char arg[2];
                UBYTE cmd = p[1];
                if (p > run) tputraw(run, p - run, h->srccs);
                arg[0] = p[2]; arg[1] = 0;
                p += 3;
                s->pos = p - s->buf;
                if (s->paged && N.page_abort) { run = p = end; break; }    /* skipped at -- More -- */
                mci_act(io, h, cmd, arg);
                run = p;
                if (h->nst != depth || s->wait != MW_NONE) break;
            } else p++;
        }
        if (p > run) tputraw(run, p - run, h->srccs);
        s->pos = p - s->buf;
        if (h->nst != depth || s->wait != MW_NONE) continue;   /* waiting, or a {*} file on top */
        mci_pop(h);
    }
    if (TOPW(h) != MW_NONE) tn_flush();
}

/* the caller answered a {G} / {I} / {?} */
static void mci_input(struct DoorIO *io, struct RexxHost *h)
{
    LONG c;
    while ((c = door_inbyte(io)) >= 0) {
        if (TOPW(h) == MW_KEY) {
            h->sreg[h->mreg][0] = (char)c; h->sreg[h->mreg][1] = 0;
            SETW(h, MW_NONE);
            break;
        }
        if (TOPW(h) == MW_YESNO) {
            int yes;
            if (c == 'y' || c == 'Y') yes = 1;
            else if (c == 'n' || c == 'N') yes = 0;
            else if (c == '\r' || c == '\n') yes = h->mdef;
            else continue;
            tputs(yes ? "Yes\n" : "No\n");
            strcpy(h->sreg[0], yes ? "1" : "0");
            SETW(h, MW_NONE);
            break;
        }
        /* MW_LINE */
        if (c == '\r' || c == '\n') {
            tn_raw((const UBYTE *)"\r\n", 2);
            h->qline[h->qlen] = 0;
            str_copy(h->sreg[0], (char *)h->qline, sizeof(h->sreg[0]));
            SETW(h, MW_NONE);
            break;
        } else if (c == 8 || c == 127) {
            if (h->qlen > 0) { h->qlen--; tn_raw((const UBYTE *)"\b \b", 3); }
        } else if (c >= 32 && h->qlen < h->mmax) {
            UBYTE b = (UBYTE)c;
            if ((h->mflags & 64) && (c < '0' || c > '9')) continue;
            if (h->mflags & 1) b = (UBYTE)toupper(b);
            h->qline[h->qlen++] = b;
            tputraw(&b, 1, h->srccs);
        }
    }
    if (TOPW(h) == MW_NONE) mci_run(io, h);
}

/* text sent after the caller has gone: no output, but the MCI that does work still runs -
 * TradeWars deletes its "game in use" file with {$ delete RAM:tw/Player!} on the way out
 * and retries until it's gone.  {#0} sub-pfiles are not started any more. */
static void mci_offline(struct DoorIO *io, struct RexxHost *h, const UBYTE *s, LONG len)
{
    const UBYTE *end = s + len;
    while (s < end) {
        if (*s == 0x11 && s + 1 < end) {
            char arg[200];
            int n = 0;
            UBYTE cmd = s[1];
            s += 2;
            while (s < end && *s != '}' && n < (int)sizeof(arg) - 1) arg[n++] = *s++;
            arg[n] = 0;
            if (s < end) s++;
            if (cmd == '$' || cmd == '+') mci_act(io, h, cmd, arg);
        } else s++;
    }
}

/* LOGENTRY text without its MCI codes */
static void mci_strip(char *s)
{
    char *o = s;
    while (*s) {
        if (*s == 0x11) { while (*s && *s != '}') s++; if (*s) s++; }
        else if (*s == 0x19) { s++; if (*s) s++; if (*s) s++; }
        else *o++ = *s++;
    }
    *o = 0;
}

/* the caller typed something: finish a blocked GETCHAR / QUERY */
static void rx_service_input(struct DoorIO *io, struct RexxHost *h)
{
    LONG c;
    if (TOPW(h) == MW_KEY || TOPW(h) == MW_LINE || TOPW(h) == MW_YESNO) {
        mci_input(io, h);
        return;
    }
    if (!h->waiting) return;
    if (h->wait_kind == 1) {
        if ((c = door_inbyte(io)) >= 0) {
            char s[2];
            if (c >= 'a' && c <= 'z' && !h->aim) c -= 32;   /* CNet's GETCHAR gives letters in capitals */
            s[0] = (char)c; s[1] = 0;
            if (io->d->qquit && (c == 'q' || c == 'Q')) {
                /* CNet quits the PFile on Q at a GETCHAR; some doors rely on it */
                rx_reply(h->waiting, 0, s, 1);
                if (h->script) Signal(h->script, SIGBREAKF_CTRL_C);
            } else rx_reply(h->waiting, 0, s, 1);
            h->waiting = NULL;
        }
        return;
    }
    if (h->wait_kind == 3) {                            /* PROMPT YESNO / NOYES */
        while ((c = door_inbyte(io)) >= 0) {
            if (c == '\r' || c == '\n') c = h->qdef;
            if (c == 'y' || c == 'Y' || c == 'n' || c == 'N') {
                const char *ans = (c == 'y' || c == 'Y') ? "Yes" : "No";
                tputs(ans); tputs("\n");
                rx_reply(h->waiting, 0, ans, strlen(ans));
                h->waiting = NULL;
                return;
            }
        }
        return;
    }
    while ((c = door_inbyte(io)) >= 0) {
        if (c == '\r' || c == '\n') {
            tn_raw((const UBYTE *)"\r\n", 2);
            rx_reply(h->waiting, 0, (char *)h->qline, h->qlen);
            h->waiting = NULL;
            return;
        } else if (c == 8) {
            if (h->qlen > 0) { h->qlen--; tn_raw((const UBYTE *)"\b \b", 3); }
        } else if (c == 21 || c == 24) {
            while (h->qlen > 0) { h->qlen--; tn_raw((const UBYTE *)"\b \b", 3); }
        } else if (c >= 32 && h->qlen < (h->qmax > 0 ? h->qmax : 200)) {
            UBYTE b = (UBYTE)c;
            h->qline[h->qlen++] = b;
            if (h->qhide) tn_raw((const UBYTE *)"*", 1);
            else tputraw(&b, 1, io->d->srccs);
        }
    }
}

/* every host command we answer (keep in step with rx_command) */
static const char *const rx_cmds[] = {
    "TRANSMIT", "PRINT", "SENDSTRING", "SEND", "NEWLINE", "SENDFILE", "GETCHAR", "GETKEY",
    "MAYGETCHAR", "BUFFERFLUSH", "CHECKIO", "IREADY", "QUERY", "RECEIVE", "PROMPT", "GETCARRIER",
    "BBSIDENTIFY", "VERSION", "LOGENTRY", "SYSOPLOG", "CHANGEWHERE", "SETNODELOCATION", "CHANGEWHAT",
    "ADDTIME", "ADDKEYS", "SPAWN", "SHUTDOWN", "SCREENOUT", "BBSCOMMAND", "OPENDISPLAY",
    "CLOSEDISPLAY", "BAUD", "MODEM", "SENDMODEM", "GETUSER", "CLS", "BYE", "HANGUP", "DROPCARRIER",
    "CARRIER", "CHECKCARRIER", "SETOBJECT", "PUTUSER", "SETLENGTH", "CLEAREDITOR", "LOADEDITOR",
    "SAVEEDITOR", "CALLEDITOR", "SETMAILSUBJ", "WRITEMAIL", "LOADSCRATCH", "GETSCRATCH", "PUTSCRATCH",
    "SAVESCRATCH", "FINDACCOUNT", "ADDPOINTS", "CHECKABORT", "FEEDBACK", "RESETMODEM", "SETMINFREE",
    "SETPROTOCOL", "XDN", "XUP", NULL
};

/* ...and an AIM (AmiExpress ARexx) door's */
static const char *const aim_cmds[] = {
    "GETUSER", "PUTUSER", "PUTUSTR", "TRANSMIT", "SENDMESSAGE", "SENDSTRING", "GETCHAR", "PROMPT",
    "QUERY", "SHOWFILE", "SHOWGFILE", "FLAGFILE", "SHUTDOWN", "BUFFERFLUSH", NULL
};

static BOOL rx_known(const char *w)
{
    int i;
    for (i = 0; rx_cmds[i]; i++) if (!str_icmp(w, rx_cmds[i])) return TRUE;
    return FALSE;
}

static BOOL aim_known(const char *w)
{
    int i;
    for (i = 0; aim_cmds[i]; i++) if (!str_icmp(w, aim_cmds[i])) return TRUE;
    return FALSE;
}

/* CNet's editor, for doors: a text buffer the script fills (LOADEDITOR file), lets
 * the caller edit (CALLEDITOR n - NilBBS's own editor; RESULT and PortData.edbuff
 * 1 = saved, 0 = aborted) and saves (SAVEEDITOR file) or mails (SETMAILSUBJ s,
 * WRITEMAIL <user id | handle> - into NilBBS's private mail, from the caller) */
static void rx_editor(struct DoorIO *io, struct RexxHost *h, struct RexxMsg *m,
                      const char *word, const UBYTE *arg, LONG arglen)
{
    char a2[PATHLEN];
    str_copy(a2, (const char *)arg, arglen + 1 < (LONG)sizeof(a2) ? arglen + 1 : (LONG)sizeof(a2));
    str_trim(a2);
    if (a2[0] == '"') {                                 /* "quoted" -> quoted */
        LONG n = strlen(a2);
        memmove(a2, a2 + 1, n);
        if (n > 1 && a2[n - 2] == '"') a2[n - 2] = 0;
    }
    if (!h->ed && !(h->ed = AllocVec(sizeof(struct Editor), MEMF_CLEAR))) { rx_reply(m, 0, "0", 1); return; }
    if (!str_icmp(word, "CLEAREDITOR")) {
        h->ed->n = 0;
        rx_reply(m, 0, "", 0);
    } else if (!str_icmp(word, "LOADEDITOR")) {
        BPTR f = Open((STRPTR)a2, MODE_OLDFILE);
        h->ed->n = 0;
        if (f) {
            char l[256];
            while (h->ed->n < ED_MAXLINES && FGets(f, (STRPTR)l, sizeof(l))) {
                LONG n = strlen(l);
                while (n > 0 && (l[n - 1] == '\n' || l[n - 1] == '\r')) l[--n] = 0;
                str_copy(h->ed->line[h->ed->n++], l, ED_WIDTH + 1);
            }
            Close(f);
        }
        rx_reply(m, 0, f ? "1" : "0", 1);
    } else if (!str_icmp(word, "SAVEEDITOR")) {
        BPTR f = Open((STRPTR)a2, MODE_NEWFILE);
        int i;
        if (f) {
            for (i = 0; i < h->ed->n; i++) { FPuts(f, (STRPTR)h->ed->line[i]); FPutC(f, '\n'); }
            Close(f);
        }
        rx_reply(m, 0, f ? "1" : "0", 1);
    } else if (!str_icmp(word, "CALLEDITOR")) {
        int r;
        set_activity("Door: writing");
        r = msg_door_edit(h->ed, h->mailsubj);
        if (!N.online) r = 0;
        if (r && h->ed->n == 0) r = 0;                  /* "Editor is Empty!" */
        cnet_set_edbuff((UBYTE)r);
        rx_reply(m, 0, r ? "1" : "0", 1);
    } else if (!str_icmp(word, "SETMAILSUBJ")) {
        str_copy(h->mailsubj, a2, sizeof(h->mailsubj));
        rx_reply(m, 0, "", 0);
    } else {                                            /* WRITEMAIL <id | handle | UUCP> */
        char to[NAMELEN], *text;
        int ai, i;
        LONG len = 0;
        struct UserRec u;
        to[0] = 0;
        if (a2[0] >= '0' && a2[0] <= '9') { if (userdb_read(atol(a2), &u)) str_copy(to, u.name, sizeof(to)); }
        else if (str_icmp(a2, "UUCP") && str_icmp(a2, "ID")) str_copy(to, a2, sizeof(to));
        for (ai = 0; ai < msg_area_count() && !msg_area_is_email(ai); ai++) ;
        for (i = 0; i < h->ed->n; i++) len += strlen(h->ed->line[i]) + 1;
        if (!to[0] || ai >= msg_area_count() || !h->ed->n || !(text = AllocVec(len + 1, MEMF_ANY))) {
            bbs_log(BBS_SYSLOG, "node %d: door %s WRITEMAIL %s not sent", N.node, io->d->tag, a2);
            rx_reply(m, 0, "0", 1);
            return;
        }
        text[0] = 0;
        for (i = 0; i < h->ed->n; i++) { strcat(text, h->ed->line[i]); strcat(text, "\n"); }
        i = msg_post_text(ai, to, h->mailsubj[0] ? h->mailsubj : io->d->name, 0, text) != 0;
        FreeVec(text);
        bbs_log(BBS_SYSLOG, "node %d: door %s mailed %s (%s)", N.node, io->d->tag, to, h->mailsubj);
        rx_reply(m, 0, i ? "1" : "0", 1);
    }
}

static void rx_command(struct DoorIO *io, struct RexxHost *h, struct RexxMsg *m)
{
    STRPTR a = m->rm_Args[0];
    LONG len = argstr_len(a), wl = 0;
    char word[16];
    const UBYTE *arg;
    LONG arglen;

    if (m->rm_Node.mn_ReplyPort && m->rm_Node.mn_ReplyPort->mp_SigTask)
        h->script = (struct Task *)m->rm_Node.mn_ReplyPort->mp_SigTask;
    while (wl < len && wl < 15 && a[wl] != ' ') { word[wl] = a[wl]; wl++; }
    word[wl] = 0;
    arg = (const UBYTE *)a + wl + (wl < len ? 1 : 0);
    arglen = len - (arg - (const UBYTE *)a);
    if (arglen < 0) arglen = 0;
    if (h->aim ? !aim_known(word) : !rx_known(word)) {
        /* "ss'Loading...'" (ss = SENDSTRING) reaches us as SENDSTRINGLoading... - ARexx glues
         * a command to a string that follows it directly; CNet takes the command it starts with.
         * (An AIM door's "sm'text'" is SENDMESSAGEtext: its own list, or it'd be SEND.) */
        const char *const *cl = h->aim ? aim_cmds : rx_cmds;
        int i, best = -1, bl = 0;
        for (i = 0; cl[i]; i++) {
            int l = strlen(cl[i]);
            if (l > bl && l <= len && !str_nicmp((const char *)a, cl[i], l)) { best = i; bl = l; }
        }
        if (best >= 0) {
            strcpy(word, cl[best]);
            arg = (const UBYTE *)a + bl;
            arglen = len - bl;
        }
    }
    if (h->aim) {
        if (io->hungup || !N.online) { rx_reply(m, 10, NULL, 0); return; }
        aim_command(io, h, m, word, arg, arglen);
        return;
    }
    if (io->hungup || !N.online) {
        /* after carrier loss: the carrier questions say so (games test GETCARRIER for
         * FALSE - TradeWars, TDL - and never read ###PANIC there); every other command
         * answers "###PANIC", CNet's word for it, which scripts that check it save on.
         * The CTRL-C that follows after a grace period stops scripts that never look. */
        if (h->postdrop++ < 6)                  /* what a script that won't stop keeps asking */
            bbs_log(BBS_SYSLOG, "node %d: door %s after the drop: %.40s", N.node, io->d->tag, (const char *)a);
        if (!str_icmp(word, "TRANSMIT") || !str_icmp(word, "PRINT") || !str_icmp(word, "SENDSTRING"))
            mci_offline(io, h, arg, arglen);    /* nothing shown, but {$ cmd} {+ log} still run */
        if (!str_icmp(word, "GETCARRIER")) rx_reply(m, 0, "FALSE", 5);
        else if (!str_icmp(word, "CARRIER") || !str_icmp(word, "CHECKCARRIER")) rx_reply(m, 0, "0", 1);
        else rx_reply(m, 0, "###PANIC", 8);
        return;
    }

    if (!str_icmp(word, "TRANSMIT") || !str_icmp(word, "PRINT") || !str_icmp(word, "SENDSTRING")) {
        /* the text runs as an MCI stream: answered when it's out, which is later
         * if it waits for the caller ({G} {I} {?}), a pause ({W}) or a sub-pfile ({#0}) */
        BOOL nl = str_icmp(word, "SENDSTRING") && io->d->txnewline;
        UBYTE *b = AllocVec(arglen + 3, MEMF_ANY);
        if (!b) { rx_reply(m, 0, "", 0); return; }
        memcpy(b, arg, arglen);
        if (nl) { b[arglen] = '\r'; b[arglen + 1] = '\n'; }
        mci_push(h, m, b, arglen + (nl ? 2 : 0));
        mci_run(io, h);
    } else if (!str_icmp(word, "SEND")) {                /* SEND: no MCI, verbatim */
        door_write(io, arg, arglen);
        rx_reply(m, 0, "", 0);
    } else if (!str_icmp(word, "NEWLINE")) {
        door_write(io, (const UBYTE *)"\r\n", 2);
        rx_reply(m, 0, "", 0);
    } else if (!str_icmp(word, "SENDFILE")) {
        char p[PATHLEN];
        LONG flen;
        UBYTE *b;
        str_copy(p, (const char *)arg, arglen + 1 < PATHLEN ? arglen + 1 : PATHLEN);
        if ((b = load_text(str_trim(p), &flen))) {          /* CNet text files carry MCI too */
            mci_push_file(h, m, b, flen);
            mci_run(io, h);
        } else rx_reply(m, 0, "", 0);
    } else if (!str_icmp(word, "GETCHAR") || !str_icmp(word, "GETKEY")) {
        h->waiting = m; h->wait_kind = 1;
        rx_service_input(io, h);
    } else if (!str_icmp(word, "MAYGETCHAR")) {
        LONG c = door_inbyte(io);
        char s[2];
        s[0] = (char)(c >= 'a' && c <= 'z' ? c - 32 : c); s[1] = 0;   /* CNet: letters in capitals */
        if (c >= 0) rx_reply(m, 0, s, 1);
        else rx_reply(m, 0, "NOCHAR", 6);
    } else if (!str_icmp(word, "BUFFERFLUSH")) {         /* throw away typed-ahead keys */
        while (door_inbyte(io) >= 0)
            ;
        rx_reply(m, 0, "", 0);
    } else if (!str_icmp(word, "CHECKIO") || !str_icmp(word, "IREADY")) {
        rx_reply(m, 0, in_avail() ? "1" : "0", 1);
    } else if (!str_icmp(word, "QUERY") || !str_icmp(word, "RECEIVE")) {
        host_mci_write(h->oldmci, arg, arglen, io->d->srccs);
        h->waiting = m; h->wait_kind = 2; h->qlen = 0; h->qmax = 200; h->qhide = FALSE;
        rx_service_input(io, h);
    } else if (!str_icmp(word, "PROMPT")) {
        /* PROMPT <len> NORMAL|HIDE|YESNO|NOYES <prompt>: the prompt is the rest of
         * the line, in double quotes or not (ARexx has eaten the single ones).
         * YESNO / NOYES take one key and answer "Yes" or "No" (Enter = the
         * first word), HIDE echoes stars, len caps the line */
        char a2[256], *pp, *mode;
        LONG plen, qn;
        str_copy(a2, (const char *)arg, arglen + 1 < (LONG)sizeof(a2) ? arglen + 1 : (LONG)sizeof(a2));
        pp = a2;
        while (*pp == ' ') pp++;
        qn = atol(pp);
        while (*pp && *pp != ' ') pp++;                 /* the length */
        while (*pp == ' ') pp++;
        mode = pp;
        while (*pp && *pp != ' ') pp++;                 /* the mode */
        if (*pp) *pp++ = 0;
        plen = strlen(pp);
        if (plen >= 2 && pp[0] == '"' && pp[plen - 1] == '"') { pp++; plen -= 2; }
        else if (plen >= 1 && pp[0] == '"') { pp++; plen--; }
        if (plen > 0) host_mci_write(h->oldmci, (const UBYTE *)pp, plen, io->d->srccs);
        h->waiting = m; h->qlen = 0; h->qhide = !str_icmp(mode, "HIDE");
        h->qmax = (qn > 0 && qn < 200) ? (int)qn : 200;
        if (!str_icmp(mode, "YESNO") || !str_icmp(mode, "NOYES")) {
            h->wait_kind = 3;
            h->qdef = !str_icmp(mode, "YESNO") ? 'Y' : 'N';
        } else h->wait_kind = 2;
        rx_service_input(io, h);
    } else if (!str_icmp(word, "LOADSCRATCH") || !str_icmp(word, "GETSCRATCH") ||
               !str_icmp(word, "PUTSCRATCH") || !str_icmp(word, "SAVESCRATCH") ||
               !str_icmp(word, "FINDACCOUNT")) {
        /* CNet's scratch user: LOADSCRATCH id puts another account in a buffer ("1" / "0"),
         * GETSCRATCH n reads it like GETUSER, PUTSCRATCH n sets a field from SETOBJECT.
         * SAVESCRATCH (CNet: "VERY dangerous") is refused - a door writing someone
         * else's account while they may be online would be overwritten or overwrite
         * theirs.  FINDACCOUNT handle|id -> the account number, 0 = none. */
        char out[88];
        LONG n = atol((const char *)arg);
        out[0] = 0;
        if (!str_icmp(word, "LOADSCRATCH")) {
            BOOL ok = FALSE;
            if (!h->scr) h->scr = AllocVec(sizeof(struct UserRec), MEMF_ANY | MEMF_CLEAR);
            if (h->scr && n > 0) {
                if ((ULONG)n == N.user.id) { *h->scr = N.user; ok = TRUE; }
                else {
                    ObtainSemaphore(&N.S->userlock);
                    ok = userdb_read((ULONG)n, h->scr) && h->scr->id == (ULONG)n;
                    ReleaseSemaphore(&N.S->userlock);
                }
                if (!ok) h->scr->id = 0;
            }
            strcpy(out, ok ? "1" : "0");
        } else if (!str_icmp(word, "GETSCRATCH")) {
            if (h->scr && h->scr->id) {
                struct UserRec *keep = AllocVec(sizeof(struct UserRec), MEMF_ANY);
                if (keep) {                         /* getuser() reads N.user: lend it the scratch one */
                    *keep = N.user; N.user = *h->scr; h->inscr = TRUE;
                    getuser(h, n, out);
                    h->inscr = FALSE; N.user = *keep;
                    FreeVec(keep);
                }
            }
        } else if (!str_icmp(word, "PUTSCRATCH")) {
            if (h->scr && h->scr->id) {
                if (n == 3) str_copy(h->scr->realname, h->object, LONGNAME);
                else if (n == 4) str_copy(h->scr->location, h->object, LONGNAME);
                else if (n == 21) h->scr->gamepoints = atol(h->object);
                else if (n == 34 || n == 35) h->scr->credits = atol(h->object);
            }
        } else if (!str_icmp(word, "SAVESCRATCH")) {
            bbs_log(BBS_SYSLOG, "node %d: door %s SAVESCRATCH %ld refused (writing other accounts)",
                    N.node, io->d->tag, n);
            strcpy(out, "0");
        } else {                                    /* FINDACCOUNT */
            struct UserRec *u = AllocVec(sizeof(struct UserRec), MEMF_ANY | MEMF_CLEAR);
            char nm[NAMELEN];
            LONG id = 0;
            str_copy(nm, (const char *)arg, arglen + 1 < NAMELEN ? arglen + 1 : NAMELEN);
            str_trim(nm);
            if (u && nm[0]) {
                ObtainSemaphore(&N.S->userlock);
                if (nm[0] >= '0' && nm[0] <= '9') id = (userdb_read((ULONG)atol(nm), u) && u->id) ? (LONG)u->id : 0;
                else id = userdb_find(nm, u);
                ReleaseSemaphore(&N.S->userlock);
            }
            if (u) FreeVec(u);
            sprintf(out, "%ld", (long)id);
        }
        rx_reply(m, 0, out, strlen(out));
    } else if (!str_icmp(word, "ADDPOINTS")) {
        N.user.gamepoints += atol((const char *)arg);   /* GETUSER 21 */
        rx_reply(m, 0, "", 0);
    } else if (!str_icmp(word, "CHECKABORT")) {
        rx_reply(m, 0, "0", 1);                     /* output isn't interrupted here; keys stay for the door */
    } else if (!str_icmp(word, "FEEDBACK")) {
        msg_post(cfg_str(N.cfg, "sysop_name", "Sysop"), "Feedback", 0);
        rx_reply(m, 0, "", 0);
    } else if (!str_icmp(word, "RESETMODEM") || !str_icmp(word, "SETMINFREE")) {
        rx_reply(m, 0, "", 0);                      /* telnet: no modem; uploads check space themselves */
    } else if (!str_icmp(word, "SETPROTOCOL")) {
        /* CNet's protocol letter; "" = the caller's own choice */
        const char *a = (const char *)arg;
        while (*a == ' ' || *a == '"' || *a == '\'') a++;
        h->xproto_set = TRUE;
        switch (*a | 0x20) {
        case 'z': h->xproto = PROTO_Z; break;
        case 'y': h->xproto = PROTO_Y; break;
        case 'k': case '1': h->xproto = PROTO_X1K; break;
        case 'x': h->xproto = PROTO_X; break;
        default:  h->xproto_set = FALSE; break;
        }
        rx_reply(m, 0, "", 0);
    } else if (!str_icmp(word, "XDN") || !str_icmp(word, "XUP")) {
        /* XDN path: send that file; XUP path: receive (a folder for batch protocols,
         * else the file's own name) - through NilBBS's own transfer code */
        char p[PATHLEN], dir[PATHLEN], out[16];
        UBYTE keep = N.user.proto;
        LONG got = 0;
        str_copy(p, (const char *)arg, arglen + 1 < PATHLEN ? arglen + 1 : PATHLEN);
        str_trim(p);
        if (h->xproto_set) N.user.proto = (UBYTE)h->xproto;
        if (!p[0] || !N.online) got = 0;
        else if (!str_icmp(word, "XDN")) got = file_send_path(p, (const char *)FilePart((STRPTR)p)) ? 1 : 0;
        else {
            char names[16][32], xname[32];
            BPTR l = Lock((STRPTR)p, ACCESS_READ);
            struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
            BOOL isdir = FALSE;
            if (l && fib && Examine(l, fib)) isdir = fib->fib_DirEntryType > 0;
            if (fib) FreeDosObject(DOS_FIB, fib);
            if (l) UnLock(l);
            memset(names, 0, sizeof(names));
            if (isdir || p[strlen(p) - 1] == ':' || p[strlen(p) - 1] == '/') {
                str_copy(dir, p, sizeof(dir)); strcpy(xname, "upload");
            } else {
                str_copy(dir, p, sizeof(dir)); *PathPart((STRPTR)dir) = 0;
                str_copy(xname, (const char *)FilePart((STRPTR)p), sizeof(xname));
            }
            got = file_receive_dir(dir, names, 16, xname);
            if (got > 0 && !isdir && names[0][0] && str_icmp(names[0], xname)) {   /* the name the door asked for */
                char from[PATHLEN];
                str_copy(from, dir, sizeof(from)); AddPart((STRPTR)from, (STRPTR)names[0], sizeof(from));
                DeleteFile((STRPTR)p);
                Rename((STRPTR)from, (STRPTR)p);
            }
        }
        N.user.proto = keep;
        sprintf(out, "%ld", (long)got);
        rx_reply(m, 0, out, strlen(out));
    } else if (!str_icmp(word, "SETOBJECT")) {
        /* CNet: SETOBJECT value, then PUTUSER n stores it in the user's field n */
        str_copy(h->object, (const char *)arg, arglen + 1 < (LONG)sizeof(h->object) ? arglen + 1 : (LONG)sizeof(h->object));
        str_trim(h->object);
        rx_reply(m, 0, "", 0);
    } else if (!str_icmp(word, "PUTUSER")) {
        /* the fields a door changes: time left (7, tenths of a minute) moves the
         * caller's time; the rest (credits a game pays out ...) is kept for this
         * door's GETUSERs and logged - NilBBS's account isn't CNet's */
        LONG n = atol((const char *)arg);
        if (n == 7 && N.limit_mins > 0) {
            LONG left = atol(h->object) / 10;
            if (left < 0) left = 0;
            N.limit_mins = (LONG)((bbs_now() - N.logon) / 60) + left;
        } else if (n > 0 && n < 64) {
            str_copy(h->uov[n], h->object, sizeof(h->uov[n]));
            h->uovset[n] = TRUE;
        }
        bbs_log(BBS_SYSLOG, "node %d: door %s PUTUSER %ld = %s", N.node, io->d->tag, n, h->object);
        rx_reply(m, 0, "", 0);
    } else if (!str_icmp(word, "CLEAREDITOR") || !str_icmp(word, "LOADEDITOR") ||
               !str_icmp(word, "SAVEEDITOR") || !str_icmp(word, "CALLEDITOR") ||
               !str_icmp(word, "SETMAILSUBJ") || !str_icmp(word, "WRITEMAIL")) {
        rx_editor(io, h, m, word, arg, arglen);
    } else if (!str_icmp(word, "SETLENGTH")) {
        rx_reply(m, 0, "", 0);                      /* the caller's terminal says how long a screen is */
    } else if (!str_icmp(word, "GETCARRIER")) {
        rx_reply(m, 0, N.online ? "TRUE" : "FALSE", N.online ? 4 : 5);
    } else if (!str_icmp(word, "BBSIDENTIFY")) {
        char out[160];
        const char *w = (const char *)arg;
        if (!str_nicmp(w, "EMULATION", 9)) strcpy(out, N.term == TT_ASCII ? "ASCII" : "ANSI");
        else if (!str_nicmp(w, "BBS", 3)) sprintf(out, "NilBBS %s", BBS_VERSION);
        else if (!str_nicmp(w, "NAME", 4)) strcpy(out, cfg_str(N.cfg, "bbs_name", "NilBBS"));
        else if (!str_nicmp(w, "SYSOP", 5)) strcpy(out, cfg_str(N.cfg, "sysop_name", "Sysop"));
        else if (!str_nicmp(w, "TERM", 4)) sprintf(out, "115200 %d %d %d", (int)N.cols, (int)N.rows, N.node - 1);
        else if (!str_nicmp(w, "USER", 4))
            sprintf(out, "\"%s\" \"%s\" %s", N.user.name, N.user.location,
                    N.user.id == 1 ? "SYSOP" : N.sysop ? "CO-SYSOP" : "MEMBER");
        else if (!str_nicmp(w, "ABBEREXX", 8)) strcpy(out, "1.0");
        else out[0] = 0;
        rx_reply(m, 0, out, strlen(out));
    } else if (!str_icmp(word, "VERSION")) {
        static const char ver[] = "CNet AMIGA 3.05 (NilBBS " BBS_VERSION ")";
        rx_reply(m, 0, ver, strlen(ver));
    } else if (!str_icmp(word, "LOGENTRY") || !str_icmp(word, "SYSOPLOG")) {
        char l[200];
        str_copy(l, (const char *)arg, arglen + 1 < 200 ? arglen + 1 : 200);
        mci_strip(l);
        bbs_log(BBS_SYSLOG, "node %d: [%s] %s", N.node, io->d->tag, l);
        rx_reply(m, 0, "", 0);
    } else if (!str_icmp(word, "CHANGEWHERE") || !str_icmp(word, "SETNODELOCATION") ||
               !str_icmp(word, "CHANGEWHAT")) {
        char l[LONGNAME];
        str_copy(l, (const char *)arg, arglen + 1 < LONGNAME ? arglen + 1 : LONGNAME);
        set_activity(l);
        rx_reply(m, 0, "", 0);
    } else if (!str_icmp(word, "ADDTIME")) {
        N.limit_mins += atoi((const char *)arg);
        rx_reply(m, 0, "", 0);
    } else if (!str_icmp(word, "ADDKEYS")) {
        LONG i;
        for (i = arglen - 1; i >= 0; i--) in_unget(arg[i]);
        rx_reply(m, 0, "", 0);
    } else if (!str_icmp(word, "SPAWN")) {
        /* CNet games use SPAWN to hand over: "spawn Emp.Main ; exit" - the named
         * script takes over our port (and the caller) when this one ends */
        str_copy(h->spawn, (const char *)arg, arglen + 1 < PATHLEN ? arglen + 1 : PATHLEN);
        str_trim(h->spawn);
        rx_reply(m, 0, "", 0);
    } else if (!str_icmp(word, "BBSCOMMAND")) {
        /* CNet runs one of its own commands for the script: WHO, OLM ... - ours */
        char c[24];
        int k = 0;
        const UBYTE *s = arg;
        while (s < arg + arglen && *s == ' ') s++;
        while (s < arg + arglen && *s != ' ' && k < (int)sizeof(c) - 1) c[k++] = (char)*s++;
        c[k] = 0;
        if (!str_icmp(c, "WHO") || !str_icmp(c, "WHOSON")) whos_online();
        else if (!str_icmp(c, "OLM") || !str_icmp(c, "SEND") || !str_icmp(c, "NM")) page_node();
        else if (!str_icmp(c, "US") || !str_icmp(c, "USERS") || !str_icmp(c, "UL")) userlist();
        else if (!str_icmp(c, "LC") || !str_icmp(c, "LAST")) lastcallers_show();
        else if (!str_icmp(c, "FINGER") || !str_icmp(c, "IN")) finger();
        else if (!str_icmp(c, "ST") || !str_icmp(c, "STATUS")) user_status();
        else if (!str_icmp(c, "SYSINFO") || !str_icmp(c, "SI")) sysinfo();
        else if (!str_icmp(c, "PAGE") || !str_icmp(c, "OP")) page_sysop();
        else if (!str_icmp(c, "MUFFLE") || !str_icmp(c, "UM") || !str_icmp(c, "UNMUFFLE"))
            ;       /* OLMs on/off: node messages reach a caller only after the door anyway */
        else bbs_log(BBS_SYSLOG, "node %d: CNet BBSCOMMAND %s not emulated (door %s)", N.node, c, io->d->tag);
        rx_reply(m, 0, "", 0);
    } else if (!str_icmp(word, "SHUTDOWN") || !str_icmp(word, "SCREENOUT") ||
               !str_icmp(word, "OPENDISPLAY") ||
               !str_icmp(word, "CLOSEDISPLAY") || !str_icmp(word, "BAUD") ||
               !str_icmp(word, "MODEM") || !str_icmp(word, "SENDMODEM")) {
        if (!str_icmp(word, "SENDMODEM")) door_write(io, arg, arglen);
        rx_reply(m, 0, "", 0);
    } else if (!str_icmp(word, "GETUSER")) {
        char out[88];
        getuser(h, atol((const char *)arg), out);
        rx_reply(m, 0, out, strlen(out));
    } else if (!str_icmp(word, "CLS")) {
        door_write(io, (const UBYTE *)"\x1b[2J\x1b[H", 7);
        rx_reply(m, 0, "", 0);
    } else if (!str_icmp(word, "BYE") || !str_icmp(word, "HANGUP") || !str_icmp(word, "DROPCARRIER")) {
        rx_reply(m, 0, "", 0);
        node_hangup("door hung up");
    } else if (!str_icmp(word, "CARRIER") || !str_icmp(word, "CHECKCARRIER")) {
        rx_reply(m, 0, N.online ? "1" : "0", 1);
    } else if (word[0] >= '0' && word[0] <= '9') {
        /* a bare function call as a statement (e.g. OPEN(...)) sends its
         * value to the host as a "command"; CNet ignores those, so do we */
        rx_reply(m, 0, "", 0);
    } else {
        int i;
        BOOL seen = FALSE;
        for (i = 0; i < h->nunknown; i++) if (!str_icmp(h->unknown[i], word)) seen = TRUE;
        if (!seen) {
            if (h->nunknown < 8) str_copy(h->unknown[h->nunknown++], word, 16);
            bbs_log(BBS_SYSLOG, "node %d: CNet ARexx command %s not emulated (door %s)",
                    N.node, word, io->d->tag);
        }
        rx_reply(m, 0, "", 0);
    }
}

static void run_cnetrexx(struct Door *d)
{
    struct DoorIO io;
    struct RexxHost h;
    struct MsgPort *rexx;
    char cmd[PATHLEN * 2], drop[PATHLEN];
    BPTR in, out, olddir = 0, dirlock = 0;
    ULONG sigs, hung_at = 0, broke_at = 0;

    memset(&io, 0, sizeof(io));
    memset(&h, 0, sizeof(h));
    io.d = d;
    io.next_id = 3;
    write_dropfiles(d, drop);

    if (!(RexxSysBase = OpenLibrary((STRPTR)"rexxsyslib.library", 36))) {
        tputs(L("door.run_cnetrexx.arexx_isnt_available", "|12ARexx isn't available on this system.|07\n"));
        return;
    }
    Forbid();
    rexx = FindPort((STRPTR)"REXX");
    Permit();
    if (!rexx) {
        /* start the ARexx master and give it a moment */
        SystemTags((STRPTR)"Run >NIL: RexxMast >NIL:", TAG_END);
        Delay(100);
    }

    if (!str_icmp(d->type, "aim")) {
        /* an AmiExpress ARexx door: /X's host port and its door state (the data table) */
        if (!(h.aim = aim_state())) goto fail;
        sprintf(h.name, "AERexxControl%d", N.node - 1);
    } else
    sprintf(h.name, "CNETREXX%d", N.node - 1);   /* the name CNet uses */
    if (!(io.port = CreateMsgPort()) || !(h.reply = CreateMsgPort()) || !(h.port = CreateMsgPort()))
        goto fail;
    h.port->mp_Node.ln_Name = h.name;
    h.port->mp_Node.ln_Pri = 0;
    AddPort(h.port);

    in  = make_handle(&io, H_IN);
    out = make_handle(&io, H_OUT);
    if (!in || !out) goto fail_port;
    io.in_open = io.out_open = TRUE;
    N.door_csi8 = d->amigacsi;
    h.in = in;
    h.out = out;
    h.srccs = d->srccs;
    h.oldmci = d->oldmci;

    if (d->dir[0]) {
        expand(d->dir, cmd, sizeof(cmd), drop);
        if ((dirlock = Lock((STRPTR)cmd, ACCESS_READ))) olddir = CurrentDir(dirlock);
    }
    expand(d->command, cmd, sizeof(cmd), drop);
    if (h.aim) sprintf(cmd + strlen(cmd), " %d", N.node - 1);  /* REXXDOOR: "RX <script> <node>" */
    if (!(h.launch = CreateRexxMsg(h.reply, (STRPTR)"rexx", (STRPTR)h.name))) goto fail_handles;
    h.launch->rm_Args[0] = CreateArgstring((STRPTR)cmd, strlen(cmd));
    h.launch->rm_Action = RXCOMM;
    h.launch->rm_Stdin = in;
    h.launch->rm_Stdout = out;
    Forbid();
    if ((rexx = FindPort((STRPTR)"REXX"))) PutMsg(rexx, (struct Message *)h.launch);
    Permit();
    if (!rexx) {
        DeleteArgstring(h.launch->rm_Args[0]);
        DeleteRexxMsg(h.launch);
        h.launch = NULL;
        tputs(L("door.run_cnetrexx.arexx_rexxmast_isnt", "|12ARexx (RexxMast) isn't running.|07\n"));
        goto fail_handles;
    }

    sigs = (1UL << io.port->mp_SigBit) | (1UL << h.port->mp_SigBit) | (1UL << h.reply->mp_SigBit);
    N.wait_new = 1;
    for (;;) {
        struct RexxMsg *m;
        BOOL finished = FALSE;
        if (N.online) {
            ULONG got;
            tn_wait((io.waitpkt || TOPW(&h) == MW_TIME) ? 60 : 1000, sigs, &got);
            if (N.msg_waiting) N.msg_waiting = FALSE;
            if (N.loggedin && time_left_mins() == 0) {
                door_write(&io, (const UBYTE *)time_up_text(), strlen(time_up_text()));
                tn_flush();
                node_hangup("time limit in door");
            }
        } else Delay(5);

        drain_port(&io);
        while ((m = (struct RexxMsg *)GetMsg(h.port))) rx_command(&io, &h, m);
        if (TOPW(&h) == MW_TIME && ticks_now() >= h.muntil) {       /* an {Wn} pause is over */
            SETW(&h, MW_NONE);
            mci_run(&io, &h);
        }
        while ((m = (struct RexxMsg *)GetMsg(h.reply))) {
            BOOL sub = (m != h.launch);                         /* else a {#0} sub-pfile */
            if (m->rm_Result1 && N.online) {
                char e[128];
                snprintf(e, sizeof(e), L("door.rexx_error", "\r\n[door ended with error %ld]\r\n"), m->rm_Result1);
                door_write(&io, (UBYTE *)e, strlen(e));
            }
            DeleteArgstring(m->rm_Args[0]);
            DeleteRexxMsg(m);
            if (sub) {                                          /* its caller's text goes on */
                int i;
                h.nsubs--;
                for (i = 0; i < h.nst; i++)
                    if (h.st[i].sub == m) { h.st[i].sub = NULL; if (h.st[i].wait == MW_SUB) h.st[i].wait = MW_NONE; }
                mci_run(&io, &h);
                continue;
            }
            h.launch = NULL;
            if (h.spawn[0] && N.online && !io.hungup) {         /* SPAWN: the next script takes over */
                char next[PATHLEN];
                strcpy(next, h.spawn);
                h.spawn[0] = 0;
                if ((h.launch = rx_launch(&h, next))) continue;
                bbs_log(BBS_SYSLOG, "node %d: door %s could not SPAWN %s", N.node, d->tag, next);
            }
            finished = TRUE;
        }
        /* the main script is done, but a {#0} sub-pfile may still be running (Wordo's
         * instructions after a drop): keep answering it until it ends too, or the ports
         * would have to be abandoned (~43 KB lost each time) */
        if (finished && h.nsubs > 0) finished = FALSE;
        if (!h.launch && h.nsubs <= 0) finished = TRUE;
        if (finished) break;

        if (!N.online && !io.hungup) {
            io.hungup = TRUE;
            hung_at = bbs_now();
            flush_queues_eof(&io);
            /* panic_grace = 0: a script that would SAVE "###PANIC" (BattleLands takes it as a new
             * player's name, Destroy/Hacker write half-made records) is broken BEFORE it gets its
             * answer - ARexx sees the CTRL-C at the end of that clause and halts, nothing written */
            if (d->panicgrace <= 0 && h.script) {
                Signal(h.script, SIGBREAKF_CTRL_C);
                broke_at = bbs_now();
            }
            if (h.waiting) { rx_reply(h.waiting, 0, "###PANIC", 8); h.waiting = NULL; }
            mci_reset(&h);
            h.spawn[0] = 0;
            bbs_log(BBS_SYSLOG, "node %d: caller dropped in ARexx door %s", N.node, d->tag);
        }
        if (io.hungup) {
            ULONG since = bbs_now() - hung_at;
            flush_queues_eof(&io);
            /* grace for scripts that honour ###PANIC and save; then break them
             * ONCE - a break arriving while the script's error handler is
             * still saving (Realm's SYNTAX trap) would kill the save half-way.
             * Repeat only every 30 s in case the first one was ignored. */
            if (since >= (ULONG)d->panicgrace && h.script &&
                (!broke_at || bbs_now() - broke_at >= 30)) {
                Signal(h.script, SIGBREAKF_CTRL_C);
                broke_at = bbs_now();
            }
            if (since > 120) break;                         /* script won't die */
            if (node_reset_wanted()) node_release_slot();    /* sysop RESET: free the node, keep answering */
        } else {
            /* host input (GETCHAR/QUERY) comes first; the console only takes
             * keys while the script itself is reading it (PULL, READCH) -
             * otherwise the cooked line editor would swallow every key */
            rx_service_input(&io, &h);
            if (io.nread || io.waitpkt) service_queues(&io);
            /* AIM: /X's input timeout (DT_TIMEOUT) ends the wait with rc 10 */
            if (h.aim && h.waiting && xim_timeout(h.aim) > 0 && bbs_now() - h.wait_since >= (ULONG)xim_timeout(h.aim)) {
                bbs_log(BBS_SYSLOG, "node %d: door %s input timed out", N.node, d->tag);
                rx_reply(h.waiting, 10, NULL, 0);
                h.waiting = NULL;
            }
        }
    }
    N.wait_new = 0;
    if (h.waiting) rx_reply(h.waiting, 20, NULL, 0);
    mci_reset(&h);
    RemPort(h.port);
    /* anything that raced in after the script ended */
    {
        struct RexxMsg *m;
        while ((m = (struct RexxMsg *)GetMsg(h.port))) rx_reply(m, 20, NULL, 0);
    }
    flush_queues_eof(&io);
    if (h.launch || h.nsubs > 0) {
        /* the script never finished: leave the ports as silent sinks */
        Forbid();
        h.reply->mp_Flags = PA_IGNORE;
        h.port->mp_Flags = PA_IGNORE;
        io.port->mp_Flags = PA_IGNORE;
        Permit();
        bbs_log(BBS_SYSLOG, "node %d: ARexx door %s did not stop - ports abandoned", N.node, d->tag);
    } else {
        /* we own the std handles (ARexx doesn't close them) */
        FreeDosObject(DOS_FILEHANDLE, BADDR(in));
        FreeDosObject(DOS_FILEHANDLE, BADDR(out));
        DeleteMsgPort(h.port);
        DeleteMsgPort(h.reply);
        DeleteMsgPort(io.port);
    }
    if (dirlock) { CurrentDir(olddir); UnLock(dirlock); }
    N.door_csi8 = 0;
    if (h.scr) { FreeVec(h.scr); h.scr = NULL; }
    CloseLibrary(RexxSysBase);
    RexxSysBase = NULL;
    tcolor(7);
    if (h.aim) {
        xim_retcmd(h.aim, d);                   /* RETURNCOMMAND (PUTUSER 136): G logs off */
        FreeVec(h.aim);
    }
    if (h.ed) FreeVec(h.ed);
    return;

fail_handles:
    if (dirlock) { CurrentDir(olddir); UnLock(dirlock); }
    if (in) FreeDosObject(DOS_FILEHANDLE, BADDR(in));
    if (out) FreeDosObject(DOS_FILEHANDLE, BADDR(out));
    N.door_csi8 = 0;
fail_port:
    RemPort(h.port);
fail:
    if (h.port) DeleteMsgPort(h.port);
    if (h.reply) DeleteMsgPort(h.reply);
    if (io.port) DeleteMsgPort(io.port);
    if (h.aim) FreeVec(h.aim);
    if (h.scr) { FreeVec(h.scr); h.scr = NULL; }
    CloseLibrary(RexxSysBase);
    RexxSysBase = NULL;
    tputs(L("door.run_cnetrexx.the_door_could", "|12The door could not be started.|07\n"));
}

/* ---- AmiExpress XIM doors ------------------------------------------------------------
 *
 * type = xim  runs a door written for AmiExpress (/X): the node publishes
 * AEDoorPort<n> and starts "<command> <n>"; the door (normally through
 * AEDoor.library) sends 256-byte JHMessages there.  Semantics follow the
 * MIT-licensed /X 5 source (processXimMsg).  Carrier loss answers input
 * requests with Data = -1 like /X; a door that ignores that gets one
 * CTRL-C after panic_grace seconds.  Unlike /X we also end the session
 * when the door process exits without JH_SHUTDOWN (a crashed door).
 */
struct JHMessage {
    struct Message msg;
    char   String[200];
    LONG   Data;
    LONG   Command;
    LONG   NodeID;
    LONG   LineNum;
    ULONG  signal;
    struct Process *task;
    LONG   Semi;
    LONG   Filler1, Filler2;
    char  *strptr;              /* /X 5 extension: only if mn_Length >= 264 */
    LONG   filler3;
};

#define JH_LI 0
#define JH_REGISTER 1
#define JH_SHUTDOWN 2
#define JH_WRITE 3
#define JH_SM 4
#define JH_PM 5
#define JH_HK 6
#define JH_SG 7
#define JH_SF 8
#define JH_EF 9
#define JH_CO 10
#define JH_BBSNAME 11
#define JH_SYSOP 12
#define JH_FLAGFILE 13
#define JH_SHOWFLAGS 14
#define JH_ExtHK 15
#define JH_SIGBIT 16
#define JH_FetchKey 17
#define JH_SO 18
#define JH_SMPTR 19
#define DT_NAME 100
#define DT_PASSWORD 101
#define DT_LOCATION 102
#define DT_PHONENUMBER 103
#define DT_SLOTNUMBER 104
#define DT_SECSTATUS 105
#define DT_SECBOARD 106
#define DT_SECLIBRARY 107
#define DT_SECBULLETIN 108
#define DT_MESSAGESPOSTED 109
#define DT_UPLOADS 110
#define DT_DOWNLOADS 111
#define DT_TIMESCALLED 112
#define DT_TIMELASTON 113
#define DT_TIMEUSED 114
#define DT_TIMELIMIT 115
#define DT_TIMETOTAL 116
#define DT_BYTESUPLOAD 117
#define DT_BYTEDOWNLOAD 118
#define DT_DAILYBYTELIMIT 119
#define DT_DAILYBYTEDLD 120
#define DT_EXPERT 121
#define DT_LINELENGTH 122
#define ACTIVE_NODES 123
#define DT_DUMP 124
#define DT_TIMEOUT 125
#define BB_CONFNAME 126
#define BB_CONFLOCAL 127
#define BB_LOCAL 128
#define BB_MAINLINE 131
#define RETURNCOMMAND 136
#define ZMODEMSEND 137
#define ZMODEMRECEIVE 138
#define BB_CHATFLAG 142
#define DT_STAMP_LASTON 143
#define DT_STAMP_CTIME 144
#define DT_CURR_TIME 145
#define DT_CONFACCESS 146
#define BB_NODEID 149
#define BB_CALLERSLOG 150
#define BB_UDLOG 151
#define EXPRESS_VERSION 152
#define GETKEY 500
#define RAWARROW 501
#define CHAIN 502
#define JH_MCI 507
#define BB_CONFNUM 510
#define BB_GETTASK 512
#define BB_LOGONTYPE 517
#define BB_NONSTOPTEXT 525
#define BB_LINECOUNT 526
#define DT_LANGUAGE 527
#define DT_ANSICOLOR 530
#define MOD_TYPE 540
#define DT_ISANSI 541
#define JH_20 20
#define ENVSTAT 163
#define SV_NEWMSG 177
#define NODE_DEVICE 503
#define NODE_UNIT 504
#define NODE_BAUD 505
#define BB_DROPDTR 511
#define NODE_BAUDRATE 516
#define MULTICOM 531
#define LOAD_ACCOUNT 532
#define SAVE_ACCOUNT 533
#define SEARCH_ACCOUNT 537
#define SETMCIOFF 551
#define QUICK_KEY 608
#define CONF_ACCESS 614
#define DISPLAY_FILE 617
#define CHECK_TO_DISPLAY 618
#define INTERPRET_MCI 621
#define GET_XIMPORT 622
#define EXT_LOAD_ACCOUNT 633
#define TELNET_CONNECT 706
#define TELNET_USERNAME_PROMPT 708
#define TELNET_USERNAME 709
#define TELNET_PASSWORD_PROMPT 710
#define TELNET_PASSWORD 711
#define DT_ADDBIT 1000
#define DT_REMBIT 1001
#define DT_QUERYBIT 1002

/* what a failed hot key / line request hands back.  /X 5 gives its
 * RESULT_TIMEOUT (-3) and RESULT_NO_CARRIER (-4), but doors written for the
 * older /X test the key / Command for -1 on a lost carrier: DreamSweeper
 * spun forever on -4 (FetchKey), deaf to CTRL-C.  So a drop is -1, which
 * every door reads as "gone"; a timeout stays -3. */
#define XR_TIMEOUT   (-3)
#define XR_NOCARRIER (-1)
/* /X's clock: seconds since 1970 plus 6 hours (getSystemTime); ours counts
 * from 1978 */
#define AE_EPOCH (2922UL * 86400UL + 21600UL)

struct XimState {
    struct MsgPort *port;
    char   name[20];
    LONG   registered;              /* JH_REGISTER - JH_SHUTDOWN */
    BOOL   ever_registered;
    BOOL   chain_pending;           /* CHAIN seen: the next door registers on our port */
    struct JHMessage *waiting;      /* HK / PM / LI / ExtHK / JH_20 blocked on input */
    UBYTE  wait_kind;               /* 1 hotkey (String[0]), 2 line, 3 ExtHK (Command), 4 raw key (Data) */
    LONG   maxlen;
    UBYTE  line[204];
    int    len;
    ULONG  wait_since;              /* when the request above was parked */
    BOOL   belled;                  /* the minute-to-go bell rang */
    LONG   timeout;                 /* input timeout, seconds (DT_TIMEOUT; /X default 300) */
    BOOL   rawarrow;                /* RAWARROW: arrows reach the door as 4/5/3/2 */
    BYTE   extsig;                  /* JH_SIGBIT: a door Signal()s it to wake a JH_ExtHK */
    BOOL   extsig_seen;
    LONG   envstat;                 /* ENVSTAT: what the door says the node is doing */
    char   retcmd[64];              /* RETURNCOMMAND: run after the door (G = log off) */
    int    postdrop;                /* commands logged after the caller dropped */
    struct Task *door;
    char   mainline[PATHLEN];
    char   unknown[8];              /* first few unknown commands, logged once */
    int    nunknown;
    LONG   unknown_cmds[8];
};

/* /X 5's telnet gateway (TELNET_CONNECT, telnetConnect doors): the caller is
 * connected on to host:port with NilBBS's own telnet relay (run_net), which
 * types the username / password when their prompts show up (as /X does) */
static struct {
    char uprompt[100], user[64], pprompt[100], pass[64];
    BOOL active, user_sent, pass_sent;
    char seen[200];
    int  nseen;
} g_netlogin;
static void run_net(struct Door *d);

/* /X security flags (ACS_*, DT_QUERYBIT) only a sysop has: account editing,
 * remote shell, chat override, sysop download/view/read, time overrides,
 * screen to front, EALL, message edit/delete, dir/file editing, break chat,
 * quiet node, sysop commands, hidden files, payments, account view, unknown,
 * create conference, local downloads, override defaults, hold access,
 * private EALL/ALL, time/chat limits, no timeout.  71 (censored) is FALSE
 * for everyone. */
static const UBYTE ae_sysop_acs[] = { 0, 11, 20, 21, 22, 23, 25, 32, 34, 36, 40, 46, 47, 48, 49, 50,
                                      63, 64, 72, 74, 75, 76, 78, 79, 81, 82, 83, 84, 85 };
static BOOL ae_query_bit(LONG bit)
{
    int i;
    if (bit < 0 || bit > 86 || bit == 71) return FALSE;
    if (N.sysop) return TRUE;
    for (i = 0; i < (int)sizeof(ae_sysop_acs); i++) if (ae_sysop_acs[i] == bit) return FALSE;
    return TRUE;
}

/* "Tue Sep 29 14:05:00 2026" (the C ctime() form /X's DT_STAMP_* use) from our clock */
static void ae_ctime(ULONG t, char *out)
{
    static const char *wd[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *mn[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    static const UBYTE md[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    ULONG days = t / 86400UL, secs = t % 86400UL;
    int y = 1978, m = 0, dim;
    int dow = (int)(days % 7);                  /* 1 Jan 1978 was a Sunday */
    for (;;) {
        int yl = ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 366 : 365;
        if (days < (ULONG)yl) break;
        days -= yl; y++;
    }
    for (;;) {
        dim = md[m] + (m == 1 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0));
        if (days < (ULONG)dim) break;
        days -= dim; m++;
    }
    sprintf(out, "%s %s %2lu %02lu:%02lu:%02lu %d", wd[dow], mn[m], days + 1,
            secs / 3600, (secs / 60) % 60, secs % 60, y);
}

static void xs_copy(struct JHMessage *m, const char *s)
{
    str_copy(m->String, s, sizeof(m->String));
}

static void xim_write(struct DoorIO *io, const char *s, BOOL lf)
{
    door_write(io, (const UBYTE *)s, strlen(s));
    if (lf) door_write(io, (const UBYTE *)"\n", 1);
}

/* the next key for a /X door, -1 = none yet.  Cursor keys (ESC [ A-D, or
 * ESC O A-D) are one key, as /X reads them: swallowed (-2) unless the door
 * turned RAWARROW on, then 4 up, 5 down, 3 right, 2 left.  Other sequences
 * (Home, Del = ESC [ 3 ~ ...) are swallowed whole - /X would pass their
 * last letter on, which only ever typed junk. */
static LONG xim_key(struct DoorIO *io, struct XimState *x)
{
    LONG c = door_inbyte(io), c2;
    int i;
    if (c != 27 || io->feeding) return c;
    for (i = 0; i < 10 && !in_avail(); i++) Delay(1);     /* the rest arrives with it */
    if (!in_avail()) return 27;
    c2 = door_inbyte(io);
    if (c2 != '[' && c2 != 'O') return c2;
    for (i = 0; i < 10 && !in_avail(); i++) Delay(1);
    if ((c = door_inbyte(io)) < 0) return -2;
    while ((c >= '0' && c <= '9') || c == ';') {            /* ESC [ 3 ~, ESC [ 1 ; 5 A ... */
        for (i = 0; i < 10 && !in_avail(); i++) Delay(1);
        if ((c = door_inbyte(io)) < 0) return -2;
    }
    if (c < 'A' || c > 'D') return -2;
    if (!x->rawarrow) return -2;
    return c == 'A' ? 4 : c == 'B' ? 5 : c == 'C' ? 3 : 2;
}

/* a parked input request fails (code XR_TIMEOUT / XR_NOCARRIER), replied
 * the way /X does it for each kind */
static void xim_fail(struct XimState *x, LONG code)
{
    struct JHMessage *m = x->waiting;
    if (!m) return;
    switch (x->wait_kind) {
    case 1:  /* /X 5 leaves Command = 2 (the port), but the AEDoor.library doors use
              * (checked with DreamSweeper) takes Command -1 as the lost carrier and
              * otherwise asks again, forever: so -1 */
             m->String[0] = (char)code; m->String[1] = 0; m->Command = -1; m->Data = -1; break;
    case 3:  m->Command = code; m->Data = -1; break;
    case 4:  m->Data = code; m->Command = 2; break;
    default: m->Data = -1; break;                           /* PM / LI: the text stays */
    }
    ReplyMsg((struct Message *)m);
    x->waiting = NULL;
}

/* the caller typed something: finish a blocked input request */
static void xim_service_input(struct DoorIO *io, struct XimState *x)
{
    LONG c;
    struct JHMessage *m = x->waiting;
    if (!m) return;
    if (x->wait_kind == 3 && x->extsig_seen) {              /* the door's own signal: key 0 */
        x->extsig_seen = FALSE;
        m->Command = 0; m->Data = 1;
        ReplyMsg((struct Message *)m);
        x->waiting = NULL;
        return;
    }
    if (x->wait_kind != 2) {
        do c = xim_key(io, x); while (c == -2);
        if (c < 0) return;
        if (x->wait_kind == 1) { m->String[0] = (char)c; m->String[1] = 0; m->Command = 2; m->Data = 1; }
        else if (x->wait_kind == 3) { m->Command = c; m->Data = 1; }
        else { m->Data = c; m->Command = 2; }
        ReplyMsg((struct Message *)m);
        x->waiting = NULL;
        return;
    }
    while ((c = xim_key(io, x)) >= 0 || c == -2) {
        if (c == -2) continue;
        if (c == '\r' || c == '\n') {
            tn_raw((const UBYTE *)"\r\n", 2);
            x->line[x->len] = 0;
            xs_copy(m, (char *)x->line);
            m->Data = 1;
            ReplyMsg((struct Message *)m);
            x->waiting = NULL;
            return;
        } else if (c == 8) {
            if (x->len > 0) { x->len--; tn_raw((const UBYTE *)"\b \b", 3); }
        } else if (c == 21 || c == 24) {
            while (x->len > 0) { x->len--; tn_raw((const UBYTE *)"\b \b", 3); }
        } else if (c >= 32 && x->len < x->maxlen && x->len < 199) {
            UBYTE b = (UBYTE)c;
            x->line[x->len++] = b;
            tputraw(&b, 1, io->d->srccs);
        }
    }
}

/* a door changed the time: TOTAL (today's allowance) or USED (seconds) - a
 * time bank, a bet, a reward.  Only the minutes left move (the caller's
 * limit for this call); unlimited callers stay unlimited. */
static void xim_set_time(LONG cmd, LONG val, ULONG used, ULONG allowed)
{
    LONG left;
    if (N.limit_mins <= 0 || val < 0) return;
    if (cmd == DT_TIMETOTAL) left = (val - (LONG)used) / 60;
    else left = ((LONG)allowed - val) / 60;
    if (left < 0) left = 0;
    N.limit_mins = (LONG)((bbs_now() - N.logon) / 60) + left;
    bbs_log(BBS_SYSLOG, "node %d: door set %s to %ld s - %ld min left", N.node,
            cmd == DT_TIMETOTAL ? "time total" : "time used", val, left);
}

static void xim_dt(struct XimState *x, struct JHMessage *m, LONG cmd)
{
    char s[64];
    LONG left = time_left_mins();
    ULONG used = (bbs_now() - N.logon) + (ULONG)N.user.mins_today * 60;
    ULONG allowed = left < 0 ? 999UL * 60 + used : (ULONG)left * 60 + used;
    s[0] = 0;
    if (!m->Data) {
        /* writes /X applies that matter to a caller here; the account's name,
         * level, counters and password stay the sysop's business (logged) */
        switch (cmd) {
        case DT_TIMETOTAL: case DT_TIMEUSED: xim_set_time(cmd, atol(m->String), used, allowed); break;
        case DT_TIMEOUT:   x->timeout = atol(m->String); break;
        case DT_EXPERT:
            if (m->String[0] == 'X' || m->String[0] == 'x') N.user.flags |= UF_EXPERT;
            else N.user.flags &= ~UF_EXPERT;
            break;
        case DT_LINELENGTH: {
            LONG r = atol(m->String);
            if (r >= 10 && r <= 99) N.rows = (UWORD)r;
            break;
        }
        case DT_LOCATION:
            str_copy(N.user.location, m->String, sizeof(N.user.location));
            str_nopipe(N.user.location);
            break;
        default:
            bbs_log(BBS_SYSLOG, "node %d: door write to user field %ld (\"%.30s\") not applied",
                    N.node, cmd, m->String);
        }
        return;
    }
    switch (cmd) {
    case DT_NAME:           strcpy(s, N.user.name); break;
    case DT_PASSWORD:       break;      /* doors never get it (like /X 5) */
    case DT_LOCATION:       str_copy(s, N.user.location, 31); break;
    case DT_PHONENUMBER:    strcpy(s, "000-000-0000"); break;
    case DT_SLOTNUMBER:     sprintf(s, "%lu", N.user.id); break;
    case DT_SECSTATUS:      sprintf(s, "%d", (int)N.user.level); break;
    case DT_SECBOARD: case DT_SECLIBRARY: case DT_SECBULLETIN: strcpy(s, "0"); break;
    case DT_MESSAGESPOSTED: sprintf(s, "%lu", N.user.posts & 0xFFFF); break;
    case DT_UPLOADS:        sprintf(s, "%lu", N.user.uploads & 0xFFFF); break;
    case DT_DOWNLOADS:      sprintf(s, "%lu", N.user.downloads & 0xFFFF); break;
    case DT_TIMESCALLED:    sprintf(s, "%lu", N.user.calls & 0xFFFF); break;
    case DT_TIMELASTON:     sprintf(s, "%lu", N.user.lastcall + AE_EPOCH); break;    /* /X clock */
    case DT_TIMEUSED:       sprintf(s, "%lu", used); break;           /* seconds */
    case DT_TIMELIMIT:      sprintf(s, "%lu", allowed); break;
    case DT_TIMETOTAL:      sprintf(s, "%lu", allowed); break;
    case DT_BYTESUPLOAD:    sprintf(s, "%lu", N.user.ulkb * 1024); break;
    case DT_BYTEDOWNLOAD:   sprintf(s, "%lu", N.user.dlkb * 1024); break;
    case DT_DAILYBYTELIMIT: strcpy(s, "0"); break;
    case DT_DAILYBYTEDLD:   strcpy(s, "0"); break;
    case DT_EXPERT:         strcpy(s, (N.user.flags & UF_EXPERT) ? "X" : "N"); break;
    case DT_LINELENGTH:     sprintf(s, "%d", (int)N.rows); break;
    case DT_TIMEOUT:        sprintf(s, "%ld", x->timeout); break;
    }
    xs_copy(m, s);
}

/* returns TRUE if the message was replied (FALSE = parked for input) */
static BOOL xim_command(struct DoorIO *io, struct XimState *x, struct JHMessage *m)
{
    LONG cmd = m->Command;
    BOOL gone = io->hungup || !N.online;
    char s[210];

    if (m->msg.mn_ReplyPort && m->msg.mn_ReplyPort->mp_SigTask)
        x->door = (struct Task *)m->msg.mn_ReplyPort->mp_SigTask;
    m->LineNum = 0;
    if (io->d->debug) {
        char dbg[40];
        str_copy(dbg, m->String, sizeof(dbg));
        bbs_log(BBS_SYSLOG, "node %d: [%s] /X cmd %ld data %ld len %u str \"%s\"", N.node, io->d->tag,
                cmd, m->Data, (unsigned)m->msg.mn_Length, dbg);
    }
    if (gone && x->postdrop++ < 6)      /* what a door does after the drop: for the log */
        bbs_log(BBS_SYSLOG, "node %d: door %s after the drop: command %ld data %ld", N.node,
                io->d->tag, cmd, m->Data);

    switch (cmd) {
    case JH_REGISTER:
        x->registered++;
        x->ever_registered = TRUE;
        x->chain_pending = FALSE;
        m->Command = N.rows;            /* /X 5: the user's line length */
        break;
    case CHAIN:                         /* the door hands over to another: it registers next */
        x->registered--;
        x->chain_pending = TRUE;
        break;
    case JH_SHUTDOWN:
        if (--x->registered <= 0) x->rawarrow = FALSE;
        break;
    case JH_WRITE:
        if (!gone) xim_write(io, m->String, FALSE);
        else if (io->feeding) hup_capture(io, (const UBYTE *)m->String, strlen(m->String));
        break;
    case JH_SM:
    case JH_SO:
    case JH_MCI:
        if (!gone) xim_write(io, m->String, m->Data != 0);
        else if (io->feeding) hup_capture(io, (const UBYTE *)m->String, strlen(m->String));
        break;
    case JH_SMPTR:
        if (m->msg.mn_Length >= sizeof(struct JHMessage) && m->strptr) {
            if (!gone) xim_write(io, m->strptr, m->Data != 0);
            else if (io->feeding) hup_capture(io, (const UBYTE *)m->strptr, strlen(m->strptr));
        }
        break;
    case JH_CO:                         /* console only: nothing to show */
        break;
    case JH_PM:
    case JH_LI:
    case JH_HK:
    case JH_ExtHK:
    case JH_20:
    case QUICK_KEY:
        x->waiting = m;
        x->len = 0;
        x->wait_since = bbs_now();
        x->belled = FALSE;
        if (cmd == JH_HK) {
            x->wait_kind = 1;
            if (!gone) xim_write(io, m->String, FALSE);
            else if (io->feeding) hup_capture(io, (const UBYTE *)m->String, strlen(m->String));
        }
        else if (cmd == JH_ExtHK) x->wait_kind = 3;
        else if (cmd == JH_20 || cmd == QUICK_KEY) x->wait_kind = 4;
        else {
            x->wait_kind = 2;
            x->maxlen = m->Data > 0 && m->Data < 200 ? m->Data : 199;
            if (gone) ;
            else if (cmd == JH_PM) xim_write(io, m->String, FALSE);
            else {                      /* LI: String is an editable default */
                str_copy((char *)x->line, m->String, sizeof(x->line));
                x->len = strlen((char *)x->line);
                if (x->len > x->maxlen) x->len = x->maxlen;
                door_write(io, x->line, x->len);
            }
        }
        if (gone && !io->feeding) { xim_fail(x, XR_NOCARRIER); return TRUE; }
        xim_service_input(io, x);
        return x->waiting != m;
    case JH_FetchKey:
        if (gone && !io->feeding) { m->Data = -1; m->Command = XR_NOCARRIER; break; }
        {
            LONG c;
            do c = xim_key(io, x); while (c == -2);
            m->Command = c >= 0 ? c : 0;
            m->Data = 1;
        }
        break;
    case GETKEY:
        xs_copy(m, (!gone && in_avail()) ? "1" : "0");
        break;
    case JH_SG:
    case JH_SF:
        if (!gone) {
            str_copy(s, m->String, sizeof(s));
            if (!tshowpath(s, io->d->srccs) && cmd == JH_SG) {
                char p[PATHLEN];
                static const char *ext[] = { ".txt", ".ans", ".asc", NULL };
                int i;
                for (i = 0; ext[i]; i++) {
                    sprintf(p, "%s%s", s, ext[i]);
                    if (tshowpath(p, io->d->srccs)) break;
                }
            }
        }
        break;
    case JH_EF:
        m->Data = -1;                   /* the /X editor isn't emulated */
        break;
    case JH_BBSNAME:  xs_copy(m, cfg_str(N.cfg, "bbs_name", "NilBBS")); break;
    case JH_SYSOP:    xs_copy(m, cfg_str(N.cfg, "sysop_name", "Sysop")); break;
    case JH_SIGBIT:   m->Data = x->extsig >= 0 ? x->extsig : 0; break;
    case RAWARROW:    x->rawarrow = !x->rawarrow; break;    /* /X toggles it */
    case RETURNCOMMAND: str_copy(x->retcmd, m->String, sizeof(x->retcmd)); break;
    case ENVSTAT:
        if (m->Data) { sprintf(s, "%ld", x->envstat); xs_copy(m, s); }
        else x->envstat = atol(m->String);
        break;
    case SV_NEWMSG:                     /* the door's own words for WHO */
        sprintf(s, "Door: %.40s", m->String);
        str_nopipe(s);
        shared_lock(N.S);
        str_copy(N.ni->activity, s, LONGNAME);
        shared_unlock(N.S);
        break;
    case NODE_BAUD: case NODE_BAUDRATE: xs_copy(m, "38400"); break;
    case NODE_DEVICE: xs_copy(m, "telnet.device"); break;   /* a network caller: no serial port */
    case NODE_UNIT:   sprintf(s, "%d", N.node - 1); xs_copy(m, s); break;
    case BB_DROPDTR:
        bbs_log(BBS_SYSLOG, "node %d: door %s dropped the line", N.node, io->d->tag);
        if (!gone) { tn_flush(); node_hangup("door dropped the line"); }
        break;
    case GET_XIMPORT: m->Data = 2; break;               /* serial, i.e. a remote caller */
    case TELNET_USERNAME_PROMPT: str_copy(g_netlogin.uprompt, m->String, sizeof(g_netlogin.uprompt)); break;
    case TELNET_USERNAME:        str_copy(g_netlogin.user, m->String, sizeof(g_netlogin.user)); break;
    case TELNET_PASSWORD_PROMPT: str_copy(g_netlogin.pprompt, m->String, sizeof(g_netlogin.pprompt)); break;
    case TELNET_PASSWORD:        str_copy(g_netlogin.pass, m->String, sizeof(g_netlogin.pass)); break;
    case TELNET_CONNECT:
        if (!gone && m->String[0] && m->Data > 0 && m->Data < 65536) {
            struct Door *nd = AllocVec(sizeof(struct Door), MEMF_CLEAR);
            if (nd) {
                *nd = *io->d;
                str_copy(nd->type, "telnet", sizeof(nd->type));
                str_copy(nd->host, m->String, sizeof(nd->host));
                nd->port = (UWORD)m->Data;
                sprintf(nd->name, "%.40s:%ld", m->String, m->Data);
                bbs_log(BBS_SYSLOG, "node %d: door %s connects the caller to %s", N.node, io->d->tag, nd->name);
                g_netlogin.active = TRUE;
                g_netlogin.user_sent = g_netlogin.pass_sent = FALSE;
                g_netlogin.nseen = 0;
                run_net(nd);
                g_netlogin.active = FALSE;
                FreeVec(nd);
            }
        }
        memset(&g_netlogin, 0, sizeof(g_netlogin));
        break;
    case DT_QUERYBIT: m->Command = ae_query_bit(m->Data); break;
    case DT_ADDBIT: case DT_REMBIT: case SETMCIOFF: case INTERPRET_MCI: break;
    case CONF_ACCESS: m->Data = (m->Data == 0) ? 1 : 2; break;   /* one conference: 0 */
    case MULTICOM:    m->Semi = 0; break;               /* no /X master node here */
    case LOAD_ACCOUNT: case EXT_LOAD_ACCOUNT: case SEARCH_ACCOUNT:
        /* /X copies its own binary user record into the door's buffers; there
         * is none to give: say "not found" instead of echoing success */
        m->Data = 0; m->NodeID = 0;
        if (x->nunknown < 8) {
            x->unknown_cmds[x->nunknown++] = cmd;
            bbs_log(BBS_SYSLOG, "node %d: door %s asked for a /X user record (%ld) - none given",
                    N.node, io->d->tag, cmd);
        }
        break;
    case SAVE_ACCOUNT: break;
    case DISPLAY_FILE: case CHECK_TO_DISPLAY:
        if (!gone) {
            str_copy(s, m->String, sizeof(s));
            show_paged(s, io->d->srccs);
        }
        break;
    case JH_FLAGFILE: case JH_SHOWFLAGS:
    case BB_NONSTOPTEXT: case DT_ANSICOLOR: case BB_CALLERSLOG: case BB_UDLOG:
        if (cmd == BB_CALLERSLOG || cmd == BB_UDLOG)
            bbs_log(BBS_CALLERLOG, "node %d  [%s] %s", N.node, io->d->tag, m->String);
        break;
    case ACTIVE_NODES: {
        int i;
        memset(s, ' ', 32);
        s[32] = 0;
        shared_lock(N.S);
        for (i = 0; i < N.S->nodes && i < 32; i++) if (N.S->node[i].state >= NS_ONLINE) s[i] = 'X';
        shared_unlock(N.S);
        xs_copy(m, s);
        break;
    }
    case DT_DUMP:     break;
    case BB_CONFNAME: if (m->Data) { extern const char *msg_area_name(void); xs_copy(m, msg_area_name()); } break;
    case BB_CONFLOCAL:
    case BB_LOCAL:    if (m->Data || cmd == BB_LOCAL) xs_copy(m, "BBS:"); break;
    case BB_CONFNUM:  xs_copy(m, "0"); break;
    case BB_MAINLINE: xs_copy(m, x->mainline); break;
    case BB_NODEID:   sprintf(s, "%d", N.node - 1); xs_copy(m, s); break;
    case EXPRESS_VERSION: xs_copy(m, "v5.6"); break;    /* /X 5.6's getExpressMajorVer */
    case BB_CHATFLAG: xs_copy(m, "OFF"); break;
    case DT_CURR_TIME: sprintf(s, "%lu", bbs_now() + AE_EPOCH); xs_copy(m, s); break;
    case DT_STAMP_LASTON: ae_ctime(N.user.lastcall, s); xs_copy(m, s); break;
    case DT_STAMP_CTIME:  ae_ctime(bbs_now(), s); xs_copy(m, s); break;
    case DT_CONFACCESS: if (m->Data) xs_copy(m, "XXXXXXXXX"); break;
    case BB_GETTASK:  m->task = (struct Process *)FindTask(NULL); break;
    case BB_LOGONTYPE: m->Data = 3; break;          /* LOGON_TYPE_REMOTE (2 is local) */
    case BB_LINECOUNT: if (m->Data) xs_copy(m, "0"); break;
    case DT_LANGUAGE: if (m->Data) xs_copy(m, "txt"); break;
    case MOD_TYPE:    m->Data = 0; break;
    case DT_ISANSI:   m->Data = N.term == TT_ASCII ? 0 : 1; break;
    case ZMODEMSEND: {
        struct ZStats st;
        const char *pp[1], *np[1];
        char path[204], *base;
        str_copy(path, m->String, sizeof(path));
        base = FilePart((STRPTR)path);
        pp[0] = path; np[0] = base;
        if (gone) { m->Data = -2; break; }
        m->Data = zm_send(pp, np, 1, &st) > 0 ? 1 : 0;
        if (!N.online) m->Data = -2;
        break;
    }
    case ZMODEMRECEIVE: {
        struct ZStats st;
        char names[1][32];
        if (gone) { m->Data = -2; break; }
        m->Data = zm_receive(m->String[0] ? m->String : "BBS:Files/Uploads", names, 1, &st) > 0 ? 1 : 0;
        if (!N.online) m->Data = -2;
        break;
    }
    default:
        if (cmd >= DT_NAME && cmd <= DT_TIMEOUT) { xim_dt(x, m, cmd); break; }
        {
            int i;
            BOOL seen = FALSE;
            for (i = 0; i < x->nunknown; i++) if (x->unknown_cmds[i] == cmd) seen = TRUE;
            if (!seen && x->nunknown < 8) {
                x->unknown_cmds[x->nunknown++] = cmd;
                bbs_log(BBS_SYSLOG, "node %d: XIM command %ld not emulated (door %s)",
                        N.node, cmd, io->d->tag);
            }
        }
        break;
    }
    ReplyMsg((struct Message *)m);
    return TRUE;
}

static LONG xim_timeout(struct XimState *x) { return x->timeout; }

/* an AIM door's /X state: the data table's timeout, ENVSTAT, no signal */
static struct XimState *aim_state(void)
{
    struct XimState *x = AllocVec(sizeof(struct XimState), MEMF_CLEAR);
    if (!x) return NULL;
    x->timeout = 300;
    x->envstat = 3;
    x->extsig = -1;
    return x;
}

/* RETURNCOMMAND after an /X or AIM door: G logs the caller off */
static void xim_retcmd(struct XimState *x, struct Door *d)
{
    if (!x->retcmd[0] || !N.online) return;
    if ((x->retcmd[0] == 'G' || x->retcmd[0] == 'g') && (x->retcmd[1] == 0 || x->retcmd[1] == ' ')) {
        bbs_log(BBS_SYSLOG, "node %d: door %s logs the caller off (RETURNCOMMAND G)", N.node, d->tag);
        do_logoff();
        N.loggedin = FALSE;             /* do_logoff has counted the minutes: not again */
        node_hangup("door: goodbye");
    } else bbs_log(BBS_SYSLOG, "node %d: door %s RETURNCOMMAND \"%s\" ignored", N.node, d->tag, x->retcmd);
}

/* ---- type = aim: an AmiExpress ARexx door (TYPE=AIM, run by /X's REXXDOOR).
 * The script is started as "<script> <node-1>" and talks to AERexxControl<node-1>
 * (AmiExpress's AIM chapter: "parse arg NODE / address value 'AERexxControl'NODE"):
 *   GETUSER n        /X function n, read (DT_NAME 100, JH_BBSNAME 11 ...) -> RESULT
 *   PUTUSTR text     the value for...
 *   PUTUSER n        ...function n, written (136 RETURNCOMMAND, 116 DT_TIMETOTAL ...)
 *   TRANSMIT text    text and a new line;  SENDMESSAGE / SENDSTRING text: no new line
 *   GETCHAR          one key (as typed) -> RESULT;  PROMPT / QUERY text: a line -> RESULT
 *   SHOWFILE / SHOWGFILE file, FLAGFILE, SHUTDOWN
 * n goes through the /X door server's own table (xim_command).  A lost carrier or
 * the /X input timeout answers rc 10, as REXXDOOR does. */
static void aim_command(struct DoorIO *io, struct RexxHost *h, struct RexxMsg *m,
                        const char *word, const UBYTE *arg, LONG arglen)
{
    char a2[256];
    LONG n;
    str_copy(a2, (const char *)arg, arglen + 1 < (LONG)sizeof(a2) ? arglen + 1 : (LONG)sizeof(a2));
    n = strlen(a2);                             /* PROMPT "text": the quotes are the script's */
    if (n >= 2 && a2[0] == '"' && a2[n - 1] == '"') { memmove(a2, a2 + 1, n - 2); a2[n - 2] = 0; }
    if (!str_icmp(word, "GETUSER") || !str_icmp(word, "PUTUSER")) {
        struct JHMessage jm;
        char out[210];
        memset(&jm, 0, sizeof(jm));
        jm.Command = atol(a2);
        if (!str_icmp(word, "GETUSER")) jm.Data = 1;
        else str_copy(jm.String, h->object, sizeof(jm.String));
        if (jm.Command == JH_PM || jm.Command == JH_LI || jm.Command == JH_HK || jm.Command == JH_ExtHK ||
            jm.Command == JH_20 || jm.Command == QUICK_KEY || jm.Command == TELNET_CONNECT) {
            rx_reply(m, 0, "", 0);              /* input goes through GETCHAR / PROMPT here */
            return;
        }
        xim_command(io, h->aim, &jm);           /* no reply port: ReplyMsg just frees it */
        if (jm.String[0]) str_copy(out, jm.String, sizeof(out));
        else sprintf(out, "%ld", jm.Data);
        rx_reply(m, 0, out, strlen(out));
    } else if (!str_icmp(word, "PUTUSTR")) {
        str_copy(h->object, a2, sizeof(h->object));
        rx_reply(m, 0, "", 0);
    } else if (!str_icmp(word, "TRANSMIT")) {
        door_write(io, (const UBYTE *)a2, strlen(a2));
        door_write(io, (const UBYTE *)"\r\n", 2);
        rx_reply(m, 0, "", 0);
    } else if (!str_icmp(word, "SENDMESSAGE") || !str_icmp(word, "SENDSTRING")) {
        door_write(io, (const UBYTE *)a2, strlen(a2));
        rx_reply(m, 0, "", 0);
    } else if (!str_icmp(word, "GETCHAR")) {
        h->waiting = m; h->wait_kind = 1; h->wait_since = bbs_now();
        rx_service_input(io, h);
    } else if (!str_icmp(word, "PROMPT") || !str_icmp(word, "QUERY")) {
        door_write(io, (const UBYTE *)a2, strlen(a2));
        h->waiting = m; h->wait_kind = 2; h->qlen = 0; h->qmax = 200; h->qhide = FALSE;
        h->wait_since = bbs_now();
        rx_service_input(io, h);
    } else if (!str_icmp(word, "SHOWFILE") || !str_icmp(word, "SHOWGFILE")) {
        if (!show_paged(a2, io->d->srccs) && !str_icmp(word, "SHOWGFILE")) {
            char p2[PATHLEN];
            sprintf(p2, "%.200s.txt", a2);
            show_paged(p2, io->d->srccs);
        }
        rx_reply(m, 0, "", 0);
    } else if (!str_icmp(word, "BUFFERFLUSH")) {        /* throw away typed-ahead keys */
        while (door_inbyte(io) >= 0)
            ;
        rx_reply(m, 0, "", 0);
    } else if (!str_icmp(word, "FLAGFILE") || !str_icmp(word, "SHUTDOWN")) {
        rx_reply(m, 0, "", 0);
    } else if (word[0] >= '0' && word[0] <= '9') {
        rx_reply(m, 0, "", 0);                  /* a bare function call's value */
    } else {
        int i;
        BOOL seen = FALSE;
        for (i = 0; i < h->nunknown; i++) if (!str_icmp(h->unknown[i], word)) seen = TRUE;
        if (!seen) {
            if (h->nunknown < 8) str_copy(h->unknown[h->nunknown++], word, 16);
            bbs_log(BBS_SYSLOG, "node %d: AIM command %s not emulated (door %s)", N.node, word, io->d->tag);
        }
        rx_reply(m, 0, "", 0);
    }
}

static void run_xim(struct Door *d)
{
    struct DoorIO io;
    struct XimState *x;
    char cmd[PATHLEN * 2], drop[PATHLEN];
    ULONG sigs, hung_at = 0, broke_at = 0, chain_at = 0;
    BYTE sb;
    struct Process *lp;

    memset(&io, 0, sizeof(io));
    io.d = d;
    write_dropfiles(d, drop);
    if (!(x = AllocVec(sizeof(struct XimState), MEMF_CLEAR))) return;
    x->timeout = 300;                   /* /X INPUT_TIMEOUT; DT_TIMEOUT changes it */
    x->envstat = 3;                     /* ENV_DOORS */

    sprintf(x->name, "AEDoorPort%d", N.node - 1);
    Forbid();
    if (FindPort((STRPTR)x->name)) { Permit(); FreeVec(x); tputs(L("door.run_xim.door_port_busy", "|12Door port busy.|07\n")); return; }
    if ((x->port = CreateMsgPort())) {
        x->port->mp_Node.ln_Name = x->name;
        x->port->mp_Node.ln_Pri = 0;
        AddPort(x->port);
    }
    Permit();
    if (!x->port) { FreeVec(x); return; }
    x->extsig = AllocSignal(-1);        /* JH_SIGBIT (-1 = none left: 0 is reported) */

    /* the door is started as "<command> <node>"; AEDoor.library finds
     * AEDoorPort<node> from those digits */
    expand(d->command, cmd, sizeof(cmd), drop);
    str_copy(x->mainline, cmd, sizeof(x->mainline));
    if ((sb = AllocSignal(-1)) < 0) goto out_port;
    memset(&LN, 0, sizeof(LN));
    if (d->dir[0]) expand(d->dir, LN.dir, sizeof(LN.dir), drop);
    sprintf(LN.cmd, "%s %d", cmd, N.node - 1);
    LN.sync = TRUE;
    LN.stack = d->stack;
    LN.parent = FindTask(NULL);
    LN.donesig = 1UL << sb;
    if (!(lp = CreateNewProcTags(NP_Entry, (ULONG)launcher, NP_Name, (ULONG)"NilBBS door launcher",
                                 NP_StackSize, 8192, NP_Priority, 0, TAG_END))) {
        FreeSignal(sb);
        goto out_port;
    }
    N.door_csi8 = d->amigacsi;
    sigs = (1UL << x->port->mp_SigBit) | LN.donesig;
    if (x->extsig >= 0) sigs |= 1UL << x->extsig;
    N.wait_new = 1;

    for (;;) {
        struct JHMessage *m;
        if (N.online) {
            ULONG got = 0;
            tn_wait(1000, sigs, &got);
            if (x->extsig >= 0 && (got & (1UL << x->extsig))) x->extsig_seen = TRUE;
            if (N.msg_waiting) N.msg_waiting = FALSE;
            if (N.loggedin && time_left_mins() == 0) {
                door_write(&io, (const UBYTE *)time_up_text(), strlen(time_up_text()));
                tn_flush();
                node_hangup("time limit in door");
            }
        } else Delay(5);

        while ((m = (struct JHMessage *)GetMsg(x->port))) xim_command(&io, x, m);

        if (!N.online && !io.hungup && !io.feeding && !io.fed && (d->hupkeys[0] || d->hupon[0][0])) {
            /* a door that ignores the lost carrier (DreamSweeper asks for a key
             * forever): answer its key requests from hangup_keys, like the CLI
             * doors (feed_byte) */
            io.feeding = TRUE;
            io.fpos = io.frounds = 0; io.fpause = 0;
            io.fexp[0] = 0; io.hlast = 0;
            hung_at = bbs_now();
            bbs_log(BBS_SYSLOG, "node %d: caller dropped in XIM door %s - typing its hang-up keys", N.node, d->tag);
        }
        if (io.feeding && (io.fed || bbs_now() - hung_at > (ULONG)d->hupgrace)) {
            io.feeding = FALSE;
            io.fed = TRUE;
            bbs_log(BBS_SYSLOG, "node %d: door %s is still running after its hang-up keys", N.node, d->tag);
        }
        if (!N.online && !io.hungup && !io.feeding) {
            io.hungup = TRUE;
            hung_at = bbs_now();
            xim_fail(x, XR_NOCARRIER);
            bbs_log(BBS_SYSLOG, "node %d: caller dropped in XIM door %s", N.node, d->tag);
        }
        if (io.hungup) {
            if (node_reset_wanted()) node_release_slot();   /* sysop RESET: free the node now */
            if (bbs_now() - hung_at >= (ULONG)d->panicgrace && x->door &&
                (!broke_at || bbs_now() - broke_at >= 30)) {
                Signal(x->door, SIGBREAKF_CTRL_C);
                broke_at = bbs_now();
            }
        } else {
            xim_service_input(&io, x);
            if (io.feeding) ;               /* no timeouts while the hang-up keys are typed */
            else
            /* /X's input timeout (DT_TIMEOUT, 300 s): a bell a minute before, then
             * the request fails - doors that wait on a key get to give up */
            if (x->waiting && x->timeout > 0) {
                ULONG idle = bbs_now() - x->wait_since;
                if (!x->belled && x->timeout > 60 && idle >= (ULONG)(x->timeout - 60)) {
                    door_write(&io, (const UBYTE *)"\a", 1);
                    x->belled = TRUE;
                }
                if (idle >= (ULONG)x->timeout) {
                    bbs_log(BBS_SYSLOG, "node %d: door %s input timed out", N.node, d->tag);
                    xim_fail(x, XR_TIMEOUT);
                }
            }
        }

        /* finished: the door unregistered, or its process is gone.  After a
         * CHAIN the next door registers on this port, perhaps after the first
         * one's process has ended: wait up to 15 s for it, then serve it until
         * it shuts down (or, after a drop, until the grace runs out) */
        if (LN.done) {
            if (!chain_at && x->chain_pending) chain_at = bbs_now();
            if (x->chain_pending) { if (bbs_now() - chain_at < 15) continue; break; }
            if (x->registered <= 0 || !x->ever_registered) break;
            if (io.hungup && bbs_now() - hung_at >= (ULONG)d->panicgrace + 60) break;
            continue;
        }
        if (x->ever_registered && x->registered <= 0 && !x->waiting && !x->chain_pending) {
            /* give the launcher a moment to see the process end */
            int i;
            for (i = 0; i < 100 && !LN.done; i++) {
                Delay(5);
                while ((m = (struct JHMessage *)GetMsg(x->port))) xim_command(&io, x, m);
            }
            break;
        }
    }
    /* the launcher must be gone before this code can go away */
    while (!LN.done) {
        struct JHMessage *m;
        Delay(5);
        if (node_reset_wanted()) node_release_slot();
        while ((m = (struct JHMessage *)GetMsg(x->port))) {
            m->Data = -1;
            ReplyMsg((struct Message *)m);
        }
        if (!N.online && x->door && (!broke_at || bbs_now() - broke_at >= 30)) {
            Signal(x->door, SIGBREAKF_CTRL_C);
            broke_at = bbs_now();
        }
    }
    FreeSignal(sb);
    N.wait_new = 0;
    N.door_csi8 = 0;
    if (LN.rc == -1 && N.online) tputs(L("door.run_xim.the_door_could", "|12The door could not be started.|07\n"));
    /* RETURNCOMMAND: /X runs it as a menu command after the door */
    xim_retcmd(x, d);
out_port:
    if (x->extsig >= 0) FreeSignal(x->extsig);
    Forbid();
    RemPort(x->port);
    {
        struct Message *m;
        while ((m = GetMsg(x->port))) ReplyMsg(m);
    }
    Permit();
    DeleteMsgPort(x->port);
    FreeVec(x);
    tcolor(7);
}

/* ---- network doors ------------------------------------------------------------------ */

static LONG tcp_connect(const char *host, UWORD port)
{
    volatile UBYTE sa[16];
    struct hostent *he;
    ULONG ip = 0;
    LONG s, one = 1, zero = 0, i;

    if (!ip_parse(host, &ip)) {
        if (!(he = gethostbyname((STRPTR)host)) || !he->h_addr_list[0]) return -1;
        ip = *(ULONG *)he->h_addr_list[0];
    }
    if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0) return -1;
    for (i = 0; i < 16; i++) sa[i] = 0;
    sa[1] = AF_INET;
    sa[2] = (UBYTE)(port >> 8); sa[3] = (UBYTE)port;
    sa[4] = (UBYTE)(ip >> 24); sa[5] = (UBYTE)(ip >> 16);
    sa[6] = (UBYTE)(ip >> 8);  sa[7] = (UBYTE)ip;

    IoctlSocket(s, FIONBIO, (char *)&one);
    if (connect(s, (struct sockaddr *)sa, 16) < 0 && Errno() != EINPROGRESS) {
        CloseSocket(s);
        return -1;
    }
    for (i = 0; i < 20; i++) {                  /* up to 20 seconds */
        fd_set w;
        struct timeval tv;
        ULONG sigs = SIGBREAKF_CTRL_C;
        LONG err = 0;
        socklen_t el = sizeof(err);
        FD_ZERO(&w); FD_SET(s, &w);
        tv.tv_secs = 1; tv.tv_micro = 0;
        if (WaitSelect(s + 1, NULL, &w, NULL, &tv, &sigs) > 0) {
            getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el);
            if (err) break;
            IoctlSocket(s, FIONBIO, (char *)&zero);
            return s;
        }
        if (sigs & SIGBREAKF_CTRL_C) break;
        if (i % 3 == 2) { tputs("."); tn_flush(); }
    }
    CloseSocket(s);
    return -1;
}

static BOOL send_all(LONG s, const UBYTE *buf, LONG len)
{
    while (len > 0) {
        LONG n = send(s, (APTR)buf, len, 0);
        if (n <= 0) return FALSE;
        buf += n; len -= n;
    }
    return TRUE;
}

/* TELNET_CONNECT's auto-login: the last 199 characters the server sent (no
 * escape codes) are searched for the username prompt, then the password
 * prompt; each answer is sent once, with a CR */
static void net_autologin(LONG s, const UBYTE *buf, LONG n)
{
    LONG i;
    for (i = 0; i < n; i++) {
        if (buf[i] < 32 || buf[i] == 127) continue;
        if (g_netlogin.nseen >= (int)sizeof(g_netlogin.seen) - 1) {
            memmove(g_netlogin.seen, g_netlogin.seen + 50, g_netlogin.nseen - 50);
            g_netlogin.nseen -= 50;
        }
        g_netlogin.seen[g_netlogin.nseen++] = (char)buf[i];
    }
    g_netlogin.seen[g_netlogin.nseen] = 0;
    if (!g_netlogin.user_sent && g_netlogin.user[0] && g_netlogin.uprompt[0] &&
        str_istr(g_netlogin.seen, g_netlogin.uprompt)) {
        send_all(s, (const UBYTE *)g_netlogin.user, strlen(g_netlogin.user));
        send_all(s, (const UBYTE *)"\r\0", 2);
        g_netlogin.user_sent = TRUE;
        g_netlogin.nseen = 0;
    } else if (!g_netlogin.pass_sent && g_netlogin.pass[0] && g_netlogin.pprompt[0] &&
               str_istr(g_netlogin.seen, g_netlogin.pprompt)) {
        send_all(s, (const UBYTE *)g_netlogin.pass, strlen(g_netlogin.pass));
        send_all(s, (const UBYTE *)"\r\0", 2);
        g_netlogin.pass_sent = TRUE;
        g_netlogin.nseen = 0;
    }
}

static void run_net(struct Door *d)
{
    BOOL telnet = !str_icmp(d->type, "telnet");
    BOOL rlogin = !str_icmp(d->type, "rlogin");
    UBYTE buf[1024], tstate = 0, tverb = 0;
    LONG s;

    tprintf(L("door.run_net.connecting_to", "|07Connecting to |15%s|07"), d->name);
    tn_flush();
    if ((s = tcp_connect(d->host, d->port)) < 0) {
        tprintf(L("door.run_net.couldnt_reach_try", "\n|12Couldn't reach %s:%u - try again later.|07\n"), d->host, (unsigned)d->port);
        bbs_log(BBS_SYSLOG, "node %d: door %s: connect to %s:%u failed", N.node, d->tag,
                d->host, (unsigned)d->port);
        return;
    }
    tputs(L("door.run_net.connected", " |10connected.|07\n"));
    tn_flush();

    if (rlogin) {
        char u[NAMELEN * 2], t[40];
        LONG n = 0;
        expand(d->ruser, u, sizeof(u), "");
        expand(d->rterm, t, sizeof(t), "");
        buf[n++] = 0;
        strcpy((char *)buf + n, u); n += strlen(u) + 1;       /* client user */
        strcpy((char *)buf + n, u); n += strlen(u) + 1;       /* server user */
        strcpy((char *)buf + n, t); n += strlen(t) + 1;       /* term/speed  */
        send_all(s, buf, n);
    }

    while (N.online) {
        fd_set r;
        struct timeval tv;
        ULONG sigs = SIGBREAKF_CTRL_C;
        LONG n, maxfd = (s > N.sock ? s : N.sock) + 1;     /* N.sock = -1: a local or serial caller */

        /* caller -> door server */
        if (in_avail()) {
            LONG c, k = 0;
            while (k < (LONG)sizeof(buf) - 2 && (c = in_get()) >= 0) {
                buf[k++] = (UBYTE)c;
                if (telnet && c == 255) buf[k++] = 255;
                if (c == '\r' && telnet) buf[k++] = 0;
            }
            if (!send_all(s, buf, k)) break;
        }
        tn_flush();

        FD_ZERO(&r);
        FD_SET(s, &r);
        if (N.sock >= 0) FD_SET(N.sock, &r);
        tv.tv_secs = N.sock >= 0 ? 1 : 0;
        tv.tv_micro = N.sock >= 0 ? 0 : 100000;    /* no socket to wake us: look at the caller 10x a second */
        if (WaitSelect(maxfd, &r, NULL, NULL, &tv, &sigs) < 0 && !(sigs & SIGBREAKF_CTRL_C)) continue;
        node_heartbeat();
        if (sigs & SIGBREAKF_CTRL_C) { node_hangup("disconnected by sysop"); break; }

        if (N.sock < 0 || FD_ISSET(N.sock, &r)) {
            tn_wait(0, 0, NULL);                /* pull the caller's bytes in */
            if (!N.online) break;
        }
        if (FD_ISSET(s, &r)) {
            n = recv(s, buf, sizeof(buf), 0);
            if (n <= 0) break;                  /* the server hung up: back to the BBS */
            if (telnet) {
                /* minimal client-side telnet: refuse every option, keep data */
                LONG i, o = 0;
                for (i = 0; i < n; i++) {
                    UBYTE c = buf[i];
                    switch (tstate) {
                    case 0:
                        if (c == 255) tstate = 1; else buf[o++] = c;
                        break;
                    case 1:
                        if (c == 255) { buf[o++] = 255; tstate = 0; }
                        else if (c >= 251 && c <= 254) { tverb = c; tstate = 2; }
                        else if (c == 250) tstate = 3;
                        else tstate = 0;
                        break;
                    case 2: {
                        UBYTE rep[3];
                        rep[0] = 255;
                        /* accept the server's ECHO and SGA, refuse the rest */
                        if (tverb == 251) rep[1] = (c == 1 || c == 3) ? 253 : 254;
                        else if (tverb == 253) rep[1] = 252;
                        else rep[1] = 0;
                        rep[2] = c;
                        if (rep[1]) send_all(s, rep, 3);
                        tstate = 0;
                        break;
                    }
                    case 3:
                        if (c == 255) tstate = 4;
                        break;
                    case 4:
                        tstate = (c == 240) ? 0 : 3;
                        break;
                    }
                }
                n = o;
            }
            if (d->rawout) tn_raw(buf, n);
            else tputraw(buf, n, d->srccs);
            if (g_netlogin.active) net_autologin(s, buf, n);
        }
    }
    CloseSocket(s);
    tcolor(7);
}

/* ---- entry points ---------------------------------------------------------------- */

/* "PFILES: BBS:Doors/CNet" - create the assign if it isn't there yet
 * (CNet doors hard-code paths like pfiles:Realm/...) */
static void make_assign(const char *spec)
{
    char name[40], *sp;
    const char *path;
    BPTR l;
    str_copy(name, spec, sizeof(name));
    if (!(sp = strchr(name, ' ')) && !(sp = strchr(name, '='))) return;
    *sp = 0;
    path = str_trim((char *)spec + (sp - name) + 1);
    if ((sp = strchr(name, ':'))) *sp = 0;
    {
        char dev[44];
        sprintf(dev, "%s:", name);
        if ((l = Lock((STRPTR)dev, ACCESS_READ))) { UnLock(l); return; }   /* already there */
    }
    if ((l = Lock((STRPTR)path, ACCESS_READ)) && !AssignLock((STRPTR)name, l)) UnLock(l);
}

/* CNet doors keep per-caller files in MAIL:users/<GETUSER 39>/ (CNet's own mail tree):
 * when MAIL: isn't assigned, point it at BBS:Mail/CNet, and make the caller's folder.
 * Looked up in the DOS list, not with Lock(): an unknown MAIL: would put up an
 * "insert volume" requester on the BBS screen. */
static void cnet_mail_dir(void)
{
    struct DosList *dl;
    BOOL have;
    char p[64];
    BPTR l;
    dl = LockDosList(LDF_ASSIGNS | LDF_VOLUMES | LDF_DEVICES | LDF_READ);
    have = FindDosEntry(dl, (STRPTR)"MAIL", LDF_ASSIGNS | LDF_VOLUMES | LDF_DEVICES) != NULL;
    UnLockDosList(LDF_ASSIGNS | LDF_VOLUMES | LDF_DEVICES | LDF_READ);
    if (!have) {
        if ((l = CreateDir((STRPTR)"BBS:Mail"))) UnLock(l);
        if ((l = CreateDir((STRPTR)"BBS:Mail/CNet"))) UnLock(l);
        if ((l = Lock((STRPTR)"BBS:Mail/CNet", ACCESS_READ)) && !AssignLock((STRPTR)"MAIL", l)) UnLock(l);
    }
    if ((l = CreateDir((STRPTR)"MAIL:users"))) UnLock(l);
    sprintf(p, "MAIL:users/%lu", N.user.id);
    if ((l = CreateDir((STRPTR)p))) UnLock(l);
}

static BOOL door_busy_elsewhere(const char *name)
{
    char act[LONGNAME];
    int n;
    BOOL busy = FALSE;
    sprintf(act, "Door: %.40s", name);
    shared_lock(N.S);
    for (n = 0; n < N.S->nodes; n++)
        if (n + 1 != N.node && N.S->node[n].state == NS_DOOR &&
            !strcmp(N.S->node[n].activity, act)) busy = TRUE;
    shared_unlock(N.S);
    return busy;
}

void door_run(const char *tag)
{
    struct Cfg *dc = load_doors();
    struct Door *d = AllocVec(sizeof(struct Door), MEMF_CLEAR);
    char act[LONGNAME];
    ULONG start;

    if (!d) { cfg_free(dc); return; }
    if (!door_get(dc, tag, d)) {
        tprintf(L("door.run.theres_no_door", "|12There's no door called \"%s\".|07\n"), tag);
        goto done;
    }
    if (d->level > N.user.level || !acs_check(d->acs)) {
        tputs(L("door.run.you_dont_have", "|12You don't have access to that door.|07\n"));
        goto done;
    }
    if (d->single && door_busy_elsewhere(d->name)) {
        tputs(L("door.run.someone_else_is", "|14Someone else is in that door right now - try again shortly.|07\n"));
        goto done;
    }
    if (d->single && d->stalelock[0] && !strchr(d->stalelock, '/') && !strchr(d->stalelock, ':')) {
        char lp[PATHLEN];
        path_join(lp, d->dir, d->stalelock);
        if (file_exists(lp) && DeleteFile((STRPTR)lp))
            bbs_log(BBS_SYSLOG, "node %d: removed door %s's stale %s", N.node, d->tag, d->stalelock);
    }

    bbs_log(BBS_SYSLOG, "node %d: %s opened door %s", N.node, N.user.name, d->tag);
    sprintf(act, "Door: %.40s", d->name);
    shared_lock(N.S);
    N.ni->state = NS_DOOR;
    str_copy(N.ni->activity, act, LONGNAME);
    shared_unlock(N.S);
    user_save();                                /* the door may read our stats */

    start = bbs_now();
    if (d->assign[0]) make_assign(d->assign);
    if (!str_icmp(d->type, "cnetrexx") || !str_icmp(d->type, "arexx") || !str_icmp(d->type, "cnetc")) {
        /* CNet doors: the caller's MAIL:users/<n>/ folder, and the pfile's own title and
         * folder where CNet keeps them (PortData Select0: GETUSER 1311960 / 1311992) */
        char c2[PATHLEN * 2], *e;
        cnet_mail_dir();
        expand(d->command, c2, sizeof(c2), "");
        if ((e = strchr(c2, ' '))) *e = 0;
        *PathPart((STRPTR)c2) = 0;
        if (c2[0] && c2[strlen(c2) - 1] != ':' && c2[strlen(c2) - 1] != '/') strcat(c2, "/");
        cnet_set_pfile(d->name, c2);
    }
    if (!str_icmp(d->type, "cli")) run_cli(d);
    else if (!str_icmp(d->type, "cnetrexx") || !str_icmp(d->type, "arexx") || !str_icmp(d->type, "aim")) run_cnetrexx(d);
    else if (!str_icmp(d->type, "xim") || !str_icmp(d->type, "amiexpress")) run_xim(d);
    else if (!str_icmp(d->type, "cnetc")) {
        char c2[PATHLEN * 2], dd[PATHLEN];
        expand(d->command, c2, sizeof(c2), "");
        dd[0] = 0;
        if (d->dir[0]) expand(d->dir, dd, sizeof(dd), "");
        run_cnetc(d->tag, c2, dd[0] ? dd : NULL, d->stack, (UBYTE)d->cnetver, d->srccs, d->panicgrace);
    }
    else run_net(d);

    N.user.doors++;
    shared_lock(N.S);
    if (N.ni->state == NS_DOOR) N.ni->state = NS_ONLINE;
    str_copy(N.ni->activity, "Main menu", LONGNAME);
    shared_unlock(N.S);
    bbs_log(BBS_SYSLOG, "node %d: %s left door %s after %lu s", N.node, N.user.name, d->tag,
            bbs_now() - start);
    if (N.online) {
        tputs(L("door.run.returning_to", "\n|08Returning to |15|BN|08...|07\n"));
        tcheck_messages();
    }
done:
    FreeVec(d);
    cfg_free(dc);
}

/* the doors this caller may open, a screen at a time: two columns on an 80-column
 * terminal, pages as tall as the caller's screen (N / P to turn), and any door's
 * number works from any page.  (It stopped at 40 doors, in one column that ran
 * off a 24-line screen.) */
#define DOOR_LIST_MAX 250
void door_list(void)
{
    struct Cfg *dc = load_doors();
    LONG i, n = cfg_sections(dc), shown = 0, page = 0, per, rows, cols;
    char (*tags)[NAMELEN];
    char (*names)[LONGNAME];
    char buf[8];

    if (!(tags = AllocVec(DOOR_LIST_MAX * NAMELEN, MEMF_ANY)) ||
        !(names = AllocVec(DOOR_LIST_MAX * LONGNAME, MEMF_ANY))) {
        if (tags) FreeVec(tags);
        cfg_free(dc);
        return;
    }
    for (i = 0; i < n && shown < DOOR_LIST_MAX; i++) {
        struct Door d;
        const char *tag = cfg_section(dc, i);
        if (!door_get(dc, tag, &d) || d.level > N.user.level || !acs_check(d.acs)) continue;
        str_copy(tags[shown], tag, NAMELEN);
        str_copy(names[shown], d.name, LONGNAME);
        shown++;
    }
    cfg_free(dc);
    if (!shown) {
        tputs(L("door.list.no_doors_are", "  |08No doors are installed.|07\n"));
        goto out;
    }
    cols = N.cols >= 80 ? 2 : 1;
    rows = (N.rows > 12 ? N.rows : 24) - 7;             /* blanks, title, page + prompt lines: <= 24 */
    per = rows * cols;
    for (;;) {
        LONG first = page * per, r, c;
        tputs(L("door.list.doors", "\n|09-=[ |15Doors|09 ]=-|07\n\n"));
        for (r = 0; r < rows; r++) {
            BOOL any = FALSE;
            for (c = 0; c < cols; c++) {
                LONG k = first + c * rows + r;          /* down the first column, then the next */
                if (k >= shown) continue;
                any = TRUE;
                /* two columns stay under 80: a full 80-column line makes most terminals wrap,
                   which double-spaces the list and scrolls its first rows away */
                if (cols == 2 && c == 0) tprintf("  |08[|15%3ld|08] |07%-30.30s ", k + 1, names[k]);
                else if (cols == 2) tprintf(" |08[|15%3ld|08] |07%.32s", k + 1, names[k]);
                else tprintf("  |08[|15%3ld|08] |07%.*s", k + 1, (int)(N.cols > 12 ? N.cols - 10 : 30), names[k]);
            }
            if (!any) break;
            tputs("\n");
        }
        if (shown > per)
            tprintf(L("door.list.page_of", "\n|08Page %ld of %ld - |15N|08ext, |15P|08revious"),
                    page + 1, (shown + per - 1) / per);
        tputs(L("door.list.door_number_enter", "\n|07Door number |08(Enter to cancel)|07: |15"));
        if (tgetline(buf, 4, 0) <= 0) break;
        str_trim(buf);
        if ((buf[0] == 'n' || buf[0] == 'N') && shown > per) { if (first + per < shown) page++; else page = 0; continue; }
        if ((buf[0] == 'p' || buf[0] == 'P') && shown > per) { if (page > 0) page--; continue; }
        i = atol(buf);
        if (i >= 1 && i <= shown) door_run(tags[i - 1]);
        break;
    }
out:
    FreeVec(tags);
    FreeVec(names);
}
