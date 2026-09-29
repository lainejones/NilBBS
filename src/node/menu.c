/*
 * menu.c - data-driven menus from BBS:Menus/<name>.mnu
 *
 *   ; comment
 *   title  = Main Menu
 *   screen = main              ; Text/main.ans (or .asc/.txt) drawn on entry
 *   header = main              ; or: a banner drawn ABOVE the generated item list
 *   prompt = |08[|15|TL|08 min] |11Main|08 > |07
 *   level  = 0                 ; minimum level to enter this menu
 *   item   = key | level | command | argument | description
 *   acs    = FA                ; optional extra condition to enter (acs.c)
 *   accent = 4                 ; colour of the hot keys + title tab (0-7, the |16..|23 order)
 *   frame  = 8                 ; colour of the box around the items (0-15)
 *
 * An item's level field may be an ACS instead of a number, e.g. "L20 FB".
 *
 * With no screen file the menu is drawn from the item descriptions, in a box:
 * two columns at 80+ columns (one below), hot keys as coloured "pills".  A
 * missing main.mnu falls back to a built-in menu so a bare install works.
 */
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "node.h"

#define MAX_ITEMS 48
#define MENU_DEPTH 8

struct MenuItem {
    char  key;
    UBYTE level;
    char  acs[40];
    char  cmd[16];
    char  arg[64];
    char  desc[48];
};

struct Menu {
    char title[48];
    char screen[32];
    char header[32];
    char prompt[160];
    UBYTE level;
    UBYTE accent, frame;                        /* colours of the generated menu */
    char acs[64];
    int  nitems;
    struct MenuItem item[MAX_ITEMS];
};

static const char *builtin_main[] = {
    "title  = Main Menu",
    "prompt = |08[|15|TL|08 min] |11Main Menu|08 > |07",
    "item = M | 0   | msgarea   |          | Change message area",
    "item = R | 0   | msgread   |          | Read messages",
    "item = N | 0   | msgnew    |          | New message scan",
    "item = P | 0   | msgpost   |          | Post a message",
    "item = E | 0   | email     |          | Private mail",
    "item = F | 0   | filearea  |          | Change file area",
    "item = L | 0   | filelist  |          | List files",
    "item = D | 0   | download  |          | Download",
    "item = U | 0   | upload    |          | Upload",
    "item = X | 0   | doors     |          | Doors / games",
    "item = O | 0   | oneliners |          | The wall (one-liners)",
    "item = W | 0   | who       |          | Who's online",
    "item = C | 0   | lastcallers |        | Last callers",
    "item = S | 0   | page      |          | Send a node message",
    "item = I | 0   | sysinfo   |          | System information",
    "item = A | 0   | status    |          | Your account status",
    "item = K | 0   | settings  |          | Your settings",
    "item = ! | 255 | sysop     |          | Sysop menu",
    "item = G | 0   | logoff    |          | Goodbye (log off)",
    NULL
};

static void parse_line(struct Menu *m, char *line)
{
    char *p = str_trim(line), *eq, *val;
    if (!*p || *p == ';' || *p == '#') return;
    if (!(eq = strchr(p, '='))) return;
    *eq = 0;
    val = str_trim(eq + 1);
    p = str_trim(p);
    if (!str_icmp(p, "title"))       str_copy(m->title, val, sizeof(m->title));
    else if (!str_icmp(p, "screen")) str_copy(m->screen, val, sizeof(m->screen));
    else if (!str_icmp(p, "header")) str_copy(m->header, val, sizeof(m->header));
    else if (!str_icmp(p, "prompt")) str_copy(m->prompt, val, sizeof(m->prompt));
    else if (!str_icmp(p, "level"))  m->level = (UBYTE)atoi(val);
    else if (!str_icmp(p, "acs"))    str_copy(m->acs, val, sizeof(m->acs));
    else if (!str_icmp(p, "accent")) m->accent = (UBYTE)(atoi(val) & 7);
    else if (!str_icmp(p, "frame"))  m->frame = (UBYTE)(atoi(val) & 15);
    else if (!str_icmp(p, "item") && m->nitems < MAX_ITEMS) {
        char *f[5];
        struct MenuItem *it = &m->item[m->nitems];
        LONG n = str_split(val, '|', f, 5);
        if (n < 3 || !f[0][0]) return;
        memset(it, 0, sizeof(*it));
        it->key = f[0][0];
        if (it->key >= 'a' && it->key <= 'z') it->key -= 32;
        {   /* a number is a level; anything else is an ACS */
            char *lv = str_trim(f[1]);
            if (*lv >= '0' && *lv <= '9' && !strchr(lv, ' ')) it->level = (UBYTE)atoi(lv);
            else str_copy(it->acs, lv, sizeof(it->acs));
        }
        str_copy(it->cmd, f[2], sizeof(it->cmd));
        if (n > 3) str_copy(it->arg, f[3], sizeof(it->arg));
        if (n > 4) str_copy(it->desc, f[4], sizeof(it->desc));
        m->nitems++;
    }
}

static BOOL menu_load(const char *name, struct Menu *m)
{
    struct LineReader lr;
    char path[PATHLEN], line[LINELEN];
    BOOL open = FALSE;
    memset(m, 0, sizeof(*m));
    m->accent = 4;                              /* red hot keys in a dark grey box */
    m->frame = 8;
    if (strlen(name) >= NAMELEN) return FALSE;
    /* a translation's own menus (BBS:Menus/<language>/) first; the rest are shared */
    if (lang_count()) {
        sprintf(path, "BBS:Menus/%s/%s.mnu", lang_current(), name);
        open = lr_open(&lr, path);
    }
    if (!open) {
        sprintf(path, "BBS:Menus/%s.mnu", name);
        open = lr_open(&lr, path);
    }
    if (open) {
        while (lr_gets(&lr, line, sizeof(line)) >= 0) parse_line(m, line);
        lr_close(&lr);
        return m->nitems > 0;
    }
    if (!str_icmp(name, "main")) {
        const char **b;
        for (b = builtin_main; *b; b++) {
            str_copy(line, *b, sizeof(line));
            parse_line(m, line);
        }
        return TRUE;
    }
    return FALSE;
}

static BOOL item_ok(struct MenuItem *it)
{
    return it->level <= N.user.level && acs_check(it->acs);
}

/* ---- the generated menu -----------------------------------------------------
 *
 *   +------------------------[ Main Menu ]------------------------+
 *   | [M] Message base                   [F] File areas           |
 *   +-------------------------------------------------------------+
 *
 * drawn in CP437 single-line box characters, the title in a tab of the accent
 * colour and each hot key a "pill" (half blocks round the key on the accent
 * colour).  The box lines degrade by themselves (charset.c: DEC graphics for
 * VT100, + - | for 7-bit and Amiga-charset callers).  The pills need CP437 or
 * UTF-8 AND colour: an ANSI caller without the blocks (the Amiga console, a
 * Latin-1 terminal) gets the key on a coloured background, a caller without
 * colour gets [K].  Every line stays within 79 columns.
 */
static BOOL menu_blocks(void)
{
    return N.term == TT_ANSI && !N.local && (N.charset == CS_CP437 || N.charset == CS_UTF8);
}

/* visible width of a string with |xx colour codes */
int vis_len(const char *s)
{
    int n = 0;
    while (*s) {
        if (s[0] == '|' && s[1] >= '0' && s[1] <= '2' && s[2] >= '0' && s[2] <= '9') { s += 3; continue; }
        n++; s++;
    }
    return n;
}

void put_rep(const char *glyph, int n)
{
    char buf[100];
    int i = 0;
    while (n-- > 0 && i < (int)sizeof(buf) - 1) buf[i++] = *glyph;
    buf[i] = 0;
    tputs(buf);
}

/* text in the accent colour: a pill, a coloured block or [brackets] */
static void put_tab(struct Menu *m, const char *s, BOOL wide)
{
    if (menu_blocks())
        tprintf(wide ? "|%02d\xDE|15|%02d %s |16|%02d\xDD" : "|%02d\xDE|15|%02d%s|16|%02d\xDD",
                m->accent, 16 + m->accent, s, m->accent);
    else if (N.term == TT_ANSI)
        tprintf(wide ? "|15|%02d  %s  |16" : "|15|%02d %s |16", 16 + m->accent, s);
    else
        tprintf(wide ? "[ %s ]" : "[%s]", s);
}

/* the top edge: plain, or with the title in a tab in the middle */
static void frame_top(struct Menu *m, const char *title, int inner)
{
    int tlen = title ? vis_len(title) + 4 : 0, left, right;
    if (tlen > inner - 2) { tlen = 0; title = NULL; }
    left = (inner + 1 - tlen) / 2;
    right = inner + 1 - tlen - left;
    tprintf(" |%02d\xDA", m->frame);
    put_rep("\xC4", left);
    if (title) { put_tab(m, title, TRUE); tprintf("|%02d", m->frame); }
    put_rep("\xC4", right);
    tputs("\xBF|07\n");
}

static void menu_draw(struct Menu *m)
{
    int i, col = 0, visible = 0;
    int cols = N.cols > 80 ? 80 : N.cols;
    int ncol = cols >= 80 ? 2 : 1;
    int inner = cols - 5;                       /* " | " + the items + "|" + a spare column */
    int cw, dw;
    BOOL header = FALSE;
    if (m->screen[0] && tshowfile(m->screen)) return;
    if (inner < 20) inner = 20;
    cw = inner / ncol;                          /* a column: key (3), blank, text, blank */
    dw = cw - 5;

    tputs("\n");
    /* a header banner replaces the title (the box then has a plain top); the items
     * stay generated, so menu edits and level/ACS-gated items still show as they are */
    if (m->header[0] && tshowfile(m->header)) { tputs("\n"); header = TRUE; }
    frame_top(m, header || !m->title[0] ? NULL : m->title, inner);
    for (i = 0; i < m->nitems; i++) {
        struct MenuItem *it = &m->item[i];
        char d[sizeof(it->desc)], k[2];
        int n;
        if (!item_ok(it) || !it->desc[0]) continue;
        visible++;
        if (col == 0) tprintf(" |%02d\xB3 ", m->frame);
        k[0] = it->key; k[1] = 0;
        put_tab(m, k, FALSE);
        str_copy(d, it->desc, sizeof(d));
        if ((int)strlen(d) > dw) d[dw] = 0;
        n = strlen(d);
        tputs(" |07");
        tputraw((const UBYTE *)d, n, CS_CP437); /* as it is: a '|' in the text is no code */
        put_rep(" ", cw - 4 - n);
        if (++col >= ncol) { put_rep(" ", inner % ncol); tprintf("|%02d\xB3|07\n", m->frame); col = 0; }
    }
    if (col) { put_rep(" ", cw * (ncol - col) + inner % ncol); tprintf("|%02d\xB3|07\n", m->frame); }
    if (!visible) {
        const char *t = L("menu.draw.nothing_here", "(nothing here for you yet)");
        tprintf(" |%02d\xB3 |08", m->frame);
        tputs(t);
        put_rep(" ", inner - vis_len(t));
        tprintf("|%02d\xB3|07\n", m->frame);
    }
    tprintf(" |%02d\xC0", m->frame);
    put_rep("\xC4", inner + 1);
    tputs("\xD9|07\n");
}

/* run one command; returns 0 = stay, 1 = logoff, 2 = goto (menu in newmenu) */
static int run_cmd(struct MenuItem *it, char *newmenu, int *gosub)
{
    const char *c = it->cmd;
    const char *a = it->arg;
    *gosub = 0;

    if (!str_icmp(c, "menu"))   { str_copy(newmenu, a, NAMELEN); return 2; }
    if (!str_icmp(c, "gosub"))  { str_copy(newmenu, a, NAMELEN); *gosub = 1; return 2; }
    if (!str_icmp(c, "return")) { newmenu[0] = 0; *gosub = -1; return 2; }
    if (!str_icmp(c, "logoff")) {
        if (tyesno(L("menu.run_cmd.log_off_now", "\n|07Log off now?"), TRUE)) return 1;
        return 0;
    }
    if (!str_icmp(c, "hangup")) return 1;
    if (!str_icmp(c, "text"))   { tpage_start(); if (!tshowfile(a)) tputs(L("menu.run_cmd.missing_screen", "|08(missing screen)|07\n")); tpage_end(); tpause(); return 0; }
    if (!str_icmp(c, "door"))   { door_run(a); return 0; }
    if (!str_icmp(c, "doors"))  { door_list(); return 0; }
    if (!str_icmp(c, "msgarea")) { msg_select_area(); return 0; }
    if (!str_icmp(c, "msgread")) { msg_read_area(FALSE); return 0; }
    if (!str_icmp(c, "msgnew"))  { msg_scan_all(); return 0; }
    if (!str_icmp(c, "msgpost")) { msg_post(NULL, NULL, 0); return 0; }
    if (!str_icmp(c, "email"))   { msg_email(); return 0; }
    if (!str_icmp(c, "filearea")) { file_select_area(); return 0; }
    if (!str_icmp(c, "filelist")) { file_list(FALSE); return 0; }
    if (!str_icmp(c, "filenew"))  { file_list(TRUE); return 0; }
    if (!str_icmp(c, "download")) { file_download(); return 0; }
    if (!str_icmp(c, "upload"))   { file_upload(); return 0; }
    if (!str_icmp(c, "filesearch")) { file_search(); return 0; }
    if (!str_icmp(c, "filestats"))  { file_stats(); return 0; }
    if (!str_icmp(c, "filesubop"))  { file_subop(); return 0; }
    if (!str_icmp(c, "join"))       { conf_join(a); return 0; }
    if (!str_icmp(c, "bulletins"))  { bulletins(); return 0; }
    if (!str_icmp(c, "vote"))       { voting_booth(); return 0; }
    if (!str_icmp(c, "finger"))     { finger(); return 0; }
    if (!str_icmp(c, "qwkdown"))    { qwk_download(); return 0; }
    if (!str_icmp(c, "qwkup"))      { qwk_upload(); return 0; }
    if (!str_icmp(c, "tele"))       { teleconference(a); return 0; }
    if (!str_icmp(c, "pagesysop"))  { page_sysop(); return 0; }
    if (!str_icmp(c, "chat"))       { caller_chat(); return 0; }
    if (!str_icmp(c, "oneliners"))  { oneliners(); return 0; }
    if (!str_icmp(c, "status"))     { user_status(); return 0; }
    if (!str_icmp(c, "lastcallers")) { lastcallers_show(); return 0; }
    if (!str_icmp(c, "who"))       { whos_online(); return 0; }
    if (!str_icmp(c, "users"))     { userlist(); return 0; }
    if (!str_icmp(c, "page"))      { page_node(); return 0; }
    if (!str_icmp(c, "sysinfo"))   { sysinfo(); return 0; }
    if (!str_icmp(c, "settings"))  { user_settings(); return 0; }
    if (!str_icmp(c, "terminal"))  { tdetect(); tprintf(L("menu.run_cmd.detected", "|07Detected |15%s|07, |15%s|07.\n"), term_name(N.term), charset_name(N.charset)); return 0; }
    if (!str_icmp(c, "sysop"))     { if (N.sysop) sysop_menu(); return 0; }
    if (!str_icmp(c, "feedback"))  { msg_post(cfg_str(N.cfg, "sysop_name", "Sysop"), "Feedback", 0); return 0; }
    if (!str_icmp(c, "time")) {
        LONG t = time_left_mins();
        if (t < 0) tputs(L("menu.run_cmd.you_have_unlimited", "\n|07You have |15unlimited|07 time.\n"));
        else tprintf(L("menu.run_cmd.you_have_minutes", "\n|07You have |15%ld|07 minutes left.\n"), t);
        return 0;
    }
    tprintf(L("menu.run_cmd.unknown_menu_command", "\n|12Unknown menu command \"%s\".|07\n"), c);
    return 0;
}

void menu_run(const char *start)
{
    struct Menu *m = AllocVec(sizeof(struct Menu), 0);
    char cur[NAMELEN], next[NAMELEN];
    char stack[MENU_DEPTH][NAMELEN];
    int depth = 0;
    BOOL redraw = TRUE;

    if (!m) return;
    str_copy(cur, start, sizeof(cur));
    if (!menu_load(cur, m)) {
        str_copy(cur, "main", sizeof(cur));
        menu_load(cur, m);
    }

    while (N.online) {
        char keys[MAX_ITEMS + 4];
        int i, nk = 0, r, gosub;
        LONG k;

        user_refresh();                         /* the sysop may have changed our level (validation) */
        if (redraw && !(N.user.flags & UF_EXPERT)) menu_draw(m);
        redraw = FALSE;

        for (i = 0; i < m->nitems; i++)
            if (item_ok(&m->item[i])) keys[nk++] = m->item[i].key;
        keys[nk++] = '?';
        keys[nk] = 0;

        tputs("\n");
        N.reprompt = m->prompt[0] ? m->prompt : L("menu.run.command", "|07Command: ");
        tputs(N.reprompt);
        do k = tgethot(keys); while (k == KEY_NONE);
        N.reprompt = NULL;
        if (k == KEY_HANGUP) break;
        tprintf("%c\n", (int)k);
        if (k == '?') { redraw = TRUE; continue; }

        for (i = 0; i < m->nitems; i++)
            if (m->item[i].key == k && item_ok(&m->item[i])) break;
        if (i >= m->nitems) continue;

        r = run_cmd(&m->item[i], next, &gosub);
        if (r == 1) break;
        if (r == 2) {
            if (gosub == -1) {
                if (depth == 0) continue;
                str_copy(next, stack[--depth], NAMELEN);
            } else if (gosub == 1) {
                if (depth < MENU_DEPTH) str_copy(stack[depth++], cur, NAMELEN);
            }
            {
                struct Menu *nm = AllocVec(sizeof(struct Menu), 0);
                if (nm && menu_load(next, nm) && nm->level <= N.user.level && acs_check(nm->acs)) {
                    FreeVec(m);
                    m = nm;
                    str_copy(cur, next, sizeof(cur));
                    redraw = TRUE;
                } else {
                    if (nm) FreeVec(nm);
                    if (gosub == 1) depth--;
                    tprintf(L("menu.run.menu_is_not", "|12Menu \"%s\" is not available.|07\n"), next);
                }
            }
            set_menu_activity(m->title[0] ? m->title : cur, strstr(m->prompt, "|FA") != NULL);
        }
    }
    FreeVec(m);
}
