/*
 * BBSNode - one caller's session.  Started by NilBBS as
 *
 *   BBSNode NODE=<n> SOCKET=<id>
 *
 * where <id> is the ReleaseSocket() key for the accepted connection.
 */
#include <exec/types.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <proto/bsdsocket.h>

#include "node.h"

unsigned long __stack = 32768;
static const char __attribute__((used)) verstag[] = "$VER: BBSNode " BBS_VERSION " (" BBS_VERDATE ")";

struct Library *SocketBase = NULL;
struct NodeCtx N;

static char taskname[24];

extern LONG msg_unread_mail(void);
extern int users_waiting(void);          /* sysop.c */
extern int msg_mod_waiting(char *where, int size);   /* msgui.c */

/* what a caller sees between logging in and the main menu */
static void logon_sequence(void)
{
    LONG mail;
    int n;
    if (tshowfile("welcome")) tpause();
    if (tshowfile("news")) tpause();
    mail = msg_unread_mail();
    if (mail == 1) tputs(L("node.logon.mail_one", "\n|14You have |151|14 unread private message.|07\n"));
    else if (mail) tprintf(L("node.logon.mail_many", "\n|14You have |15%ld|14 unread private messages.|07\n"), mail);
    if (cfg_bool(N.cfg, "logon_bulletins", TRUE) && (n = bulletins_new()) > 0) {
        if (n == 1) tputs(L("node.logon.bulletins_one", "\n|141 bulletin changed since your last call.|07\n"));
        else tprintf(L("node.logon.bulletins_many", "\n|14%d bulletins changed since your last call.|07\n"), n);
        if (tyesno(L("node.logon.read_bulletins", "|07Read the bulletins now?"), TRUE)) bulletins();
    }
    if (N.user.flags & UF_NEWUSER)
        tputs(L("node.logon.awaiting_validation", "\n|14Your account is waiting for the sysop to validate it.|07\n"));
    {
        char where[LONGNAME];
        int nm = msg_mod_waiting(where, sizeof(where));
        if (nm == 1) tprintf(L("node.logon.moderation_one", "\n|141 post is waiting for moderation|07 |08(%s: R, then M)|07\n"), where);
        else if (nm) tprintf(L("node.logon.moderation_many", "\n|14%d posts are waiting for moderation|07 |08(%s and others: R, then M)|07\n"), nm, where);
    }
    if (N.sysop && (n = users_waiting()) > 0) {
        if (n == 1) tputs(L("node.logon.newusers_one", "\n|141 new user is waiting to be validated|07 |08(sysop menu: V)|07\n"));
        else tprintf(L("node.logon.newusers_many", "\n|14%d new users are waiting to be validated|07 |08(sysop menu: V)|07\n"), n);
    }
    if (cfg_bool(N.cfg, "logon_voting", TRUE) && (n = voting_waiting()) > 0) {
        if (n == 1) tputs(L("node.logon.voting_one", "\n|141 voting topic is waiting for your vote|07 |08(voting booth: V)|07\n"));
        else tprintf(L("node.logon.voting_many", "\n|14%d voting topics are waiting for your vote|07 |08(voting booth: V)|07\n"), n);
    }
    tprintf(L("node.logon.welcome_back", "\n|07Welcome back, |15%s|07. This is call |15#%lu|07; you have |15%s|07 minutes.\n"),
            N.user.name, N.user.calls, "|TL");
    if (cfg_bool(N.cfg, "logon_lastcallers", TRUE)) lastcallers_show();
    if (cfg_bool(N.cfg, "logon_oneliners", TRUE)) oneliners();
}

/* ---- sysop RESET (common/shared.c node_reset_start) -------------------------
 * A node stuck behind a door that won't end gives its slot back at once: the
 * slot is cleared for the next caller (a single = yes door's lock goes with it)
 * and N.ni is pointed at a private stand-in, so nothing this process does from
 * here on - it still has to outlast the door, whose launcher returns into our
 * code - touches the node table again. */
static struct NodeInfo g_detached;

BOOL node_reset_wanted(void)
{
    return N.ni && N.ni != &g_detached && N.ni->reset;
}

void node_release_slot(void)
{
    struct Task *daemon;
    if (!N.ni || N.ni == &g_detached) return;
    shared_lock(N.S);
    memset(N.ni, 0, sizeof(*N.ni));
    N.ni->userid = -1;
    daemon = N.S->daemon;
    shared_unlock(N.S);
    N.ni = &g_detached;
    N.online = FALSE;
    if (N.sock >= 0) { CloseSocket(N.sock); N.sock = -1; }   /* the caller is gone too */
    bbs_log(BBS_SYSLOG, "node %d: reset by the sysop - slot freed, the stuck door is left behind", N.node);
    if (daemon) Signal(daemon, SIGBREAKF_CTRL_E);
}

/* the daemon freed our slot by force (we didn't answer a RESET in time): let go */
void node_check_slot(void)
{
    if (!N.ni || N.ni == &g_detached) return;
    if (N.ni->task != FindTask(NULL)) {
        N.ni = &g_detached;
        N.online = FALSE;
        if (N.sock >= 0) { CloseSocket(N.sock); N.sock = -1; }
    }
}

void set_activity(const char *act)
{
    if (!N.ni) return;
    shared_lock(N.S);
    str_copy(N.ni->activity, act, LONGNAME);
    shared_unlock(N.S);
}

/* what BBSControl / WHO show between commands: the menu the caller is in - and, for a menu
   whose prompt shows the file area (|FA), which area */
static char g_menu_act[LONGNAME] = "Main menu";
static BOOL g_menu_area;
void back_to_menu(void)
{
    char act[LONGNAME];
    const char *fa = g_menu_area ? file_area_name() : "";
    if (fa[0]) { snprintf(act, sizeof(act), "%s (%s)", g_menu_act, fa); set_activity(act); }
    else set_activity(g_menu_act);
}
void set_menu_activity(const char *title, BOOL with_area)
{
    str_copy(g_menu_act, title, LONGNAME);
    g_menu_area = with_area;
    back_to_menu();
}

static int real_main(void)
{
    struct RDArgs *rda;
    LONG args[3] = { 0, 0, 0 };
    struct Task *me = FindTask(NULL);
    APTR oldwin;
    char *oldname;
    int rc = RETURN_FAIL;

    memset(&N, 0, sizeof(N));
    N.sock = -1;

    rda = ReadArgs((STRPTR)"NODE/N,SOCKET/N,LOCAL/S", args, NULL);
    if (!rda) { PrintFault(IoErr(), (STRPTR)"BBSNode"); return RETURN_FAIL; }
    N.local = args[2] != 0;
    if (!N.local && (!args[0] || !args[1])) {
        PutStr((STRPTR)"BBSNode is started by NilBBS.  For a local logon: BBSNode LOCAL\n");
        goto out;
    }
    if (!(N.S = shared_find())) {
        if (N.local) PutStr((STRPTR)"BBSNode: NilBBS isn't running.\n");
        goto out;
    }
    if (N.local) {
        /* claim a free node for ourselves (the daemon hands out the rest) */
        int n, want = args[0] ? (int)*(LONG *)args[0] : 0;
        shared_lock(N.S);
        for (n = 1; n <= N.S->nodes && !N.node; n++)
            if ((!want || want == n) && N.S->node[n - 1].state == NS_FREE) {
                memset(&N.S->node[n - 1], 0, sizeof(struct NodeInfo));
                N.S->node[n - 1].state = NS_LOGIN;
                N.S->node[n - 1].task = me;
                N.S->node[n - 1].local = 1;
                N.S->node[n - 1].connected = bbs_now();
                N.node = n;
            }
        shared_unlock(N.S);
        if (!N.node) { PutStr((STRPTR)"BBSNode: no free node.\n"); goto out; }
    } else N.node = (int)*(LONG *)args[0];
    if (N.node < 1 || N.node > N.S->nodes) goto out;
    N.ni = &N.S->node[N.node - 1];

    /* no "insert volume" requesters: a node has no one at the console */
    oldwin = ((struct Process *)me)->pr_WindowPtr;
    ((struct Process *)me)->pr_WindowPtr = (APTR)-1L;
    oldname = me->tc_Node.ln_Name;
    sprintf(taskname, "NilBBS Node %d", N.node);
    me->tc_Node.ln_Name = taskname;

    if (N.local) {
        char spec[80];
        /* network doors still work if a stack is running; nothing else needs it */
        SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 4);
        sprintf(spec, "CON:0/12/640/244/NilBBS - local logon, node %d/CLOSE", N.node);
        if (!(N.lcon = Open((STRPTR)spec, MODE_NEWFILE))) goto out_free;
        SetMode(N.lcon, 1);                     /* raw: key by key, no echo */
    } else {
        if (!(SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 4))) goto out_free;
        N.sock = ObtainSocket(*(LONG *)args[1], AF_INET, SOCK_STREAM, 0);
        if (N.sock < 0) goto out_free;
    }

    N.cfg = cfg_load(bbs_config());
    lang_board();                               /* the login screens: the board's language */
    diz_env.cfg = N.cfg;                        /* FILE_ID.DIZ handling (dizcore.c) */
    sprintf(diz_env.work, "BBS:Nodes/Node%d", N.node);
    diz_env.lock = &N.S->filelock;
    diz_env.uploader = N.user.name;
    N.online = TRUE;
    N.last_input = bbs_now();
    N.fg = 7;
    N.term = TT_ASCII;
    N.charset = CS_ASCII;

    shared_lock(N.S);
    N.ip = N.ni->ip;
    N.ni->task = me;
    N.ni->state = NS_LOGIN;
    N.ni->beat = bbs_now();
    N.ni->userid = -1;
    str_copy(N.ni->activity, "Logging in", LONGNAME);
    shared_unlock(N.S);
    if (N.local) strcpy(N.ipstr, "local");
    else {
        LONG one = 1;
        ip_tostr(N.ip, N.ipstr);
        setsockopt(N.sock, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    }

    {
        /* per-node work drawer for drop files and door scripts */
        char dir[40];
        BPTR l;
        sprintf(dir, "BBS:Nodes/Node%d", N.node);
        if ((l = Lock((STRPTR)dir, ACCESS_READ))) UnLock(l);
        else if ((l = CreateDir((STRPTR)dir))) UnLock(l);
    }

    tn_start();
    if (N.local) {
        /* the Amiga console: ANSI colours, Amiga characters */
        N.term = TT_ANSI;
        N.charset = CS_LATIN1;
        if (N.ni) { N.ni->cols = N.cols; N.ni->rows = N.rows; N.ni->termtype = N.term; }
    } else tdetect();

    if (do_login()) {
        lang_user();                            /* the caller's own language from here on */
        conf_load();
        msg_areas_load();
        file_areas_load();
        logon_sequence();
        set_activity("Main menu");
        menu_run(cfg_str(N.cfg, "start_menu", "main"));
        do_logoff();
    }
    rc = RETURN_OK;
    tn_flush();

out_free:
    if (N.lcon) {
        SetMode(N.lcon, 0);
        if (N.online) Delay(50);                /* let the goodbye be read */
        Close(N.lcon);
    }
    if (N.sock >= 0) {
        /* let queued output drain before the socket goes */
        shutdown(N.sock, 1);
        Delay(25);
        CloseSocket(N.sock);
    }
    if (SocketBase) CloseLibrary(SocketBase);
    if (N.cfg) cfg_free(N.cfg);
    lang_free();

    node_check_slot();                          /* not ours any more after a forced RESET */
    if (N.ni) {
        struct Task *daemon;
        shared_lock(N.S);
        memset(N.ni, 0, sizeof(*N.ni));
        N.ni->userid = -1;
        daemon = N.S->daemon;
        shared_unlock(N.S);
        if (daemon) Signal(daemon, SIGBREAKF_CTRL_E);
    }
    me->tc_Node.ln_Name = oldname;
    ((struct Process *)me)->pr_WindowPtr = oldwin;
out:
    FreeArgs(rda);
    return rc;
}

/* a local logon is started from a Shell, whose stack is usually far too small */
int main(void)
{
    return run_with_stack(32768, real_main);
}
