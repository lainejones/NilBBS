/*
 * BBSSchedule - the sysop's Workbench editor for NilBBS's scheduled tasks
 * (BBS:Config/Events.cfg: nightly maintenance, mail tossing, backups...).
 *
 * The list of tasks; New / Delete / Run now; an edit panel for the chosen one
 * (name, a time or every N minutes, the days, the command, exclusive + how many
 * minutes' warning callers get).  "Add door maintenance" picks a door from
 * Doors.cfg and schedules its maint command on its own ("BBSMaint DOOR=<tag>";
 * the nightly BBSMaint then leaves that door alone).  Save rewrites Events.cfg -
 * keeping its comments - and tells the running BBS to reload it (ARexx RELOAD
 * to the NILBBS port); Run now sends EVENT <name>.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <intuition/gadgetclass.h>
#include <libraries/gadtools.h>
#include <rexx/storage.h>
#include <rexx/rxslib.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>
#include <proto/graphics.h>
#include <proto/rexxsyslib.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "../common/bbs.h"
#include "../common/cfg.h"

static const char __attribute__((used)) verstag[] = "$VER: BBSSchedule " BBS_VERSION " (" BBS_VERDATE ")";

struct IntuitionBase *IntuitionBase;
struct Library *GadToolsBase;
struct GfxBase *GfxBase;
struct RxsLib *RexxSysBase;

#define EVENTS_CFG "BBS:Config/Events.cfg"
#define MAXEV   24
#define TAGLEN  16
#define CMDLEN  250

struct Ev {
    char  tag[TAGLEN];
    char  time[8];              /* "04:00", or "" when it runs every N minutes */
    LONG  every;                /* minutes, when time is "" */
    UBYTE days;                 /* bit 0 = Sunday .. bit 6 = Saturday */
    BOOL  excl;
    LONG  warn;
    char  cmd[CMDLEN];
    char *comment;              /* the comment lines above its [TAG] (kept when saving) */
    char *extra;                /* keys we don't edit, kept as they were */
};

static struct Ev ev[MAXEV];
static int nev, sel = -1;
static char *header, *trailer;      /* the file's opening comments, and what follows the last task */
static BOOL dirty;

static const char *DAYNAME[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *daylong[7] = { "sun", "mon", "tue", "wed", "thu", "fri", "sat" };

/* ---- Events.cfg: read and write, comments and all ------------------------------------------ */

static char *dupstr(const char *s) { char *d = malloc(strlen(s) + 1); if (d) strcpy(d, s); return d; }
static void addtext(char **buf, const char *line)
{
    size_t a = *buf ? strlen(*buf) : 0, b = strlen(line);
    char *n = realloc(*buf, a + b + 2);
    if (!n) return;
    memcpy(n + a, line, b);
    n[a + b] = '\n'; n[a + b + 1] = 0;
    *buf = n;
}
static char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

static void free_all(void)
{
    int i;
    for (i = 0; i < nev; i++) { free(ev[i].comment); free(ev[i].extra); }
    free(header); free(trailer);
    header = trailer = NULL;
    nev = 0;
}

static void load_events(void)
{
    BPTR f;
    char line[400], *pending = NULL;
    struct Ev *cur = NULL;
    free_all();
    if (!(f = Open((STRPTR)EVENTS_CFG, MODE_OLDFILE))) return;
    while (FGets(f, (STRPTR)line, sizeof(line))) {
        char *t;
        line[strcspn(line, "\r\n")] = 0;
        t = trim(line);
        if (*t == '[' && strchr(t, ']')) {                     /* a new task */
            if (nev >= MAXEV) break;
            if (!nev && pending) {                             /* the file's header vs this task's comment: */
                char *cut = NULL, *p = pending;                /* split at the last blank line */
                while ((p = strstr(p, "\n\n"))) { cut = p; p++; }
                if (cut) { cut[1] = 0; header = pending; pending = dupstr(cut + 2); }
                else { header = pending; pending = NULL; }
            }
            cur = &ev[nev++];
            memset(cur, 0, sizeof(*cur));
            *strchr(t, ']') = 0;
            str_copy(cur->tag, t + 1, TAGLEN);
            cur->days = 0x7F; cur->warn = 5;
            cur->comment = pending; pending = NULL;
            continue;
        }
        if (!*t || *t == ';' || *t == '#') { addtext(&pending, line); continue; }
        if (!cur) { addtext(&pending, line); continue; }
        if (pending) {                                          /* a comment inside a task: keep it with the extras */
            addtext(&cur->extra, trim(pending)); free(pending); pending = NULL;
        }
        {
            char *eq = strchr(t, '='), *k, *v;
            if (!eq) { addtext(&cur->extra, t); continue; }
            *eq = 0; k = trim(t); v = trim(eq + 1);
            if (!str_icmp(k, "time")) str_copy(cur->time, v, sizeof(cur->time));
            else if (!str_icmp(k, "every")) cur->every = atol(v);
            else if (!str_icmp(k, "days")) {
                if (!str_icmp(v, "daily") || !strcmp(v, "*")) cur->days = 0x7F;
                else { int d; cur->days = 0; for (d = 0; d < 7; d++) if (str_istr(v, daylong[d])) cur->days |= 1 << d; }
            }
            else if (!str_icmp(k, "exclusive")) cur->excl = !str_icmp(v, "yes") || !str_icmp(v, "true") || !strcmp(v, "1");
            else if (!str_icmp(k, "warn")) cur->warn = atol(v);
            else if (!str_icmp(k, "command")) str_copy(cur->cmd, v, CMDLEN);
            else { char kv[400]; sprintf(kv, "%-9s = %s", k, v); addtext(&cur->extra, kv); }
        }
    }
    Close(f);
    if (!nev) { header = pending; pending = NULL; }
    trailer = pending;
    /* a comment block that came after the last task's keys is the trailer (the commented examples) */
}

static BOOL save_events(void)
{
    BPTR f;
    int i, d;
    char line[400];
    const char *tmp = EVENTS_CFG ".new";
    if (!(f = Open((STRPTR)tmp, MODE_NEWFILE))) return FALSE;
    if (header) FPuts(f, (STRPTR)header);
    else FPuts(f, (STRPTR)"; NilBBS events                                            BBS:Config/Events.cfg\n"
                          "; (written by BBSSchedule - see the NilBBS manual for the keys)\n\n");
    for (i = 0; i < nev; i++) {
        struct Ev *e = &ev[i];
        if (e->comment) FPuts(f, (STRPTR)e->comment);
        sprintf(line, "[%s]\n", e->tag); FPuts(f, (STRPTR)line);
        if (e->time[0]) { sprintf(line, "time      = %s\n", e->time); FPuts(f, (STRPTR)line); }
        else { sprintf(line, "every     = %ld\n", e->every); FPuts(f, (STRPTR)line); }
        if (e->days == 0x7F) FPuts(f, (STRPTR)"days      = daily\n");
        else {
            FPuts(f, (STRPTR)"days      =");
            for (d = 0; d < 7; d++) if (e->days & (1 << d)) { FPuts(f, (STRPTR)" "); FPuts(f, (STRPTR)DAYNAME[d]); }
            FPuts(f, (STRPTR)"\n");
        }
        sprintf(line, "exclusive = %s\n", e->excl ? "yes" : "no"); FPuts(f, (STRPTR)line);
        if (e->excl) { sprintf(line, "warn      = %ld\n", e->warn); FPuts(f, (STRPTR)line); }
        sprintf(line, "command   = %s\n", e->cmd); FPuts(f, (STRPTR)line);
        if (e->extra) FPuts(f, (STRPTR)e->extra);
        FPuts(f, (STRPTR)"\n");
    }
    if (trailer) FPuts(f, (STRPTR)trailer);
    if (!Close(f)) return FALSE;
    DeleteFile((STRPTR)EVENTS_CFG);
    return Rename((STRPTR)tmp, (STRPTR)EVENTS_CFG) != 0;
}

/* ---- telling the running BBS (its ARexx port; no RexxMast needed) ---------------------------- */

static LONG bbs_rexx(const char *cmd)
{
    struct MsgPort *port, *reply;
    struct RexxMsg *rm;
    LONG rc = -1;
    if (!(RexxSysBase = (struct RxsLib *)OpenLibrary((STRPTR)"rexxsyslib.library", 0))) return -1;
    if ((reply = CreateMsgPort())) {
        if ((rm = CreateRexxMsg(reply, NULL, NULL))) {
            rm->rm_Args[0] = (STRPTR)CreateArgstring((STRPTR)cmd, strlen(cmd));
            rm->rm_Action = RXCOMM;
            Forbid();
            if ((port = FindPort((STRPTR)"NILBBS"))) PutMsg(port, (struct Message *)rm);
            Permit();
            if (port) { WaitPort(reply); GetMsg(reply); rc = rm->rm_Result1; }
            DeleteArgstring((UBYTE *)rm->rm_Args[0]);
            DeleteRexxMsg(rm);
        }
        DeleteMsgPort(reply);
    }
    CloseLibrary((struct Library *)RexxSysBase);
    return rc;
}

/* ---- the window --------------------------------------------------------------------------------- */

enum { GID_LIST = 1, GID_NEW, GID_DOOR, GID_DEL, GID_RUN, GID_NAME, GID_WHEN, GID_TIME, GID_EVERY,
       GID_DAY0, GID_CMD = GID_DAY0 + 7, GID_EXCL, GID_WARN, GID_SAVE, GID_REVERT, GID_QUIT };

static struct Window *win;
static struct Screen *scr;
static APTR vi;
static struct Gadget *glist, *gad[GID_QUIT + 1];
static struct List evlist;
static struct Node evnode[MAXEV];
static char evtext[MAXEV][100];
static char wtitle[80];
static const char *WHEN[] = { "At a time", "Every", NULL };

static void list_init(struct List *l)
{
    l->lh_Head = (struct Node *)&l->lh_Tail; l->lh_Tail = NULL; l->lh_TailPred = (struct Node *)&l->lh_Head;
}
static void title(const char *what)
{
    sprintf(wtitle, "NilBBS Schedule%s%s", what ? " - " : "", what ? what : (dirty ? " - not saved yet" : ""));
    if (win) SetWindowTitles(win, (UBYTE *)wtitle, (UBYTE *)~0);
}

/* the days, short: "daily", "Mon-Fri", "Sat Sun", "Mo We Fr" (the week read Monday first) */
static void days_text(UBYTE m, char *out)
{
    static const int wk[7] = { 1, 2, 3, 4, 5, 6, 0 };
    int i, n = 0, first = -1, last = -1, runs = 0;
    out[0] = 0;
    if (m == 0x7F) { strcpy(out, "daily"); return; }
    if (!m) { strcpy(out, "never"); return; }
    for (i = 0; i < 7; i++)
        if (m & (1 << wk[i])) { n++; if (first < 0) first = i; if (last != i - 1) runs++; last = i; }
    if (runs == 1 && n >= 3) { sprintf(out, "%s-%s", DAYNAME[wk[first]], DAYNAME[wk[last]]); return; }
    for (i = 0; i < 7; i++)
        if (m & (1 << wk[i])) { if (out[0]) strcat(out, " "); strncat(out, DAYNAME[wk[i]], n <= 3 ? 3 : 2); }
}

static void describe(int i)
{
    struct Ev *e = &ev[i];
    char when[20], days[30];
    if (e->time[0]) sprintf(when, "at %s", e->time); else sprintf(when, "every %ldm", e->every);
    days_text(e->days, days);
    sprintf(evtext[i], "%-12.12s %-11s %-14.14s %s %.40s", e->tag, when, days, e->excl ? "X" : " ", e->cmd);
}
static void show_list(void)
{
    int i;
    GT_SetGadgetAttrs(gad[GID_LIST], win, NULL, GTLV_Labels, ~0UL, TAG_END);
    list_init(&evlist);
    for (i = 0; i < nev; i++) { describe(i); evnode[i].ln_Name = evtext[i]; AddTail(&evlist, &evnode[i]); }
    GT_SetGadgetAttrs(gad[GID_LIST], win, NULL, GTLV_Labels, (ULONG)&evlist,
                      GTLV_Selected, sel >= 0 ? (ULONG)sel : ~0UL, TAG_END);
}

static void set_disabled(BOOL off)
{
    int g;
    for (g = GID_NAME; g <= GID_WARN; g++) GT_SetGadgetAttrs(gad[g], win, NULL, GA_Disabled, off, TAG_END);
    GT_SetGadgetAttrs(gad[GID_DEL], win, NULL, GA_Disabled, off, TAG_END);
    GT_SetGadgetAttrs(gad[GID_RUN], win, NULL, GA_Disabled, off, TAG_END);
}

/* the chosen task into the edit panel */
static void show_fields(void)
{
    struct Ev *e;
    int d;
    if (sel < 0 || sel >= nev) { sel = -1; set_disabled(TRUE); return; }
    e = &ev[sel];
    set_disabled(FALSE);
    GT_SetGadgetAttrs(gad[GID_NAME], win, NULL, GTST_String, (ULONG)e->tag, TAG_END);
    GT_SetGadgetAttrs(gad[GID_WHEN], win, NULL, GTCY_Active, e->time[0] ? 0UL : 1UL, TAG_END);
    GT_SetGadgetAttrs(gad[GID_TIME], win, NULL, GTST_String, (ULONG)(e->time[0] ? e->time : ""), GA_Disabled, !e->time[0], TAG_END);
    GT_SetGadgetAttrs(gad[GID_EVERY], win, NULL, GTIN_Number, (ULONG)(e->every > 0 ? e->every : 30), GA_Disabled, e->time[0] != 0, TAG_END);
    for (d = 0; d < 7; d++) GT_SetGadgetAttrs(gad[GID_DAY0 + d], win, NULL, GTCB_Checked, (e->days >> d) & 1, TAG_END);
    GT_SetGadgetAttrs(gad[GID_CMD], win, NULL, GTST_String, (ULONG)e->cmd, TAG_END);
    GT_SetGadgetAttrs(gad[GID_EXCL], win, NULL, GTCB_Checked, (ULONG)e->excl, TAG_END);
    GT_SetGadgetAttrs(gad[GID_WARN], win, NULL, GTIN_Number, (ULONG)e->warn, GA_Disabled, !e->excl, TAG_END);
}

static const char *sbuf(int g) { return (const char *)((struct StringInfo *)gad[g]->SpecialInfo)->Buffer; }
static LONG ibuf(int g) { return ((struct StringInfo *)gad[g]->SpecialInfo)->LongInt; }

static BOOL tag_used(const char *t, int except)
{
    int i;
    for (i = 0; i < nev; i++) if (i != except && !str_icmp(ev[i].tag, t)) return TRUE;
    return FALSE;
}

/* the edit panel back into the chosen task (string gadgets only report Return, so read them all) */
static void pull_fields(void)
{
    struct Ev *e;
    char t[TAGLEN], tm[8];
    int i, j, d;
    BOOL changed = FALSE;
    if (sel < 0 || sel >= nev) return;
    e = &ev[sel];
    for (i = j = 0; sbuf(GID_NAME)[i] && j < TAGLEN - 1; i++) {
        char c = toupper((unsigned char)sbuf(GID_NAME)[i]);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') t[j++] = c;
    }
    t[j] = 0;
    if (j && strcmp(t, e->tag) && !tag_used(t, sel)) { strcpy(e->tag, t); changed = TRUE; }
    if (e->time[0]) {
        int h = -1, m = -1;
        if (sscanf(sbuf(GID_TIME), "%d:%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60) {
            sprintf(tm, "%02d:%02d", h, m);
            if (strcmp(tm, e->time)) { strcpy(e->time, tm); changed = TRUE; }
        }
    } else {
        LONG n = ibuf(GID_EVERY);
        if (n >= 1 && n <= 1440 && n != e->every) { e->every = n; changed = TRUE; }
    }
    for (d = 0; d < 7; d++) {
        BOOL on = (gad[GID_DAY0 + d]->Flags & GFLG_SELECTED) != 0;
        if (on != ((e->days >> d) & 1)) { e->days ^= 1 << d; changed = TRUE; }
    }
    if (strcmp(sbuf(GID_CMD), e->cmd)) { str_copy(e->cmd, sbuf(GID_CMD), CMDLEN); changed = TRUE; }
    if (((gad[GID_EXCL]->Flags & GFLG_SELECTED) != 0) != (e->excl != 0)) { e->excl = !e->excl; changed = TRUE; }
    if (ibuf(GID_WARN) >= 0 && ibuf(GID_WARN) <= 60 && ibuf(GID_WARN) != e->warn) { e->warn = ibuf(GID_WARN); changed = TRUE; }
    if (changed) { dirty = TRUE; show_list(); title(NULL); }
}

static int new_event(const char *base, const char *cmd, BOOL excl)
{
    struct Ev *e;
    char t[TAGLEN];
    int n = 1;
    if (nev >= MAXEV) { DisplayBeep(NULL); return -1; }
    str_copy(t, base, TAGLEN);
    while (tag_used(t, -1)) { char b[TAGLEN]; str_copy(b, base, TAGLEN - 3); sprintf(t, "%s%d", b, ++n); }
    e = &ev[nev];
    memset(e, 0, sizeof(*e));
    strcpy(e->tag, t);
    strcpy(e->time, "04:30");
    e->every = 30; e->days = 0x7F; e->excl = excl; e->warn = 2;
    str_copy(e->cmd, cmd, CMDLEN);
    dirty = TRUE;
    return nev++;
}

/* the door picker: every door in Doors.cfg, with its maint command */
static int pick_door(char *tag_out, char *cmd_out)
{
    struct Cfg *dc = cfg_load("BBS:Config/Doors.cfg");
    static char dtext[32][90], dtag[32][NAMELEN], dmaint[32][CMDLEN];
    static struct Node dnode[32];
    struct List dl;
    struct Gadget *gl = NULL, *g, *lv;
    struct NewGadget ng;
    struct Window *w;
    LONG i, n = dc ? cfg_sections(dc) : 0, nd = 0, pick = -1;
    BOOL done = FALSE, ok = FALSE;
    UWORD fh = scr->Font->ta_YSize, bh = fh + 6;
    list_init(&dl);
    for (i = 0; i < n && nd < 32; i++) {
        const char *t = cfg_section(dc, i);
        str_copy(dtag[nd], t, NAMELEN);
        str_copy(dmaint[nd], cfg_sget(dc, t, "maint", ""), CMDLEN);
        sprintf(dtext[nd], "%-26.26s %s", cfg_sget(dc, t, "name", t), dmaint[nd][0] ? dmaint[nd] : "(no maint command)");
        dnode[nd].ln_Name = dtext[nd];
        AddTail(&dl, &dnode[nd]);
        nd++;
    }
    if (dc) cfg_free(dc);
    if (!nd) {
        struct EasyStruct es = { sizeof(struct EasyStruct), 0, (UBYTE *)"NilBBS Schedule",
                                 (UBYTE *)"There are no doors in BBS:Config/Doors.cfg.", (UBYTE *)"OK" };
        EasyRequestArgs(win, &es, NULL, NULL);
        return 0;
    }
    g = CreateContext(&gl);
    memset(&ng, 0, sizeof(ng));
    ng.ng_VisualInfo = vi; ng.ng_TextAttr = scr->Font;
    ng.ng_LeftEdge = scr->WBorLeft + 8; ng.ng_TopEdge = scr->WBorTop + fh + 1 + fh + 8;
    ng.ng_Width = 520; ng.ng_Height = fh * 8 + 4;
    ng.ng_GadgetText = "Schedule which door's maintenance?"; ng.ng_Flags = PLACETEXT_ABOVE; ng.ng_GadgetID = 1;
    g = lv = CreateGadget(LISTVIEW_KIND, g, &ng, GTLV_Labels, (ULONG)&dl, GTLV_ShowSelected, 0UL, TAG_END);
    ng.ng_TopEdge += ng.ng_Height + 4; ng.ng_Height = bh; ng.ng_Width = 256; ng.ng_Flags = 0;
    ng.ng_GadgetText = "Schedule it"; ng.ng_GadgetID = 2;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    ng.ng_LeftEdge += 264; ng.ng_GadgetText = "Cancel"; ng.ng_GadgetID = 3;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    if (!g) { FreeGadgets(gl); return 0; }
    w = OpenWindowTags(NULL, WA_Title, (ULONG)"NilBBS Schedule - door maintenance",
                       WA_Left, win->LeftEdge + 30, WA_Top, win->TopEdge + 16,
                       WA_InnerWidth, 536, WA_InnerHeight, ng.ng_TopEdge + bh + 6 - scr->WBorTop - fh - 1,
                       WA_Gadgets, (ULONG)gl, WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE,
                       WA_Activate, TRUE, WA_PubScreen, (ULONG)scr,
                       WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | LISTVIEWIDCMP | BUTTONIDCMP, TAG_END);
    if (!w) { FreeGadgets(gl); return 0; }
    GT_RefreshWindow(w, NULL);
    while (!done) {
        struct IntuiMessage *im;
        WaitPort(w->UserPort);
        while ((im = GT_GetIMsg(w->UserPort))) {
            ULONG cls = im->Class; UWORD code = im->Code; struct Gadget *gg = (struct Gadget *)im->IAddress;
            GT_ReplyIMsg(im);
            if (cls == IDCMP_CLOSEWINDOW) done = TRUE;
            else if (cls == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(w); GT_EndRefresh(w, TRUE); }
            else if (cls == IDCMP_GADGETUP) {
                if (gg->GadgetID == 1) pick = code;
                else if (gg->GadgetID == 2) { if (pick >= 0) { ok = TRUE; done = TRUE; } else DisplayBeep(scr); }
                else done = TRUE;
            }
        }
    }
    CloseWindow(w);
    FreeGadgets(gl);
    if (!ok) return 0;
    strcpy(tag_out, dtag[pick]);
    if (dmaint[pick][0]) sprintf(cmd_out, "BBS:BBSMaint DOOR=%s", dtag[pick]);
    else cmd_out[0] = 0;
    return 1;
}

static UWORD g_bottom;                 /* where the gadgets end */
static struct Gadget *make_gadgets(void)
{
    struct NewGadget ng;
    struct Gadget *g;
    UWORD fh = scr->Font->ta_YSize, bh = fh + 6, left = scr->WBorLeft + 8, width = 600, bw = (600 - 18) / 4;
    int d;
    g = CreateContext(&glist);
    memset(&ng, 0, sizeof(ng));
    ng.ng_VisualInfo = vi; ng.ng_TextAttr = scr->Font;

    ng.ng_LeftEdge = left; ng.ng_TopEdge = scr->WBorTop + fh + 1 + fh + 6; ng.ng_Width = width; ng.ng_Height = fh * 6 + 4;
    ng.ng_GadgetText = "Scheduled tasks  (X = exclusive)"; ng.ng_Flags = PLACETEXT_ABOVE | NG_HIGHLABEL;
    ng.ng_GadgetID = GID_LIST;
    g = gad[GID_LIST] = CreateGadget(LISTVIEW_KIND, g, &ng, GTLV_ShowSelected, 0UL, TAG_END);

    ng.ng_TopEdge += ng.ng_Height + 4; ng.ng_Height = bh; ng.ng_Width = bw; ng.ng_Flags = 0;
    ng.ng_GadgetText = "New task"; ng.ng_GadgetID = GID_NEW;
    g = gad[GID_NEW] = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    ng.ng_LeftEdge += bw + 6; ng.ng_GadgetText = "Door maint..."; ng.ng_GadgetID = GID_DOOR;
    g = gad[GID_DOOR] = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    ng.ng_LeftEdge += bw + 6; ng.ng_GadgetText = "Delete"; ng.ng_GadgetID = GID_DEL;
    g = gad[GID_DEL] = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    ng.ng_LeftEdge += bw + 6; ng.ng_Width = width - (ng.ng_LeftEdge - left); ng.ng_GadgetText = "Run now"; ng.ng_GadgetID = GID_RUN;
    g = gad[GID_RUN] = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);

    /* the edit panel */
    ng.ng_TopEdge += bh + 8; ng.ng_LeftEdge = left + 48; ng.ng_Width = 132;
    ng.ng_GadgetText = "Name"; ng.ng_Flags = PLACETEXT_LEFT; ng.ng_GadgetID = GID_NAME;
    g = gad[GID_NAME] = CreateGadget(STRING_KIND, g, &ng, GTST_MaxChars, TAGLEN - 1, TAG_END);
    ng.ng_LeftEdge += 190; ng.ng_Width = 120; ng.ng_GadgetText = "Runs"; ng.ng_GadgetID = GID_WHEN;
    g = gad[GID_WHEN] = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)WHEN, TAG_END);
    ng.ng_LeftEdge += 126; ng.ng_Width = 62; ng.ng_GadgetText = NULL; ng.ng_GadgetID = GID_TIME;
    g = gad[GID_TIME] = CreateGadget(STRING_KIND, g, &ng, GTST_MaxChars, 5, TAG_END);
    ng.ng_LeftEdge += 110; ng.ng_Width = 56; ng.ng_GadgetText = "or"; ng.ng_GadgetID = GID_EVERY;
    g = gad[GID_EVERY] = CreateGadget(INTEGER_KIND, g, &ng, GTIN_MaxChars, 4, GTIN_Number, 30, TAG_END);
    ng.ng_LeftEdge += 62; ng.ng_Width = 60; ng.ng_GadgetText = "minutes"; ng.ng_Flags = PLACETEXT_RIGHT; ng.ng_GadgetID = 0;
    ng.ng_Width = 1;   /* just the label */
    g = CreateGadget(TEXT_KIND, g, &ng, TAG_END);

    ng.ng_TopEdge += bh + 4; ng.ng_Flags = PLACETEXT_RIGHT; ng.ng_Width = 26; ng.ng_Height = fh + 3;
    for (d = 0; d < 7; d++) {
        ng.ng_LeftEdge = left + 48 + d * 78; ng.ng_GadgetText = (UBYTE *)DAYNAME[d]; ng.ng_GadgetID = GID_DAY0 + d;
        g = gad[GID_DAY0 + d] = CreateGadget(CHECKBOX_KIND, g, &ng, GTCB_Scaled, TRUE, TAG_END);
    }
    ng.ng_TopEdge += fh + 7; ng.ng_LeftEdge = left + 48; ng.ng_Width = width - 48; ng.ng_Height = bh;
    ng.ng_GadgetText = "Do"; ng.ng_Flags = PLACETEXT_LEFT; ng.ng_GadgetID = GID_CMD;
    g = gad[GID_CMD] = CreateGadget(STRING_KIND, g, &ng, GTST_MaxChars, CMDLEN - 1, TAG_END);

    ng.ng_TopEdge += bh + 4; ng.ng_LeftEdge = left + 48; ng.ng_Width = 26; ng.ng_Height = fh + 3;
    ng.ng_GadgetText = "Exclusive: log callers off, after"; ng.ng_Flags = PLACETEXT_RIGHT; ng.ng_GadgetID = GID_EXCL;
    g = gad[GID_EXCL] = CreateGadget(CHECKBOX_KIND, g, &ng, GTCB_Scaled, TRUE, TAG_END);
    ng.ng_LeftEdge = left + 360; ng.ng_Width = 40; ng.ng_Height = bh; ng.ng_TopEdge -= 2;
    ng.ng_GadgetText = "minutes' warning"; ng.ng_GadgetID = GID_WARN;
    g = gad[GID_WARN] = CreateGadget(INTEGER_KIND, g, &ng, GTIN_MaxChars, 2, GTIN_Number, 5, TAG_END);

    ng.ng_TopEdge += bh + 8; ng.ng_LeftEdge = left; ng.ng_Width = 196; ng.ng_Flags = 0;
    ng.ng_GadgetText = "Save"; ng.ng_GadgetID = GID_SAVE;
    g = gad[GID_SAVE] = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    ng.ng_LeftEdge = left + 202; ng.ng_GadgetText = "Revert"; ng.ng_GadgetID = GID_REVERT;
    g = gad[GID_REVERT] = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    ng.ng_LeftEdge = left + 404; ng.ng_Width = width - 404; ng.ng_GadgetText = "Quit"; ng.ng_GadgetID = GID_QUIT;
    g = gad[GID_QUIT] = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    g_bottom = ng.ng_TopEdge + bh;
    return g;
}

static int ask(const char *text, const char *buttons)
{
    struct EasyStruct es = { sizeof(struct EasyStruct), 0, (UBYTE *)"NilBBS Schedule", (UBYTE *)text, (UBYTE *)buttons };
    return EasyRequestArgs(win, &es, NULL, NULL);
}

static BOOL do_save(void)
{
    LONG rc;
    pull_fields();
    if (!save_events()) { ask("Couldn't write BBS:Config/Events.cfg.", "OK"); return FALSE; }
    dirty = FALSE;
    rc = bbs_rexx("RELOAD");
    title(rc == 0 ? "saved; the BBS has reloaded it" : "saved (NilBBS isn't running - it reads it at start)");
    return TRUE;
}

int main(void)
{
    ULONG bottom;
    BOOL done = FALSE;
    struct Gadget *ok;
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 37);
    GadToolsBase = OpenLibrary((STRPTR)"gadtools.library", 37);
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR)"graphics.library", 37);
    if (!IntuitionBase || !GadToolsBase || !GfxBase) goto out;
    {   /* BBS: must exist - default to our own drawer, like NilBBS does */
        BPTR l = Lock((STRPTR)"BBS:", ACCESS_READ);
        if (l) UnLock(l);
        else if ((l = DupLock(GetProgramDir()))) { if (!AssignLock((STRPTR)"BBS", l)) UnLock(l); }
    }
    load_events();
    if (!(scr = LockPubScreen(NULL))) goto out;
    vi = GetVisualInfo(scr, TAG_END);
    if (!(ok = make_gadgets())) goto out;
    bottom = g_bottom;
    title(NULL);
    win = OpenWindowTags(NULL, WA_Title, (ULONG)wtitle,
                         WA_Left, scr->Width >= 660 ? 20 : 0, WA_Top, scr->Height >= (LONG)bottom + 40 ? 20 : 0,
                         WA_InnerWidth, 616, WA_Height, bottom + 6 + scr->WBorBottom,
                         WA_Gadgets, (ULONG)glist, WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE,
                         WA_Activate, TRUE, WA_PubScreen, (ULONG)scr,
                         WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | LISTVIEWIDCMP | BUTTONIDCMP | STRINGIDCMP |
                                   CHECKBOXIDCMP | CYCLEIDCMP | INTEGERIDCMP, TAG_END);
    if (!win) goto out;
    GT_RefreshWindow(win, NULL);
    sel = nev ? 0 : -1;
    show_list();
    show_fields();

    while (!done) {
        struct IntuiMessage *im;
        WaitPort(win->UserPort);
        while ((im = GT_GetIMsg(win->UserPort))) {
            ULONG cls = im->Class; UWORD code = im->Code; struct Gadget *g = (struct Gadget *)im->IAddress;
            int id;
            GT_ReplyIMsg(im);
            if (cls == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(win); GT_EndRefresh(win, TRUE); continue; }
            if (cls == IDCMP_CLOSEWINDOW) id = GID_QUIT;
            else if (cls == IDCMP_GADGETUP) id = g->GadgetID;
            else continue;
            switch (id) {
            case GID_LIST:
                pull_fields();
                sel = code; show_fields();
                break;
            case GID_NEW:
                pull_fields();
                if ((sel = new_event("NEWTASK", "", FALSE)) >= 0) { show_list(); show_fields(); title(NULL); }
                break;
            case GID_DOOR: {
                char t[NAMELEN], c[CMDLEN];
                pull_fields();
                if (pick_door(t, c)) {
                    if ((sel = new_event(t, c, TRUE)) >= 0) {
                        show_list(); show_fields(); title(NULL);
                        if (!c[0]) ask("That door has no maint command in Doors.cfg.\n"
                                       "Type the command to run in the Do box.", "OK");
                    }
                }
                break;
            }
            case GID_DEL:
                if (sel >= 0) {
                    char q[80];
                    sprintf(q, "Delete the task %s?", ev[sel].tag);
                    if (ask(q, "Delete|Cancel") == 1) {
                        free(ev[sel].comment); free(ev[sel].extra);
                        memmove(&ev[sel], &ev[sel + 1], (nev - sel - 1) * sizeof(ev[0]));
                        nev--; dirty = TRUE;
                        if (sel >= nev) sel = nev - 1;
                        show_list(); show_fields(); title(NULL);
                    }
                }
                break;
            case GID_RUN:
                if (sel >= 0) {
                    char c[40];
                    pull_fields();
                    if (dirty && ask("Save your changes first, so the BBS runs\nthe task as it is now?", "Save|Cancel") != 1) break;
                    if (dirty && !do_save()) break;
                    sprintf(c, "EVENT %s", ev[sel].tag);
                    title(bbs_rexx(c) == 0 ? "started - see BBS:Logs/Event-<name>.log" : "couldn't start it (is NilBBS running?)");
                }
                break;
            case GID_WHEN:
                if (sel >= 0) {
                    struct Ev *e = &ev[sel];
                    if (code == 0 && !e->time[0]) strcpy(e->time, "04:00");
                    if (code == 1 && e->time[0]) { e->time[0] = 0; if (e->every <= 0) e->every = 30; }
                    dirty = TRUE; show_fields(); show_list(); title(NULL);
                }
                break;
            case GID_EXCL:
                pull_fields();
                GT_SetGadgetAttrs(gad[GID_WARN], win, NULL, GA_Disabled, sel < 0 || !ev[sel].excl, TAG_END);
                break;
            case GID_SAVE: do_save(); break;
            case GID_REVERT:
                if (!dirty || ask("Throw away your changes and read\nEvents.cfg again?", "Revert|Cancel") == 1) {
                    load_events(); dirty = FALSE; sel = nev ? 0 : -1; show_list(); show_fields(); title("as saved");
                }
                break;
            case GID_QUIT:
                pull_fields();
                if (dirty) {
                    int a = ask("Your changes aren't saved yet.", "Save|Don't save|Cancel");
                    if (a == 0) break;
                    if (a == 1 && !do_save()) break;
                }
                done = TRUE;
                break;
            default:                                    /* a field of the edit panel */
                pull_fields();
            }
        }
    }
out:
    if (win) CloseWindow(win);
    if (glist) FreeGadgets(glist);
    if (vi) FreeVisualInfo(vi);
    if (scr) UnlockPubScreen(NULL, scr);
    free_all();
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    if (GadToolsBase) CloseLibrary(GadToolsBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    return 0;
}
