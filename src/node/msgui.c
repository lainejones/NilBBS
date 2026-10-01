/*
 * msgui.c - message areas: choose, read, scan for new, post, reply, e-mail.
 *
 * Areas come from BBS:Config/MsgAreas.cfg, one [TAG] section each:
 *   [GENERAL]
 *   name    = General Discussion
 *   read    = 0          ; level needed to read
 *   write   = 10         ; level needed to post
 *   type    = local      ; local | echo | email
 *   echotag = AMIGA      ; FidoNet echo name (type = echo)
 *   acs      = FA        ; optional extra read condition (see acs.c)
 *   post_acs = L20       ; optional extra post condition
 *   subop    = @Sysop    ; who moderates it (may delete any message, approves held posts)
 *   hold_below = 10      ; posts by callers below this level wait for a moderator (0 = off)
 *   hold_first = 3       ; and a caller's posts wait until 3 of theirs here are approved (0 = off)
 * Everyone else's posts go up at once; anyone can report a message (! in the reader).
 *   conf     = MAIN      ; conference(s) it belongs to (Conferences.cfg)
 * The order of the sections is the area number; keep it stable, because
 * each user's last-read pointers are stored by area number.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "node.h"
#include "../common/msgbase.h"

#define AT_LOCAL 0
#define AT_ECHO  1
#define AT_EMAIL 2

struct MsgArea {
    char  tag[NAMELEN];
    char  name[LONGNAME];
    char  echotag[NAMELEN];
    UBYTE rlevel, wlevel, type;
    char  acs[64];              /* extra condition to read  */
    char  pacs[64];             /* extra condition to post  */
    char  subop[64];            /* who moderates this area  */
    UBYTE hold_below, hold_first;   /* moderation: see the top of the file */
    char  conf[48];             /* conference tag(s), "" = every conference */
};

static struct MsgArea areas[MAX_MSGAREAS];
static int nareas;
static int email_area = -1;

#define MAXLINES ED_MAXLINES
#define EDWIDTH  ED_WIDTH

/* may read (and see it in lists): level, ACS, and it belongs to this conference.
 * Private mail is in every conference. */
static BOOL area_readable(struct MsgArea *a)
{
    return a->rlevel <= N.user.level && acs_check(a->acs) &&
           (a->type == AT_EMAIL || conf_visible(a->conf));
}

static BOOL area_writable(struct MsgArea *a)
{
    return area_readable(a) && a->wlevel <= N.user.level && acs_check(a->pacs);
}

/* the sysop, or whoever the area's "subop" ACS names */
static BOOL area_subop(struct MsgArea *a)
{
    return N.sysop || (a->subop[0] && acs_check(a->subop));
}

void msg_areas_load(void)
{
    struct Cfg *c = cfg_load("BBS:Config/MsgAreas.cfg");
    LONG i, n = cfg_sections(c);
    nareas = 0;
    email_area = -1;
    for (i = 0; i < n && nareas < MAX_MSGAREAS; i++) {
        const char *tag = cfg_section(c, i), *type;
        struct MsgArea *a = &areas[nareas];
        memset(a, 0, sizeof(*a));
        str_copy(a->tag, tag, NAMELEN);
        str_copy(a->name, cfg_sget(c, tag, "name", tag), LONGNAME);
        str_copy(a->echotag, cfg_sget(c, tag, "echotag", tag), NAMELEN);
        a->rlevel = (UBYTE)cfg_sint(c, tag, "read", 0);
        a->wlevel = (UBYTE)cfg_sint(c, tag, "write", 10);
        str_copy(a->acs, cfg_sget(c, tag, "acs", ""), sizeof(a->acs));
        str_copy(a->pacs, cfg_sget(c, tag, "post_acs", ""), sizeof(a->pacs));
        str_copy(a->subop, cfg_sget(c, tag, "subop", ""), sizeof(a->subop));
        a->hold_below = (UBYTE)cfg_sint(c, tag, "hold_below", 0);
        a->hold_first = (UBYTE)cfg_sint(c, tag, "hold_first", 0);
        str_copy(a->conf, cfg_sget(c, tag, "conf", ""), sizeof(a->conf));
        type = cfg_sget(c, tag, "type", "local");
        a->type = !str_icmp(type, "echo") ? AT_ECHO : !str_icmp(type, "email") ? AT_EMAIL : AT_LOCAL;
        if (a->type == AT_EMAIL && email_area < 0) email_area = nareas;
        nareas++;
    }
    cfg_free(c);
    if (!nareas) {                              /* bare install: one area + mail */
        memset(areas, 0, sizeof(areas[0]) * 2);
        strcpy(areas[0].tag, "GENERAL"); strcpy(areas[0].name, "General");
        areas[0].wlevel = 10;
        strcpy(areas[1].tag, "EMAIL"); strcpy(areas[1].name, "Private Mail");
        areas[1].type = AT_EMAIL; areas[1].wlevel = 10;
        nareas = 2;
        email_area = 1;
    }
    /* start in the first public area the user can read (none: an invalid index) */
    N.cur_msgarea = (UBYTE)nareas;
    for (i = 0; i < nareas; i++)
        if (areas[i].type != AT_EMAIL && area_readable(&areas[i])) { N.cur_msgarea = (UBYTE)i; break; }
}

const char *msg_area_name(void)
{
    return (N.cur_msgarea < nareas) ? areas[N.cur_msgarea].name : "";
}

/* ---- helpers ------------------------------------------------------------------- */

static BOOL can_see(struct MsgArea *a, struct MsgHdr *h)
{
    if (h->flags & MF_DELETED) return FALSE;
    if ((h->flags & MF_HELD) && h->fromid != N.user.id && !area_subop(a)) return FALSE;
    if (a->type != AT_EMAIL && !(h->flags & MF_PRIVATE)) return TRUE;
    if (N.sysop) return TRUE;
    return h->toid == N.user.id || h->fromid == N.user.id ||
           !str_icmp(h->to, N.user.name) || !str_icmp(h->from, N.user.name);
}

/* a dark rule across the screen */
static void rule(void)
{
    int i;
    char line[132];
    int w = N.cols > 130 ? 130 : N.cols - 1;
    for (i = 0; i < w; i++) line[i] = (N.term == TT_ASCII || N.charset == CS_ASCII) ? '-' : (char)0xC4;
    line[w] = 0;
    tputs("|08");
    tputs(line);
    tputs("|07\n");
}

static void show_header(struct MsgArea *a, struct MsgHdr *h, LONG total)
{
    char when[20];
    bbs_datetimestr(h->date, when);
    tcls();
    tprintf(L("msg.show_header.msg_of", "|09%s |08- |07msg |15%lu|07 of |15%ld|07\n"), a->name, h->num, total);
    tprintf(L("msg.show_header.from_date", "|08From: |11%-30s |08Date: |07%s"), h->from, when);
    if (h->origaddr[0] && (h->flags & MF_IMPORTED)) tprintf(" |08(%s)", h->origaddr);
    tprintf(L("msg.show_header.to", "\n|08To:   |11%s\n"), h->to);
    tprintf(L("msg.show_header.subj", "|08Subj: |15%s|07\n"), h->subject);
    if (h->flags & MF_HELD) tputs(L("msg.show_header.waiting_for_moderator", "|14(waiting for a moderator - only you and the moderators can see it)|07\n"));
    if ((h->flags & MF_REPORTED) && area_subop(a)) tprintf(L("msg.show_header.reported_by", "|12(reported by %s)|07\n"), h->reportby);
    if (h->replyto) tprintf(L("msg.show_header.reply_to", "|08(reply to #%lu)|07\n"), h->replyto);
    rule();
    tputs("\n");
}

/* quoted lines (">" or "XX>") in a different colour */
static void show_body(const char *text)
{
    const char *p = text;
    while (*p && tmore()) {
        const char *e = strchr(p, '\n');
        LONG len = e ? e - p : (LONG)strlen(p);
        const char *q = p;
        int i;
        BOOL quote = FALSE;
        if (*p == 1 || !strncmp(p, "SEEN-BY:", 8)) {        /* FidoNet control lines */
            if (!e) break;
            p = e + 1;
            continue;
        }
        for (i = 0; i < 4 && i < len; i++, q++) if (*q == '>') { quote = TRUE; break; }
        if (len >= 4 && !strncmp(p, "---", 3)) tputs("|08");
        else if (!strncmp(p, " * Origin", 9)) tputs("|08");
        else tputs(quote ? "|03" : "|07");
        {   /* word-wrap to the screen: a long line (FidoNet, QWK, other editors) is never cut off */
            const char *s = p;
            LONG left = len, w = N.cols > 20 ? N.cols - 1 : 79;
            while (left > w) {
                LONG cut = w;
                while (cut > w / 2 && s[cut] != ' ') cut--;
                if (cut <= w / 2) cut = w;               /* one long word: break it */
                tputraw((const UBYTE *)s, cut, CS_CP437);
                tputs("\n");
                s += cut; left -= cut;
                while (left > 0 && *s == ' ') { s++; left--; }
                if (!tmore()) break;
            }
            if (left > 0) tputraw((const UBYTE *)s, left, CS_CP437);
            tputs("\n");
        }
        if (!e) break;
        p = e + 1;
    }
    tputs("|07");
}

/* ---- the line editor ------------------------------------------------------------ */


/* one editor line with word-wrap.  `carry` holds text wrapped from the last
 * line on entry and receives the overflow on exit.  Returns -1 on hangup. */
static int ed_getline(char *buf, char *carry)
{
    LONG len = strlen(carry), k;
    strcpy(buf, carry);
    carry[0] = 0;
    tputraw((UBYTE *)buf, len, CS_CP437);
    for (;;) {
        k = tgetkey(0);
        if (k == KEY_HANGUP) return -1;
        if (k == KEY_NONE) { tputraw((UBYTE *)buf, len, CS_CP437); continue; }
        if (k == '\r') break;
        if (k == 8) {
            if (len > 0) { len--; tn_raw((const UBYTE *)"\b \b", 3); }
            continue;
        }
        if (k < 32 || k > 255) continue;
        buf[len++] = (char)k;
        buf[len] = 0;
        { UBYTE b = (UBYTE)k; tputraw(&b, 1, CS_CP437); }
        if (len >= EDWIDTH) {
            /* wrap: move the last word down to the next line */
            LONG sp = len - 1;
            while (sp > 0 && buf[sp] != ' ') sp--;
            if (sp > 0 && len - sp - 1 < EDWIDTH / 2) {
                LONG wl = len - sp - 1, i;
                strcpy(carry, buf + sp + 1);
                for (i = 0; i < wl; i++) tn_raw((const UBYTE *)"\b \b", 3);
                buf[sp] = 0;
            }
            break;
        }
    }
    buf[EDWIDTH] = 0;
    tputs("\n");
    return 0;
}

static void ed_list(struct Editor *ed)
{
    int i;
    tpage_start();
    for (i = 0; i < ed->n && tmore(); i++) tprintf("|08%3d: |07%s\n", i + 1, ed->line[i]);
    tpage_end();
}

/* insert the original, quoted with the author's initials, as lines at `at`;
 * returns how many lines went in */
int ed_quote(struct Editor *ed, int at, const char *quote, const char *qfrom)
{
    char ini[4];
    const char *p = quote, *s = qfrom;
    int k = 0, added = 0;
    if (!quote) return 0;
    while (*s && k < 2) {
        if (s == qfrom || s[-1] == ' ') ini[k++] = *s;
        s++;
    }
    ini[k] = 0;
    while (*p && ed->n < MAXLINES - 1) {
        const char *e = strchr(p, '\n');
        LONG len = e ? e - p : (LONG)strlen(p);
        if (len > 0 && *p != 1 && strncmp(p, " * Origin", 9) && strncmp(p, "---", 3) &&
            strncmp(p, "SEEN-BY:", 8)) {
            const char *s = p;
            LONG left = len, room = EDWIDTH - (LONG)strlen(ini) - 3;
            /* a line longer than the room left after " XX> " wraps at a space */
            while (left > 0 && ed->n < MAXLINES - 1) {
                LONG take = left;
                if (take > room) {
                    take = room;
                    while (take > room / 2 && s[take] != ' ') take--;
                    if (s[take] != ' ') take = room;
                }
                memmove(ed->line[at + 1], ed->line[at], (ed->n - at) * sizeof(ed->line[0]));
                sprintf(ed->line[at], " %s> ", ini);
                strncat(ed->line[at], s, take);
                ed->n++;
                at++;
                added++;
                s += take;
                left -= take;
                while (left > 0 && *s == ' ') { s++; left--; }
            }
        }
        if (!e) break;
        p = e + 1;
    }
    return added;
}

/* returns TRUE to save.  `quote` (may be NULL) is the text being replied to.
 * ANSI/VT100 callers get the full-screen editor unless they chose the line one. */
static BOOL editor(struct Editor *ed, const char *quote, const char *qfrom, const char *to, const char *subj)
{
    char carry[EDWIDTH + 2];
    if (N.term != TT_ASCII && !(N.user.flags & UF_LINEEDIT) && N.rows >= 12 && N.cols >= 40)
        return fse_edit(ed, quote, qfrom, to, subj) == 1;
    carry[0] = 0;
    tputs(L("msg.editor.enter_your_message", "\n|07Enter your message. |08/S|07 save  |08/A|07 abort  |08/L|07 list  "
          "|08/D n|07 delete line  |08/Q|07 quote  |08/?|07 help\n"));
    tputs("|08");
    {
        int i;
        for (i = 0; i < EDWIDTH; i++) tputs("-");
    }
    tputs("|07\n");
    for (;;) {
        char *buf;
        if (ed->n >= MAXLINES) {
            tputs(L("msg.editor.message_is_full", "|12Message is full - /S to save.|07\n"));
            ed->n = MAXLINES - 1;
        }
        buf = ed->line[ed->n];
        if (ed_getline(buf, carry) < 0) return FALSE;
        if (buf[0] == '/' && strlen(buf) <= 6) {
            char c = buf[1];
            if (c >= 'a' && c <= 'z') c -= 32;
            if (c == 'S') return ed->n > 0;
            if (c == 'A') {
                if (tyesno(L("msg.editor.abort_this_message", "|12Abort this message?|07"), FALSE)) return FALSE;
                continue;
            }
            if (c == 'L') { ed_list(ed); continue; }
            if (c == 'D') {
                int ln = atoi(buf + 2);
                if (ln >= 1 && ln <= ed->n) {
                    memmove(ed->line[ln - 1], ed->line[ln], (ed->n - ln) * sizeof(ed->line[0]));
                    ed->n--;
                    tprintf(L("msg.editor.line_deleted", "|08Line %d deleted.|07\n"), ln);
                }
                continue;
            }
            if (c == 'Q' && quote) {
                ed_quote(ed, ed->n, quote, qfrom);
                ed_list(ed);
                continue;
            }
            if (c == '?') {
                tputs(L("msg.editor.save_abort_list", "|08/S save  /A abort  /L list  /D n delete line n  /Q quote original  /C clear|07\n"));
                continue;
            }
            if (c == 'C') { if (tyesno(L("msg.editor.clear_everything", "|07Clear everything?"), FALSE)) ed->n = 0; continue; }
        }
        ed->n++;
    }
}

/* ---- post ---------------------------------------------------------------------------- */

/* a door's text in NilBBS's own editor (CNet CALLEDITOR): 1 = saved, 0 = aborted */
int msg_door_edit(struct Editor *ed, const char *subj)
{
    return editor(ed, NULL, NULL, "", subj ? subj : "") ? 1 : 0;
}

static ULONG store_post(int ai, struct MsgHdr *hp, const char *text, LONG len);

static BOOL post_in(int ai, const char *to, const char *subj, ULONG replyto,
                    const char *quote, const char *qfrom)
{
    struct MsgArea *a = &areas[ai];
    struct Editor *ed;
    struct MsgHdr h;
    char *text;
    LONG len = 0, i;
    ULONG num;
    ULONG toid = 0;

    if (!area_writable(a)) { tputs(L("msg.post_in.you_cant_post", "|12You can't post in this area.|07\n")); return FALSE; }
    memset(&h, 0, sizeof(h));
    if (to && to[0]) str_copy(h.to, to, sizeof(h.to));
    else {
        tputs(L("msg.post_in.to_enter_all", "\n|07To |08(Enter = All)|07: |15"));
        if (tgetline(h.to, 35, GL_NAME) < 0) return FALSE;
        if (!h.to[0]) strcpy(h.to, "All");
    }
    if (a->type == AT_EMAIL) {
        struct UserRec *u = AllocVec(sizeof(struct UserRec), 0);
        if (!u) return FALSE;
        ObtainSemaphore(&N.S->userlock);
        toid = userdb_find(h.to, u);
        if (toid) str_copy(h.to, u->name, sizeof(h.to));
        ReleaseSemaphore(&N.S->userlock);
        FreeVec(u);
        if (!toid && str_icmp(h.to, cfg_str(N.cfg, "sysop_name", "Sysop"))) {
            tprintf(L("msg.post_in.theres_no_user", "|12There's no user called %s.|07\n"), h.to);
            return FALSE;
        }
    }
    if (subj && subj[0]) {
        str_copy(h.subject, subj, sizeof(h.subject));
        if (replyto && str_nicmp(subj, "Re:", 3)) {
            strcpy(h.subject, "Re: ");
            strncat(h.subject, subj, sizeof(h.subject) - 5);
        }
        tprintf(L("msg.post_in.subject", "|07Subject: |15%s|07\n"), h.subject);
    } else {
        tputs(L("msg.post_in.subject_2", "|07Subject: |15"));
        if (tgetline(h.subject, 70, 0) <= 0) return FALSE;
    }

    if (!(ed = AllocVec(sizeof(struct Editor), MEMF_CLEAR))) return FALSE;
    set_activity("Writing a message");
    if (!editor(ed, quote, qfrom, h.to, h.subject)) {
        tputs(L("msg.post_in.message_discarded", "|08Message discarded.|07\n"));
        FreeVec(ed);
        return FALSE;
    }
    if (!(text = AllocVec(MAXLINES * (EDWIDTH + 2) + 200, 0))) { FreeVec(ed); return FALSE; }
    for (i = 0; i < ed->n; i++) {
        LONG l = strlen(ed->line[i]);
        memcpy(text + len, ed->line[i], l);
        len += l;
        text[len++] = '\n';
    }
    /* echomail: BBSToss adds the tear line, origin (with our address),
     * SEEN-BY and PATH when it exports the message */
    FreeVec(ed);
    h.toid = toid;
    h.replyto = replyto;
    num = store_post(ai, &h, text, len);
    FreeVec(text);
    if (num) {
        tprintf(L("msg.post_in.saved_as_message", "|10Saved as message #%lu in %s.|07\n"), num, a->name);
        return TRUE;
    }
    tputs(L("msg.post_in.couldnt_save_the", "|12Couldn't save the message.|07\n"));
    return FALSE;
}

/* should this caller's post wait for a moderator?  (level below hold_below, or one of their
   first hold_first posts in the area - counting the ones already approved) */
static BOOL must_hold(int ai)
{
    struct MsgArea *a = &areas[ai];
    struct MsgHdr h;
    LONG i, n, mine = 0;
    if (a->hold_below && N.user.level < a->hold_below) return TRUE;
    if (!a->hold_first) return FALSE;
    ObtainSemaphore(&N.S->msglock);
    n = msg_count(a->tag);
    for (i = 1; i <= n && mine < a->hold_first; i++)
        if (msg_read_hdr(a->tag, i, &h) && h.fromid == N.user.id && !(h.flags & (MF_HELD | MF_DELETED))) mine++;
    ReleaseSemaphore(&N.S->msglock);
    return mine < a->hold_first;
}

/* save a finished message (h has to/subject/toid/replyto) as the current
 * user: link the reply, count the post, nudge an online mail recipient */
static ULONG store_post(int ai, struct MsgHdr *hp, const char *text, LONG len)
{
    struct MsgArea *a = &areas[ai];
    struct MsgHdr h = *hp;
    ULONG num, toid = h.toid, replyto = h.replyto;

    str_copy(h.from, N.user.name, sizeof(h.from));
    h.fromid  = N.user.id;
    h.date    = bbs_now();
    h.flags   = MF_LOCAL | (a->type == AT_EMAIL ? MF_PRIVATE : 0);
    if (a->type != AT_EMAIL && !area_subop(a) && must_hold(ai)) h.flags |= MF_HELD;

    ObtainSemaphore(&N.S->msglock);
    num = msg_add(a->tag, &h, (char *)text, len);
    if (num && replyto) {
        struct MsgHdr orig;
        if (msg_read_hdr(a->tag, replyto, &orig) && !orig.replies) {
            orig.replies = num;
            msg_write_hdr(a->tag, &orig);
        }
    }
    ReleaseSemaphore(&N.S->msglock);

    if (num) {
        N.user.posts++;
        if (N.user.lastread[ai] == num - 1) N.user.lastread[ai] = num;
        user_save();
        bbs_log(BBS_SYSLOG, "node %d: %s posted #%lu in %s%s", N.node, N.user.name, num, a->tag,
                (h.flags & MF_HELD) ? " (held for a moderator)" : "");
        if (h.flags & MF_HELD)
            tputs(L("msg.store_post.your_message_is", "|14Your message is waiting for a moderator - others will see it once it's approved.|07\n"));
        /* nudge the recipient if they're online */
        if (a->type == AT_EMAIL && toid) {
            int nn;
            for (nn = 0; nn < N.S->nodes; nn++) {
                struct Task *t = NULL;
                shared_lock(N.S);
                if (N.S->node[nn].state >= NS_ONLINE && N.S->node[nn].userid == (LONG)toid) {
                    struct NodeInfo *ni = &N.S->node[nn];
                    UBYTE next = (ni->msg_head + 1) % NODE_MSGQ;
                    if (next != ni->msg_tail) {
                        struct NodeMsg *m = &ni->msgq[ni->msg_head];
                        m->from = (UBYTE)N.node; m->type = NM_TEXT;
                        str_copy(m->fromname, N.user.name, NAMELEN);
                        str_copy(m->text, "(sent you private mail)", sizeof(m->text));
                        ni->msg_head = next;
                        t = ni->task;
                    }
                }
                shared_unlock(N.S);
                if (t) Signal(t, SIGBREAKF_CTRL_D);
            }
        }
    }
    return num;
}

/* ---- for QWK (qwk.c) --------------------------------------------------------------- */

int  msg_area_count(void)            { return nareas; }
const char *msg_area_tag_n(int i)    { return areas[i].tag; }
const char *msg_area_name_n(int i)   { return areas[i].name; }
BOOL msg_area_readable_n(int i)      { return i >= 0 && i < nareas && area_readable(&areas[i]); }
BOOL msg_area_is_email(int i)        { return areas[i].type == AT_EMAIL; }
BOOL msg_visible(int i, struct MsgHdr *h)
{
    /* in private mail, only what's addressed to this caller */
    if (areas[i].type == AT_EMAIL) return !(h->flags & MF_DELETED) && h->toid == N.user.id;
    return can_see(&areas[i], h);
}

/* post a finished text (a QWK reply) as the current user; 0 = refused */
ULONG msg_post_text(int ai, const char *to, const char *subj, ULONG replyto, const char *text)
{
    struct MsgHdr h;
    if (ai < 0 || ai >= nareas || !area_writable(&areas[ai])) return 0;
    memset(&h, 0, sizeof(h));
    str_copy(h.to, to && to[0] ? to : "All", sizeof(h.to));
    str_copy(h.subject, subj, sizeof(h.subject));
    h.replyto = replyto;
    if (areas[ai].type == AT_EMAIL) {
        struct UserRec *u = AllocVec(sizeof(struct UserRec), 0);
        if (!u) return 0;
        ObtainSemaphore(&N.S->userlock);
        h.toid = userdb_find(h.to, u);
        if (h.toid) str_copy(h.to, u->name, sizeof(h.to));
        ReleaseSemaphore(&N.S->userlock);
        FreeVec(u);
        if (!h.toid) return 0;
    }
    return store_post(ai, &h, text, strlen(text));
}

void msg_post(const char *to, const char *subj, ULONG replyto)
{
    int ai = N.cur_msgarea;
    if (to && subj && !replyto && email_area >= 0) ai = email_area;     /* feedback */
    if (ai >= nareas) return;
    post_in(ai, to, subj, replyto, NULL, NULL);
}

/* ---- reading ------------------------------------------------------------------------ */

/* anyone but the author (and the moderators, who can just delete) may report a message, once */
static BOOL can_report(struct MsgArea *a, struct MsgHdr *h)
{
    return a->type != AT_EMAIL && h->fromid != N.user.id && !area_subop(a) &&
           !(h->flags & (MF_REPORTED | MF_HELD)) && N.user.id;
}

/* held + reported posts in an area (for its moderators) */
static int mod_waiting(int ai)
{
    struct MsgHdr h;
    LONG i, n;
    int w = 0;
    ObtainSemaphore(&N.S->msglock);
    n = msg_count(areas[ai].tag);
    for (i = 1; i <= n; i++)
        if (msg_read_hdr(areas[ai].tag, i, &h) && !(h.flags & MF_DELETED) && (h.flags & (MF_HELD | MF_REPORTED))) w++;
    ReleaseSemaphore(&N.S->msglock);
    return w;
}

/* logon: how many posts wait for this caller as a moderator, anywhere ("" = none) */
int msg_mod_waiting(char *where, int size)
{
    int i, total = 0;
    where[0] = 0;
    for (i = 0; i < nareas; i++) {
        int w;
        if (areas[i].type == AT_EMAIL || !area_readable(&areas[i]) || !area_subop(&areas[i])) continue;
        if ((w = mod_waiting(i))) {
            if (!where[0]) str_copy(where, areas[i].name, size);
            total += w;
        }
    }
    return total;
}

static void show_header(struct MsgArea *a, struct MsgHdr *h, LONG total);
static void show_body(const char *text);

/* the moderation queue of one area: held posts (approve / reject) and reported ones (keep / delete) */
static void mod_queue(int ai)
{
    struct MsgArea *a = &areas[ai];
    struct MsgHdr h;
    char *text = AllocVec(MAX_MSGTEXT, 0);
    LONG i, n;
    if (!text) return;
    ObtainSemaphore(&N.S->msglock);
    n = msg_count(a->tag);
    ReleaseSemaphore(&N.S->msglock);
    for (i = 1; i <= n && N.online; i++) {
        LONG k;
        BOOL held;
        ObtainSemaphore(&N.S->msglock);
        if (!msg_read_hdr(a->tag, i, &h) || (h.flags & MF_DELETED) || !(h.flags & (MF_HELD | MF_REPORTED))) {
            ReleaseSemaphore(&N.S->msglock);
            continue;
        }
        msg_read_text(a->tag, &h, text, MAX_MSGTEXT);
        ReleaseSemaphore(&N.S->msglock);
        held = (h.flags & MF_HELD) != 0;
        show_header(a, &h, n);
        tpage_start(); show_body(text); tpage_end();
        tputs("\n");
        rule();
        if (held) tputs(L("msg.mod_queue.approve_reject_skip", "|14A|07pprove  |14R|07eject  |14S|07kip  |14Q|07uit |08>|07 "));
        else tputs(L("msg.mod_queue.keep_it_delete", "|14K|07eep it  |14D|07elete it  |14S|07kip  |14Q|07uit |08>|07 "));
        k = tgethot(held ? "ARSQ\r" : "KDSQ\r");
        if (k == KEY_HANGUP || k == 'Q') break;
        tprintf("%c\n", (int)k);
        if (k == 'S' || k == '\r') continue;
        ObtainSemaphore(&N.S->msglock);
        if (msg_read_hdr(a->tag, i, &h)) {                 /* fresh: another moderator may have been here */
            if (k == 'A') h.flags &= ~MF_HELD;
            else if (k == 'R' || k == 'D') h.flags |= MF_DELETED;
            else if (k == 'K') { h.flags &= ~MF_REPORTED; h.reportby[0] = 0; }
            msg_write_hdr(a->tag, &h);
        }
        ReleaseSemaphore(&N.S->msglock);
        bbs_log(BBS_SYSLOG, "node %d: moderator %s %s #%lu in %s", N.node, N.user.name,
                k == 'A' ? "approved" : k == 'R' ? "rejected" : k == 'D' ? "deleted" : "kept", h.num, a->tag);
        tprintf("|10%s.|07\n", k == 'A' ? L("msg.mod_queue.approved_everyone_can", "Approved - everyone can see it now") : k == 'R' ? L("msg.mod_queue.rejected", "Rejected") :
                               k == 'D' ? L("msg.mod_queue.deleted", "Deleted") : L("msg.mod_queue.kept_the_report", "Kept - the report is cleared"));
        tn_flush(); Delay(40);
    }
    FreeVec(text);
}

static BOOL from_list;
static ULONG last_read_at;       /* where the reader stopped: the list reopens on that page */            /* the reader was opened from the message list: Q = back to it */

/* read area `ai` starting at `start`; returns FALSE if the user quit the scan */
static BOOL read_from(int ai, ULONG start, BOOL newscan)
{
    struct MsgArea *a = &areas[ai];
    struct MsgHdr h;
    char *text = AllocVec(MAX_MSGTEXT, 0);
    LONG total;
    ULONG cur = start;
    BOOL keep = TRUE;

    if (!text) return FALSE;
    ObtainSemaphore(&N.S->msglock);
    total = msg_count(a->tag);
    ReleaseSemaphore(&N.S->msglock);
    set_activity("Reading messages");

    while (N.online && cur >= 1 && cur <= (ULONG)total) {
        LONG k;
        BOOL ok;
        ObtainSemaphore(&N.S->msglock);
        ok = msg_read_hdr(a->tag, cur, &h);
        if (ok) msg_read_text(a->tag, &h, text, MAX_MSGTEXT);
        ReleaseSemaphore(&N.S->msglock);
        if (!ok || !can_see(a, &h)) { cur++; continue; }

        show_header(a, &h, total);
        last_read_at = cur;                     /* the list reopens on this message's page */
        tpage_start();
        show_body(text);
        tpage_end();
        if (cur > N.user.lastread[ai]) N.user.lastread[ai] = cur;
        if (a->type == AT_EMAIL && h.toid == N.user.id && !(h.flags & MF_READ)) {
            ObtainSemaphore(&N.S->msglock);
            h.flags |= MF_READ;
            msg_write_hdr(a->tag, &h);
            ReleaseSemaphore(&N.S->msglock);
        }

        tputs("\n");
        rule();
        tputs(L("msg.read_from.next_prev_reply", "|14N|07ext  |14P|07rev  |14R|07eply  |14A|07gain  |14#|07 jump"));
        if (area_subop(a) || h.fromid == N.user.id) tputs(L("msg.read_from.delete", "  |14D|07elete"));
        if (h.replies) tputs(L("msg.read_from.thread", "  |14T|07hread"));
        if (can_report(a, &h)) tputs(L("msg.read_from.report", "  |14!|07 report"));
        tputs(newscan ? L("msg.read_from.skip_area_quit", "  |14S|07kip area  |14Q|07uit |08>|07 ") : from_list ? L("msg.read_from.list", "  |14Q|07 list |08>|07 ") : L("msg.read_from.quit", "  |14Q|07uit |08>|07 "));
        k = tgethot("NPRADTSQ!#0123456789\r");
        if (k == KEY_HANGUP) break;
        if (k == '\r') k = 'N';
        tprintf("%c\n", (int)k);
        switch (k) {
        case 'N': cur++; break;
        case 'P': if (cur > 1) cur--; break;
        case 'A': break;
        case 'T': if (h.replies) cur = h.replies; break;
        case '!':
            if (can_report(a, &h) && tyesno(L("msg.read_from.report_this_message", "|07Report this message to the moderators?"), FALSE)) {
                ObtainSemaphore(&N.S->msglock);
                if (msg_read_hdr(a->tag, h.num, &h)) {
                    h.flags |= MF_REPORTED;
                    str_copy(h.reportby, N.user.name, sizeof(h.reportby));
                    msg_write_hdr(a->tag, &h);
                }
                ReleaseSemaphore(&N.S->msglock);
                bbs_log(BBS_SYSLOG, "node %d: %s reported #%lu in %s", N.node, N.user.name, h.num, a->tag);
                tputs(L("msg.read_from.reported_thanks_moderator", "|10Reported - thanks. A moderator will take a look.|07\n"));
            }
            break;
        case 'R': {
            const char *to = h.from;
            post_in(ai, to, h.subject, h.num, text, h.from);
            ObtainSemaphore(&N.S->msglock);
            total = msg_count(a->tag);
            ReleaseSemaphore(&N.S->msglock);
            break;
        }
        case 'D':
            if ((area_subop(a) || h.fromid == N.user.id) && tyesno(L("msg.read_from.delete_this_message", "|12Delete this message?|07"), FALSE)) {
                ObtainSemaphore(&N.S->msglock);
                h.flags |= MF_DELETED;
                msg_write_hdr(a->tag, &h);
                ReleaseSemaphore(&N.S->msglock);
                cur++;
            }
            break;
        case 'S': cur = total + 1; break;
        case 'Q': keep = FALSE; cur = total + 1; break;
        default: {
            char b[8];
            ULONG n;
            b[0] = (k >= '0' && k <= '9') ? (char)k : 0;
            b[1] = 0;
            tputs(L("msg.read_from.message_number", "|07Message number: |15"));
            if (tgetline(b, 7, GL_DIGITS | GL_EDIT) > 0) {
                n = strtoul(b, NULL, 10);
                if (n >= 1 && n <= (ULONG)total) cur = n;
            }
        }
        }
    }
    if (keep && N.online && cur > (ULONG)total && !from_list) tprintf(L("msg.read_from.end_of", "\n|08End of |07%s|08.|07\n"), a->name);
    user_save();
    FreeVec(text);
    back_to_menu();
    return keep;
}

/* ---- the message list: an area as a forum index, a page at a time ----------------------------------
 * Number, subject, who from, date; * = new.  Opens on the page with the first new message.  A number
 * reads that message (the reader's Q comes back here), N/P page, F = the first new, Enter = the first
 * new one (or the next page when nothing's new), Q = back to the menu. */
static void msg_list(int ai)
{
    struct MsgArea *a = &areas[ai];
    struct MsgHdr h;
    ULONG *vis, lastread;
    LONG total, nvis = 0, i, per, page, pages, firstnew = -1;
    total = msg_count(a->tag);
    if (!(vis = AllocVec((total + 1) * sizeof(ULONG), 0))) return;
    ObtainSemaphore(&N.S->msglock);
    for (i = 1; i <= total; i++)
        if (msg_read_hdr(a->tag, i, &h) && !(h.flags & MF_DELETED) && can_see(a, &h)) vis[nvis++] = i;
    ReleaseSemaphore(&N.S->msglock);
    if (!nvis) { tprintf(L("msg.list.has_no_messages", "\n|08%s has no messages yet.|07\n"), a->name); FreeVec(vis); return; }
    per = (N.rows >= 12 ? N.rows : 24) - 7;
    pages = (nvis + per - 1) / per;
    lastread = N.user.lastread[ai];
    for (i = 0; i < nvis; i++) if (vis[i] > lastread) { firstnew = i; break; }
    page = (firstnew >= 0 ? firstnew : nvis - 1) / per;
    set_activity("Reading messages");
    while (N.online) {
        LONG k, first = page * per, last = first + per > nvis ? nvis : first + per;
        int nnew = 0;
        lastread = N.user.lastread[ai];
        for (i = 0; i < nvis; i++) if (vis[i] > lastread) nnew++;
        tcls();
        tprintf(L("msg.list.page_of_messages", "|09-=[ |15%s |09]=-  |07page |15%ld|07 of |15%ld|07   |15%ld|07 messages"), a->name, page + 1, pages, nvis);
        if (nnew) tprintf(L("msg.list.new", ", |14%d new|07"), nnew);
        tputs(L("msg.list.subject_from_date", "\n\n|08    #   Subject                                  From             Date\n"));
        ObtainSemaphore(&N.S->msglock);
        for (i = first; i < last; i++) {
            char when[12];
            if (!msg_read_hdr(a->tag, vis[i], &h)) continue;
            bbs_datestr(h.date, when);
            tprintf("%s%5lu%s |%s%-40.40s |11%-16.16s |07%s%s\n",
                    vis[i] > lastread ? "|14" : "|07", vis[i], vis[i] > lastread ? "|14*" : " ",
                    vis[i] > lastread ? "15" : "07", h.subject, h.from, when,
                    (h.flags & MF_HELD) ? L("msg.list.held", " |14held") : (h.flags & MF_REPORTED) && area_subop(a) ? L("msg.list.reported", " |12reported") : "");
        }
        ReleaseSemaphore(&N.S->msglock);
        tputs("\n");
        rule();
        tputs(L("msg.list.read", "|14#|07 read"));
        if (page + 1 < pages) tputs(L("msg.list.next_page", "  |14N|07ext page"));
        if (page > 0) tputs(L("msg.list.prev_page", "  |14P|07rev page"));
        if (nnew) tputs(L("msg.list.first_new", "  |14F|07irst new"));
        {
            int nm = area_subop(a) ? mod_waiting(ai) : 0;
            if (nm) tprintf(L("msg.list.moderate", "  |14M|07oderate |08(|15%d|08)|07"), nm);
        }
        tputs(L("msg.list.quit", "  |14Q|07uit |08>|07 "));
        k = tgethot("NPFMQ\r0123456789");
        if (k == 'M') {
            if (area_subop(a) && mod_waiting(ai)) { tputs("M\n"); mod_queue(ai); }
            continue;
        }
        if (k == KEY_HANGUP) break;
        if (k == 'Q') { tputs("Q\n"); break; }
        if (k == 'N') { if (page + 1 < pages) page++; continue; }
        if (k == 'P') { if (page > 0) page--; continue; }
        if (k == 'F' || k == '\r') {
            LONG fn = -1;
            for (i = 0; i < nvis; i++) if (vis[i] > N.user.lastread[ai]) { fn = i; break; }
            if (fn < 0) { if (k == '\r' && page + 1 < pages) page++; continue; }
            k = 0;
            last_read_at = vis[fn];
        } else {                                  /* a number: read that message */
            char b[8];
            ULONG n;
            b[0] = (char)k; b[1] = 0;
            tputs(L("msg.list.message_number", "\n|07Message number: |15"));
            if (tgetline(b, 7, GL_DIGITS | GL_EDIT) <= 0) continue;
            n = strtoul(b, NULL, 10);
            if (n < 1 || n > (ULONG)total) continue;
            last_read_at = n;
        }
        from_list = TRUE;
        read_from(ai, last_read_at, FALSE);
        from_list = FALSE;
        for (i = 0; i < nvis && vis[i] < last_read_at; i++) ;    /* back on the page it stopped at */
        page = (i < nvis ? i : nvis - 1) / per;
        ObtainSemaphore(&N.S->msglock);                           /* a reply may have added one */
        if (msg_count(a->tag) != total) {
            ULONG *nv;
            LONG t2 = msg_count(a->tag);
            if ((nv = AllocVec((t2 + 1) * sizeof(ULONG), 0))) {
                FreeVec(vis); vis = nv; total = t2; nvis = 0;
                for (i = 1; i <= total; i++)
                    if (msg_read_hdr(a->tag, i, &h) && !(h.flags & MF_DELETED) && can_see(a, &h)) vis[nvis++] = i;
                pages = (nvis + per - 1) / per;
            }
        }
        ReleaseSemaphore(&N.S->msglock);
        if (!nvis) break;
    }
    FreeVec(vis);
    back_to_menu();
}

void msg_read_area(BOOL newonly)
{
    int ai = N.cur_msgarea;
    LONG total;
    ULONG start;
    if (ai >= nareas) return;
    if (!area_readable(&areas[ai])) { tputs(L("msg.read_area.you_cant_read", "|12You can't read this area.|07\n")); return; }
    ObtainSemaphore(&N.S->msglock);
    total = msg_count(areas[ai].tag);
    ReleaseSemaphore(&N.S->msglock);
    if (!total) { tprintf(L("msg.read_area.has_no_messages", "\n|08%s has no messages yet.|07\n"), areas[ai].name); return; }
    start = N.user.lastread[ai] + 1;
    if (newonly) {
        if (start > (ULONG)total) { tputs(L("msg.read_area.no_new_messages", "\n|08No new messages.|07\n")); return; }
        read_from(ai, start, FALSE);         /* the new-message scan reads straight through */
        return;
    }
    msg_list(ai);                                   /* R: the area as a forum list, page by page */
}

void msg_select_area(void)
{
    int i, shown = 0;
    char b[6];
    tputs(L("msg.select_area.message_areas", "\n|09-=[ |15Message Areas|09 ]=-|07\n\n"));
    tputs(L("msg.select_area.area_msgs_new", "|08   #  Area                                   Msgs   New|07\n"));
    for (i = 0; i < nareas; i++) {
        LONG total, nw;
        if (!area_readable(&areas[i]) || areas[i].type == AT_EMAIL) continue;
        ObtainSemaphore(&N.S->msglock);
        total = msg_count(areas[i].tag);
        ReleaseSemaphore(&N.S->msglock);
        nw = total - (LONG)N.user.lastread[i];
        if (nw < 0) nw = 0;
        tprintf("  |%s%2d|08) |07%-38s |15%5ld  %s%4ld|07\n", i == N.cur_msgarea ? "14" : "15",
                i + 1, areas[i].name, total, nw ? "|10" : "|08", nw);
        shown++;
    }
    if (!shown) { tputs(L("msg.select_area.no_areas_available", "  |08No areas available.|07\n")); return; }
    tputs(L("msg.select_area.area_number", "\n|07Area number: |15"));
    if (tgetline(b, 4, GL_DIGITS) <= 0) return;
    i = atoi(b) - 1;
    if (i >= 0 && i < nareas && areas[i].type != AT_EMAIL && area_readable(&areas[i])) {
        N.cur_msgarea = (UBYTE)i;
        tprintf(L("msg.select_area.now_in", "|07Now in |15%s|07.\n"), areas[i].name);
    }
}

void msg_scan_all(void)
{
    int i;
    LONG totalnew = 0;
    tputs(L("msg.scan_all.scanning_for_new", "\n|07Scanning for new messages...\n"));
    for (i = 0; i < nareas; i++) {
        LONG total, nw;
        if (!area_readable(&areas[i]) || areas[i].type == AT_EMAIL) continue;
        ObtainSemaphore(&N.S->msglock);
        total = msg_count(areas[i].tag);
        ReleaseSemaphore(&N.S->msglock);
        nw = total - (LONG)N.user.lastread[i];
        if (nw <= 0) continue;
        totalnew += nw;
        tprintf(L("msg.scan_all.new", "\n|15%s|07: |10%ld new|07. "), areas[i].name, nw);
        {
            LONG k;
            tputs(L("msg.scan_all.read_skip_mark", "|08[|15R|08]ead [|15S|08]kip [|15M|08]ark read [|15Q|08]uit: |07"));
            k = tgethot("RSMQ\r");
            if (k == KEY_HANGUP) return;
            tprintf("%c\n", (int)(k == '\r' ? 'R' : k));
            if (k == 'Q') break;
            if (k == 'M') { N.user.lastread[i] = total; user_save(); continue; }
            if (k == 'S') continue;
            if (!read_from(i, N.user.lastread[i] + 1, TRUE)) break;
        }
    }
    if (!totalnew) tputs(L("msg.scan_all.no_new_messages", "|08No new messages anywhere.|07\n"));
}

/* msg_scan callbacks for private mail */
struct MailCount { LONG mine, unread; ULONG first; };

static BOOL mail_list_one(const struct MsgHdr *h, void *ud)
{
    struct MailCount *mc = ud;
    if (!tmore()) return FALSE;
    if ((h->flags & MF_DELETED) || h->toid != N.user.id) return TRUE;
    mc->mine++;
    if (!(h->flags & MF_READ)) mc->unread++;
    {
        char d[16];
        bbs_datestr(h->date, d);
        tprintf("  |15%4ld |08%s |11%-20.20s |07%-36.36s %s\n", h->num, d, h->from, h->subject,
                (h->flags & MF_READ) ? "" : L("msg.mail_list_one.new", "|10NEW|07"));
    }
    return TRUE;
}

static BOOL mail_first_unread(const struct MsgHdr *h, void *ud)
{
    struct MailCount *mc = ud;
    if (h->toid == N.user.id && !(h->flags & (MF_READ | MF_DELETED))) { mc->first = h->num; return FALSE; }
    return TRUE;
}

static BOOL mail_count_unread(const struct MsgHdr *h, void *ud)
{
    struct MailCount *mc = ud;
    if (h->toid == N.user.id && !(h->flags & (MF_READ | MF_DELETED))) mc->unread++;
    return TRUE;
}

void msg_email(void)
{
    struct MailCount mc;
    LONG total, mine, unread;
    LONG k;
    if (email_area < 0) { tputs(L("msg.email.private_mail_isnt", "|12Private mail isn't set up.|07\n")); return; }
    ObtainSemaphore(&N.S->msglock);
    total = msg_count(areas[email_area].tag);
    ReleaseSemaphore(&N.S->msglock);

    tputs(L("msg.email.private_mail", "\n|09-=[ |15Private Mail|09 ]=-|07\n\n"));
    tpage_start();
    memset(&mc, 0, sizeof(mc));
    if (total > 0) msg_scan(areas[email_area].tag, 1, total, &N.S->msglock, mail_list_one, &mc);
    mine = mc.mine;
    unread = mc.unread;
    tpage_end();
    if (!mine) tputs(L("msg.email.your_mailbox_is", "  |08Your mailbox is empty.|07\n"));
    tprintf(L("msg.email.read_send_quit", "\n|08[|15R|08]ead%s [|15S|08]end [|15Q|08]uit: |07"), unread ? L("msg.email.new", " new") : "");
    k = tgethot("RSQ\r");
    if (k == KEY_HANGUP) return;
    tprintf("%c\n", (int)(k == '\r' ? 'Q' : k));
    if (k == 'S') post_in(email_area, NULL, NULL, 0, NULL, NULL);
    else if (k == 'R' && mine) {
        char b[8];
        tputs(L("msg.email.message_number_enter", "|07Message number |08(Enter = first unread)|07: |15"));
        if (tgetline(b, 7, GL_DIGITS) < 0) return;
        if (b[0]) read_from(email_area, strtoul(b, NULL, 10), FALSE);
        else {
            /* first unread addressed to me */
            memset(&mc, 0, sizeof(mc));
            if (total > 0) msg_scan(areas[email_area].tag, 1, total, &N.S->msglock, mail_first_unread, &mc);
            read_from(email_area, mc.first ? mc.first : 1, FALSE);
        }
    }
}

/* for the logon sequence: how much private mail is waiting */
LONG msg_unread_mail(void)
{
    struct MailCount mc;
    if (email_area < 0) return 0;
    memset(&mc, 0, sizeof(mc));
    /* 16 headers per read; msglock only while a block is read */
    msg_scan(areas[email_area].tag, 1, 0, &N.S->msglock, mail_count_unread, &mc);
    return mc.unread;
}
