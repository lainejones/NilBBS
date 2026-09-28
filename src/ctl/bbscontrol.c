/*
 * BBSControl - the sysop's Workbench console for a running NilBBS.
 *
 * A GadTools window with the live node list (kick, kick + ban, message a
 * node or everyone) and buttons to reload Config/IPFilter.cfg, import new
 * files, shut the BBS down, and the one-liners / last callers cleanup (trim,
 * clear).  The IP ban list (unban, add a ban) is a window of its own, behind
 * the "Banned IPs..." button.  Refreshes every 2 s.
 *
 * The zoom gadget shrinks it to its title bar, which keeps a live summary
 * ("NilBBS: 2 on, 2 free"); iconify (OS 3.2's gadget, or Project > Iconify on
 * any Workbench) swaps the window for an AppIcon - double-click it to come back.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <devices/timer.h>
#include <intuition/intuition.h>
#include <intuition/gadgetclass.h>
#include <libraries/gadtools.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dostags.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>
#include <proto/graphics.h>
#include <workbench/workbench.h>
#include <workbench/startup.h>
#include <proto/wb.h>
#include <proto/icon.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "../common/bbs.h"

static const char __attribute__((used)) verstag[] = "$VER: BBSControl " BBS_VERSION " (" BBS_VERDATE ")";

struct IntuitionBase *IntuitionBase;
struct Library *GadToolsBase;
struct GfxBase *GfxBase;
struct Library *WorkbenchBase, *IconBase;

enum { GID_NODES = 1, GID_KICK, GID_KICKBAN, GID_MSG, GID_SEND, GID_SENDALL,
       GID_BANS, GID_UNBAN, GID_BANIP, GID_BANMIN, GID_BAN, GID_RELOAD, GID_SHUTDOWN, GID_STATUS,
       GID_IMPORT, GID_OLTRIM, GID_OLCLEAR, GID_LCCLEAR, GID_BANWIN, GID_BANCLOSE,
       GID_CHAT, GID_PAGECHAT, GID_PAGENO, GID_CHATLV, GID_CHATIN, GID_CHATEND, GID_RESET,
       GID_BANWHY, GID_LOGON, GID_BANLOG, GID_LOGLIST, GID_LOGREFRESH, GID_LOGCLOSE };
       /* new IDs go last: tests click gadgets by number (t35) */

#define MAXLINES (MAX_NODES > MAX_BANS ? MAX_NODES : MAX_BANS)

static struct BBSShared *S;
static struct Window *win, *banwin, *logwin;     /* logwin: BBS:Logs/Bans.log, newest first */
static struct Gadget *glist, *banglist, *g_nodes, *g_bans, *g_msg, *g_banip, *g_banmin, *g_banwhy, *g_status;
static WORD ban_w, ban_h;
static APTR vi;
static struct Menu *menus;
static struct Screen *pubscr;          /* the Workbench, for re-opening after an iconify */
static WORD win_x = 20, win_y = 20, win_h, zoomed;
static WORD zoombox[4];
static char wintitle[80] = "NilBBS Control";

enum { MN_ICONIFY = 1, MN_SHRINK, MN_QUIT };
static struct NewMenu newmenus[] = {
    { NM_TITLE, (STRPTR)"Project", NULL, 0, 0, NULL },
    { NM_ITEM, (STRPTR)"Iconify", (STRPTR)"I", 0, 0, (APTR)MN_ICONIFY },
    { NM_ITEM, (STRPTR)"Shrink / grow", (STRPTR)"S", 0, 0, (APTR)MN_SHRINK },
    { NM_ITEM, NM_BARLABEL, NULL, 0, 0, NULL },
    { NM_ITEM, (STRPTR)"Quit", (STRPTR)"Q", 0, 0, (APTR)MN_QUIT },
    { NM_END, NULL, NULL, 0, 0, NULL },
};

static struct List nodelist, banlist;
static struct Node nodenodes[MAX_NODES], bannodes[MAX_BANS];
static char nodetext[MAX_NODES][100], bantext[MAX_BANS][80];
static ULONG banip_of[MAX_BANS];
static LONG sel_node = -1, sel_ban = -1, nban;
static char statustext[100];

static void list_init(struct List *l)
{
    l->lh_Head = (struct Node *)&l->lh_Tail;
    l->lh_Tail = NULL;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
}

static const char *statename(UBYTE s)
{
    switch (s) {
    case NS_STARTING: return "connect";
    case NS_LOGIN:    return "login";
    case NS_ONLINE:   return "online";
    case NS_DOOR:     return "door";
    }
    return "waiting";
}

static void refresh(void)
{
    ULONG now = bbs_now();
    int i;
    char up[20];

    if (!(S = shared_find())) {
        strcpy(statustext, "NilBBS is not running");
        GT_SetGadgetAttrs(g_status, win, NULL, GTTX_Text, (ULONG)statustext, TAG_END);
        strcpy(wintitle, "NilBBS Control - not running");
        if (win) SetWindowTitles(win, (UBYTE *)wintitle, (UBYTE *)~0);
        return;
    }
    if (S->ctl_task != FindTask(NULL)) { shared_lock(S); S->ctl_task = FindTask(NULL); shared_unlock(S); }
    GT_SetGadgetAttrs(g_nodes, win, NULL, GTLV_Labels, ~0UL, TAG_END);
    GT_SetGadgetAttrs(g_bans, banwin, NULL, GTLV_Labels, ~0UL, TAG_END);
    list_init(&nodelist);
    list_init(&banlist);

    shared_lock(S);
    for (i = 0; i < S->nodes; i++) {
        struct NodeInfo *ni = &S->node[i];
        char ip[16];
        ip_tostr(ni->ip, ip);
        if (ni->state == NS_FREE) sprintf(nodetext[i], "%2d waiting", i + 1);
        else {
            /* the list is about 72 columns: keep the door's name in view ("In the X" -> "X") */
            const char *act = ni->activity;
            if (ni->state == NS_DOOR && !str_nicmp(act, "In the ", 7)) act += 7;
            else if (ni->state == NS_DOOR && !str_nicmp(act, "In ", 3)) act += 3;
            sprintf(nodetext[i], "%2d %-7s %-14.14s %-15s %3lum   %.30s", i + 1,
                    statename(ni->state), ni->user[0] ? ni->user : "-", ip,
                    (now - ni->connected) / 60, act);
        }
        nodenodes[i].ln_Name = nodetext[i];
        AddTail(&nodelist, &nodenodes[i]);
    }
    nban = 0;
    for (i = 0; i < MAX_BANS; i++) {
        struct IPBan *b = &S->bans[i];
        char ip[16], ex[20];
        if (!b->ip) continue;
        ip_tostr(b->ip, ip);
        if (!b->expires) strcpy(ex, "permanent");
        else sprintf(ex, "%lu min left", (b->expires - now + 59) / 60);
        sprintf(bantext[nban], "%-15s  %-13s  %.36s", ip, ex, b->reason);
        banip_of[nban] = b->ip;
        bannodes[nban].ln_Name = bantext[nban];
        AddTail(&banlist, &bannodes[nban]);
        nban++;
    }
    bbs_datetimestr(S->started, up);
    sprintf(statustext, "Port %u  -  %u nodes  -  up since %s  -  %lu calls",
            (unsigned)S->port, (unsigned)S->nodes, up, S->total_calls);
    {
        int on = 0;
        for (i = 0; i < S->nodes; i++) if (S->node[i].state != NS_FREE) on++;
        sprintf(wintitle, "NilBBS: %d on, %d free", on, (int)S->nodes - on);
    }
    shared_unlock(S);
    if (win) SetWindowTitles(win, (UBYTE *)wintitle, (UBYTE *)~0);

    GT_SetGadgetAttrs(g_nodes, win, NULL, GTLV_Labels, (ULONG)&nodelist, TAG_END);
    GT_SetGadgetAttrs(g_bans, banwin, NULL, GTLV_Labels, (ULONG)&banlist, TAG_END);
    GT_SetGadgetAttrs(g_status, win, NULL, GTTX_Text, (ULONG)statustext, TAG_END);
    if (sel_ban >= nban) sel_ban = -1;
}

static void node_msg(int node, const char *text)
{
    int n;
    if (!S || !text[0]) return;
    for (n = 1; n <= S->nodes; n++) {
        struct NodeInfo *ni = &S->node[n - 1];
        struct Task *t = NULL;
        if (node && n != node) continue;
        shared_lock(S);
        if (ni->state >= NS_ONLINE) {
            UBYTE next = (ni->msg_head + 1) % NODE_MSGQ;
            if (next != ni->msg_tail) {
                struct NodeMsg *m = &ni->msgq[ni->msg_head];
                m->from = 0; m->type = NM_SYSOP;
                strcpy(m->fromname, "Sysop");
                str_copy(m->text, text, sizeof(m->text));
                ni->msg_head = next;
                t = ni->task;
            }
        }
        shared_unlock(S);
        if (t) Signal(t, SIGBREAKF_CTRL_D);
    }
}

static void kick(BOOL ban)
{
    struct Task *t = NULL;
    if (!S || sel_node < 0 || sel_node >= S->nodes) return;
    shared_lock(S);
    if (S->node[sel_node].state >= NS_LOGIN) {
        t = S->node[sel_node].task;
        /* an address already banned keeps its reason (edit it in the ban window) */
        if (ban) ipf_ban(S, S->node[sel_node].ip, 60,
                         ipf_findban(S, S->node[sel_node].ip) ? "" : "kicked by sysop");
    }
    shared_unlock(S);
    if (t) {
        Signal(t, SIGBREAKF_CTRL_C);
        bbs_log(BBS_SYSLOG, "BBSControl: kicked node %ld%s", sel_node + 1, ban ? " and banned for 60 min" : "");
    }
}

/* Reset: a node stuck in a door (crashed, never ends) - hang up and free it.
 * Doesn't wait: the node lets go at once, or the daemon forces it in 15 s. */
static void reset_node(void)
{
    char q[140];
    struct EasyStruct es = { sizeof(struct EasyStruct), 0, (UBYTE *)"NilBBS Control", NULL, (UBYTE *)"Reset|Cancel" };
    if (!S || sel_node < 0 || sel_node >= S->nodes) { DisplayBeep(NULL); return; }
    sprintf(q, "Force-reset node %ld?\nThe caller is hung up and the node is freed,\neven from a door that won't end.", (LONG)(sel_node + 1));
    es.es_TextFormat = (UBYTE *)q;
    if (EasyRequestArgs(win, &es, NULL, NULL) != 1) return;
    if (node_reset_start(S, sel_node + 1)) {
        bbs_log(BBS_SYSLOG, "BBSControl: reset node %ld", (LONG)(sel_node + 1));
        sprintf(statustext, "Node %ld reset - free again within %d seconds.", (LONG)(sel_node + 1), NODE_RESET_FORCE);
    } else sprintf(statustext, "Node %ld is already free.", (LONG)(sel_node + 1));
    GT_SetGadgetAttrs(g_status, win, NULL, GTTX_Text, (ULONG)statustext, TAG_END);
}

/* the one-liners / last callers buttons: ask, do it, say what happened */
static void lists_action(int gid)
{
    struct Cfg *c = cfg_load(bbs_config());
    LONG keep = c ? cfg_int(c, "oneliners_keep", 50) : 50, n = 0;
    char q[120], done[120];
    struct EasyStruct es = { sizeof(struct EasyStruct), 0, (UBYTE *)"NilBBS Control", NULL, NULL };
    if (c) cfg_free(c);
    if (gid == GID_OLTRIM) {
        if (keep <= 0) keep = 50;
        sprintf(q, "Keep only the newest %ld one-liners?", keep);
    } else if (gid == GID_OLCLEAR) strcpy(q, "Delete ALL one-liners?");
    else strcpy(q, "Clear the last-callers list?");
    es.es_TextFormat = (UBYTE *)q; es.es_GadgetFormat = (UBYTE *)"Yes|Cancel";
    if (EasyRequestArgs(win, &es, NULL, NULL) != 1) return;
    if (S) ObtainSemaphore(&S->msglock);
    if (gid == GID_OLTRIM) { n = oneliners_trim(keep); sprintf(done, "%ld old one-liner(s) removed.", n); }
    else if (gid == GID_OLCLEAR) { n = oneliners_clear(); sprintf(done, "%ld one-liner(s) removed.", n); }
    else { n = lastcallers_clear(); sprintf(done, "%ld last-caller entr%s removed.", n, n == 1 ? "y" : "ies"); }
    if (S) ReleaseSemaphore(&S->msglock);
    bbs_log(BBS_SYSLOG, "BBSControl: %s", done);
    es.es_TextFormat = (UBYTE *)done; es.es_GadgetFormat = (UBYTE *)"OK";
    EasyRequestArgs(win, &es, NULL, NULL);
}

static WORD main_bottom;                   /* where the main window's gadgets end */
static struct Gadget *make_gadgets(struct Screen *scr)
{
    struct NewGadget ng;
    struct Gadget *g;
    UWORD fh = scr->Font->ta_YSize, bh = fh + 6, top = scr->WBorTop + fh + 1 + 4;
    UWORD left = scr->WBorLeft + 8, width = 600, bw = (600 - 3 * 6) / 4;
    UWORD lvh = fh * 7 + 4;

    g = CreateContext(&glist);
    memset(&ng, 0, sizeof(ng));
    ng.ng_VisualInfo = vi;
    ng.ng_TextAttr = scr->Font;

    ng.ng_LeftEdge = left; ng.ng_TopEdge = top; ng.ng_Width = width; ng.ng_Height = bh;
    ng.ng_GadgetID = GID_STATUS;
    g = g_status = CreateGadget(TEXT_KIND, g, &ng, GTTX_Border, TRUE, TAG_END);

    ng.ng_TopEdge += bh + fh + 6; ng.ng_Height = lvh;
    {   /* column headings, lined up with the rows (same font, same format as update_lists) */
        static char head[100];
        struct NewGadget hg = ng;
        sprintf(head, "%2s %-7s %-14s %-15s %4s   %s", "#", "State", "Handle", "From (IP)", "Time", "Activity");
        hg.ng_TopEdge = ng.ng_TopEdge - fh - 3; hg.ng_Height = fh + 2;
        hg.ng_LeftEdge = ng.ng_LeftEdge + 4; hg.ng_Width = width - 8;
        hg.ng_GadgetText = NULL; hg.ng_GadgetID = 0;
        g = CreateGadget(TEXT_KIND, g, &hg, GTTX_Text, (ULONG)head, TAG_END);
    }
    ng.ng_GadgetText = NULL; ng.ng_Flags = 0; ng.ng_GadgetID = GID_NODES;
    g = g_nodes = CreateGadget(LISTVIEW_KIND, g, &ng, GTLV_ShowSelected, 0UL, TAG_END);

    ng.ng_TopEdge += lvh + 4; ng.ng_Height = bh; ng.ng_Flags = 0;
    ng.ng_Width = 60; ng.ng_GadgetText = "Kick"; ng.ng_GadgetID = GID_KICK;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    ng.ng_LeftEdge += 66; ng.ng_Width = 96; ng.ng_GadgetText = "Kick + Ban"; ng.ng_GadgetID = GID_KICKBAN;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    ng.ng_LeftEdge += 102; ng.ng_Width = 60; ng.ng_GadgetText = "Reset"; ng.ng_GadgetID = GID_RESET;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    ng.ng_LeftEdge += 66; ng.ng_Width = 54; ng.ng_GadgetText = "Chat"; ng.ng_GadgetID = GID_CHAT;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    ng.ng_LeftEdge += 60; ng.ng_Width = 140; ng.ng_GadgetText = NULL; ng.ng_GadgetID = GID_MSG;
    g = g_msg = CreateGadget(STRING_KIND, g, &ng, GTST_MaxChars, 100, TAG_END);
    ng.ng_LeftEdge += 146; ng.ng_Width = 80; ng.ng_GadgetText = "To node"; ng.ng_GadgetID = GID_SEND;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    ng.ng_LeftEdge += 86; ng.ng_GadgetText = "To all"; ng.ng_GadgetID = GID_SENDALL;
    ng.ng_Width = width - (ng.ng_LeftEdge - left);
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);

    ng.ng_LeftEdge = left; ng.ng_TopEdge += bh + 8; ng.ng_Width = bw;
    ng.ng_GadgetText = "Banned IPs..."; ng.ng_GadgetID = GID_BANWIN;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    ng.ng_LeftEdge += bw + 6; ng.ng_GadgetText = "Reload IP filter"; ng.ng_GadgetID = GID_RELOAD;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    ng.ng_LeftEdge += bw + 6; ng.ng_GadgetText = "Import new files"; ng.ng_GadgetID = GID_IMPORT;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    ng.ng_LeftEdge += bw + 6; ng.ng_Width = width - (ng.ng_LeftEdge - left);
    ng.ng_GadgetText = "Shut down BBS"; ng.ng_GadgetID = GID_SHUTDOWN;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);

    ng.ng_LeftEdge = left; ng.ng_TopEdge += bh + 6; ng.ng_Width = 140;
    ng.ng_GadgetText = "Trim one-liners"; ng.ng_GadgetID = GID_OLTRIM;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    ng.ng_LeftEdge = left + 146; ng.ng_Width = 140;
    ng.ng_GadgetText = "Clear one-liners"; ng.ng_GadgetID = GID_OLCLEAR;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    ng.ng_LeftEdge = left + 292; ng.ng_Width = 162;
    ng.ng_GadgetText = "Clear last callers"; ng.ng_GadgetID = GID_LCCLEAR;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    ng.ng_LeftEdge = left + 460; ng.ng_Width = width - 460;
    ng.ng_GadgetText = "Logon"; ng.ng_GadgetID = GID_LOGON;      /* NilTerm on a node */
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    main_bottom = ng.ng_TopEdge + bh;
    return g;
}

/* the ban list's own window: the list, unban, add a ban, close */
static struct Gadget *make_ban_gadgets(struct Screen *scr)
{
    struct NewGadget ng;
    struct Gadget *g;
    UWORD fh = scr->Font->ta_YSize, bh = fh + 6, top = scr->WBorTop + fh + 1 + fh + 8;
    UWORD left = scr->WBorLeft + 8, width = 480, lvh = fh * 9 + 4;

    g = CreateContext(&banglist);
    memset(&ng, 0, sizeof(ng));
    ng.ng_VisualInfo = vi;
    ng.ng_TextAttr = scr->Font;

    ng.ng_LeftEdge = left; ng.ng_TopEdge = top; ng.ng_Width = width; ng.ng_Height = lvh;
    {   /* column headings, lined up with the rows (the same format as bantext in update_lists) */
        static char head[80];
        struct NewGadget hg = ng;
        sprintf(head, "%-15s  %-13s  %s", "Address", "Expires", "Reason");
        hg.ng_TopEdge = ng.ng_TopEdge - fh - 3; hg.ng_Height = fh + 2;
        hg.ng_LeftEdge = ng.ng_LeftEdge + 4; hg.ng_Width = width - 8;
        hg.ng_GadgetText = NULL; hg.ng_Flags = 0; hg.ng_GadgetID = 0;
        g = CreateGadget(TEXT_KIND, g, &hg, GTTX_Text, (ULONG)head, TAG_END);
    }
    ng.ng_GadgetText = NULL; ng.ng_Flags = 0; ng.ng_GadgetID = GID_BANS;
    g = g_bans = CreateGadget(LISTVIEW_KIND, g, &ng, GTLV_ShowSelected, 0UL, TAG_END);

    ng.ng_TopEdge += lvh + 4; ng.ng_Height = bh; ng.ng_Flags = 0;
    ng.ng_Width = 80; ng.ng_GadgetText = "Unban"; ng.ng_GadgetID = GID_UNBAN;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    ng.ng_LeftEdge += 110; ng.ng_Width = 132; ng.ng_GadgetText = "IP"; ng.ng_Flags = PLACETEXT_LEFT;
    ng.ng_GadgetID = GID_BANIP;
    g = g_banip = CreateGadget(STRING_KIND, g, &ng, GTST_MaxChars, 15, TAG_END);
    ng.ng_LeftEdge += 176; ng.ng_Width = 52; ng.ng_GadgetText = "Mins"; ng.ng_GadgetID = GID_BANMIN;
    g = g_banmin = CreateGadget(INTEGER_KIND, g, &ng, GTIN_Number, 60, GTIN_MaxChars, 6, TAG_END);
    ng.ng_LeftEdge += 58; ng.ng_Width = width - (ng.ng_LeftEdge - left); ng.ng_Flags = 0;
    ng.ng_GadgetText = "Ban"; ng.ng_GadgetID = GID_BAN;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);

    /* the reason: picking a ban fills it in; left empty, a re-ban keeps the old one */
    ng.ng_TopEdge += bh + 4; ng.ng_LeftEdge = left + 72; ng.ng_Width = width - 72;
    ng.ng_GadgetText = "Reason"; ng.ng_Flags = PLACETEXT_LEFT; ng.ng_GadgetID = GID_BANWHY;
    g = g_banwhy = CreateGadget(STRING_KIND, g, &ng, GTST_MaxChars, 39, TAG_END);

    ng.ng_LeftEdge = left; ng.ng_TopEdge += bh + 6; ng.ng_Width = 100; ng.ng_Flags = 0;
    ng.ng_GadgetText = "Ban log..."; ng.ng_GadgetID = GID_BANLOG;          /* Bans.log, newest first */
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    ng.ng_LeftEdge = left + 106; ng.ng_Width = width - 106;
    ng.ng_GadgetText = "Close  (0 minutes = a permanent ban)"; ng.ng_GadgetID = GID_BANCLOSE;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    ban_w = width + 16;
    ban_h = ng.ng_TopEdge + bh + 6 + scr->WBorBottom;
    return g;
}

static void open_banwin(void)
{
    if (banwin) { WindowToFront(banwin); ActivateWindow(banwin); return; }
    banwin = OpenWindowTags(NULL,
        WA_Title, (ULONG)"NilBBS - banned IPs",
        WA_Left, win ? win->LeftEdge + 40 : 40, WA_Top, win ? win->TopEdge + 20 : 20,
        WA_InnerWidth, ban_w, WA_Height, ban_h,
        WA_Gadgets, (ULONG)banglist,
        WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE, WA_Activate, TRUE,
        WA_PubScreen, (ULONG)pubscr,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | LISTVIEWIDCMP | BUTTONIDCMP | STRINGIDCMP,
        TAG_END);
    if (!banwin) { DisplayBeep(NULL); return; }
    GT_RefreshWindow(banwin, NULL);
    refresh();
}
static void close_banwin(void)
{
    if (!banwin) return;
    CloseWindow(banwin);            /* its gadget list stays, for next time */
    banwin = NULL;
}

/* ---- the ban log (BBS:Logs/Bans.log): every ban, unban and expiry, newest first ---- */
#define LOG_MAXLINES 400                    /* the newest ones; the file itself keeps everything */
static struct Gadget *logglist, *g_log;
static struct List loglist;
static struct Node *lognodes;
static char *logbuf;

static void free_banlog(void)
{
    if (lognodes) FreeVec(lognodes);
    if (logbuf) FreeVec(logbuf);
    lognodes = NULL; logbuf = NULL;
    loglist.lh_Head = (struct Node *)&loglist.lh_Tail;         /* an empty list, set up by hand */
    loglist.lh_Tail = NULL;
    loglist.lh_TailPred = (struct Node *)&loglist.lh_Head;
}

static void load_banlog(void)
{
    LONG size = file_size(BBS_BANLOG), n = 0, i;
    BPTR fh;
    char *p, *line[LOG_MAXLINES];
    free_banlog();
    if (size <= 0 || !(logbuf = AllocVec(size + 1, MEMF_CLEAR))) return;
    if ((fh = Open((STRPTR)BBS_BANLOG, MODE_OLDFILE))) { size = Read(fh, logbuf, size); Close(fh); }
    else size = 0;
    if (size < 0) size = 0;
    logbuf[size] = 0;
    for (p = logbuf; *p; ) {                /* keep the newest LOG_MAXLINES (a ring of pointers) */
        char *e = strchr(p, '\n');
        if (e) *e = 0;
        if (*p && *p != '#' && strncmp(p, "---", 3)) {      /* # and --- lines: the file's own notes */
            char *tag = strstr(p, "   [from System.log]");     /* carried-over history: the tag only */
            if (tag) *tag = 0;                                  /* pushes the reason out of view */
            line[n++ % LOG_MAXLINES] = p;
        }
        if (!e) break;
        p = e + 1;
    }
    if (!n) return;
    i = n > LOG_MAXLINES ? LOG_MAXLINES : n;
    if (!(lognodes = AllocVec(sizeof(struct Node) * i, MEMF_CLEAR))) return;
    { LONG k; for (k = 0; k < i; k++) {                  /* newest first */
        lognodes[k].ln_Name = line[(n - 1 - k) % LOG_MAXLINES];
        AddTail(&loglist, &lognodes[k]);
    } }
}

/* re-read the file into the open window: detach the list before its nodes are freed */
static void reload_logwin(void)
{
    if (!logwin) return;
    GT_SetGadgetAttrs(g_log, logwin, NULL, GTLV_Labels, ~0UL, TAG_END);
    load_banlog();
    GT_SetGadgetAttrs(g_log, logwin, NULL, GTLV_Labels, (ULONG)&loglist, GTLV_Top, 0, TAG_END);
}

static void close_logwin(void)
{
    if (!logwin) return;
    CloseWindow(logwin);
    logwin = NULL;
    if (logglist) FreeGadgets(logglist);
    logglist = NULL;
    free_banlog();
}

static void open_logwin(void)
{
    struct NewGadget ng;
    struct Gadget *g;
    struct Screen *scr = pubscr;
    UWORD fh, bh, top, w, h, lvh;
    if (logwin) { reload_logwin(); WindowToFront(logwin); ActivateWindow(logwin); return; }
    if (!scr) return;
    fh = scr->Font->ta_YSize; bh = fh + 6; top = scr->WBorTop + fh + 1 + 4;
    w = scr->Width - 40 > 620 ? 620 : scr->Width - 40;
    lvh = fh * 16 + 4;
    h = top + lvh + 4 + bh + 6 + scr->WBorBottom;
    if (h > scr->Height - 10) { lvh -= h - (scr->Height - 10); h = scr->Height - 10; }
    free_banlog();
    load_banlog();
    g = CreateContext(&logglist);
    memset(&ng, 0, sizeof(ng));
    ng.ng_VisualInfo = vi; ng.ng_TextAttr = scr->Font;
    ng.ng_LeftEdge = scr->WBorLeft + 8; ng.ng_TopEdge = top; ng.ng_Width = w - 16; ng.ng_Height = lvh;
    ng.ng_GadgetID = GID_LOGLIST;
    g = g_log = CreateGadget(LISTVIEW_KIND, g, &ng, GTLV_Labels, (ULONG)&loglist, GTLV_ReadOnly, TRUE, TAG_END);
    ng.ng_TopEdge += lvh + 4; ng.ng_Height = bh; ng.ng_Width = 100;
    ng.ng_GadgetText = "Refresh"; ng.ng_GadgetID = GID_LOGREFRESH;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    ng.ng_LeftEdge = scr->WBorLeft + w - 8 - 100;
    ng.ng_GadgetText = "Close"; ng.ng_GadgetID = GID_LOGCLOSE;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    if (!g || !(logwin = OpenWindowTags(NULL,
            WA_Title, (ULONG)"NilBBS - ban log (BBS:Logs/Bans.log), newest first",
            WA_Left, 10, WA_Top, win ? win->TopEdge + 10 : 10,
            WA_InnerWidth, w, WA_Height, h,
            WA_Gadgets, (ULONG)logglist,
            WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE, WA_Activate, TRUE,
            WA_PubScreen, (ULONG)pubscr,
            WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | LISTVIEWIDCMP | BUTTONIDCMP,
            TAG_END))) {
        if (logglist) FreeGadgets(logglist);
        logglist = NULL;
        free_banlog();
        DisplayBeep(NULL);
        return;
    }
    GT_RefreshWindow(logwin, NULL);
}

static BOOL open_win(void)
{
    struct Screen *scr = pubscr;
    zoombox[0] = win_x; zoombox[1] = win_y;
    zoombox[2] = 300; zoombox[3] = scr->WBorTop + scr->Font->ta_YSize + 1;   /* shrunk: the title bar */
    win = OpenWindowTags(NULL,
        WA_Title, (ULONG)wintitle,
        WA_Left, win_x, WA_Top, win_y,
        WA_InnerWidth, 616, WA_Height, win_h,
        WA_Gadgets, (ULONG)glist,
        WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE, WA_Activate, TRUE,
        WA_Zoom, (ULONG)zoombox,
        WA_IconifyGadget, TRUE,             /* OS 3.2; older Intuition ignores it (the menu still works) */
        WA_NewLookMenus, TRUE,
        WA_PubScreen, (ULONG)scr,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_MENUPICK | IDCMP_NEWSIZE |
                  LISTVIEWIDCMP | BUTTONIDCMP | STRINGIDCMP,
        TAG_END);
    if (!win) return FALSE;
    if (menus) SetMenuStrip(win, menus);
    if (zoomed) ZipWindow(win);
    GT_RefreshWindow(win, NULL);
    return TRUE;
}

/* the window goes, an AppIcon comes; a double-click on it brings the window back */
static struct AppIcon *appicon;
static struct DiskObject *dobj;
static struct MsgPort *appport;
static void iconify(void)
{
    if (!WorkbenchBase || !IconBase || !appport) { DisplayBeep(NULL); return; }
    if (!(dobj = GetDiskObject((STRPTR)"PROGDIR:BBSControl")) && !(dobj = GetDefDiskObject(WBTOOL))) { DisplayBeep(NULL); return; }
    dobj->do_CurrentX = NO_ICON_POSITION; dobj->do_CurrentY = NO_ICON_POSITION;
    if (!(appicon = AddAppIconA(0, 0, (STRPTR)"NilBBS", appport, 0, dobj, NULL))) {
        FreeDiskObject(dobj); dobj = NULL; DisplayBeep(NULL); return;
    }
    close_logwin();
    close_banwin();
    win_x = win->LeftEdge; win_y = win->TopEdge;
    zoomed = (win->Flags & WFLG_ZOOMED) ? 1 : 0;
    ClearMenuStrip(win);
    CloseWindow(win);
    win = NULL;
}
static BOOL uniconify(void)
{
    if (appicon) { RemoveAppIcon(appicon); appicon = NULL; }
    if (dobj) { FreeDiskObject(dobj); dobj = NULL; }
    return open_win();
}

/* ==== pages and chat ==================================================================
 * A caller's page lands in the shared block (page_node / page_from / page_text) and CTRL_F
 * wakes us: the alert window asks Chat or Not now (the caller waits up to 30 s).  The chat
 * window is the sysop's side of the private channel PRIVATE_BASE + node - the one Sysop menu
 * H uses: our lines go into the tele ring from node 0 and CTRL_D wakes the caller's node;
 * its lines wake us with CTRL_F.  "Chat" on the node list breaks in without a page.
 */
#define PRIVATE_BASE 100
#define CHAT_LINES   120
#define CHAT_ROWS    10

static struct Window *pagewin, *chatwin;
static struct Gadget *pageglist, *chatglist, *g_chatlv, *g_chatin;
static ULONG page_shown, chat_seen, chat_started;
static int page_for, chat_node;
static BOOL chat_joined, chat_over, chat_nudged;
static struct List chatlist;
static struct Node chatnodes[CHAT_LINES];
static char chattext[CHAT_LINES][176];
static int nchat;
static char sysopname[NAMELEN] = "Sysop";
static char pagetitle[80], pageline1[100], pageline2[100], chattitle[80];

static void load_sysopname(void)
{
    struct Cfg *c = cfg_load(bbs_config());
    if (c) { str_copy(sysopname, cfg_str(c, "sysop_name", "Sysop"), NAMELEN); cfg_free(c); }
}

static void chat_add(const char *s)
{
    int i;
    if (chatwin) GT_SetGadgetAttrs(g_chatlv, chatwin, NULL, GTLV_Labels, ~0UL, TAG_END);
    if (nchat == CHAT_LINES) { memmove(chattext[0], chattext[1], sizeof(chattext[0]) * (CHAT_LINES - 1)); nchat--; }
    str_copy(chattext[nchat++], s, sizeof(chattext[0]));
    list_init(&chatlist);
    for (i = 0; i < nchat; i++) { chatnodes[i].ln_Name = chattext[i]; chatnodes[i].ln_Type = 0; AddTail(&chatlist, &chatnodes[i]); }
    if (chatwin) GT_SetGadgetAttrs(g_chatlv, chatwin, NULL, GTLV_Labels, (ULONG)&chatlist,
                                   GTLV_Top, (ULONG)(nchat > CHAT_ROWS ? nchat - CHAT_ROWS : 0), TAG_END);
}

/* a line into the ring, from the sysop at the console (node 0); wake the node(s) on the channel */
static void ctl_post(UBYTE chan, UBYTE kind, const char *text)
{
    struct Task *wake[MAX_NODES];
    struct TeleLine *l;
    int n, nw = 0;
    if (!S) return;
    shared_lock(S);
    l = &S->tele[S->tele_seq % TELE_RING];
    memset(l, 0, sizeof(*l));
    l->seq = S->tele_seq++;
    l->chan = chan; l->fromnode = 0; l->kind = kind;
    str_copy(l->from, sysopname, NAMELEN);
    str_copy(l->text, text, sizeof(l->text));
    for (n = 0; n < S->nodes; n++) {
        struct NodeInfo *ni = &S->node[n];
        if (ni->state >= NS_ONLINE && ni->tele == chan && ni->task) wake[nw++] = ni->task;
    }
    shared_unlock(S);
    for (n = 0; n < nw; n++) Signal(wake[n], SIGBREAKF_CTRL_D);
}

/* ask a node to join the private channel (the node's own "#chat:" break-in) */
static BOOL chat_request(int node)
{
    struct NodeInfo *ni;
    struct Task *t = NULL;
    if (!S || node < 1 || node > S->nodes) return FALSE;
    ni = &S->node[node - 1];
    shared_lock(S);
    if (ni->state >= NS_ONLINE) {
        UBYTE next = (ni->msg_head + 1) % NODE_MSGQ;
        if (next != ni->msg_tail) {
            struct NodeMsg *m = &ni->msgq[ni->msg_head];
            m->from = 0; m->type = NM_CHATREQ;
            str_copy(m->fromname, sysopname, NAMELEN);
            sprintf(m->text, "#chat:%d", PRIVATE_BASE + node);
            ni->msg_head = next;
            t = ni->task;
        }
    }
    shared_unlock(S);
    if (t) Signal(t, SIGBREAKF_CTRL_D);
    return t != NULL;
}

static void close_chat(void)
{
    if (chat_node && !chat_over) ctl_post((UBYTE)(PRIVATE_BASE + chat_node), TK_LEAVE, "The sysop has left the chat");
    if (S) { shared_lock(S); S->ctl_chat = 0; shared_unlock(S); }
    if (chatwin) { CloseWindow(chatwin); chatwin = NULL; }
    if (chatglist) { FreeGadgets(chatglist); chatglist = NULL; }
    chat_node = 0;
}

static void open_chat(int node, BOOL breakin)
{
    struct Screen *scr = pubscr;
    struct NewGadget ng;
    struct Gadget *g;
    UWORD fh = scr->Font->ta_YSize, bh = fh + 6, left = scr->WBorLeft + 8, top = scr->WBorTop + fh + 1 + 4, w = 540;
    char who[NAMELEN];
    if (!S || node < 1 || node > S->nodes) return;
    if (chatwin) { if (chat_node == node) { WindowToFront(chatwin); return; } close_chat(); }
    load_sysopname();
    shared_lock(S);
    str_copy(who, S->node[node - 1].user, NAMELEN);
    chat_seen = S->tele_seq;
    S->ctl_chat = (UBYTE)(PRIVATE_BASE + node);
    shared_unlock(S);
    chat_node = node; chat_joined = chat_over = chat_nudged = FALSE; chat_started = bbs_now();
    nchat = 0; list_init(&chatlist);

    g = CreateContext(&chatglist);
    memset(&ng, 0, sizeof(ng));
    ng.ng_VisualInfo = vi; ng.ng_TextAttr = scr->Font;
    ng.ng_LeftEdge = left; ng.ng_TopEdge = top; ng.ng_Width = w; ng.ng_Height = fh * CHAT_ROWS + 4;
    ng.ng_GadgetID = GID_CHATLV;
    g = g_chatlv = CreateGadget(LISTVIEW_KIND, g, &ng, GTLV_ReadOnly, TRUE, GTLV_Labels, (ULONG)&chatlist, TAG_END);
    ng.ng_TopEdge += ng.ng_Height + 4; ng.ng_Height = bh; ng.ng_Width = w - 96; ng.ng_GadgetID = GID_CHATIN;
    g = g_chatin = CreateGadget(STRING_KIND, g, &ng, GTST_MaxChars, 150, TAG_END);
    ng.ng_LeftEdge += w - 90; ng.ng_Width = 90; ng.ng_GadgetText = "End chat"; ng.ng_GadgetID = GID_CHATEND;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    if (!g) { close_chat(); DisplayBeep(NULL); return; }
    sprintf(chattitle, "Chat with %s (node %d)", who, node);
    chatwin = OpenWindowTags(NULL,
        WA_Title, (ULONG)chattitle,
        WA_Left, win ? win->LeftEdge + 30 : 40, WA_Top, win ? win->TopEdge + 30 : 40,
        WA_InnerWidth, w + 16, WA_Height, ng.ng_TopEdge + bh + 6 + scr->WBorBottom,
        WA_Gadgets, (ULONG)chatglist,
        WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE, WA_Activate, TRUE,
        WA_PubScreen, (ULONG)scr,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | LISTVIEWIDCMP | BUTTONIDCMP | STRINGIDCMP,
        TAG_END);
    if (!chatwin) { close_chat(); DisplayBeep(NULL); return; }
    GT_RefreshWindow(chatwin, NULL);
    if (breakin) {
        char s[80];
        sprintf(s, "*** Breaking in on node %d...", node);
        chat_add(s);
        if (!chat_request(node)) chat_add("*** Nobody's on that node.");
        bbs_log(BBS_SYSLOG, "BBSControl: the sysop broke in on node %d (%s)", node, who);
    } else chat_add("*** You answered the page. Type below - Enter sends.");
    ActivateGadget(g_chatin, chatwin, NULL);
}

/* new lines from the caller; the end of the chat (a leave, or they're gone) */
static void chat_poll(void)
{
    static struct TeleLine batch[16];              /* static: BBSControl may run on a 4K Workbench stack */
    int nb = 0, i;
    ULONG s;
    UBYTE chan;
    BOOL left = FALSE, gone = FALSE;
    if (!S || !chat_node || chat_over) return;
    chan = (UBYTE)(PRIVATE_BASE + chat_node);
    shared_lock(S);
    if (S->tele_seq - chat_seen > TELE_RING) chat_seen = S->tele_seq - TELE_RING;
    for (s = chat_seen; s < S->tele_seq && nb < 16; s++) {
        struct TeleLine *l = &S->tele[s % TELE_RING];
        if (l->chan != chan || l->fromnode == 0) continue;
        batch[nb++] = *l;
    }
    chat_seen = s;
    if (S->node[chat_node - 1].tele == chan) chat_joined = TRUE;
    else if (chat_joined) gone = TRUE;                   /* hung up in the middle */
    shared_unlock(S);
    for (i = 0; i < nb; i++) {
        char t[176];
        if (batch[i].kind == TK_SAY) sprintf(t, "%s: %.150s", batch[i].from, batch[i].text);
        else if (batch[i].kind == TK_ACTION) sprintf(t, "* %s %.150s", batch[i].from, batch[i].text);
        else sprintf(t, "*** %.160s", batch[i].text);
        chat_add(t);
        if (batch[i].kind == TK_LEAVE) left = TRUE;
    }
    if (!chat_joined && !chat_nudged && bbs_now() - chat_started > 20) {
        chat_nudged = TRUE;
        chat_add("*** They haven't picked up yet (a door or a transfer can hold them up).");
    }
    if (left || (gone && !nb)) {
        chat_over = TRUE;
        chat_add(left ? "*** The chat has ended." : "*** The caller has gone. The chat has ended.");
        if (chatwin) GT_SetGadgetAttrs(g_chatin, chatwin, NULL, GA_Disabled, TRUE, TAG_END);
        shared_lock(S); S->ctl_chat = 0; shared_unlock(S);
    }
}

static void chat_send(void)
{
    char *txt = ((struct StringInfo *)g_chatin->SpecialInfo)->Buffer, t[176];
    if (!chat_node || chat_over || !txt[0]) return;
    ctl_post((UBYTE)(PRIVATE_BASE + chat_node), TK_SAY, txt);
    sprintf(t, "%s: %.150s", sysopname, txt);
    chat_add(t);
    GT_SetGadgetAttrs(g_chatin, chatwin, NULL, GTST_String, (ULONG)"", TAG_END);
    ActivateGadget(g_chatin, chatwin, NULL);
}

/* NilTerm (the sysop's ANSI terminal, its own screen) with these arguments - we keep running.
 * FALSE if it isn't installed or wouldn't start. */
static BOOL run_nilterm(const char *args)
{
    char cmd[64];
    BPTR in, out, l;
    if (!(l = Lock((STRPTR)"BBS:NilTerm", ACCESS_READ))) return FALSE;
    UnLock(l);
    in = Open((STRPTR)"NIL:", MODE_OLDFILE);
    out = Open((STRPTR)"NIL:", MODE_NEWFILE);
    sprintf(cmd, "BBS:NilTerm %s", args);
    if (!in || !out || SystemTags((STRPTR)cmd, SYS_Input, in, SYS_Output, out,
                                  SYS_Asynch, TRUE, NP_StackSize, 16384, TAG_END) == -1) {
        if (in) Close(in);
        if (out) Close(out);
        return FALSE;
    }
    return TRUE;
}

static void nilterm_logon(void)
{
    char a[16];
    sprintf(a, "PORT=%d", S && S->port ? (int)S->port : 2323);
    if (!run_nilterm(a)) DisplayBeep(NULL);
}

/* who "Chat" talks to: the caller picked in the node list - or, if none is, the only one online */
/* double-click on a node: a caller on it - watch their screen live in NilTerm (proper ANSI,
 * the VGA font; they aren't told); a free node - log on there with NilTerm.  Without NilTerm
 * the watch falls back to BBSCtl WATCH in a console window. */
static void watch_node(int n)
{
    char con[120], cmd[40];
    BPTR in;
    WORD w = win ? win->WScreen->Width : 640, h = win ? win->WScreen->Height : 256;
    if (!S || n < 1 || n > S->nodes) return;
    if (S->node[n - 1].state == NS_FREE) { nilterm_logon(); return; }
    sprintf(cmd, "WATCH=%d", n);
    if (run_nilterm(cmd)) return;
    sprintf(con, "CON:0/%d/%d/%d/NilBBS - watching node %d/CLOSE", (int)(win ? win->WScreen->BarHeight + 1 : 11),
            (int)w, (int)(h - (win ? win->WScreen->BarHeight + 1 : 11)), n);
    sprintf(cmd, "BBS:BBSCtl WATCH %d", n);
    if (!(in = Open((STRPTR)con, MODE_NEWFILE))) { DisplayBeep(NULL); return; }
    /* output NULL + an interactive input: DOS gives the command the same console for output */
    if (SystemTags((STRPTR)cmd, SYS_Input, in, SYS_Output, 0, SYS_Asynch, TRUE,
                   NP_StackSize, 16384, TAG_END) == -1) {
        Close(in);
        DisplayBeep(NULL);
    }
}

static int chat_target(void)
{
    int n, found = 0, count = 0;
    if (!S) return 0;
    shared_lock(S);
    if (sel_node >= 0 && sel_node < S->nodes && S->node[sel_node].state >= NS_ONLINE) found = sel_node + 1;
    else for (n = 0; n < S->nodes; n++) if (S->node[n].state >= NS_ONLINE) { count++; found = n + 1; }
    shared_unlock(S);
    return count > 1 ? 0 : found;
}

static void close_page(void)
{
    if (pagewin) { CloseWindow(pagewin); pagewin = NULL; }
    if (pageglist) { FreeGadgets(pageglist); pageglist = NULL; }
}

static void open_page(void)
{
    struct Screen *scr = pubscr;
    struct NewGadget ng;
    struct Gadget *g;
    UWORD fh = scr->Font->ta_YSize, bh = fh + 6, left = scr->WBorLeft + 8, top = scr->WBorTop + fh + 1 + 6, w = 420;
    close_page();
    g = CreateContext(&pageglist);
    memset(&ng, 0, sizeof(ng));
    ng.ng_VisualInfo = vi; ng.ng_TextAttr = scr->Font;
    ng.ng_LeftEdge = left; ng.ng_TopEdge = top; ng.ng_Width = w; ng.ng_Height = bh;
    g = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)pageline1, TAG_END);
    ng.ng_TopEdge += bh + 2;
    g = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)pageline2, GTTX_Border, TRUE, TAG_END);
    ng.ng_TopEdge += bh + 8; ng.ng_Width = (w - 12) / 2; ng.ng_GadgetText = "Chat"; ng.ng_GadgetID = GID_PAGECHAT;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    ng.ng_LeftEdge += ng.ng_Width + 12; ng.ng_GadgetText = "Not now"; ng.ng_GadgetID = GID_PAGENO;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    if (!g) { close_page(); return; }
    pagewin = OpenWindowTags(NULL,
        WA_Title, (ULONG)pagetitle,
        WA_Left, (scr->Width - w - 16) / 2, WA_Top, scr->Height / 3,
        WA_InnerWidth, w + 16, WA_Height, ng.ng_TopEdge + bh + 6 + scr->WBorBottom,
        WA_Gadgets, (ULONG)pageglist,
        WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE, WA_Activate, TRUE,
        WA_PubScreen, (ULONG)scr,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | BUTTONIDCMP,
        TAG_END);
    if (pagewin) { GT_RefreshWindow(pagewin, NULL); ScreenToFront(scr); }
}

/* a new page to show, or the one on screen withdrawn (the caller gave up or timed out) */
static void check_page(void)
{
    UBYTE node, ans;
    ULONG seq;
    char from[NAMELEN], text[80];
    if (!S) return;
    shared_lock(S);
    node = S->page_node; ans = S->page_answer; seq = S->page_seq;
    str_copy(from, S->page_from, NAMELEN); str_copy(text, S->page_text, sizeof(text));
    shared_unlock(S);
    if (node && !ans && seq != page_shown) {
        page_shown = seq; page_for = node;
        if (!win) uniconify();
        sprintf(pagetitle, "NilBBS - node %d is paging you!", node);
        sprintf(pageline1, "%s (node %d) wants to chat:", from, node);
        sprintf(pageline2, "%.60s", text[0] ? text : "(no reason given)");
        open_page();
        DisplayBeep(NULL);
        bbs_log(BBS_SYSLOG, "BBSControl: %s on node %d paged the sysop", from, node);
    } else if (pagewin && (seq != page_shown || !node || ans)) {
        close_page();
        if (ans == 3) {
            sprintf(statustext, "%s gave up waiting for an answer to the page.", from);
            if (win) GT_SetGadgetAttrs(g_status, win, NULL, GTTX_Text, (ULONG)statustext, TAG_END);
        }
    }
}

static void answer_page(UBYTE how)
{
    BOOL ok = FALSE;
    if (S) {
        shared_lock(S);
        if (S->page_node == page_for && !S->page_answer) { S->page_answer = how; ok = TRUE; }
        shared_unlock(S);
    }
    close_page();
    if (ok && how == 1) open_chat(page_for, FALSE);
}

int main(void)
{
    struct Screen *scr;
    struct MsgPort *tport = NULL;
    struct timerequest *treq = NULL;
    BOOL done = FALSE, timer_open = FALSE;
    UWORD wh;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 37);
    GadToolsBase = OpenLibrary((STRPTR)"gadtools.library", 37);
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR)"graphics.library", 37);
    if (!IntuitionBase || !GadToolsBase || !GfxBase) goto out;
    WorkbenchBase = OpenLibrary((STRPTR)"workbench.library", 37);     /* for iconify; optional */
    IconBase = OpenLibrary((STRPTR)"icon.library", 37);
    appport = CreateMsgPort();

    if (!(scr = LockPubScreen(NULL))) goto out;
    pubscr = scr;                                  /* kept locked while we run: we reopen on it */
    vi = GetVisualInfo(scr, TAG_END);
    if (!make_gadgets(scr) || !make_ban_gadgets(scr)) goto out;
    if ((menus = CreateMenus(newmenus, GTMN_FrontPen, 0UL, TAG_END)) && !LayoutMenus(menus, vi, GTMN_NewLookMenus, TRUE, TAG_END)) {
        FreeMenus(menus); menus = NULL;
    }
    win_h = wh = main_bottom + 6 + scr->WBorBottom;
    {   /* on a narrow screen start flush left, so the title-bar gadgets are on screen */
        WORD ww = 616 + scr->WBorLeft + scr->WBorRight + 20;
        win_x = ww + 20 <= scr->Width ? 20 : (WORD)(scr->Width - ww > 0 ? scr->Width - ww : 0);
        win_y = win_h + 20 <= scr->Height ? 20 : 0;
    }
    if (!open_win()) goto out;

    if ((tport = CreateMsgPort()) &&
        (treq = (struct timerequest *)CreateIORequest(tport, sizeof(struct timerequest))) &&
        !OpenDevice((STRPTR)TIMERNAME, UNIT_VBLANK, (struct IORequest *)treq, 0)) {
        timer_open = TRUE;
        treq->tr_node.io_Command = TR_ADDREQUEST;
        treq->tr_time.tv_secs = 2; treq->tr_time.tv_micro = 0;
        SendIO((struct IORequest *)treq);
    }
    refresh();

    while (!done) {
        ULONG sigs = Wait((win ? 1UL << win->UserPort->mp_SigBit : 0) | SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_F |
                          (pagewin ? 1UL << pagewin->UserPort->mp_SigBit : 0) |
                          (chatwin ? 1UL << chatwin->UserPort->mp_SigBit : 0) |
                          (banwin ? 1UL << banwin->UserPort->mp_SigBit : 0) |
                          (logwin ? 1UL << logwin->UserPort->mp_SigBit : 0) |
                          (appport ? 1UL << appport->mp_SigBit : 0) |
                          (timer_open ? 1UL << tport->mp_SigBit : 0));
        struct IntuiMessage *im;
        struct AppMessage *am;
        if (sigs & SIGBREAKF_CTRL_C) done = TRUE;
        if (sigs & SIGBREAKF_CTRL_F) { check_page(); chat_poll(); }
        while (logwin && (im = GT_GetIMsg(logwin->UserPort))) {
            ULONG cls = im->Class;
            struct Gadget *gad = (struct Gadget *)im->IAddress;
            GT_ReplyIMsg(im);
            if (cls == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(logwin); GT_EndRefresh(logwin, TRUE); continue; }
            if (cls == IDCMP_CLOSEWINDOW) { close_logwin(); break; }
            if (cls == IDCMP_GADGETUP) {
                if (gad->GadgetID == GID_LOGCLOSE) { close_logwin(); break; }
                if (gad->GadgetID == GID_LOGREFRESH) reload_logwin();
            }
        }
        while (pagewin && (im = GT_GetIMsg(pagewin->UserPort))) {
            ULONG cls = im->Class;
            struct Gadget *gad = (struct Gadget *)im->IAddress;
            GT_ReplyIMsg(im);
            if (cls == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(pagewin); GT_EndRefresh(pagewin, TRUE); continue; }
            if (cls == IDCMP_CLOSEWINDOW) { answer_page(2); break; }
            if (cls == IDCMP_GADGETUP) { answer_page(gad->GadgetID == GID_PAGECHAT ? 1 : 2); break; }
        }
        while (chatwin && (im = GT_GetIMsg(chatwin->UserPort))) {
            ULONG cls = im->Class;
            struct Gadget *gad = (struct Gadget *)im->IAddress;
            GT_ReplyIMsg(im);
            if (cls == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(chatwin); GT_EndRefresh(chatwin, TRUE); continue; }
            if (cls == IDCMP_CLOSEWINDOW) { close_chat(); break; }
            if (cls == IDCMP_GADGETUP && gad->GadgetID == GID_CHATEND) { close_chat(); break; }
            if (cls == IDCMP_GADGETUP && gad->GadgetID == GID_CHATIN) chat_send();
        }
        if (appport)
            while ((am = (struct AppMessage *)GetMsg(appport))) {
                BOOL back = am->am_Type == AMTYPE_APPICON;
                ReplyMsg((struct Message *)am);
                if (back && !win && !uniconify()) done = TRUE;
            }
        if (timer_open && (sigs & (1UL << tport->mp_SigBit)) && CheckIO((struct IORequest *)treq)) {
            WaitIO((struct IORequest *)treq);
            refresh();
            check_page();
            chat_poll();
            treq->tr_time.tv_secs = 2; treq->tr_time.tv_micro = 0;
            SendIO((struct IORequest *)treq);
        }
        while (banwin && (im = GT_GetIMsg(banwin->UserPort))) {
            ULONG cls = im->Class;
            UWORD code = im->Code;
            struct Gadget *gad = (struct Gadget *)im->IAddress;
            GT_ReplyIMsg(im);
            if (cls == IDCMP_CLOSEWINDOW) { close_banwin(); break; }
            if (cls == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(banwin); GT_EndRefresh(banwin, TRUE); continue; }
            if (cls != IDCMP_GADGETUP) continue;
            switch (gad->GadgetID) {
            case GID_BANS:                              /* the picked address goes in the IP box */
                sel_ban = code;
                if (sel_ban >= 0 && sel_ban < nban) {
                    char ip[16], why[40];
                    why[0] = 0;
                    ip_tostr(banip_of[sel_ban], ip);
                    if (S) {                            /* ...and its reason in the Reason box */
                        struct IPBan *b;
                        shared_lock(S);
                        if ((b = ipf_findban(S, banip_of[sel_ban]))) str_copy(why, b->reason, sizeof(why));
                        shared_unlock(S);
                    }
                    GT_SetGadgetAttrs(g_banip, banwin, NULL, GTST_String, (ULONG)ip, TAG_END);
                    GT_SetGadgetAttrs(g_banwhy, banwin, NULL, GTST_String, (ULONG)why, TAG_END);
                }
                break;
            case GID_BANCLOSE: close_banwin(); break;
            case GID_BANLOG: open_logwin(); break;
            case GID_UNBAN:
                if (S && sel_ban >= 0 && sel_ban < nban) {
                    char ip[16];
                    shared_lock(S); ipf_unban(S, banip_of[sel_ban]); shared_unlock(S);
                    ip_tostr(banip_of[sel_ban], ip);
                    bbs_log(BBS_SYSLOG, "BBSControl: unbanned %s", ip);
                    sel_ban = -1;
                    refresh();
                }
                break;
            case GID_BAN: {
                char *ips = ((struct StringInfo *)g_banip->SpecialInfo)->Buffer;
                LONG mins = ((struct StringInfo *)g_banmin->SpecialInfo)->LongInt;
                char why[40];
                ULONG ip;
                str_copy(why, ((struct StringInfo *)g_banwhy->SpecialInfo)->Buffer, sizeof(why));
                str_nopipe(why);
                if (S && ip_parse(ips, &ip)) {
                    struct IPBan *b;
                    shared_lock(S);
                    ipf_ban(S, ip, mins < 0 ? 0 : (ULONG)mins, why);   /* "" keeps the old reason */
                    if ((b = ipf_findban(S, ip))) str_copy(why, b->reason, sizeof(why));
                    shared_unlock(S);
                    bbs_log(BBS_SYSLOG, "BBSControl: banned %s (%ld min: %s)", ips, mins, why);
                    refresh();
                } else DisplayBeep(NULL);
                break;
            }
            }
            if (!banwin) break;
        }
        while (win && (im = GT_GetIMsg(win->UserPort))) {
            ULONG cls = im->Class;
            UWORD code = im->Code;
            struct Gadget *gad = (struct Gadget *)im->IAddress;
            ULONG secs = im->Seconds, mics = im->Micros;
            GT_ReplyIMsg(im);
            if (cls == IDCMP_CLOSEWINDOW) {
                if (code == 1) { iconify(); break; }        /* OS 3.2's iconify gadget */
                done = TRUE;
            }
            else if (cls == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(win); GT_EndRefresh(win, TRUE); }
            else if (cls == IDCMP_NEWSIZE) {                  /* grown back from the title bar: redraw */
                if (!(win->Flags & WFLG_ZOOMED)) { RefreshGList(glist, win, NULL, -1); GT_RefreshWindow(win, NULL); }
            }
            else if (cls == IDCMP_MENUPICK) {
                UWORD mc = code;
                int act = 0;
                while (mc != MENUNULL) {
                    struct MenuItem *it = ItemAddress(menus, mc);
                    if (!it) break;
                    if (!act) act = (int)(ULONG)GTMENUITEM_USERDATA(it);
                    mc = it->NextSelect;
                }
                if (act == MN_QUIT) done = TRUE;
                else if (act == MN_SHRINK) ZipWindow(win);
                else if (act == MN_ICONIFY) { iconify(); break; }
            }
            else if (cls == IDCMP_GADGETUP) {
                char *msg = ((struct StringInfo *)g_msg->SpecialInfo)->Buffer;
                switch (gad->GadgetID) {
                case GID_NODES: {
                    /* a double-click: a busy node - watch it in NilTerm; a free one - log on there */
                    static ULONG lsecs, lmics;
                    static int lcode = -1;
                    if (code == lcode && DoubleClick(lsecs, lmics, secs, mics)) { watch_node(code + 1); lcode = -1; }
                    else { lcode = code; lsecs = secs; lmics = mics; }
                    sel_node = code;
                    break;
                }
                case GID_BANWIN: open_banwin(); break;
                case GID_KICK:  kick(FALSE); refresh(); break;
                case GID_KICKBAN: kick(TRUE); refresh(); break;
                case GID_RESET: reset_node(); refresh(); break;
                case GID_CHAT: {
                    int n = chat_target();
                    if (n) open_chat(n, TRUE);
                    else {
                        DisplayBeep(NULL);
                        strcpy(statustext, "Chat: pick a caller in the node list first.");
                        GT_SetGadgetAttrs(g_status, win, NULL, GTTX_Text, (ULONG)statustext, TAG_END);
                    }
                    break;
                }
                case GID_SEND:  if (sel_node >= 0) node_msg(sel_node + 1, msg); break;
                case GID_SENDALL: node_msg(0, msg); break;
                case GID_IMPORT: {
                    /* BBSMaint IMPORT in a window of its own, so we don't block */
                    BPTR in = Open((STRPTR)"NIL:", MODE_OLDFILE);
                    BPTR out = Open((STRPTR)"CON:40/30/560/180/NilBBS - importing new files/CLOSE/WAIT", MODE_NEWFILE);
                    if (!out || SystemTags((STRPTR)"BBS:BBSMaint IMPORT", SYS_Input, in, SYS_Output, out,
                                           SYS_Asynch, TRUE, NP_StackSize, 32768, TAG_END) == -1) {
                        if (in) Close(in);
                        if (out) Close(out);
                        DisplayBeep(NULL);
                    }
                    break;
                }
                case GID_OLTRIM: case GID_OLCLEAR: case GID_LCCLEAR:
                    lists_action(gad->GadgetID);
                    break;
                case GID_LOGON: nilterm_logon(); break;   /* the sysop's ANSI terminal on a free node */
                case GID_RELOAD:
                    if (S && S->daemon) Signal(S->daemon, SIGBREAKF_CTRL_F);
                    break;
                case GID_SHUTDOWN:
                    if (S) {
                        struct EasyStruct es = { sizeof(struct EasyStruct), 0, (UBYTE *)"NilBBS Control",
                            (UBYTE *)"Hang up every caller and stop NilBBS?", (UBYTE *)"Shut down|Cancel" };
                        if (EasyRequestArgs(win, &es, NULL, NULL) == 1) {
                            shared_lock(S); S->shutdown = 1; shared_unlock(S);
                            if (S->daemon) Signal(S->daemon, SIGBREAKF_CTRL_C);
                        }
                    }
                    break;
                }
            }
        }
    }

out:
    close_chat();
    close_page();
    if (S && S->ctl_task == FindTask(NULL)) { shared_lock(S); S->ctl_task = NULL; shared_unlock(S); }
    if (timer_open) {
        if (!CheckIO((struct IORequest *)treq)) AbortIO((struct IORequest *)treq);
        WaitIO((struct IORequest *)treq);
        CloseDevice((struct IORequest *)treq);
    }
    if (treq) DeleteIORequest((struct IORequest *)treq);
    if (tport) DeleteMsgPort(tport);
    if (appicon) RemoveAppIcon(appicon);
    if (dobj) FreeDiskObject(dobj);
    if (appport) {
        struct Message *m;
        while ((m = GetMsg(appport))) ReplyMsg(m);
        DeleteMsgPort(appport);
    }
    close_logwin();
    close_banwin();
    if (win) { ClearMenuStrip(win); CloseWindow(win); }
    if (menus) FreeMenus(menus);
    if (pubscr) UnlockPubScreen(NULL, pubscr);
    if (glist) FreeGadgets(glist);
    if (banglist) FreeGadgets(banglist);
    if (vi) FreeVisualInfo(vi);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    if (GadToolsBase) CloseLibrary(GadToolsBase);
    if (IconBase) CloseLibrary(IconBase);
    if (WorkbenchBase) CloseLibrary(WorkbenchBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    return 0;
}
