/*
 * BBSCtl - sysop control of a running NilBBS from any Shell.
 *
 *   BBSCtl WHO                         nodes and callers
 *   BBSCtl BANS                        IP rules and bans
 *   BBSCtl BAN <ip> [<minutes>] [reason...]   0 / omitted = permanent
 *   BBSCtl UNBAN <ip>
 *   BBSCtl KICK <node>
 *   BBSCtl RESET <node>                force a node stuck in a door free again
 *   BBSCtl SEND <node|ALL> <text...>   message to callers
 *   BBSCtl RELOAD                      re-read Config/IPFilter.cfg
 *   BBSCtl SHUTDOWN                    hang everyone up and stop the daemon
 *
 * These two work whether or not NilBBS is running (the installer uses them):
 *   BBSCtl SET <key> "<value>"         set a key in BBS:Config/NilBBS.cfg (comments kept)
 *   BBSCtl ADDSYSOP <handle> <password>   create the sysop account (or reset its
 *                                      password and level if the handle exists)
 * with DIR=<drawer> they work on that BBS drawer instead of BBS: (the installer
 * sets up a new drawer without touching an assign a running BBS may be using).
 */
#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "../common/bbs.h"
#include "ini.h"

static const char __attribute__((used)) verstag[] = "$VER: BBSCtl " BBS_VERSION " (" BBS_VERDATE ")";

static const char *statename(UBYTE s)
{
    switch (s) {
    case NS_FREE: return "waiting";
    case NS_STARTING: return "connecting";
    case NS_LOGIN: return "logging in";
    case NS_ONLINE: return "online";
    case NS_DOOR: return "in a door";
    }
    return "?";
}

static void who(struct BBSShared *S)
{
    int n;
    ULONG now = bbs_now();
    char up[20];
    bbs_datetimestr(S->started, up);
    Printf((STRPTR)"NilBBS on port %ld, up since %s, %ld calls\n\n",
           (LONG)S->port, (LONG)up, (LONG)S->total_calls);
    PutStr((STRPTR)"Node  State        User                  From             Mins  Activity\n");
    shared_lock(S);
    for (n = 0; n < S->nodes; n++) {
        struct NodeInfo *ni = &S->node[n];
        char ip[16];
        char line[160];
        ip_tostr(ni->ip, ip);
        if (ni->state == NS_FREE) sprintf(line, "%4d  %-12s\n", n + 1, statename(ni->state));
        else sprintf(line, "%4d  %-12s %-20.20s  %-15s  %4lu  %s\n", n + 1, statename(ni->state),
                     ni->user[0] ? ni->user : "-", ip, (now - ni->connected) / 60, ni->activity);
        PutStr((STRPTR)line);
    }
    shared_unlock(S);
}

static void bans(struct BBSShared *S)
{
    int i, shown = 0;
    ULONG now = bbs_now();
    shared_lock(S);
    Printf((STRPTR)"Default policy: %s\n", (LONG)(S->default_deny ? "deny" : "allow"));
    for (i = 0; i < S->nrules; i++) {
        char r[40];
        ipf_rule_str(&S->rules[i], r);
        Printf((STRPTR)"  rule  %s\n", (LONG)r);
    }
    PutStr((STRPTR)"\nBanned address   Expires        Reason\n");
    for (i = 0; i < MAX_BANS; i++) {
        struct IPBan *b = &S->bans[i];
        char ip[16], ex[24], line[120];
        if (!b->ip) continue;
        ip_tostr(b->ip, ip);
        if (!b->expires) strcpy(ex, "never");
        else sprintf(ex, "in %lu min", (b->expires - now + 59) / 60);
        sprintf(line, "%-15s  %-13s  %s\n", ip, ex, b->reason);
        PutStr((STRPTR)line);
        shown++;
    }
    shared_unlock(S);
    if (!shown) PutStr((STRPTR)"(none)\n");
}

static void send_msg(struct BBSShared *S, int node, const char *text)
{
    int n;
    for (n = 1; n <= S->nodes; n++) {
        struct NodeInfo *ni = &S->node[n - 1];
        struct Task *t = NULL;
        if (node && n != node) continue;
        shared_lock(S);
        if (ni->state >= NS_ONLINE) {
            UBYTE next = (ni->msg_head + 1) % NODE_MSGQ;
            if (next != ni->msg_tail) {
                struct NodeMsg *m = &ni->msgq[ni->msg_head];
                m->from = 0;
                m->type = NM_SYSOP;
                strcpy(m->fromname, "Sysop");
                str_copy(m->text, text, sizeof(m->text));
                ni->msg_head = next;
                t = ni->task;
            }
        }
        shared_unlock(S);
        if (t) { Signal(t, SIGBREAKF_CTRL_D); Printf((STRPTR)"sent to node %ld\n", (LONG)n); }
    }
}

/* BBSCtl SET key value - one line of NilBBS.cfg, the rest of the file untouched */
static char cfgpath[300], logpath[300] = BBS_SYSLOG, userpath[300];      /* cfgpath: bbs_config() at start */
static BOOL own_dir;                     /* DIR= given: a drawer that isn't the running BBS */

static int set_key(const char *key, const char *val)
{
    struct Ini *ini;
    if (!key || !*key || !val) { PutStr((STRPTR)"Usage: BBSCtl SET <key> \"<value>\"\n"); return RETURN_ERROR; }
    if (!file_exists(cfgpath) || !(ini = ini_load(cfgpath))) { Printf((STRPTR)"Can't read %s\n", (LONG)cfgpath); return RETURN_FAIL; }
    ini_set(ini, NULL, key, val);
    if (!ini_save(ini)) { ini_free(ini); Printf((STRPTR)"Can't write %s\n", (LONG)cfgpath); return RETURN_FAIL; }
    ini_free(ini);
    Printf((STRPTR)"%s = %s\n", (LONG)key, (LONG)val);
    return RETURN_OK;
}

/* BBSCtl ADDSYSOP handle password - the sysop account, at sysop_level */
static int add_sysop(struct BBSShared *S, const char *name, const char *pw)
{
    struct UserRec *u;
    struct Cfg *c;
    LONG id, level;
    const char *p;
    if (!name || !pw) { PutStr((STRPTR)"Usage: BBSCtl ADDSYSOP <handle> <password>\n"); return RETURN_ERROR; }
    if (strlen(name) < 2 || strlen(name) >= NAMELEN) { PutStr((STRPTR)"The handle must be 2 to 31 characters.\n"); return RETURN_ERROR; }
    for (p = name; *p; p++)
        if (*p < ' ' || *p == '|' || *p == ':' || *p == '/') { PutStr((STRPTR)"The handle can't contain | : / or control characters.\n"); return RETURN_ERROR; }
    if (strlen(pw) < 4) { PutStr((STRPTR)"The password must be at least 4 characters.\n"); return RETURN_ERROR; }
    c = cfg_load(cfgpath);
    level = c ? cfg_int(c, "sysop_level", 255) : 255;
    if (c) cfg_free(c);
    if (!(u = AllocVec(sizeof(struct UserRec), MEMF_CLEAR))) return RETURN_FAIL;
    if (S) ObtainSemaphore(&S->userlock);
    if ((id = userdb_find(name, u))) {
        u->level = (UBYTE)level;
        u->flags &= ~(UF_LOCKED | UF_DELETED);
        user_setpass(u, pw);
        userdb_write(u);
    } else {
        memset(u, 0, sizeof(*u));
        str_copy(u->name, name, NAMELEN);
        str_copy(u->realname, name, LONGNAME);
        u->level = (UBYTE)level;
        u->flags = UF_HOTKEYS;
        u->cols = 80; u->rows = 24;
        u->firstcall = bbs_now();
        user_setpass(u, pw);
        id = userdb_add(u);
    }
    if (S) ReleaseSemaphore(&S->userlock);
    FreeVec(u);
    if (!id) { PutStr((STRPTR)"Couldn't write BBS:Data/Users.dat\n"); return RETURN_FAIL; }
    Printf((STRPTR)"Sysop account %s is user #%ld (level %ld).\n", (LONG)name, id, level);
    bbs_log(logpath, "BBSCtl: sysop account %s set up (#%ld)", name, id);
    return RETURN_OK;
}

/* ---- WATCH n: see what a caller sees, live, without them knowing (node/spy.c sends the copies) ----
 * The stream is the caller's telnet bytes: IAC codes are dropped, ANSI kept only where the Amiga
 * console understands it (cursor moves, clears - colours off: the Workbench palette isn't ANSI's),
 * and the PC (CP437) line drawing becomes + - | #.  Stops on Ctrl-C, the window's close gadget,
 * Q + Return, or when the caller leaves. */
#define CP437_HI \
    "CueaaaaceeeiiiAAEaAooouuyOUcLY?f"          /* 0x80 */ \
    "aiounNao?++24!<>"                          /* 0xA0 */ \
    "###||||+++|+++++"                          /* 0xB0 */ \
    "++++-++++++++-++"                          /* 0xC0 */ \
    "+++++++++++#_||\""                         /* 0xD0 */ \
    "aBGpSsmtFTOd8fen"                          /* 0xE0 */ \
    "=+><()/=o..vn2# "                          /* 0xF0 */
static const char cp437_hi[] = CP437_HI;
typedef char cp437_hi_is_128[(sizeof(CP437_HI) == 129) ? 1 : -1];

static int wt_state;                       /* telnet: 0 data, 1 after IAC, 2 option byte, 3 in SB, 4 IAC in SB */
static int esc_state;                      /* 0, 1 after ESC, 2 in CSI */
static char csi[24];
static int csilen;

static void wt_out(BPTR out, char *buf, int *n, const char *s, int len)
{
    while (len-- > 0) {
        if (*n >= 250) { Write(out, buf, *n); *n = 0; }
        buf[(*n)++] = *s++;
    }
}

static void watch_bytes(BPTR out, const UBYTE *p, LONG len)
{
    char buf[256];
    int n = 0;
    while (len-- > 0) {
        UBYTE c = *p++;
        switch (wt_state) {                      /* telnet first */
        case 1:
            if (c == 255) break;                  /* IAC IAC = a real 0xFF: fall through below */
            wt_state = (c >= 251 && c <= 254) ? 2 : c == 250 ? 3 : 0;
            continue;
        case 2: wt_state = 0; continue;
        case 3: if (c == 255) wt_state = 4; continue;
        case 4: wt_state = (c == 240) ? 0 : 3; continue;
        default:
            if (c == 255) { wt_state = 1; continue; }
        }
        wt_state = 0;
        if (esc_state == 1) {
            if (c == '[') { esc_state = 2; csilen = 0; continue; }
            esc_state = 0; continue;              /* ESC ( B and friends: dropped */
        }
        if (esc_state == 2) {
            if (c >= 0x40 && c <= 0x7e) {         /* the final byte */
                csi[csilen] = 0;
                esc_state = 0;
                if (csi[0] == '?' || csi[0] == '=') continue;             /* private modes: dropped */
                if (c == 'J' && !strcmp(csi, "2")) { wt_out(out, buf, &n, "\x9bH\x9bJ", 4); continue; }
                if (strchr("ABCDHJKf", c)) {
                    char seq[30];
                    int l = sprintf(seq, "\x9b%s%c", csi, c == 'f' ? 'H' : c);
                    wt_out(out, buf, &n, seq, l);
                }
                continue;                          /* m (colours) and the rest: dropped */
            }
            if (csilen < (int)sizeof(csi) - 1) csi[csilen++] = c;
            continue;
        }
        if (c == 27) { esc_state = 1; continue; }
        if (c == 7 || c == 0) continue;           /* no bell on the sysop's Amiga */
        if (c >= 0x80) { char t = cp437_hi[c - 0x80]; wt_out(out, buf, &n, &t, 1); continue; }
        { char t = (char)c; wt_out(out, buf, &n, &t, 1); }
    }
    if (n) Write(out, buf, n);
}

static int watch_node(struct BBSShared *S, int nd)
{
    struct MsgPort *port;
    char name[32], user[NAMELEN], line[8];
    BPTR in = Input(), out = Output();
    int rc = RETURN_OK, idle = 0;
    if (nd < 1 || nd > S->nodes || S->node[nd - 1].state == NS_FREE) { PutStr((STRPTR)"Nobody on that node.\n"); return RETURN_WARN; }
    sprintf(name, SPY_PORTFMT, nd);
    if (!(port = CreateMsgPort())) return RETURN_FAIL;
    Forbid();
    if (FindPort((STRPTR)name)) { Permit(); DeleteMsgPort(port); PutStr((STRPTR)"That node is already being watched.\n"); return RETURN_WARN; }
    port->mp_Node.ln_Name = name;
    port->mp_Node.ln_Pri = 0;
    AddPort(port);
    Permit();
    str_copy(user, S->node[nd - 1].user[0] ? S->node[nd - 1].user : "(logging in)", sizeof(user));
    Printf((STRPTR)"\x9b" "0 p\x0cWatching node %ld: %s  -  close the window, or Q + Return, to stop.\n\n", (LONG)nd, (LONG)user);
    Write(out, "\x9b" " p", 3);               /* cursor back on */
    wt_state = esc_state = 0;
    S->node[nd - 1].spy = 1;                   /* the node starts copying (history first) */
    bbs_log(BBS_SYSLOG, "BBSCtl: sysop watching node %ld (%s)", (LONG)nd, user);
    for (;;) {
        struct SpyMsg *m;
        while ((m = (struct SpyMsg *)GetMsg(port))) { watch_bytes(out, m->data, m->len); FreeVec(m); }
        if (CheckSignal(SIGBREAKF_CTRL_C)) break;
        if (S->node[nd - 1].state == NS_FREE) {
            if (++idle > 4) { PutStr((STRPTR)"\n\n-- the caller has left --\n"); Delay(100); break; }
        } else idle = 0;
        if (in && WaitForChar(in, 200000)) {      /* 0.2 s: also the loop's tick */
            LONG r = Read(in, line, sizeof(line));
            if (r <= 0) break;                    /* the close gadget (EOF) */
            if (line[0] == 'q' || line[0] == 'Q') break;
        } else if (!in) Delay(10);
    }
    S->node[nd - 1].spy = 0;
    Forbid();
    RemPort(port);
    Permit();
    { struct Message *m; while ((m = GetMsg(port))) FreeVec(m); }
    DeleteMsgPort(port);
    return rc;
}

int main(void)
{
    struct RDArgs *rda;
    LONG args[5] = { 0, 0, 0, 0, 0 };
    struct BBSShared *S;
    const char *cmd;
    int rc = RETURN_OK;

    rda = ReadArgs((STRPTR)"COMMAND/A,ARG1,ARG2,DIR/K,REST/F", args, NULL);
    if (!rda) {
        PutStr((STRPTR)"Usage: BBSCtl WHO|BANS|BAN ip [mins] [reason]|UNBAN ip|KICK node|RESET node|"
                       "SEND node|ALL text|RELOAD|SHUTDOWN|SET key value|ADDSYSOP handle password|WATCH node\n");
        return RETURN_ERROR;
    }
    cmd = (const char *)args[0];
    str_copy(cfgpath, bbs_config(), sizeof(cfgpath));
    if (args[3]) {                                  /* DIR=<BBS drawer> */
        const char *d = (const char *)args[3];
        own_dir = TRUE;
        str_copy(cfgpath, d, 250); AddPart((STRPTR)cfgpath, (STRPTR)"Config/NilBBS.cfg", sizeof(cfgpath));
        if (!file_exists(cfgpath)) {             /* a board set up before the rename */
            str_copy(cfgpath, d, 250); AddPart((STRPTR)cfgpath, (STRPTR)"Config/" "NuzBBS.cfg", sizeof(cfgpath));
            if (!file_exists(cfgpath)) { str_copy(cfgpath, d, 250); AddPart((STRPTR)cfgpath, (STRPTR)"Config/NilBBS.cfg", sizeof(cfgpath)); }
        }
        str_copy(logpath, d, 250); AddPart((STRPTR)logpath, (STRPTR)"Logs/System.log", sizeof(logpath));
        str_copy(userpath, d, 250); AddPart((STRPTR)userpath, (STRPTR)"Data/Users.dat", sizeof(userpath));
        userdb_path = userpath;
    }
    if (!str_icmp(cmd, "SET")) {
        rc = set_key((const char *)args[1], (const char *)args[2]);
        FreeArgs(rda);
        return rc;
    }
    if (!str_icmp(cmd, "ADDSYSOP")) {
        rc = add_sysop(own_dir ? NULL : shared_find(), (const char *)args[1], (const char *)args[2]);
        FreeArgs(rda);
        return rc;
    }
    if (!(S = shared_find())) {
        PutStr((STRPTR)"NilBBS isn't running.\n");
        FreeArgs(rda);
        return RETURN_WARN;
    }

    if (!str_icmp(cmd, "WHO")) who(S);
    else if (!str_icmp(cmd, "BANS")) bans(S);
    else if (!str_icmp(cmd, "BAN") || !str_icmp(cmd, "UNBAN")) {
        ULONG ip;
        if (!args[1] || !ip_parse((char *)args[1], &ip)) {
            PutStr((STRPTR)"Give an IP address, e.g. BBSCtl BAN 203.0.113.9 60 spammer\n");
            rc = RETURN_ERROR;
        } else if (!str_icmp(cmd, "BAN")) {
            ULONG mins = args[2] ? strtoul((char *)args[2], NULL, 10) : 0;
            char why[48];
            BOOL ok, white;
            shared_lock(S);
            white = ipf_whitelisted(S, ip);
            /* the reason is REST/F = args[4] (args[3] is DIR/K - reading it lost every reason) */
            ok = ipf_ban(S, ip, mins, args[4] ? (const char *)args[4] : "");   /* none: keep the old one */
            { struct IPBan *b = ipf_findban(S, ip); str_copy(why, b ? b->reason : "", sizeof(why)); }
            shared_unlock(S);
            if (white) PutStr((STRPTR)"Warning: an allow rule covers that address, so the ban has no effect.\n");
            if (ok) {
                Printf((STRPTR)"Banned %s %s.\n", args[1], (LONG)(mins ? "for a while" : "permanently"));
                bbs_log(BBS_SYSLOG, "BBSCtl: banned %s (%lu min: %s)", (char *)args[1], mins, why);
            } else { PutStr((STRPTR)"Ban table is full.\n"); rc = RETURN_ERROR; }
        } else {
            BOOL ok;
            shared_lock(S);
            ok = ipf_unban(S, ip);
            shared_unlock(S);
            Printf((STRPTR)"%s\n", (LONG)(ok ? "Unbanned." : "That address isn't banned."));
            if (ok) bbs_log(BBS_SYSLOG, "BBSCtl: unbanned %s", (char *)args[1]);
        }
    }
    else if (!str_icmp(cmd, "KICK")) {
        int n = args[1] ? atoi((char *)args[1]) : 0;
        struct Task *t = NULL;
        shared_lock(S);
        if (n >= 1 && n <= S->nodes && S->node[n - 1].state >= NS_LOGIN) t = S->node[n - 1].task;
        shared_unlock(S);
        if (t) { Signal(t, SIGBREAKF_CTRL_C); Printf((STRPTR)"Node %ld disconnected.\n", (LONG)n); }
        else { PutStr((STRPTR)"Nobody on that node.\n"); rc = RETURN_WARN; }
    }
    else if (!str_icmp(cmd, "WATCH")) rc = watch_node(S, args[1] ? atoi((char *)args[1]) : 0);
    else if (!str_icmp(cmd, "RESET")) {
        /* hang up + free the slot even if a door won't end; the daemon forces it
         * after NODE_RESET_FORCE seconds if the node doesn't answer at all */
        int n = args[1] ? atoi((char *)args[1]) : 0, i;
        if (!node_reset_start(S, n)) { PutStr((STRPTR)"Nobody on that node.\n"); rc = RETURN_WARN; }
        else {
            bbs_log(BBS_SYSLOG, "BBSCtl: reset node %ld", (LONG)n);
            for (i = 0; i < (NODE_RESET_FORCE + 5) * 2 && S->node[n - 1].state != NS_FREE; i++) Delay(25);
            if (S->node[n - 1].state != NS_FREE) { Printf((STRPTR)"Node %ld is still busy.\n", (LONG)n); rc = RETURN_WARN; }
            else if (i < NODE_RESET_FORCE * 2) Printf((STRPTR)"Node %ld reset - free again.\n", (LONG)n);
            else Printf((STRPTR)"Node %ld didn't answer - its slot was freed by force.\n", (LONG)n);
        }
    }
    else if (!str_icmp(cmd, "SEND")) {
        char text[120];
        int node;
        if (!args[1] || !args[2]) { PutStr((STRPTR)"BBSCtl SEND <node|ALL> <text>\n"); rc = RETURN_ERROR; }
        else {
            node = str_icmp((char *)args[1], "ALL") ? atoi((char *)args[1]) : 0;
            str_copy(text, (char *)args[2], sizeof(text));
            if (args[3]) { strncat(text, " ", sizeof(text) - strlen(text) - 1);
                           strncat(text, (char *)args[3], sizeof(text) - strlen(text) - 1); }
            send_msg(S, node, text);
        }
    }
    else if (!str_icmp(cmd, "RELOAD")) {
        if (S->daemon) Signal(S->daemon, SIGBREAKF_CTRL_F);
        PutStr((STRPTR)"Asked NilBBS to reload the IP filter.\n");
    }
    else if (!str_icmp(cmd, "SHUTDOWN")) {
        shared_lock(S);
        S->shutdown = 1;
        shared_unlock(S);
        if (S->daemon) Signal(S->daemon, SIGBREAKF_CTRL_C);
        PutStr((STRPTR)"NilBBS is shutting down.\n");
    }
    else {
        Printf((STRPTR)"Unknown command %s\n", (LONG)cmd);
        rc = RETURN_ERROR;
    }
    FreeArgs(rda);
    return rc;
}
