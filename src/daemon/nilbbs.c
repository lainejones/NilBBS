/*
 * NilBBS - the listener daemon.
 *
 *   NilBBS [PORT=<n>] [NODES=<n>] [QUIET]
 *
 * Accepts telnet connections, screens the caller's address through the IP
 * filter, picks a free node and hands the socket to a fresh BBSNode process
 * (ReleaseSocket -> ObtainSocket).  Owns the shared state block.
 *
 * Signals:  CTRL-C  shut down (nodes are told to hang up first)
 *           CTRL-E  a node finished / something in the shared block changed
 *           CTRL-F  reload Config/IPFilter.cfg
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <rexx/storage.h>
#include <proto/rexxsyslib.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <proto/bsdsocket.h>
#include <libraries/bsdsocket.h>

#include "bbs.h"
#include "cfg.h"

unsigned long __stack = 16384;
static const char __attribute__((used)) verstag[] = "$VER: NilBBS " BBS_VERSION " (" BBS_VERDATE ")";

extern struct ExecBase *SysBase;
struct Library *SocketBase = NULL;

static struct BBSShared *S;
static struct Cfg *C;
static LONG  g_listen = -1;
static BOOL  g_quiet = FALSE;
static char  g_nodecmd[PATHLEN];
static char  g_denymsg[LINELEN];
static char  g_busymsg[LINELEN];
static char  g_banmsg[LINELEN];
static BOOL  g_closed;           /* an exclusive event is due: no new calls */

/* console + system log */
static void say(const char *fmt, ...)
{
    char line[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    bbs_log(BBS_SYSLOG, "%s", line);
    if (!g_quiet) {
        char ts[20];
        bbs_timestr(bbs_now(), ts);
        Printf((STRPTR)"%s %s\n", (LONG)ts, (LONG)line);
    }
}

/* ---- sockets -------------------------------------------------------------- */

static LONG listen_on(UWORD port)
{
    volatile UBYTE sa[16];
    LONG one = 1, fd;
    int i;
    if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (APTR)&one, sizeof(one));
    /* volatile bytes: -O2 can merge sin_family+sin_port stores and lose the
     * family (seen in NetHarness/AmiTime); big-endian family(2) port(2) addr(4) */
    for (i = 0; i < 16; i++) sa[i] = 0;
    sa[1] = AF_INET;
    sa[2] = (UBYTE)(port >> 8);
    sa[3] = (UBYTE)port;
    if (bind(fd, (struct sockaddr *)sa, 16) < 0 || listen(fd, 5) < 0) {
        CloseSocket(fd);
        return -1;
    }
    return fd;
}

static BOOL listen_up(UWORD port)
{
    return (g_listen = listen_on(port)) >= 0;
}

static void send_str(LONG fd, const char *s)
{
    send(fd, (APTR)s, strlen(s), 0);
}

/* text from the config may use \r\n escapes */
static void unescape(char *s)
{
    char *d = s;
    while (*s) {
        if (*s == '\\' && s[1]) {
            s++;
            *d++ = (*s == 'n') ? '\n' : (*s == 'r') ? '\r' : (*s == 'e') ? 27 : *s;
            s++;
        } else *d++ = *s++;
    }
    *d = 0;
}

/* (task_alive() is in common/shared.c - the tools use it too) */

/* ---- node launch ------------------------------------------------------------ */

static void free_node(int n)
{
    struct NodeInfo *ni = &S->node[n];
    memset(ni, 0, sizeof(*ni));
    ni->userid = -1;
}

static BOOL start_node(int n, LONG fd, ULONG ip)
{
    struct NodeInfo *ni = &S->node[n];
    char cmd[PATHLEN + 40];
    BPTR in, out;
    LONG id, rc;

    shared_lock(S);
    free_node(n);
    ni->state = NS_STARTING;
    ni->ip = ip;
    ni->connected = ni->beat = bbs_now();
    str_copy(ni->activity, "Connecting", sizeof(ni->activity));
    shared_unlock(S);

    id = ReleaseSocket(fd, UNIQUE_ID);
    if (id == -1) {
        say("ReleaseSocket failed (errno %ld)", Errno());
        CloseSocket(fd);            /* still ours: the release didn't happen */
        shared_lock(S); free_node(n); shared_unlock(S);
        return FALSE;
    }
    shared_lock(S); ni->sockid = id; shared_unlock(S);

    sprintf(cmd, "%s NODE=%d SOCKET=%ld", g_nodecmd, n + 1, id);
    in  = Open((STRPTR)"NIL:", MODE_OLDFILE);
    out = Open((STRPTR)"NIL:", MODE_NEWFILE);
    rc = SystemTags((STRPTR)cmd,
                    SYS_Input,   in,
                    SYS_Output,  out,
                    SYS_Asynch,  TRUE,
                    NP_StackSize, 16384,
                    NP_Priority, 0,
                    TAG_END);
    if (rc == -1) {
        LONG s2;
        if (in) Close(in);
        if (out) Close(out);
        say("could not start %s", g_nodecmd);
        /* take the socket back so it doesn't leak in the stack */
        if ((s2 = ObtainSocket(id, AF_INET, SOCK_STREAM, 0)) >= 0) {
            send_str(s2, "\r\nSystem error - please try again later.\r\n");
            CloseSocket(s2);
        }
        shared_lock(S); free_node(n); shared_unlock(S);
        return FALSE;
    }
    return TRUE;
}

static void handle_connect(void)
{
    UBYTE peer[16];
    socklen_t plen = sizeof(peer);
    ULONG ip, now = bbs_now();
    char ipstr[16];
    LONG fd;
    int verdict, n, freen = -1;

    fd = accept(g_listen, (struct sockaddr *)peer, &plen);
    if (fd < 0) {
        Delay(10);
        return;
    }
    ip = ((ULONG)peer[4] << 24) | ((ULONG)peer[5] << 16) | ((ULONG)peer[6] << 8) | peer[7];
    ip_tostr(ip, ipstr);

    shared_lock(S);
    verdict = ipf_check(S, ip, now);
    if (verdict == IPV_ALLOW) {
        for (n = 0; n < S->nodes; n++)
            if (S->node[n].state == NS_FREE) { freen = n; break; }
        if (freen >= 0) {
            S->node[freen].state = NS_STARTING;     /* reserve it now */
            S->total_calls++;
        }
    }
    shared_unlock(S);

    switch (verdict) {
    case IPV_DENY:
        say("refused %s (IP filter)", ipstr);
        if (g_denymsg[0]) send_str(fd, g_denymsg);
        CloseSocket(fd);
        return;
    case IPV_BANNED:
        say("refused %s (banned)", ipstr);
        if (g_banmsg[0]) send_str(fd, g_banmsg);
        CloseSocket(fd);
        return;
    case IPV_FLOOD:
        say("connect flood from %s - auto-banned", ipstr);
        if (g_banmsg[0]) send_str(fd, g_banmsg);
        CloseSocket(fd);
        return;
    }

    if (freen >= 0 && g_closed) {
        say("closed for an event, turned away %s", ipstr);
        send_str(fd, "\r\nThe BBS is down for maintenance - please call back in a few minutes.\r\n");
        CloseSocket(fd);
        shared_lock(S); S->node[freen].state = NS_FREE; shared_unlock(S);
        return;
    }
    if (freen < 0) {
        say("all nodes busy, turned away %s", ipstr);
        send_str(fd, g_busymsg);
        CloseSocket(fd);
        return;
    }

    say("call from %s on node %ld", ipstr, (LONG)(freen + 1));
    start_node(freen, fd, ip);     /* disposes of fd on every path */
}

/* ---- finger (RFC 1288): "finger @bbs" lists who's online, "finger user@bbs"
 * shows a profile and plan.  finger_port = 79 in NilBBS.cfg, 0 = off. ------- */

static LONG g_finger = -1;

static void fsend(LONG fd, const char *fmt, ...)
{
    char line[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    send(fd, (APTR)line, strlen(line), 0);
}

static void finger_user(LONG fd, const char *name)
{
    struct UserRec *u = AllocVec(sizeof(struct UserRec), 0);
    char d1[20], d2[20], path[PATHLEN], line[120];
    struct LineReader lr;
    LONG id;
    int n, on = 0;
    if (!u) return;
    ObtainSemaphore(&S->userlock);
    id = userdb_find(name, u);
    ReleaseSemaphore(&S->userlock);
    if (!id || (u->flags & UF_DELETED)) { fsend(fd, "No such user: %s\r\n", name); FreeVec(u); return; }
    shared_lock(S);
    for (n = 0; n < S->nodes; n++)
        if (S->node[n].state >= NS_ONLINE && S->node[n].userid == id) on = n + 1;
    shared_unlock(S);
    bbs_datestr(u->firstcall, d1);
    bbs_datetimestr(u->lastcall, d2);
    fsend(fd, "User: %s\r\n", u->name);
    if (u->location[0]) fsend(fd, "From: %s\r\n", u->location);
    fsend(fd, "Member since %s, %lu calls, %lu posts\r\n", d1, u->calls, u->posts);
    if (on) fsend(fd, "Online now on node %ld\r\n", (LONG)on);
    else fsend(fd, "Last on %s\r\n", d2);
    sprintf(path, "BBS:Data/Plans/%lu.plan", u->id);
    if (lr_open(&lr, path)) {
        fsend(fd, "Plan:\r\n");
        while (lr_gets(&lr, line, sizeof(line)) >= 0) fsend(fd, "  %s\r\n", line);
        lr_close(&lr);
    } else fsend(fd, "No plan.\r\n");
    FreeVec(u);
}

static void handle_finger(void)
{
    UBYTE peer[16];
    socklen_t plen = sizeof(peer);
    char q[130], ipstr[16];
    LONG fd, got = 0, verdict;
    ULONG ip, now = bbs_now();
    int n;

    if ((fd = accept(g_finger, (struct sockaddr *)peer, &plen)) < 0) return;
    ip = ((ULONG)peer[4] << 24) | ((ULONG)peer[5] << 16) | ((ULONG)peer[6] << 8) | peer[7];
    ip_tostr(ip, ipstr);
    shared_lock(S);
    verdict = ipf_screen(S, ip, now);               /* banned is banned; lookups aren't calls */
    shared_unlock(S);
    if (verdict != IPV_ALLOW) { CloseSocket(fd); return; }

    /* the query line, with a 5 second limit so a silent client can't stall us */
    while (got < (LONG)sizeof(q) - 1) {
        fd_set rfds;
        struct timeval tv;
        LONG r;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_secs = 5;
        tv.tv_micro = 0;
        if (WaitSelect(fd + 1, &rfds, NULL, NULL, &tv, NULL) <= 0) break;
        if ((r = recv(fd, (APTR)(q + got), sizeof(q) - 1 - got, 0)) <= 0) break;
        got += r;
        q[got] = 0;
        if (strchr(q, '\n')) break;
    }
    q[got] = 0;
    {
        char *p = q, *e;
        if (!str_nicmp(p, "/W", 2)) p += 2;         /* verbose flag: same answer */
        p = str_trim(p);
        for (e = p; *e && *e != '\r' && *e != '\n'; e++) ;
        *e = 0;
        say("finger from %s: \"%s\"", ipstr, p);
        fsend(fd, "%s - NilBBS %s\r\n\r\n", cfg_str(C, "bbs_name", "NilBBS"), BBS_VERSION);
        if (strchr(p, '@')) fsend(fd, "Forwarding is not supported.\r\n");
        else if (*p && cfg_bool(C, "finger_users", TRUE)) finger_user(fd, p);
        else {
            int shown = 0;
            fsend(fd, "Node  User                  Activity                        Mins\r\n");
            shared_lock(S);
            for (n = 0; n < S->nodes; n++) {
                struct NodeInfo *ni = &S->node[n];
                char line[120];
                if (ni->state < NS_ONLINE) continue;
                sprintf(line, "%4ld  %-20.20s  %-30.30s  %4lu\r\n", (LONG)(n + 1), ni->user,
                        ni->activity, (now - ni->connected) / 60);
                shared_unlock(S);
                send(fd, (APTR)line, strlen(line), 0);
                shared_lock(S);
                shown++;
            }
            shared_unlock(S);
            if (!shown) fsend(fd, "Nobody is online.\r\n");
        }
    }
    shutdown(fd, 1);
    CloseSocket(fd);
}

static void housekeeping(void);
static int  active_nodes(void);

/* ---- node messages from the daemon (ARexx SEND/BROADCAST, event warnings) ---- */

static BOOL daemon_msg(int n, UBYTE type, const char *text)
{
    struct NodeInfo *ni;
    struct Task *t = NULL;
    if (n < 1 || n > S->nodes) return FALSE;
    shared_lock(S);
    ni = &S->node[n - 1];
    if (ni->state >= NS_ONLINE) {
        UBYTE next = (ni->msg_head + 1) % NODE_MSGQ;
        if (next != ni->msg_tail) {
            struct NodeMsg *m = &ni->msgq[ni->msg_head];
            m->from = 0;
            m->type = type;
            str_copy(m->fromname, cfg_str(C, "sysop_name", "Sysop"), NAMELEN);
            str_copy(m->text, text, sizeof(m->text));
            ni->msg_head = next;
            t = ni->task;
        }
    }
    shared_unlock(S);
    if (t) Signal(t, SIGBREAKF_CTRL_D);
    return t != NULL;
}

static int broadcast_all(const char *text)
{
    int n, sent = 0;
    for (n = 1; n <= S->nodes; n++) if (daemon_msg(n, NM_SYSOP, text)) sent++;
    return sent;
}

static void kick_all(void)
{
    int n;
    shared_lock(S);
    for (n = 0; n < S->nodes; n++)
        if (S->node[n].state >= NS_LOGIN && task_alive(S->node[n].task))
            Signal(S->node[n].task, SIGBREAKF_CTRL_C);
    shared_unlock(S);
}

/* ---- events (BBS:Config/Events.cfg) ------------------------------------------
 *   [TOSS]
 *   time      = 03:00          ; or:  every = 30   (minutes, from midnight)
 *   days      = daily          ; or e.g.  Mon Wed Fri
 *   command   = BBS:BBSToss TOSS SCAN
 *   exclusive = no             ; yes: warn callers, close the doors, log
 *   warn      = 5              ;      everyone off, run it, then reopen
 */
#define MAX_EVENTS 16

struct Event {
    char  tag[NAMELEN];
    char  cmd[PATHLEN];
    WORD  at;                   /* minute of the day, -1 = use every */
    UWORD every;
    UBYTE days;                 /* bit 0 = Sunday */
    UBYTE exclusive;
    UWORD warn;
    ULONG lastrun;              /* minute stamp it last ran in */
    ULONG warned;               /* minute stamp of the warning */
};

static struct Event g_ev[MAX_EVENTS];
static struct Task *g_self;
static BYTE  g_evsig = -1;        /* an exclusive event's command has finished */
static char  g_evtag[NAMELEN];
static BOOL  g_evrunning;         /* its process still runs our exit hook: don't unload */

/* NP_ExitCode: runs in the event's process as it exits */
static void event_exit(void)
{
    if (g_self && g_evsig >= 0) Signal(g_self, 1UL << g_evsig);
}
static int   g_nev;
static ULONG g_lastmin;

static void load_events(void)
{
    static const char *dn[7] = { "sun", "mon", "tue", "wed", "thu", "fri", "sat" };
    struct Cfg *c = cfg_load("BBS:Config/Events.cfg");
    LONG i, n = cfg_sections(c);
    g_nev = 0;
    for (i = 0; i < n && g_nev < MAX_EVENTS; i++) {
        const char *tag = cfg_section(c, i), *t, *d;
        struct Event *e = &g_ev[g_nev];
        memset(e, 0, sizeof(*e));
        str_copy(e->tag, tag, NAMELEN);
        str_copy(e->cmd, cfg_sget(c, tag, "command", ""), PATHLEN);
        if (!e->cmd[0]) continue;
        t = cfg_sget(c, tag, "time", "");
        e->every = (UWORD)cfg_sint(c, tag, "every", 0);
        e->at = (t[0] && strchr(t, ':')) ? (WORD)(atoi(t) * 60 + atoi(strchr(t, ':') + 1)) : -1;
        if (e->at < 0 && !e->every) continue;
        d = cfg_sget(c, tag, "days", "daily");
        if (!str_icmp(d, "daily") || !str_icmp(d, "*")) e->days = 0x7F;
        else { int k; for (k = 0; k < 7; k++) if (str_istr(d, dn[k])) e->days |= 1 << k; }
        e->exclusive = (UBYTE)cfg_sbool(c, tag, "exclusive", FALSE);
        e->warn = (UWORD)cfg_sint(c, tag, "warn", 5);
        g_nev++;
    }
    cfg_free(c);
}

static void run_event(struct Event *e)
{
    BPTR in, out;
    LONG rc;
    char logline[80];
    if (e->exclusive) {
        int waited;
        g_closed = TRUE;
        broadcast_all("The system is going down for maintenance now - please call back soon.");
        Delay(150);
        kick_all();
        for (waited = 0; waited < 60 && active_nodes(); waited++) { housekeeping(); Delay(50); }
    }
    say("event %s: %s", e->tag, e->cmd);
    in  = Open((STRPTR)"NIL:", MODE_OLDFILE);
    sprintf(logline, "BBS:Logs/Event-%s.log", e->tag);
    out = Open((STRPTR)logline, MODE_NEWFILE);
    if (!out) out = Open((STRPTR)"NIL:", MODE_NEWFILE);
    if (e->exclusive) {
        /* asynchronous, so we keep answering (and turning away) calls; the
         * exit hook tells us when it's done */
        str_copy(g_evtag, e->tag, NAMELEN);
        rc = SystemTags((STRPTR)e->cmd, SYS_Input, in, SYS_Output, out, SYS_Asynch, TRUE,
                        NP_StackSize, 32768, NP_ExitCode, (ULONG)event_exit, TAG_END);
        if (rc == -1) {
            Close(in);
            Close(out);
            say("event %s: couldn't start it - open for calls again", e->tag);
            g_closed = FALSE;
        } else g_evrunning = TRUE;
    } else {
        rc = SystemTags((STRPTR)e->cmd, SYS_Input, in, SYS_Output, out, SYS_Asynch, TRUE,
                        NP_StackSize, 32768, TAG_END);
        if (rc == -1) { Close(in); Close(out); say("event %s: couldn't start it", e->tag); }
    }
}

/* once a minute */
static void check_events(ULONG now)
{
    ULONG minstamp = now / 60, days = now / 86400;
    int mod = (int)((now % 86400) / 60), dow = (int)(days % 7);    /* 1-Jan-78 was a Sunday */
    int i;
    for (i = 0; i < g_nev; i++) {
        struct Event *e = &g_ev[i];
        BOOL today = (e->days >> dow) & 1;
        BOOL due = today && (e->every ? mod % e->every == 0 : mod == e->at);
        if (e->exclusive && e->at >= 0 && today && e->warn && mod == e->at - e->warn &&
            e->warned != minstamp) {
            char msg[100];
            e->warned = minstamp;
            g_closed = TRUE;
            sprintf(msg, "System maintenance in %d minute%s - please finish up.", (int)e->warn, e->warn == 1 ? "" : "s");
            broadcast_all(msg);
            say("event %s in %d min: new calls refused", e->tag, (int)e->warn);
        }
        if (due && e->lastrun != minstamp) {
            e->lastrun = minstamp;
            run_event(e);
        }
    }
}

/* ---- ARexx port "NILBBS" ---------------------------------------------------------
 *   STATUS  WHO  NODEUSER n  KICK n  RESET n  SEND n text  BROADCAST text
 *   BAN ip [minutes [reason]]  UNBAN ip  USERINFO name  EVENT tag
 *   RELOAD  SHUTDOWN  VERSION
 * Results come back in RESULT (OPTIONS RESULTS); rc 0 = ok, 5 = no such
 * node/user/event, 10 = bad command. */

static struct MsgPort *g_rexx;
static struct Library *RexxSysBase_d;
#define RexxSysBase RexxSysBase_d

static void rexx_open(void)
{
    if (!(RexxSysBase = OpenLibrary((STRPTR)"rexxsyslib.library", 36))) return;
    if (!(g_rexx = CreateMsgPort())) return;
    Forbid();
    if (FindPort((STRPTR)"NILBBS")) { Permit(); DeleteMsgPort(g_rexx); g_rexx = NULL; return; }
    g_rexx->mp_Node.ln_Name = "NILBBS";
    g_rexx->mp_Node.ln_Pri = 0;
    AddPort(g_rexx);
    Permit();
}

static void rexx_close(void)
{
    if (g_rexx) {
        struct Message *m;
        RemPort(g_rexx);
        while ((m = GetMsg(g_rexx))) {
            ((struct RexxMsg *)m)->rm_Result1 = 20;
            ((struct RexxMsg *)m)->rm_Result2 = 0;
            ReplyMsg(m);
        }
        DeleteMsgPort(g_rexx);
        g_rexx = NULL;
    }
    if (RexxSysBase) { CloseLibrary(RexxSysBase); RexxSysBase = NULL; }
}

static const char *state_name(UBYTE s)
{
    return s == NS_FREE ? "free" : s == NS_STARTING || s == NS_LOGIN ? "login" : s == NS_DOOR ? "door" : "online";
}

/* one command; fills res, returns the rc */
static LONG rexx_cmd(char *cmd, char *res)
{
    char *arg = cmd, word[16];
    int i = 0;
    ULONG now = bbs_now();
    while (*arg == ' ') arg++;
    while (*arg && *arg != ' ' && i < 15) word[i++] = *arg++;
    word[i] = 0;
    while (*arg == ' ') arg++;
    res[0] = 0;

    if (!str_icmp(word, "VERSION")) { sprintf(res, "NilBBS %s", BBS_VERSION); return 0; }
    if (!str_icmp(word, "STATUS")) {
        int n, on = 0;
        shared_lock(S);
        for (n = 0; n < S->nodes; n++) if (S->node[n].state >= NS_ONLINE) on++;
        sprintf(res, "port %ld nodes %ld online %ld calls %lu up %lu %s", (LONG)S->port, (LONG)S->nodes,
                (LONG)on, S->total_calls, (now - S->started) / 60, g_closed ? "closed" : "open");
        shared_unlock(S);
        return 0;
    }
    if (!str_icmp(word, "WHO")) {
        int n;
        char *p = res;
        shared_lock(S);
        for (n = 0; n < S->nodes; n++) {
            struct NodeInfo *ni = &S->node[n];
            p += sprintf(p, "%s%ld %s %s", n ? "\n" : "", (LONG)(n + 1), state_name(ni->state),
                         ni->state >= NS_ONLINE ? ni->user : "-");
            if (ni->state >= NS_ONLINE) p += sprintf(p, " (%s)", ni->activity);
        }
        shared_unlock(S);
        return 0;
    }
    if (!str_icmp(word, "NODEUSER")) {
        int n = atoi(arg);
        if (n < 1 || n > S->nodes) return 5;
        shared_lock(S);
        if (S->node[n - 1].state >= NS_ONLINE) str_copy(res, S->node[n - 1].user, NAMELEN);
        shared_unlock(S);
        return 0;
    }
    if (!str_icmp(word, "KICK")) {
        int n = atoi(arg);
        struct Task *t = NULL;
        if (n < 1 || n > S->nodes) return 5;
        shared_lock(S);
        if (S->node[n - 1].state >= NS_LOGIN) t = S->node[n - 1].task;
        shared_unlock(S);
        if (!t) return 5;
        Signal(t, SIGBREAKF_CTRL_C);
        say("ARexx: kicked node %ld", (LONG)n);
        return 0;
    }
    if (!str_icmp(word, "RESET")) {
        int n = atoi(arg);
        if (!node_reset_start(S, n)) return 5;
        say("ARexx: reset node %ld", (LONG)n);
        return 0;
    }
    if (!str_icmp(word, "SEND")) {
        int n = atoi(arg);
        while (*arg && *arg != ' ') arg++;
        while (*arg == ' ') arg++;
        return daemon_msg(n, NM_TEXT, arg) ? 0 : 5;
    }
    if (!str_icmp(word, "BROADCAST")) { sprintf(res, "%ld", (LONG)broadcast_all(arg)); return 0; }
    if (!str_icmp(word, "BAN") || !str_icmp(word, "UNBAN")) {
        char ipb[20];
        ULONG ip;
        LONG mins = 0;
        BOOL ok;
        for (i = 0; *arg && *arg != ' ' && i < 19; i++) ipb[i] = *arg++;
        ipb[i] = 0;
        if (!ip_parse(ipb, &ip)) return 10;
        while (*arg == ' ') arg++;
        if (*arg >= '0' && *arg <= '9') { mins = atol(arg); while (*arg && *arg != ' ') arg++; while (*arg == ' ') arg++; }
        shared_lock(S);
        /* no reason given: a re-ban keeps the one on file, a new ban says ARexx */
        ok = !str_icmp(word, "BAN") ? ipf_ban(S, ip, (ULONG)mins, *arg ? arg : (ipf_findban(S, ip) ? "" : "ARexx"))
                                    : ipf_unban(S, ip);
        shared_unlock(S);
        say("ARexx: %s %s", word, ipb);
        return ok ? 0 : 5;
    }
    if (!str_icmp(word, "USERINFO")) {
        struct UserRec *u = AllocVec(sizeof(struct UserRec), 0);
        LONG id;
        char d[20];
        if (!u) return 20;
        ObtainSemaphore(&S->userlock);
        id = userdb_find(arg, u);
        ReleaseSemaphore(&S->userlock);
        if (!id || (u->flags & UF_DELETED)) { FreeVec(u); return 5; }
        bbs_datetimestr(u->lastcall, d);
        sprintf(res, "%s %ld %lu %lu %s", u->name, (LONG)u->level, u->calls, u->posts, d);
        FreeVec(u);
        return 0;
    }
    if (!str_icmp(word, "EVENT")) {
        for (i = 0; i < g_nev; i++)
            if (!str_icmp(g_ev[i].tag, arg)) { run_event(&g_ev[i]); return 0; }
        return 5;
    }
    if (!str_icmp(word, "RELOAD")) {
        shared_lock(S);
        ipf_load_rules(S, BBS_IPRULES);
        shared_unlock(S);
        load_events();
        say("ARexx: reloaded (%ld IP rules, %ld events)", (LONG)S->nrules, (LONG)g_nev);
        return 0;
    }
    if (!str_icmp(word, "SHUTDOWN")) { S->shutdown = 1; return 0; }
    return 10;
}

static void rexx_handle(void)
{
    struct RexxMsg *m;
    static char res[2048];
    while ((m = (struct RexxMsg *)GetMsg(g_rexx))) {
        char cmd[300];
        LONG rc;
        str_copy(cmd, (char *)m->rm_Args[0], sizeof(cmd));
        rc = rexx_cmd(cmd, res);
        m->rm_Result1 = rc;
        m->rm_Result2 = 0;
        if (rc == 0 && (m->rm_Action & RXFF_RESULT))
            m->rm_Result2 = (LONG)CreateArgstring((STRPTR)res, strlen(res));
        ReplyMsg((struct Message *)m);
    }
}

/* the serial line (NilBBS.cfg serial_device): one "BBSNode SERIAL" waits for a call, takes
 * it and exits; a new one is started once it has gone (not more often than every 10 s, so a
 * port that won't open doesn't spin).  It is found by its task name, SERIAL_TASKNAME. */
static ULONG g_serial_last;
static void serial_keep(ULONG now)
{
    char cmd[PATHLEN + 16];
    BPTR in, out;
    BOOL there;
    if (!cfg_str(C, "serial_device", "")[0] || S->shutdown || now - g_serial_last < 10) return;
    Forbid();
    there = FindTask((STRPTR)SERIAL_TASKNAME) != NULL;
    Permit();
    if (there) return;
    g_serial_last = now;
    sprintf(cmd, "%s SERIAL", g_nodecmd);
    in  = Open((STRPTR)"NIL:", MODE_OLDFILE);
    out = Open((STRPTR)"NIL:", MODE_NEWFILE);
    if (SystemTags((STRPTR)cmd, SYS_Input, in, SYS_Output, out, SYS_Asynch, TRUE,
                   NP_StackSize, 16384, TAG_END) == -1) {
        if (in) Close(in);
        if (out) Close(out);
        say("could not start %s SERIAL", g_nodecmd);
    }
}

/* once a second */
static ULONG last_save;
static ULONG g_reset_seen[MAX_NODES];   /* when a node's RESET was first seen, 0 = none */
static void housekeeping(void)
{
    ULONG now = bbs_now();
    int n;
    shared_lock(S);
    if (ipf_expire(S, now)) say("expired bans removed");
    /* a RESET the node didn't answer (hung outside any door loop): free the slot
     * by force.  If the old process ever wakes it sees the slot isn't its own
     * any more (node_heartbeat) and keeps its hands off it. */
    for (n = 0; n < S->nodes; n++) {
        if (!S->node[n].reset || S->node[n].state == NS_FREE) { g_reset_seen[n] = 0; continue; }
        if (!g_reset_seen[n]) g_reset_seen[n] = now ? now : 1;
        else if (now - g_reset_seen[n] >= NODE_RESET_FORCE) {
            free_node(n);
            g_reset_seen[n] = 0;
            say("node %ld didn't answer its RESET - slot freed by force", (LONG)(n + 1));
        }
    }
    for (n = 0; n < S->nodes; n++) {
        struct NodeInfo *ni = &S->node[n];
        if (ni->state == NS_STARTING && now - ni->connected > 60) {
            /* BBSNode never picked the socket up: reclaim and drop it */
            LONG fd = ObtainSocket(ni->sockid, AF_INET, SOCK_STREAM, 0);
            if (fd >= 0) CloseSocket(fd);
            free_node(n);
            say("node %ld failed to start - freed", (LONG)(n + 1));
        } else if (ni->state >= NS_LOGIN && !task_alive(ni->task)) {
            free_node(n);
            say("node %ld process vanished - freed", (LONG)(n + 1));
        }
    }
    shared_unlock(S);
    serial_keep(now);
    shared_lock(S);
    if (S->bans_dirty && now - last_save >= 5) {
        ipf_save_bans(S, BBS_BANS);
        last_save = now;
    }
    shared_unlock(S);
}

static int active_nodes(void)
{
    int n, c = 0;
    shared_lock(S);
    for (n = 0; n < S->nodes; n++) if (S->node[n].state != NS_FREE) c++;
    shared_unlock(S);
    return c;
}

static void load_policy(void)
{
    const char *m;
    S->flood_conns    = (UWORD)cfg_int(C, "flood_connects", 5);
    S->flood_secs     = (UWORD)cfg_int(C, "flood_seconds", 30);
    S->flood_ban_mins = (UWORD)cfg_int(C, "flood_ban_minutes", 60);
    S->fail_logins    = (UWORD)cfg_int(C, "failed_logins", 5);
    S->fail_ban_mins  = (UWORD)cfg_int(C, "failed_ban_minutes", 30);

    str_copy(g_nodecmd, cfg_str(C, "node_command", "BBS:BBSNode"), sizeof(g_nodecmd));
    m = cfg_str(C, "deny_message", "\\r\\nAccess denied.\\r\\n");
    str_copy(g_denymsg, m, sizeof(g_denymsg)); unescape(g_denymsg);
    m = cfg_str(C, "ban_message", "\\r\\nYour address is banned from this system.\\r\\n");
    str_copy(g_banmsg, m, sizeof(g_banmsg)); unescape(g_banmsg);
    m = cfg_str(C, "busy_message", "\\r\\nAll nodes are busy - please call back later.\\r\\n");
    str_copy(g_busymsg, m, sizeof(g_busymsg)); unescape(g_busymsg);
}

/* BBS: must exist; default it to the program's own drawer */
static BOOL ensure_bbs_assign(void)
{
    BPTR l = Lock((STRPTR)"BBS:", ACCESS_READ);
    if (l) { UnLock(l); return TRUE; }
    l = DupLock(GetProgramDir());
    if (!l) return FALSE;
    if (!AssignLock((STRPTR)"BBS", l)) { UnLock(l); return FALSE; }
    return TRUE;
}

static int real_main(void)
{
    struct RDArgs *rda;
    LONG args[3] = { 0, 0, 0 };
    LONG port, nodes, i;
    BOOL added = FALSE;
    int rc = RETURN_FAIL;

    rda = ReadArgs((STRPTR)"PORT/N,NODES/N,QUIET/S", args, NULL);
    if (!rda) { PrintFault(IoErr(), (STRPTR)"NilBBS"); return RETURN_FAIL; }
    g_quiet = args[2] != 0;

    if (!ensure_bbs_assign()) { PutStr((STRPTR)"NilBBS: can't assign BBS:\n"); goto out; }
    if (shared_find()) { PutStr((STRPTR)"NilBBS is already running.\n"); goto out; }

    C = cfg_load(bbs_config());
    port  = args[0] ? *(LONG *)args[0] : cfg_int(C, "port", 23);
    nodes = args[1] ? *(LONG *)args[1] : cfg_int(C, "nodes", 4);
    if (nodes < 1) nodes = 1;
    if (nodes > MAX_NODES) nodes = MAX_NODES;

    S = AllocVec(sizeof(*S), MEMF_PUBLIC | MEMF_CLEAR);
    if (!S) { PutStr((STRPTR)"NilBBS: out of memory\n"); goto out; }
    strcpy(S->semname, BBS_SEMNAME);
    S->sem.ss_Link.ln_Name = S->semname;
    S->sem.ss_Link.ln_Pri  = 0;
    S->magic   = BBS_SHARED_MAGIC;
    S->version = BBS_SHARED_VER;
    S->nodes   = (UWORD)nodes;
    S->port    = (UWORD)port;
    S->daemon  = FindTask(NULL);
    S->started = bbs_now();
    for (i = 0; i < MAX_NODES; i++) S->node[i].userid = -1;
    InitSemaphore(&S->userlock);
    InitSemaphore(&S->msglock);
    InitSemaphore(&S->filelock);
    load_policy();
    ipf_load_rules(S, BBS_IPRULES);
    i = ipf_load_bans(S, BBS_BANS);
    AddSemaphore(&S->sem);          /* also initialises it */
    added = TRUE;

    Printf((STRPTR)"NilBBS %s - %ld nodes, %ld IP rules, %ld bans loaded\n",
           (LONG)BBS_VERSION, nodes, (LONG)S->nrules, i);

    if (!(SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 4))) {
        PutStr((STRPTR)"NilBBS: no bsdsocket.library - start your TCP/IP stack first\n");
        goto out;
    }

    for (i = 0; !listen_up((UWORD)port); i++) {
        if (CheckSignal(SIGBREAKF_CTRL_C)) goto out;
        if (i == 0) Printf((STRPTR)"NilBBS: can't listen on port %ld yet (errno %ld), retrying...\n",
                           port, Errno());
        Delay(i < 30 ? 100 : 500);
    }
    say("NilBBS " BBS_VERSION " listening on port %ld", port);
    if (cfg_str(C, "serial_device", "")[0])
        say("serial line: %s unit %ld, %ld baud, %s", cfg_str(C, "serial_device", ""), cfg_int(C, "serial_unit", 0),
            cfg_int(C, "serial_baud", 19200), cfg_str(C, "modem_init", "")[0] ? "modem" : "direct");
    load_events();
    rexx_open();
    g_self = FindTask(NULL);
    g_evsig = AllocSignal(-1);
    if (g_rexx) say("ARexx port NILBBS open, %ld event%s scheduled", (LONG)g_nev, g_nev == 1 ? "" : "s");
    g_lastmin = bbs_now() / 60;
    {
        LONG fp = cfg_int(C, "finger_port", 0);
        if (fp > 0) {
            if ((g_finger = listen_on((UWORD)fp)) >= 0) say("finger service on port %ld", fp);
            else say("can't listen for finger on port %ld (errno %ld)", fp, Errno());
        }
    }

    for (;;) {
        fd_set rfds;
        struct timeval tv;
        ULONG rexxsig = g_rexx ? 1UL << g_rexx->mp_SigBit : 0;
        ULONG evsig = g_evsig >= 0 ? 1UL << g_evsig : 0;
        ULONG sigs = SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_E | SIGBREAKF_CTRL_F | rexxsig | evsig;
        LONG r;

        FD_ZERO(&rfds);
        FD_SET(g_listen, &rfds);
        if (g_finger >= 0) FD_SET(g_finger, &rfds);
        tv.tv_secs = 1;
        tv.tv_micro = 0;
        r = WaitSelect((g_finger > g_listen ? g_finger : g_listen) + 1, &rfds, NULL, NULL, &tv, &sigs);

        if ((sigs & SIGBREAKF_CTRL_C) || S->shutdown) break;
        if (sigs & SIGBREAKF_CTRL_F) {
            shared_lock(S);
            ipf_load_rules(S, BBS_IPRULES);
            shared_unlock(S);
            say("IP filter reloaded: %ld rules", (LONG)S->nrules);
        }
        if (g_rexx && (sigs & rexxsig)) rexx_handle();
        if (sigs & evsig) { g_evrunning = FALSE; g_closed = FALSE; say("event %s finished - open for calls again", g_evtag); }
        if (bbs_now() / 60 != g_lastmin) { g_lastmin = bbs_now() / 60; check_events(bbs_now()); }
        if (S->shutdown) break;
        if (r > 0 && FD_ISSET(g_listen, &rfds)) handle_connect();
        if (r > 0 && g_finger >= 0 && FD_ISSET(g_finger, &rfds)) handle_finger();
        housekeeping();
    }

    say("shutting down");
    rc = RETURN_OK;

out:
    if (g_listen >= 0) { CloseSocket(g_listen); g_listen = -1; }
    if (g_finger >= 0) { CloseSocket(g_finger); g_finger = -1; }
    rexx_close();
    if (g_evrunning) {
        /* the event's process will call event_exit() in our code as it ends */
        say("waiting for event %s to finish", g_evtag);
        Wait(1UL << g_evsig);
    }
    if (g_evsig >= 0) FreeSignal(g_evsig);
    if (S && added) {
        int n, waited;
        shared_lock(S);
        S->shutdown = 1;
        for (n = 0; n < S->nodes; n++)
            if (S->node[n].state >= NS_LOGIN && task_alive(S->node[n].task))
                Signal(S->node[n].task, SIGBREAKF_CTRL_C);
        shared_unlock(S);
        {   /* the serial line's waiting BBSNode (on a call it got the CTRL-C above too) */
            struct Task *t;
            Forbid();
            if ((t = FindTask((STRPTR)SERIAL_TASKNAME))) Signal(t, SIGBREAKF_CTRL_C);
            Permit();
        }
        for (waited = 0; waited < 30 && active_nodes(); waited++) {
            housekeeping();
            Delay(50);
        }
        shared_lock(S);
        if (S->bans_dirty) ipf_save_bans(S, BBS_BANS);
        shared_unlock(S);
        if (!active_nodes()) {
            RemSemaphore(&S->sem);
            FreeVec(S);
        } else {
            /* a node is still running and holds a pointer into S: leak it
             * rather than pull memory out from under a live process */
            PutStr((STRPTR)"NilBBS: nodes still active - shared block left in place\n");
            RemSemaphore(&S->sem);
        }
    } else if (S) FreeVec(S);
    if (SocketBase) CloseLibrary(SocketBase);
    if (C) cfg_free(C);
    FreeArgs(rda);
    return rc;
}

int main(void)
{
    return run_with_stack(16384, real_main);
}
