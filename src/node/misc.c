/*
 * misc.c - one-liners, last callers, who's online, user list, node messages,
 * system info and the caller's own settings.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "node.h"
#include "zmodem.h"

/* ---- the wall (one-liners): Data/OneLiners.dat, newest last, "name|date|text[|flags]" ---------
 * flags: 'a' = anonymous (shown as "Someone"; the sysop's list still has the name),
 *        'c' + a pen number 9..15 = the line's own colour.  Older 3-field lines still read. */

#define WALL_SHOW 12                    /* lines on the wall */
#define WALL_W    76                    /* inside the frame: " |" + 76 + "|" = 79 columns */
#define WALL_TEXT 50                    /* longest line a caller can write */
static const UBYTE wall_cycle[] = { 11, 13, 15 };

/* the top edge with the title in a tab: " ┌──┤ title ├──┐" */
static void box_top(const char *title, int w, int pen)
{
    int t = vis_len(title) + 4, l = (w - t) / 2, r = w - t - l;
    tprintf(" |%02d\xDA", pen); put_rep("\xC4", l);
    tprintf("\xB4 |15%s |%02d\xC3", title, pen);
    put_rep("\xC4", r);
    tputs("\xBF|07\n");
}

static void box_bottom(int w, int pen)
{
    tprintf(" |%02d\xC0", pen); put_rep("\xC4", w); tputs("\xD9|07\n");
}

static void box_rule(int w, int pen)
{
    tprintf(" |%02d\xC3", pen); put_rep("\xC4", w); tputs("\xB4|07\n");
}

/* one line of the wall, centred, in its colour (the text as it is: a '|' is no code) */
static void wall_row(const char *vis, int pen)
{
    int n = strlen(vis), l;
    if (n > WALL_W - 2) n = WALL_W - 2;
    l = (WALL_W - n) / 2;
    tputs(" |09\xB3");
    put_rep(" ", l);
    tprintf("|%02d", pen);
    tputraw((const UBYTE *)vis, n, CS_CP437);
    put_rep(" ", WALL_W - l - n);
    tputs("|09\xB3|07\n");
}

static void wall_add(void)
{
    char text[WALL_TEXT + 8], date[16], flags[8] = "", line[200];
    char *p;
    LONG k;
    tputs(L("misc.wall.style", "|08[|15N|08]|07ormal  |08[|15A|08]|07nonymous  |08[|15C|08]|07olour  |08[|15Q|08]|07uit: "));
    k = tgethot("NACQ\r");
    if (k == KEY_HANGUP) return;
    tprintf("%c\n", (int)(k == '\r' ? 'N' : k));
    if (k == 'Q') return;
    if (k == 'A') strcpy(flags, "a");
    if (k == 'C') {
        int c;
        tputs("  ");
        for (c = 9; c <= 15; c++) tprintf("|%02d%d|08) |%02d\xDB\xDB  ", c, c - 8, c);
        tputs(L("misc.wall.colour", "\n|07Colour |08(1-7)|07: "));
        k = tgethot("1234567Q\r");
        if (k == KEY_HANGUP) return;
        if (k >= '1' && k <= '7') { tprintf("%c\n", (int)k); sprintf(flags, "c%d", (int)(k - '1' + 9)); }
        else tputs("\n");
    }
    tputs("|07> |15");
    if (tgetline(text, WALL_TEXT, 0) <= 0) return;
    for (p = text; *p; p++) if (*p == '|') *p = '!';
    bbs_datestr(bbs_now(), date);
    sprintf(line, "%s|%s|%s%s%s\n", N.user.name, date, text, flags[0] ? "|" : "", flags);
    ObtainSemaphore(&N.S->msglock);
    {
        BPTR fh = Open((STRPTR)BBS_ONELINERS, MODE_READWRITE);
        if (fh) { Seek(fh, 0, OFFSET_END); FPuts(fh, (STRPTR)line); Close(fh); }
    }
    ReleaseSemaphore(&N.S->msglock);
    tputs(L("misc.oneliners.added", "|10Added.|07\n"));
}

/* the wall's lines, loaded by oneliners(); |WA..|WL in Text/wall.ans are these (term.c) */
static char wall_lines[WALL_SHOW][OL_LINE];
static int wall_n;

static void wall_load(void)
{
    struct LineReader lr;
    char line[OL_LINE + 40];
    wall_n = 0;
    ObtainSemaphore(&N.S->msglock);
    if (lr_open(&lr, BBS_ONELINERS)) {
        while (lr_gets(&lr, line, sizeof(line)) >= 0) {
            if (!line[0]) continue;
            if (wall_n == WALL_SHOW) { memmove(wall_lines[0], wall_lines[1], sizeof(wall_lines[0]) * (WALL_SHOW - 1)); wall_n--; }
            str_copy(wall_lines[wall_n++], line, sizeof(wall_lines[0]));
        }
        lr_close(&lr);
    }
    ReleaseSemaphore(&N.S->msglock);
}

/* wall line i (0 = the oldest shown) as "Name says: text"; *pen = the writer's own colour, or -1.
 * An empty wall's first line is the "nobody yet" note; lines past the end are empty. */
void wall_mci(int i, char *out, int max, int *pen)
{
    char tmp[OL_LINE], *f[4];
    const char *name;
    LONG nf;
    *out = 0; *pen = -1;
    if (i < 0 || i >= WALL_SHOW) return;
    if (!wall_n) {
        if (!i) { str_copy(out, L("misc.wall.empty", "Nobody has written on the wall yet - be the first."), max); *pen = 8; }
        return;
    }
    if (i >= wall_n) return;
    str_copy(tmp, wall_lines[i], sizeof(tmp));
    if ((nf = str_split(tmp, '|', f, 4)) < 3) return;
    name = f[0];
    if (nf == 4) {
        char *q;
        for (q = f[3]; *q; q++) {
            if (*q == 'a') name = L("misc.wall.someone", "Someone");
            else if (*q == 'c' && q[1]) { int c = atoi(q + 1); if (c >= 9 && c <= 15) *pen = c; }
        }
    }
    snprintf(out, max, L("misc.wall.says", "%s says: %s"), name, f[2]);
}

void oneliners(void)
{
    char vis[WALL_W + 40];
    int i;

    wall_load();
    tcls();
    /* the board's own wall (Text/wall.ans, lines as |WA..|WL); this built-in one is the fallback */
    if (!tshowfile("wall")) {
        box_top(L("misc.wall.title", "The Wall"), WALL_W, 9);
        wall_row("", 9);
        for (i = 0; i < (wall_n ? wall_n : 1); i++) {
            int pen;
            wall_mci(i, vis, sizeof(vis), &pen);
            wall_row(vis, pen >= 0 ? pen : wall_cycle[i % sizeof(wall_cycle)]);
        }
        wall_row("", 9);
        box_bottom(WALL_W, 9);
    }

    for (;;) {
        LONG k;
        tputs(N.sysop ? L("misc.wall.add_sysop", "\n|07Add a line to the wall? |08(|15y|08/|15N|08/|15?|08, |15*|08 clears it)|07 ")
                      : L("misc.wall.add", "\n|07Add a line to the wall? |08(|15y|08/|15N|08/|15?|08)|07 "));
        k = tgethot(N.sysop ? "YN?*\r" : "YN?\r");
        if (k == KEY_HANGUP) return;
        tprintf("%c\n", (int)(k == '\r' ? 'N' : k));
        if (k == '?') {
            tputs(L("misc.wall.help",
                "\n|07Write one line (up to 50 characters) for every caller to see at logon.\n"
                "  |15N|07ormal     shows your name\n"
                "  |15A|07nonymous  shows \"Someone\" instead (the sysop can still see who)\n"
                "  |15C|07olour     pick the colour of your line\n"
                "The newest 12 lines are on the wall.\n"));
            continue;
        }
        if (k == '*') {
            if (tyesno(L("misc.wall.clear_confirm", "|12Clear the whole wall?"), FALSE)) {
                LONG gone;
                ObtainSemaphore(&N.S->msglock);
                gone = oneliners_clear();
                ReleaseSemaphore(&N.S->msglock);
                tprintf(L("misc.wall.cleared", "|10Wall cleared (%ld lines).|07\n"), gone);
            }
            return;
        }
        if (k == 'Y') wall_add();
        return;
    }
}

/* ---- Your account status: Text/userstatus.ans if the board has one, else this, in two columns ---- */

#define ST_W 76

/* one row: two "label: value" fields, labels right-aligned to a colon */
static void st_row(const char *l1, const char *v1, const char *l2, const char *v2)
{
    int a = vis_len(l1), b = vis_len(l2), n1 = strlen(v1), n2 = strlen(v2);
    if (n1 > 18) n1 = 18;
    if (n2 > 18) n2 = 18;
    tputs(" |09\xB3");
    put_rep(" ", 18 - a);
    tprintf("|03%s|08: |15", l1);
    tputraw((const UBYTE *)v1, n1, CS_CP437);
    put_rep(" ", 18 - n1);
    put_rep(" ", 18 - b);
    tprintf("|03%s|08: |15", l2);
    tputraw((const UBYTE *)v2, n2, CS_CP437);
    put_rep(" ", ST_W - 18 - 2 - 18 - 18 - 2 - n2);
    tputs("|09\xB3|07\n");
}

/* a row with one long field on each side, for names (label colour: the header's) */
static void st_head(const char *l1, const char *v1, const char *l2, const char *v2)
{
    int a = vis_len(l1), b = vis_len(l2), n1 = strlen(v1), n2 = strlen(v2);
    int w1 = ST_W / 2 - 4 - a, w2 = ST_W - ST_W / 2 - 2 - b;
    if (n1 > w1) n1 = w1;
    if (n2 > w2) n2 = w2;
    tprintf(" |09\xB3 |12%s|08: |14", l1);
    tputraw((const UBYTE *)v1, n1, CS_CP437);
    put_rep(" ", w1 - n1 + 1);
    tprintf("|12%s|08: |14", l2);
    tputraw((const UBYTE *)v2, n2, CS_CP437);
    put_rep(" ", w2 - n2);
    tputs("|09\xB3|07\n");
}

static void kb_str(char *s, ULONG kb)
{
    if (kb >= 10240) sprintf(s, "%lu MB", (unsigned long)(kb / 1024));
    else sprintf(s, "%lu KB", (unsigned long)kb);
}

void user_status(void)
{
    char v[8][24], first[16], last[16];
    LONG left = time_left_mins(), allow;
    LONG ratio = cfg_int(N.cfg, "ratio", 0);
    const char *yes = L("misc.status.on", "on"), *no = L("misc.status.off", "off");

    user_refresh();
    tcls();
    /* the board's own screen, with MCI codes for the values (Text/userstatus.ans); this
     * built-in one is the fallback */
    if (tshowfile("userstatus")) { tpause(); return; }
    allow = ratio_allowed_kb();
    bbs_datestr(N.user.firstcall, first);
    bbs_datestr(N.user.lastcall, last);

    box_top(L("misc.status.title", "User Status"), ST_W, 9);
    st_head(L("misc.status.user", "User"), N.user.name,
            L("misc.status.location", "Location"), N.user.location[0] ? N.user.location : "-");
    box_rule(ST_W, 9);
    sprintf(v[0], "%d", (int)N.user.level);
    sprintf(v[1], "%lu", (unsigned long)N.user.calls);
    sprintf(v[2], "%lu", (unsigned long)N.user.id);
    sprintf(v[3], "%u", (unsigned)N.user.calls_today);
    sprintf(v[4], "%lu", (unsigned long)N.user.posts);
    if (left < 0) strcpy(v[5], L("misc.status.unlimited", "unlimited"));
    else sprintf(v[5], L("misc.status.mins", "%ld mins"), left);
    sprintf(v[6], "%lu", (unsigned long)N.user.doors);
    st_row(L("misc.status.level", "Security level"), v[0], L("misc.status.calls", "Total calls"), v[1]);
    st_row(L("misc.status.number", "User number"), v[2], L("misc.status.calls_today", "Calls today"), v[3]);
    st_row(L("misc.status.posts", "Messages posted"), v[4], L("misc.status.time_left", "Time left"), v[5]);
    st_row(L("misc.status.conf", "Conference"), conf_name(), L("misc.status.doors", "Door visits"), v[6]);
    st_row(L("misc.status.expert", "Expert mode"), (N.user.flags & UF_EXPERT) ? yes : no,
           L("misc.status.term", "Terminal"), term_name(N.term));
    st_row(L("misc.status.first", "First call"), first, L("misc.status.last", "Last call"), last);
    box_rule(ST_W, 9);
    if (ratio > 0) sprintf(v[0], "%ld:1", ratio); else strcpy(v[0], L("misc.status.disabled", "none"));
    if (allow < 0) strcpy(v[1], L("misc.status.unlimited", "unlimited")); else kb_str(v[1], (ULONG)allow);
    kb_str(v[2], N.user.ulkb);
    sprintf(v[3], "%lu", (unsigned long)N.user.uploads);
    kb_str(v[4], N.user.dlkb);
    sprintf(v[5], "%lu", (unsigned long)N.user.downloads);
    sprintf(v[6], "%ld KB", (long)N.user.credits);
    st_row(L("misc.status.ratio", "Ratio"), v[0], L("misc.status.dl_left", "Downloads left"), v[1]);
    st_row(L("misc.status.ul_kb", "Uploaded"), v[2], L("misc.status.ul_files", "Files uploaded"), v[3]);
    st_row(L("misc.status.dl_kb", "Downloaded"), v[4], L("misc.status.dl_files", "Files downloaded"), v[5]);
    st_row(L("misc.status.credits", "Credits"), v[6], L("misc.status.protocol", "Protocol"), proto_name(N.user.proto));
    box_rule(ST_W, 9);
    st_head(L("misc.status.board", "Board"), cfg_str(N.cfg, "bbs_name", "NilBBS"),
            L("misc.status.sysop", "Sysop"), cfg_str(N.cfg, "sysop_name", "Sysop"));
    box_bottom(ST_W, 9);
    tpause();
}

/* ---- last callers: Data/LastCallers.dat, fixed records (lists.c) ------------ */

void lastcallers_add(void)
{
    struct LastCall all[MAX_LASTCALL];
    LONG n;
    if (lastcall_hidden(N.cfg, &N.user)) return;   /* Hidden flag, or a sysop-level account */
    ObtainSemaphore(&N.S->msglock);
    n = lastcallers_load(all);
    if (n == MAX_LASTCALL) { memmove(&all[0], &all[1], sizeof(all[0]) * (MAX_LASTCALL - 1)); n--; }
    memset(&all[n], 0, sizeof(all[n]));
    str_copy(all[n].name, N.user.name, NAMELEN);
    str_copy(all[n].location, N.user.location, LONGNAME);
    all[n].when = bbs_now();
    all[n].node = (UWORD)N.node;
    all[n].term = N.term;
    lastcallers_save(all, n + 1);
    ReleaseSemaphore(&N.S->msglock);
}

void lastcallers_show(void)
{
    struct LastCall all[MAX_LASTCALL];
    LONG n, i;
    ObtainSemaphore(&N.S->msglock);
    n = lastcallers_load(all);
    ReleaseSemaphore(&N.S->msglock);
    tputs(L("misc.lastcallers_show.last_callers", "\n|09-=[ |15Last Callers|09 ]=-|07\n\n"));
    tputs(L("misc.lastcallers_show.node_handle_location", "|08  Node  Handle                Location                  When\n"));
    tputs("  ----  --------------------  ------------------------  ---------------|07\n");
    if (n <= 0) tputs(L("misc.lastcallers_show.nobody_yet", "  |08(nobody yet)|07\n"));
    for (i = n - 1; i >= 0; i--) {
        char when[20];
        bbs_datetimestr(all[i].when, when);
        tprintf("  |03%4d  |11%-20.20s  |07%-24.24s  |08%s|07\n", (int)all[i].node,
                all[i].name, all[i].location, when);
    }
}

/* ---- who's online ---------------------------------------------------------------- */

void whos_online(void)
{
    static struct NodeInfo copy[MAX_NODES];     /* ~23K: keep it off the stack */
    int n, nodes;
    ULONG now = bbs_now();

    shared_lock(N.S);
    nodes = N.S->nodes;
    memcpy(copy, N.S->node, sizeof(copy));
    shared_unlock(N.S);

    tputs(L("misc.whos_online.whos_online", "\n|09-=[ |15Who's Online|09 ]=-|07\n\n"));
    tputs(L("misc.whos_online.node_handle_activity", "|08  Node  Handle                Activity                        Mins\n"));
    tputs("  ----  --------------------  ------------------------------  ----|07\n");
    for (n = 0; n < nodes; n++) {
        struct NodeInfo *ni = &copy[n];
        if (ni->state == NS_FREE)
            tprintf("  |03%4d  |08%-20s  %-30s|07\n", n + 1, L("misc.whos_online.waiting", "(waiting)"), "");
        else if (ni->state < NS_ONLINE)
            tprintf("  |03%4d  |07%-20s  |08%-30s|07\n", n + 1, L("misc.whos_online.logging_in", "(logging in)"), ni->activity);
        else
            tprintf("  |03%4d  |11%-20.20s  |07%-30.30s  |15%4lu|07\n", n + 1, ni->user,
                    ni->activity, (now - ni->connected) / 60);
    }
}

void userlist(void)
{
    struct UserRec *u = AllocVec(sizeof(struct UserRec), 0);
    LONG i, n;
    if (!u) return;
    ObtainSemaphore(&N.S->userlock);
    n = userdb_count();
    ReleaseSemaphore(&N.S->userlock);
    tputs(L("misc.userlist.user_list", "\n|09-=[ |15User List|09 ]=-|07\n\n"));
    tputs(L("misc.userlist.handle_location_calls", "|08  Handle                Location                  Calls  Last on\n"
          "  --------------------  ------------------------  -----  ---------|07\n"));
    tpage_start();
    for (i = 1; i <= n && tmore(); i++) {
        char d[16];
        BOOL ok;
        ObtainSemaphore(&N.S->userlock);
        ok = userdb_read(i, u);
        ReleaseSemaphore(&N.S->userlock);
        if (!ok || (u->flags & UF_DELETED)) continue;
        bbs_datestr(u->lastcall, d);
        tprintf("  |11%-20.20s  |07%-24.24s  |15%5lu  |08%s|07\n", u->name, u->location, u->calls, d);
    }
    tpage_end();
    FreeVec(u);
}

/* ---- node messages ------------------------------------------------------------------ */

static BOOL send_node_msg(int to, UBYTE type, const char *text)
{
    struct NodeInfo *ni;
    struct Task *t = NULL;
    BOOL ok = FALSE;
    if (to < 1 || to > N.S->nodes) return FALSE;
    shared_lock(N.S);
    ni = &N.S->node[to - 1];
    if (ni->state >= NS_ONLINE) {
        UBYTE next = (ni->msg_head + 1) % NODE_MSGQ;
        if (next != ni->msg_tail) {
            struct NodeMsg *m = &ni->msgq[ni->msg_head];
            m->from = (UBYTE)N.node;
            m->type = type;
            str_copy(m->fromname, N.user.name, NAMELEN);
            str_copy(m->text, text, sizeof(m->text));
            ni->msg_head = next;
            t = ni->task;
            ok = TRUE;
        }
    }
    shared_unlock(N.S);
    if (t) Signal(t, SIGBREAKF_CTRL_D);
    return ok;
}

void page_node(void)
{
    char buf[8], text[110];
    int to, n;
    whos_online();
    tputs(L("misc.page_node.send_to_node", "\n|07Send to node |08(Enter cancels)|07: |15"));
    if (tgetline(buf, 3, GL_DIGITS) <= 0) return;
    to = atoi(buf);
    if (to == N.node) { tputs(L("misc.page_node.talking_to_yourself", "|08Talking to yourself?|07\n")); return; }
    shared_lock(N.S);
    n = (to >= 1 && to <= N.S->nodes && N.S->node[to - 1].state >= NS_ONLINE &&
         N.S->node[to - 1].available);
    shared_unlock(N.S);
    if (!n) { tputs(L("misc.page_node.nobody_available_on", "|12Nobody available on that node.|07\n")); return; }
    tputs(L("misc.page_node.message", "|07Message: |15"));
    if (tgetline(text, 100, 0) <= 0) return;
    if (send_node_msg(to, NM_TEXT, text)) tputs(L("misc.page_node.sent", "|10Sent.|07\n"));
    else tputs(L("misc.page_node.their_message_queue", "|12Their message queue is full.|07\n"));
}

/* ---- info ---------------------------------------------------------------------------- */

void sysinfo(void)
{
    char up[20];
    ULONG calls;
    UWORD nodes;
    bbs_datetimestr(N.S->started, up);
    shared_lock(N.S);
    calls = N.S->total_calls;
    nodes = N.S->nodes;
    shared_unlock(N.S);
    if (tshowfile("sysinfo")) { tpause(); return; }
    tputs(L("misc.sysinfo.system_information", "\n|09-=[ |15System Information|09 ]=-|07\n\n"));
    tprintf(L("misc.sysinfo.bbs_name", "  |07BBS name      |15%s\n"), cfg_str(N.cfg, "bbs_name", "NilBBS"));
    tprintf(L("misc.sysinfo.sysop", "  |07Sysop         |15%s\n"), cfg_str(N.cfg, "sysop_name", "Sysop"));
    tprintf(L("misc.sysinfo.software_nilbbs_native", "  |07Software      |15NilBBS %s|07 (native AmigaOS)\n"), BBS_VERSION);
    tprintf(L("misc.sysinfo.nodes", "  |07Nodes         |15%d\n"), (int)nodes);
    tprintf(L("misc.sysinfo.up_since_calls", "  |07Up since      |15%s|07, %lu calls\n"), up, calls);
    tprintf(L("misc.sysinfo.you_are_on", "  |07You are on    |15node %d|07 from |15%s\n"), N.node, N.ipstr);
    tprintf(L("misc.sysinfo.terminal_client_says", "  |07Terminal      |15%s|07, |15%s|07, |15%dx%d|07 (client says: %s)\n"),
            term_name(N.term), charset_name(N.charset), (int)N.cols, (int)N.rows,
            N.ttypes[0] ? N.ttypes : L("misc.sysinfo.nothing", "nothing"));
    tpause();
}

/* ---- the caller's own settings -------------------------------------------------------- */

void user_settings(void)
{
    char names[2][LANG_NAMELEN];
    int nlang = lang_list(names, 2);            /* only offer G when there's a choice */
    for (;;) {
        LONG k;
        tputs(L("misc.user_settings.your_settings", "\n|09-=[ |15Your Settings|09 ]=-|07\n\n"));
        tprintf(L("misc.user_settings.terminal_charset", "  |08[|15T|08] |07Terminal / charset   |15%s, %s%s\n"), term_name(N.term),
                charset_name(N.charset), (N.user.flags & UF_TERMSET) ? "" : L("misc.user_settings.auto", " |08(auto)"));
        tprintf(L("misc.user_settings.screen_size", "  |08[|15S|08] |07Screen size          |15%dx%d\n"), (int)N.cols, (int)N.rows);
        tprintf(L("misc.user_settings.expert_mode", "  |08[|15X|08] |07Expert mode          |15%s\n"), (N.user.flags & UF_EXPERT) ? L("misc.user_settings.on", "on") : L("misc.user_settings.off", "off"));
        tprintf(L("misc.user_settings.accept_node_messages", "  |08[|15N|08] |07Accept node messages |15%s\n"), (N.user.flags & UF_NOPAGE) ? L("misc.user_settings.no", "no") : L("misc.user_settings.yes", "yes"));
        tprintf(L("misc.user_settings.transfer_protocol", "  |08[|15F|08] |07Transfer protocol    |15%s\n"), proto_name(N.user.proto));
        tprintf(L("misc.user_settings.message_editor", "  |08[|15D|08] |07Message editor       |15%s\n"), (N.user.flags & UF_LINEEDIT) ? L("misc.user_settings.line", "line") : L("misc.user_settings.full_screen", "full-screen"));
        tprintf(L("misc.user_settings.location", "  |08[|15L|08] |07Location             |15%s\n"), N.user.location);
        tprintf(L("misc.user_settings.mail", "  |08[|15E|08] |07E-mail               |15%s\n"), N.user.email);
        if (nlang > 1)
            tprintf(L("misc.user_settings.language", "  |08[|15G|08] |07Language             |15%s\n"), lang_current());
        tputs(L("misc.user_settings.change_password", "  |08[|15P|08] |07Change password\n"));
        tputs(L("misc.user_settings.write_your_finger", "  |08[|15W|08] |07Write your finger plan\n"));
        tputs(L("misc.user_settings.auto_detect_terminal", "  |08[|15A|08] |07Auto-detect terminal again\n"));
        tputs(L("misc.user_settings.done_choice", "  |08[|15Q|08] |07Done\n\n|07Choice: "));
        k = tgethot(nlang > 1 ? "TSXNFDLEPGWAQ\r" : "TSXNFDLEPWAQ\r");
        if (k == KEY_HANGUP) return;
        tprintf("%c\n", (int)(k == '\r' ? 'Q' : k));
        switch (k) {
        case 'T':
            term_choose();
            N.user.termtype = N.term;
            N.user.charset = N.charset;
            N.user.flags |= UF_TERMSET;
            break;
        case 'A':
            tdetect();
            N.user.termtype = N.term;
            N.user.charset = N.charset;
            N.user.flags &= ~UF_TERMSET;
            tprintf(L("misc.user_settings.detected", "|07Detected |15%s|07, |15%s|07.\n"), term_name(N.term), charset_name(N.charset));
            break;
        case 'S': {
            char b[8];
            tputs(L("misc.user_settings.columns", "|07Columns: |15"));
            b[0] = 0;
            if (tgetline(b, 4, GL_DIGITS) > 0 && atoi(b) >= 20 && atoi(b) <= 255) N.cols = atoi(b);
            tputs(L("misc.user_settings.rows", "|07Rows: |15"));
            if (tgetline(b, 4, GL_DIGITS) > 0 && atoi(b) >= 5 && atoi(b) <= 255) N.rows = atoi(b);
            N.user.cols = (UBYTE)N.cols;
            N.user.rows = (UBYTE)N.rows;
            break;
        }
        case 'X': N.user.flags ^= UF_EXPERT; break;
        case 'G': {
            char pick[LANG_NAMELEN];
            if (lang_pick(pick)) {
                str_copy(N.user.lang, pick, sizeof(N.user.lang));
                lang_user();
            }
            break;
        }
        case 'W': plan_edit(); break;
        case 'D': N.user.flags ^= UF_LINEEDIT; break;
        case 'F': N.user.proto = (UBYTE)((N.user.proto + 1) % (PROTO_MAX + 1)); break;
        case 'N':
            N.user.flags ^= UF_NOPAGE;
            shared_lock(N.S);
            N.ni->available = (N.user.flags & UF_NOPAGE) ? 0 : 1;
            shared_unlock(N.S);
            break;
        case 'L':
            tputs(L("misc.user_settings.location_2", "|07Location: |15"));
            str_copy(N.user.location, N.user.location, LONGNAME);
            tgetline(N.user.location, LONGNAME, GL_EDIT);
            str_nopipe(N.user.location);
            break;
        case 'E':
            tputs(L("misc.user_settings.mail_2", "|07E-mail: |15"));
            tgetline(N.user.email, sizeof(N.user.email), GL_EDIT);
            str_nopipe(N.user.email);
            break;
        case 'P': {
            char old[40], p1[40], p2[40];
            tputs(L("misc.user_settings.current_password", "|07Current password: "));
            if (tgetline(old, 40, GL_MASK) < 0) return;
            if (!user_checkpass(&N.user, old)) { tputs(L("misc.user_settings.wrong", "|12Wrong.|07\n")); break; }
            tputs(L("misc.user_settings.new_password", "|07New password: "));
            if (tgetline(p1, 40, GL_MASK) < 0) return;
            tputs(L("misc.user_settings.again", "|07Again: "));
            if (tgetline(p2, 40, GL_MASK) < 0) return;
            if (strlen(p1) < 4 || strcmp(p1, p2)) { tputs(L("misc.user_settings.not_changed", "|12Not changed.|07\n")); break; }
            user_setpass(&N.user, p1);
            tputs(L("misc.user_settings.password_changed", "|10Password changed.|07\n"));
            break;
        }
        default:
            user_save();
            return;
        }
        user_save();
    }
}

/* ---- language (lang.c): the board's default before login, the caller's own after ---------- */

static const char *board_lang(void)
{
    return cfg_str(N.cfg, "language", LANG_ENGLISH);
}

void lang_board(void)
{
    lang_load(board_lang(), uni_to_cp437);
}

void lang_user(void)
{
    const char *want;
    N.user.lang[sizeof(N.user.lang) - 1] = 0;  /* taken from once-reserved bytes: be careful */
    want = N.user.lang[0] && lang_exists(N.user.lang) ? N.user.lang : board_lang();
    if (!str_icmp(want, lang_current()) && (lang_count() || !str_icmp(want, LANG_ENGLISH))) return;
    lang_load(want, uni_to_cp437);
}

/* a numbered list of the language files (BBS:Text/Language/<name>.lng); TRUE with the
   name in out when the caller picked one.  Nothing to pick from: FALSE at once. */
BOOL lang_pick(char *out)
{
    char names[24][LANG_NAMELEN], b[4];
    int n = lang_list(names, 24), i, k;
    if (n < 2) return FALSE;
    tputs(L("misc.lang_pick.title", "\n|15Language|07\n"));
    for (i = 0; i < n; i++)
        tprintf(L("misc.lang_pick.item", "  |15%2d|08) |07%s\n"), i + 1, names[i]);
    tprintf(L("misc.lang_pick.choice", "|07Choice |08(Enter = %s)|07: |15"), lang_current());
    if (tgetline(b, 3, GL_DIGITS) <= 0) return FALSE;
    k = atoi(b) - 1;
    if (k < 0 || k >= n) return FALSE;
    str_copy(out, names[k], LANG_NAMELEN);
    return TRUE;
}
