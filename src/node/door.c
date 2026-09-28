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
    LONG cnetver;
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
    /* CNet 3.05 ends every TRANSMIT with a newline: a CNet capture shows
     * "Q.Quit" + newline + ">>" where the script sent no CRLF, and Realm builds
     * its screens from one TRANSMIT per line.  (Once set FALSE here by mistake.) */
    d->txnewline = cfg_sbool(dc, tag, "transmit_newline", TRUE);
    d->panicgrace = cfg_sint(dc, tag, "panic_grace", 10);
    d->cnetver = cfg_sint(dc, tag, "cnet_version", 4) >= 4 ? 40 : 30;
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
};

static ULONG ticks_now(void)
{
    struct DateStamp ds;
    DateStamp(&ds);
    return (ULONG)ds.ds_Days * 4320000UL + (ULONG)ds.ds_Minute * 3000UL + ds.ds_Tick;
}

/* one byte from the caller, converted to the door's charset; -1 = none */
static LONG door_inbyte(struct DoorIO *io)
{
    LONG c = in_get();
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

static void door_write(struct DoorIO *io, const UBYTE *buf, LONG len)
{
    if (io->hungup || !N.online) return;
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
    if (io->raw) return in_avail() > 0;
    return io->readylen > 0 || io->eof_pending;
}

/* try to satisfy the oldest queued read */
static BOOL serve_read(struct DoorIO *io, struct DosPacket *p)
{
    UBYTE *buf = (UBYTE *)p->dp_Arg2;
    LONG want = p->dp_Arg3, got = 0;

    if (io->hungup) { reply(p, 0, 0); return TRUE; }
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

static void flush_queues_eof(struct DoorIO *io)
{
    while (io->nread > 0) reply(io->readq[--io->nread], 0, 0);
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

        if (!N.online && !io.hungup) {
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

struct RexxHost {
    struct MsgPort *port;           /* public: the script's host */
    struct MsgPort *reply;          /* our launch message comes back here */
    char   name[24];
    struct RexxMsg *launch;
    struct RexxMsg *waiting;        /* GETCHAR / QUERY blocked on input */
    UBYTE  wait_kind;               /* 1 = GETCHAR, 2 = QUERY */
    UBYTE  qline[256];
    int    qlen;
    struct Task *script;
    char   unknown[8][16];
    int    nunknown;
};

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

static void getuser(int n, char *out)
{
    LONG t = time_left_mins();
    if (t < 0) t = 999;
    out[0] = 0;
    switch (n) {                    /* numbers from the CNet ARexx command list */
    case 1:  strcpy(out, N.user.name); break;
    case 2:  strcpy(out, "********"); break;                /* password: never */
    case 3:  strcpy(out, N.user.realname); break;
    case 4:  strcpy(out, N.user.location); break;
    case 7:  sprintf(out, "%ld", t); break;                 /* minutes left */
    case 11: bbs_datestr(N.user.lastcall, out); break;
    case 12: bbs_datetimestr(bbs_now(), out); break;
    case 15: sprintf(out, "%d", (int)(N.user.level * 24 / 256)); break;
    case 16: strcpy(out, N.sysop ? "SysOp" : "Member"); break;
    case 17: strcpy(out, N.sysop ? "1" : "0"); break;
    case 18: sprintf(out, "%lu", (bbs_now() - N.logon) / 60 + N.user.mins_today); break;
    case 19: strcpy(out, "12"); break;
    case 22: sprintf(out, "%lu", N.user.calls); break;
    case 23: sprintf(out, "%d", N.node - 1); break;         /* CNet ports count from 0 */
    case 24: strcpy(out, "11520"); break;
    case 27: sprintf(out, "%d", (int)N.cols); break;
    case 28: strcpy(out, N.term == TT_ASCII ? "0" : "2"); break;   /* 0 dumb, 2 ANSI */
    case 29: strcpy(out, "Z"); break;
    case 30: sprintf(out, "%lu", N.user.ulkb); break;
    case 31: sprintf(out, "%lu", N.user.uploads); break;
    case 32: sprintf(out, "%lu", N.user.dlkb); break;
    case 33: sprintf(out, "%lu", N.user.downloads); break;
    case 36: sprintf(out, "%lu", N.user.posts); break;
    case 40: sprintf(out, "%lu", N.user.id); break;         /* account number */
    case 41: sprintf(out, "%lu", N.user.id); break;         /* unique id */
    default: break;
    }
}
/* the caller typed something: finish a blocked GETCHAR / QUERY */
static void rx_service_input(struct DoorIO *io, struct RexxHost *h)
{
    LONG c;
    if (!h->waiting) return;
    if (h->wait_kind == 1) {
        if ((c = door_inbyte(io)) >= 0) {
            char s[2];
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
        } else if (c >= 32 && h->qlen < 200) {
            UBYTE b = (UBYTE)c;
            h->qline[h->qlen++] = b;
            tputraw(&b, 1, io->d->srccs);
        }
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
    if (io->hungup || !N.online) {
        /* CNet's documented answer after carrier loss: input commands return
         * "###PANIC" (scripts that check it save and quit); everything else
         * just succeeds quietly.  The CTRL-C that follows after a grace
         * period stops scripts that never look (the old CNet hang). */
        rx_reply(m, 0, "###PANIC", 8);
        return;
    }
    while (wl < len && wl < 15 && a[wl] != ' ') { word[wl] = a[wl]; wl++; }
    word[wl] = 0;
    arg = (const UBYTE *)a + wl + (wl < len ? 1 : 0);
    arglen = len - (arg - (const UBYTE *)a);
    if (arglen < 0) arglen = 0;

    if (!str_icmp(word, "TRANSMIT") || !str_icmp(word, "PRINT")) {
        if (!io->hungup && N.online) cnet_mci_write(arg, arglen, io->d->srccs);
        if (io->d->txnewline) door_write(io, (const UBYTE *)"\r\n", 2);
        rx_reply(m, 0, "", 0);
    } else if (!str_icmp(word, "SENDSTRING")) {
        if (!io->hungup && N.online) cnet_mci_write(arg, arglen, io->d->srccs);
        rx_reply(m, 0, "", 0);
    } else if (!str_icmp(word, "SEND")) {                /* SEND: no MCI, verbatim */
        door_write(io, arg, arglen);
        rx_reply(m, 0, "", 0);
    } else if (!str_icmp(word, "NEWLINE")) {
        door_write(io, (const UBYTE *)"\r\n", 2);
        rx_reply(m, 0, "", 0);
    } else if (!str_icmp(word, "SENDFILE")) {
        char p[PATHLEN];
        str_copy(p, (const char *)arg, arglen + 1 < PATHLEN ? arglen + 1 : PATHLEN);
        tshowpath(str_trim(p), io->d->srccs);
        rx_reply(m, 0, "", 0);
    } else if (!str_icmp(word, "GETCHAR") || !str_icmp(word, "GETKEY")) {
        h->waiting = m; h->wait_kind = 1;
        rx_service_input(io, h);
    } else if (!str_icmp(word, "MAYGETCHAR")) {
        LONG c = door_inbyte(io);
        char s[2];
        s[0] = (char)c; s[1] = 0;
        if (c >= 0) rx_reply(m, 0, s, 1);
        else rx_reply(m, 0, "NOCHAR", 6);
    } else if (!str_icmp(word, "CHECKIO") || !str_icmp(word, "IREADY")) {
        rx_reply(m, 0, in_avail() ? "1" : "0", 1);
    } else if (!str_icmp(word, "QUERY") || !str_icmp(word, "RECEIVE")) {
        door_write(io, arg, arglen);
        h->waiting = m; h->wait_kind = 2; h->qlen = 0;
        rx_service_input(io, h);
    } else if (!str_icmp(word, "PROMPT")) {
        /* PROMPT <len> NORMAL|HIDE|YESNO|NOYES "prompt" - the prompt is shown,
         * the length/mode are accepted (HIDE isn't masked yet) */
        const char *q = strchr((const char *)arg, '"');
        if (q) {
            const char *e = strrchr(q + 1, '"');
            door_write(io, (const UBYTE *)q + 1, e ? e - q - 1 : (LONG)strlen(q + 1));
        }
        h->waiting = m; h->wait_kind = 2; h->qlen = 0;
        rx_service_input(io, h);
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
        char c2[PATHLEN + 16];
        sprintf(c2, "Run >NIL: rx %.*s", (int)(arglen < PATHLEN ? arglen : PATHLEN - 1), (const char *)arg);
        SystemTags((STRPTR)c2, TAG_END);
        rx_reply(m, 0, "", 0);
    } else if (!str_icmp(word, "SHUTDOWN") || !str_icmp(word, "SCREENOUT") ||
               !str_icmp(word, "BBSCOMMAND") || !str_icmp(word, "OPENDISPLAY") ||
               !str_icmp(word, "CLOSEDISPLAY") || !str_icmp(word, "BAUD") ||
               !str_icmp(word, "MODEM") || !str_icmp(word, "SENDMODEM")) {
        if (!str_icmp(word, "SENDMODEM")) door_write(io, arg, arglen);
        rx_reply(m, 0, "", 0);
    } else if (!str_icmp(word, "GETUSER")) {
        char out[80];
        getuser(atoi((const char *)arg), out);
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

    if (d->dir[0]) {
        expand(d->dir, cmd, sizeof(cmd), drop);
        if ((dirlock = Lock((STRPTR)cmd, ACCESS_READ))) olddir = CurrentDir(dirlock);
    }
    expand(d->command, cmd, sizeof(cmd), drop);
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
            tn_wait(io.waitpkt ? 60 : 1000, sigs, &got);
            if (N.msg_waiting) N.msg_waiting = FALSE;
            if (N.loggedin && time_left_mins() == 0) {
                door_write(&io, (const UBYTE *)time_up_text(), strlen(time_up_text()));
                tn_flush();
                node_hangup("time limit in door");
            }
        } else Delay(5);

        drain_port(&io);
        while ((m = (struct RexxMsg *)GetMsg(h.port))) rx_command(&io, &h, m);
        if ((m = (struct RexxMsg *)GetMsg(h.reply))) {
            if (m->rm_Result1 && N.online) {
                char e[128];
                snprintf(e, sizeof(e), L("door.rexx_error", "\r\n[door ended with error %ld]\r\n"), m->rm_Result1);
                door_write(&io, (UBYTE *)e, strlen(e));
            }
            DeleteArgstring(m->rm_Args[0]);
            DeleteRexxMsg(m);
            h.launch = NULL;
            finished = TRUE;
        }
        if (finished) break;

        if (!N.online && !io.hungup) {
            io.hungup = TRUE;
            hung_at = bbs_now();
            flush_queues_eof(&io);
            if (h.waiting) { rx_reply(h.waiting, 0, "###PANIC", 8); h.waiting = NULL; }
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
        }
    }
    N.wait_new = 0;
    if (h.waiting) rx_reply(h.waiting, 20, NULL, 0);
    RemPort(h.port);
    /* anything that raced in after the script ended */
    {
        struct RexxMsg *m;
        while ((m = (struct RexxMsg *)GetMsg(h.port))) rx_reply(m, 20, NULL, 0);
    }
    flush_queues_eof(&io);
    if (h.launch) {
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
    CloseLibrary(RexxSysBase);
    RexxSysBase = NULL;
    tcolor(7);
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

struct XimState {
    struct MsgPort *port;
    char   name[20];
    LONG   registered;              /* JH_REGISTER - JH_SHUTDOWN */
    BOOL   ever_registered;
    struct JHMessage *waiting;      /* HK / PM / LI / ExtHK blocked on input */
    UBYTE  wait_kind;               /* 1 hotkey (String[0]), 2 line, 3 ExtHK (Command) */
    LONG   maxlen;
    UBYTE  line[204];
    int    len;
    struct Task *door;
    char   mainline[PATHLEN];
    char   unknown[8];              /* first few unknown commands, logged once */
    int    nunknown;
    LONG   unknown_cmds[8];
};

static void xs_copy(struct JHMessage *m, const char *s)
{
    str_copy(m->String, s, sizeof(m->String));
}

static void xim_write(struct DoorIO *io, const char *s, BOOL lf)
{
    door_write(io, (const UBYTE *)s, strlen(s));
    if (lf) door_write(io, (const UBYTE *)"\n", 1);
}

/* the caller typed something: finish a blocked input request */
static void xim_service_input(struct DoorIO *io, struct XimState *x)
{
    LONG c;
    struct JHMessage *m = x->waiting;
    if (!m) return;
    if (x->wait_kind == 1 || x->wait_kind == 3) {
        if ((c = door_inbyte(io)) < 0) return;
        if (x->wait_kind == 1) { m->String[0] = (char)c; m->String[1] = 0; m->Command = 2; }
        else m->Command = c;
        m->Data = 1;
        ReplyMsg((struct Message *)m);
        x->waiting = NULL;
        return;
    }
    while ((c = door_inbyte(io)) >= 0) {
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

static void xim_dt(struct JHMessage *m, LONG cmd)
{
    char s[64];
    LONG left = time_left_mins();
    ULONG used = (bbs_now() - N.logon) + (ULONG)N.user.mins_today * 60;
    ULONG allowed = left < 0 ? 999UL * 60 + used : (ULONG)left * 60 + used;
    s[0] = 0;
    if (!m->Data) return;               /* writes: accepted, not applied */
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
    case DT_TIMELASTON:     sprintf(s, "%lu", N.user.lastcall); break;
    case DT_TIMEUSED:       sprintf(s, "%lu", used); break;           /* seconds */
    case DT_TIMELIMIT:      sprintf(s, "%lu", allowed); break;
    case DT_TIMETOTAL:      sprintf(s, "%lu", allowed); break;
    case DT_BYTESUPLOAD:    sprintf(s, "%lu", N.user.ulkb * 1024); break;
    case DT_BYTEDOWNLOAD:   sprintf(s, "%lu", N.user.dlkb * 1024); break;
    case DT_DAILYBYTELIMIT: strcpy(s, "0"); break;
    case DT_DAILYBYTEDLD:   strcpy(s, "0"); break;
    case DT_EXPERT:         strcpy(s, (N.user.flags & UF_EXPERT) ? "X" : "N"); break;
    case DT_LINELENGTH:     sprintf(s, "%d", (int)N.rows); break;
    case DT_TIMEOUT:        sprintf(s, "%ld", cfg_int(N.cfg, "idle_minutes", 10) * 60); break;
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

    switch (cmd) {
    case JH_REGISTER:
        x->registered++;
        x->ever_registered = TRUE;
        m->Command = N.rows;            /* /X 5: the user's line length */
        break;
    case JH_SHUTDOWN:
    case CHAIN:
        x->registered--;
        break;
    case JH_WRITE:
        if (!gone) xim_write(io, m->String, FALSE);
        break;
    case JH_SM:
    case JH_SO:
    case JH_MCI:
        if (!gone) xim_write(io, m->String, m->Data != 0);
        break;
    case JH_SMPTR:
        if (!gone && m->msg.mn_Length >= sizeof(struct JHMessage) && m->strptr)
            xim_write(io, m->strptr, m->Data != 0);
        break;
    case JH_CO:                         /* console only: nothing to show */
        break;
    case JH_PM:
    case JH_LI:
    case JH_HK:
    case JH_ExtHK:
        if (gone) { m->Data = -1; m->Command = -1; break; }
        x->waiting = m;
        x->len = 0;
        if (cmd == JH_HK) { x->wait_kind = 1; xim_write(io, m->String, FALSE); }
        else if (cmd == JH_ExtHK) x->wait_kind = 3;
        else {
            x->wait_kind = 2;
            x->maxlen = m->Data > 0 && m->Data < 200 ? m->Data : 199;
            if (cmd == JH_PM) xim_write(io, m->String, FALSE);
            else {                      /* LI: String is an editable default */
                str_copy((char *)x->line, m->String, sizeof(x->line));
                x->len = strlen((char *)x->line);
                if (x->len > x->maxlen) x->len = x->maxlen;
                door_write(io, x->line, x->len);
            }
        }
        xim_service_input(io, x);
        return x->waiting != m;
    case JH_FetchKey:
        if (gone) { m->Data = -1; m->Command = -1; break; }
        {
            LONG c = door_inbyte(io);
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
    case JH_SIGBIT:   m->Data = 0; break;
    case JH_FLAGFILE: case JH_SHOWFLAGS: case RETURNCOMMAND: case RAWARROW:
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
    case EXPRESS_VERSION: xs_copy(m, "5"); break;
    case BB_CHATFLAG: xs_copy(m, "OFF"); break;
    case DT_CURR_TIME: sprintf(s, "%lu", bbs_now()); xs_copy(m, s); break;
    case DT_STAMP_LASTON: bbs_datetimestr(N.user.lastcall, s); xs_copy(m, s); break;
    case DT_STAMP_CTIME:  bbs_datetimestr(bbs_now(), s); xs_copy(m, s); break;
    case DT_CONFACCESS: if (m->Data) xs_copy(m, "XXXXXXXXX"); break;
    case BB_GETTASK:  m->task = (struct Process *)FindTask(NULL); break;
    case BB_LOGONTYPE: m->Data = 2; break;          /* remote */
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
        if (cmd >= DT_NAME && cmd <= DT_TIMEOUT) { xim_dt(m, cmd); break; }
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

static void run_xim(struct Door *d)
{
    struct DoorIO io;
    struct XimState *x;
    char cmd[PATHLEN * 2], drop[PATHLEN];
    ULONG sigs, hung_at = 0, broke_at = 0;
    BYTE sb;
    struct Process *lp;

    memset(&io, 0, sizeof(io));
    io.d = d;
    write_dropfiles(d, drop);
    if (!(x = AllocVec(sizeof(struct XimState), MEMF_CLEAR))) return;

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
    N.wait_new = 1;

    for (;;) {
        struct JHMessage *m;
        if (N.online) {
            ULONG got;
            tn_wait(1000, sigs, &got);
            if (N.msg_waiting) N.msg_waiting = FALSE;
            if (N.loggedin && time_left_mins() == 0) {
                door_write(&io, (const UBYTE *)time_up_text(), strlen(time_up_text()));
                tn_flush();
                node_hangup("time limit in door");
            }
        } else Delay(5);

        while ((m = (struct JHMessage *)GetMsg(x->port))) xim_command(&io, x, m);

        if (!N.online && !io.hungup) {
            io.hungup = TRUE;
            hung_at = bbs_now();
            if (x->waiting) {
                x->waiting->Data = -1;
                x->waiting->Command = -1;
                ReplyMsg((struct Message *)x->waiting);
                x->waiting = NULL;
            }
            bbs_log(BBS_SYSLOG, "node %d: caller dropped in XIM door %s", N.node, d->tag);
        }
        if (io.hungup) {
            if (node_reset_wanted()) node_release_slot();   /* sysop RESET: free the node now */
            if (bbs_now() - hung_at >= (ULONG)d->panicgrace && x->door &&
                (!broke_at || bbs_now() - broke_at >= 30)) {
                Signal(x->door, SIGBREAKF_CTRL_C);
                broke_at = bbs_now();
            }
        } else xim_service_input(&io, x);

        /* finished: the door unregistered, or its process is gone */
        if (LN.done) break;
        if (x->ever_registered && x->registered <= 0 && !x->waiting) {
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
out_port:
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
        LONG n, maxfd = (s > N.sock ? s : N.sock) + 1;

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
        FD_SET(N.sock, &r);
        tv.tv_secs = 1; tv.tv_micro = 0;
        if (WaitSelect(maxfd, &r, NULL, NULL, &tv, &sigs) < 0 && !(sigs & SIGBREAKF_CTRL_C)) continue;
        node_heartbeat();
        if (sigs & SIGBREAKF_CTRL_C) { node_hangup("disconnected by sysop"); break; }

        if (FD_ISSET(N.sock, &r)) {
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

    bbs_log(BBS_SYSLOG, "node %d: %s opened door %s", N.node, N.user.name, d->tag);
    sprintf(act, "Door: %.40s", d->name);
    shared_lock(N.S);
    N.ni->state = NS_DOOR;
    str_copy(N.ni->activity, act, LONGNAME);
    shared_unlock(N.S);
    user_save();                                /* the door may read our stats */

    start = bbs_now();
    if (d->assign[0]) make_assign(d->assign);
    if (!str_icmp(d->type, "cli")) run_cli(d);
    else if (!str_icmp(d->type, "cnetrexx") || !str_icmp(d->type, "arexx")) run_cnetrexx(d);
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

void door_list(void)
{
    struct Cfg *dc = load_doors();
    LONG i, n = cfg_sections(dc), shown = 0;
    char tags[40][NAMELEN];
    char buf[8];

    tputs(L("door.list.doors", "\n|09-=[ |15Doors|09 ]=-|07\n\n"));
    for (i = 0; i < n && shown < 40; i++) {
        struct Door d;
        const char *tag = cfg_section(dc, i);
        if (!door_get(dc, tag, &d) || d.level > N.user.level || !acs_check(d.acs)) continue;
        str_copy(tags[shown], tag, NAMELEN);
        shown++;
        tprintf("  |08[|15%2ld|08] |07%-36s |08%s|07\n", shown, d.name,
                !str_icmp(d.type, "cli") ? "" : d.type);
    }
    cfg_free(dc);
    if (!shown) { tputs(L("door.list.no_doors_are", "  |08No doors are installed.|07\n")); return; }
    tputs(L("door.list.door_number_enter", "\n|07Door number |08(Enter to cancel)|07: |15"));
    if (tgetline(buf, 4, GL_DIGITS) <= 0) return;
    i = atol(buf);
    if (i >= 1 && i <= shown) door_run(tags[i - 1]);
}
