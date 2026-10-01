/*
 * rexxmaint.c - BBSMaint as the CNet host for a door's ARexx maintenance script.
 *
 *   Doors.cfg:   maint_rexx = PFILES:Stocks/Date
 *
 * CNet games ship nightly scripts that CNet ran as ARexx events (Wall Street's Date, Empire's
 * Emp.Maint, BattleLands' BLmaint, TradeWars' TwNightlyMaint).  They print with TRANSMIT,
 * read the date with GETUSER 12, hand over with SPAWN or {#0 file} and run DOS commands with
 * {$ cmd} - with "rx" alone every TRANSMIT fails and the script's error trap ends it.
 * So BBSMaint answers them as CNet would, with nobody on line: what they print goes to
 * Maint.log (MCI and ANSI codes left out), input commands get "" (###PANIC after a
 * hundred, so a script waiting for a caller ends), and a script still running after ten
 * minutes gets CTRL-C.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <rexx/storage.h>
#include <rexx/rxslib.h>
#include <proto/rexxsyslib.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "bbs.h"

#define MAINT_LOG   "BBS:Logs/Maint.log"
#define MAX_SECS    600
#define MAX_INPUTS  100
#define MAX_DEPTH   4

void say(const char *fmt, ...);

struct RxsLib *RexxSysBase;

struct MHost {
    struct MsgPort *port, *reply;
    char   name[24];
    char   spawn[PATHLEN];
    int    inputs;
    ULONG  started;
    BOOL   broke;
    struct Task *script;
    char   line[240];
    int    llen;
};

struct RexxArgHdr { LONG size; UWORD length; UBYTE flags, hash; };

static LONG arg_len(STRPTR a)
{
    return a ? ((struct RexxArgHdr *)(a - sizeof(struct RexxArgHdr)))->length : 0;
}

static void reply(struct RexxMsg *m, const char *res)
{
    m->rm_Result1 = 0;
    m->rm_Result2 = 0;
    if ((m->rm_Action & RXFF_RESULT) && res)
        m->rm_Result2 = (LONG)CreateArgstring((STRPTR)res, strlen(res));
    ReplyMsg((struct Message *)m);
}

static void flush_line(struct MHost *h)
{
    int i = h->llen;
    while (i > 0 && h->line[i - 1] == ' ') i--;
    h->line[i] = 0;
    if (i > 0) bbs_log(MAINT_LOG, "  | %s", h->line);
    h->llen = 0;
}

static LONG run_script(struct MHost *h, const char *path, int depth);

/* what the script prints: to the log a line at a time; {#0 file} and {$ cmd} are run */
static void out_text(struct MHost *h, const UBYTE *s, LONG len, int depth)
{
    const UBYTE *end = s + len;
    while (s < end) {
        UBYTE c = *s++;
        if (c == 0x11) {                                /* ^Q cmd args } */
            char arg[200];
            int n = 0;
            UBYTE cmd = s < end ? *s++ : 0;
            while (s < end && *s != '}' && n < (int)sizeof(arg) - 1) arg[n++] = *s++;
            arg[n] = 0;
            if (s < end) s++;
            if (cmd == '#' || cmd == '$') {
                const char *p = arg;
                char n0 = arg[0];
                while (*p >= '0' && *p <= '9') p++;
                while (*p == ' ') p++;
                flush_line(h);
                if (cmd == '$' && *p) {
                    BPTR in = Open((STRPTR)"NIL:", MODE_OLDFILE), out = Open((STRPTR)"NIL:", MODE_NEWFILE);
                    bbs_log(MAINT_LOG, "  $ %s", p);
                    if (in && out) SystemTags((STRPTR)p, SYS_Input, in, SYS_Output, out, TAG_END);
                    if (in) Close(in);
                    if (out) Close(out);
                } else if (cmd == '#' && (n0 == '0' || n0 == '1') && *p) {
                    if (depth < MAX_DEPTH) run_script(h, p, depth + 1);
                    else bbs_log(MAINT_LOG, "  (too deep to run %s)", p);
                }
            } else if (cmd == 'n' || cmd == 'N') flush_line(h);
        } else if (c == 0x19) {                         /* ^Y cmd arg */
            UBYTE cmd = s < end ? *s : 0;
            s += 2;
            if (cmd == 'n' || cmd == 'N') flush_line(h);
        } else if (c == 27 || c == 0x9B) {              /* ANSI: skip to the final letter */
            while (s < end && !((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z'))) s++;
            if (s < end) s++;
        } else if (c == '\r' || c == '\n') {
            flush_line(h);
        } else if (c >= 32 && h->llen < (int)sizeof(h->line) - 1) {
            h->line[h->llen++] = (char)c;
        }
    }
}

static const char *const cmds[] = {
    "TRANSMIT", "PRINT", "SENDSTRING", "SEND", "NEWLINE", "SENDFILE", "GETCHAR", "GETKEY",
    "MAYGETCHAR", "BUFFERFLUSH", "CHECKIO", "IREADY", "QUERY", "RECEIVE", "PROMPT", "GETCARRIER",
    "BBSIDENTIFY", "VERSION", "LOGENTRY", "SYSOPLOG", "CHANGEWHERE", "SETNODELOCATION", "CHANGEWHAT",
    "ADDTIME", "ADDKEYS", "SPAWN", "SHUTDOWN", "SCREENOUT", "BBSCOMMAND", "OPENDISPLAY",
    "CLOSEDISPLAY", "BAUD", "MODEM", "SENDMODEM", "GETUSER", "CLS", "BYE", "HANGUP", "DROPCARRIER",
    "CARRIER", "CHECKCARRIER", NULL
};

static void command(struct MHost *h, struct RexxMsg *m, int depth)
{
    STRPTR a = m->rm_Args[0];
    LONG len = arg_len(a);
    const UBYTE *arg = (const UBYTE *)"";
    LONG arglen = 0;
    const char *w = NULL;
    int i, bl = 0;

    if (m->rm_Node.mn_ReplyPort && m->rm_Node.mn_ReplyPort->mp_SigTask)
        h->script = (struct Task *)m->rm_Node.mn_ReplyPort->mp_SigTask;
    /* the command, also when glued to its text ("ss'Loading'" = SENDSTRINGLoading) */
    for (i = 0; cmds[i]; i++) {
        int l = strlen(cmds[i]);
        if (l > bl && l <= len && !str_nicmp((const char *)a, cmds[i], l)) { w = cmds[i]; bl = l; }
    }
    if (w) {
        arg = (const UBYTE *)a + bl;
        if (bl < len && *arg == ' ') arg++;
        arglen = len - (arg - (const UBYTE *)a);
    }
    if (!w) { reply(m, ""); return; }

    if (!str_icmp(w, "TRANSMIT") || !str_icmp(w, "PRINT")) {
        out_text(h, arg, arglen, depth);
        flush_line(h);
        reply(m, "");
    } else if (!str_icmp(w, "SENDSTRING") || !str_icmp(w, "SEND")) {
        out_text(h, arg, arglen, depth);
        reply(m, "");
    } else if (!str_icmp(w, "NEWLINE")) {
        flush_line(h);
        reply(m, "");
    } else if (!str_icmp(w, "GETCHAR") || !str_icmp(w, "GETKEY") || !str_icmp(w, "QUERY") ||
               !str_icmp(w, "RECEIVE") || !str_icmp(w, "PROMPT")) {
        reply(m, ++h->inputs > MAX_INPUTS ? "###PANIC" : "");
    } else if (!str_icmp(w, "MAYGETCHAR")) {
        reply(m, "NOCHAR");
    } else if (!str_icmp(w, "CHECKIO") || !str_icmp(w, "IREADY")) {
        reply(m, "0");
    } else if (!str_icmp(w, "GETCARRIER")) {
        reply(m, "TRUE");                               /* CNet: a local (console) port */
    } else if (!str_icmp(w, "CARRIER") || !str_icmp(w, "CHECKCARRIER")) {
        reply(m, "1");
    } else if (!str_icmp(w, "LOGENTRY") || !str_icmp(w, "SYSOPLOG")) {
        char l[200];
        int n = 0;
        const UBYTE *p = arg, *e = arg + arglen;
        while (p < e && n < (int)sizeof(l) - 1) {
            if (*p == 0x11) { while (p < e && *p != '}') p++; if (p < e) p++; }
            else if (*p == 0x19) p += 3;
            else l[n++] = *p++;
        }
        l[n] = 0;
        bbs_log(MAINT_LOG, "  log: %s", l);
        reply(m, "");
    } else if (!str_icmp(w, "SPAWN")) {
        str_copy(h->spawn, (const char *)arg, arglen + 1 < PATHLEN ? arglen + 1 : PATHLEN);
        str_trim(h->spawn);
        reply(m, "");
    } else if (!str_icmp(w, "GETUSER")) {
        char out[64];
        LONG n = atol((const char *)arg);
        static const char *const wd[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
        out[0] = 0;
        switch (n) {
        case 1:  strcpy(out, "Maintenance"); break;
        case 7:  strcpy(out, "9990"); break;
        case 12: strcpy(out, wd[(bbs_now() / 86400) % 7]); out[3] = ' '; bbs_datetimestr(bbs_now(), out + 4); break;
        case 17: strcpy(out, "1"); break;
        case 23: strcpy(out, "0"); break;
        case 27: strcpy(out, "80"); break;
        case 28: strcpy(out, "2"); break;
        }
        reply(m, out);
    } else if (!str_icmp(w, "VERSION")) {
        reply(m, "CNet AMIGA 3.05 (NilBBS " BBS_VERSION ")");
    } else if (!str_icmp(w, "BBSIDENTIFY")) {
        reply(m, !str_nicmp((const char *)arg, "BBS", 3) ? "NilBBS " BBS_VERSION : "");
    } else {
        reply(m, "");
    }
}

/* run one script on our port until it (and whatever it SPAWNs) has ended */
static LONG run_script(struct MHost *h, const char *path, int depth)
{
    struct RexxMsg *launch, *m;
    struct MsgPort *rexx;
    LONG rc = 0;
    char next[PATHLEN];

    str_copy(next, path, sizeof(next));
    while (next[0]) {
        if (!(launch = CreateRexxMsg(h->reply, (STRPTR)"rexx", (STRPTR)h->name))) return -1;
        launch->rm_Args[0] = CreateArgstring((STRPTR)next, strlen(next));
        launch->rm_Action = RXCOMM;
        bbs_log(MAINT_LOG, "  run %s", next);
        next[0] = 0;
        Forbid();
        if ((rexx = FindPort((STRPTR)"REXX"))) PutMsg(rexx, (struct Message *)launch);
        Permit();
        if (!rexx) {
            DeleteArgstring(launch->rm_Args[0]);
            DeleteRexxMsg(launch);
            say("  ARexx (RexxMast) isn't running.");
            return -1;
        }
        for (;;) {
            BOOL done = FALSE;
            while ((m = (struct RexxMsg *)GetMsg(h->port))) command(h, m, depth);
            while ((m = (struct RexxMsg *)GetMsg(h->reply))) {
                if (m == launch) { done = TRUE; rc = m->rm_Result1; }
                DeleteArgstring(m->rm_Args[0]);
                DeleteRexxMsg(m);
            }
            if (done) break;
            if (!h->broke && bbs_now() - h->started > MAX_SECS && h->script) {
                say("  still running after %d minutes - stopping it", MAX_SECS / 60);
                Signal(h->script, SIGBREAKF_CTRL_C);
                h->broke = TRUE;
            }
            if (h->broke && bbs_now() - h->started > MAX_SECS + 60) {
                say("  it won't stop - left running");
                return -2;
            }
            Delay(5);
        }
        flush_line(h);
        if (rc) bbs_log(MAINT_LOG, "  ended with error %ld", rc);
        if (h->spawn[0]) {                              /* SPAWN: the next script takes over */
            str_copy(next, h->spawn, sizeof(next));
            h->spawn[0] = 0;
        }
    }
    return rc;
}

/* a door's maint_rexx script, in the current directory; returns its rc (or <0) */
LONG rexx_maint(const char *script)
{
    struct MHost h;
    LONG rc = -1;
    memset(&h, 0, sizeof(h));
    if (!(RexxSysBase = (struct RxsLib *)OpenLibrary((STRPTR)"rexxsyslib.library", 36))) {
        say("  ARexx isn't available.");
        return -1;
    }
    Forbid();
    rc = FindPort((STRPTR)"REXX") ? 0 : 1;
    Permit();
    if (rc) { SystemTags((STRPTR)"Run >NIL: RexxMast >NIL:", TAG_END); Delay(100); }
    strcpy(h.name, "CNETREXX-MAINT");
    if ((h.port = CreateMsgPort()) && (h.reply = CreateMsgPort())) {
        h.port->mp_Node.ln_Name = h.name;
        h.port->mp_Node.ln_Pri = 0;
        AddPort(h.port);
        h.started = bbs_now();
        rc = run_script(&h, script, 0);
        RemPort(h.port);
        if (rc == -2) {                                 /* a script that won't die: leave the ports */
            Forbid();
            h.port->mp_Flags = PA_IGNORE;
            h.reply->mp_Flags = PA_IGNORE;
            Permit();
            h.port = h.reply = NULL;
        }
    }
    if (h.port) {
        struct RexxMsg *m;
        while ((m = (struct RexxMsg *)GetMsg(h.port))) reply(m, "");
        DeleteMsgPort(h.port);
    }
    if (h.reply) DeleteMsgPort(h.reply);
    CloseLibrary((struct Library *)RexxSysBase);
    return rc;
}
