/*
 * community.c - bulletins, the voting booth, and finger (user profiles + plans).
 *
 * Bulletins: BBS:Config/Bulletins.cfg, one [TAG] per bulletin
 *   [RULES]
 *   name = System rules
 *   file = Bulletins/rules      ; BBS:Text/Bulletins/rules.ans|.asc|.txt
 *   acs  =                      ; optional: who may read it
 * A bulletin whose file changed since the caller's last call is NEW - until they read it (this
 * call; next call it's older than their last call anyway).  A new user's first call has no last
 * call, so without the "read this call" list every bulletin stayed NEW after reading it.
 *
 * Voting: BBS:Data/Voting.dat, fixed records; who voted is a bitmap by
 * user number, so nobody votes twice.
 *
 * Plans: BBS:Data/Plans/<user#>.plan - a few lines the caller writes about
 * themselves, shown by finger (here and over the network, see the daemon).
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "node.h"

/* ---- bulletins ------------------------------------------------------------------ */

#define MAX_BULLETINS 32

struct Bulletin {
    char  name[LONGNAME];
    char  file[64];
    ULONG date;
};

/* newest date of Text/<name>.ans|.asc|.txt|.vt, 0 if none exists */
static ULONG text_date(const char *name)
{
    static const char *ext[] = { ".ans", ".asc", ".txt", ".vt", NULL };
    const char **e;
    ULONG best = 0;
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    if (!fib) return 0;
    for (e = ext; *e; e++) {
        char path[PATHLEN];
        BPTR l;
        sprintf(path, "BBS:Text/%s%s", name, *e);
        if (!(l = Lock((STRPTR)path, ACCESS_READ))) continue;
        if (Examine(l, fib)) {
            ULONG d = fib->fib_Date.ds_Days * 86400UL + fib->fib_Date.ds_Minute * 60UL +
                      fib->fib_Date.ds_Tick / TICKS_PER_SECOND;
            if (d > best) best = d;
        }
        UnLock(l);
    }
    FreeDosObject(DOS_FIB, fib);
    return best;
}

static int bulletins_load(struct Bulletin *b)
{
    struct Cfg *c = cfg_load("BBS:Config/Bulletins.cfg");
    LONG i, n = cfg_sections(c);
    int nb = 0;
    for (i = 0; i < n && nb < MAX_BULLETINS; i++) {
        const char *tag = cfg_section(c, i);
        if (!acs_check(cfg_sget(c, tag, "acs", ""))) continue;
        str_copy(b[nb].name, cfg_sget(c, tag, "name", tag), LONGNAME);
        str_copy(b[nb].file, cfg_sget(c, tag, "file", tag), sizeof(b[nb].file));
        b[nb].date = text_date(b[nb].file);
        if (b[nb].date) nb++;               /* skip bulletins with no file */
    }
    cfg_free(c);
    return nb;
}

/* bulletins read during this call (a node runs one call, so this is per caller) */
static char read_now[MAX_BULLETINS][64];
static int nread_now;

static BOOL is_new(const struct Bulletin *b)
{
    int i;
    if (b->date <= N.prevcall) return FALSE;
    for (i = 0; i < nread_now; i++) if (!strcmp(read_now[i], b->file)) return FALSE;
    return TRUE;
}

static void bulletin_show(struct Bulletin *b)
{
    if (is_new(b) && nread_now < MAX_BULLETINS) str_copy(read_now[nread_now++], b->file, sizeof(read_now[0]));
    tpage_start();
    if (!tshowfile(b->file)) tputs(L("comm.bulletin_show.missing", "|08(missing)|07\n"));
    tpage_end();
    tpause();
}

void bulletins(void)
{
    struct Bulletin *b = AllocVec(sizeof(struct Bulletin) * MAX_BULLETINS, 0);
    int nb, i;
    if (!b) return;
    nb = bulletins_load(b);
    set_activity("Reading bulletins");
    for (;;) {
        char in[6];
        tputs(L("comm.bulletins.bulletins", "\n|09-=[ |15Bulletins|09 ]=-|07\n\n"));
        if (!nb) { tputs(L("comm.bulletins.there_are_no", "  |08There are no bulletins.|07\n")); break; }
        for (i = 0; i < nb; i++) {
            char d[16];
            bbs_datestr(b[i].date, d);
            tprintf("  |15%2d|08) |07%-40s |08%s %s|07\n", i + 1, b[i].name, d,
                    is_new(&b[i]) ? L("comm.bulletins.new", "|10NEW") : "");
        }
        tputs(L("comm.bulletins.bulletin_number_all", "\n|07Bulletin |08(number, A = all new, Enter = quit)|07: |15"));
        if (tgetline(in, 4, GL_UPPER) <= 0) break;
        if (in[0] == 'A') {
            for (i = 0; i < nb && N.online; i++) if (is_new(&b[i])) bulletin_show(&b[i]);
            continue;
        }
        i = atoi(in) - 1;
        if (i >= 0 && i < nb) bulletin_show(&b[i]);
    }
    FreeVec(b);
    back_to_menu();
}

/* at logon: say how many bulletins changed since the last call */
int bulletins_new(void)
{
    struct Bulletin *b = AllocVec(sizeof(struct Bulletin) * MAX_BULLETINS, 0);
    int nb, i, n = 0;
    if (!b) return 0;
    nb = bulletins_load(b);
    for (i = 0; i < nb; i++) if (is_new(&b[i])) n++;
    FreeVec(b);
    return n;
}

/* ---- voting booth ----------------------------------------------------------------- */

#define VOTE_FILE     "BBS:Data/Voting.dat"
#define VOTE_TOPICS   20
#define VOTE_CHOICES  10
#define VOTE_USERS    8192

struct VoteTopic {
    UBYTE used, nchoices;
    UWORD pad;
    ULONG created;
    char  by[NAMELEN];
    char  question[76];
    char  choice[VOTE_CHOICES][44];
    ULONG count[VOTE_CHOICES];
    UBYTE voted[VOTE_USERS / 8];            /* bit per user number */
};

/* the caller holds msglock */
static BOOL vote_read(int i, struct VoteTopic *t)
{
    BPTR fh = Open((STRPTR)VOTE_FILE, MODE_OLDFILE);
    BOOL ok = FALSE;
    memset(t, 0, sizeof(*t));
    if (!fh) return FALSE;
    if (Seek(fh, i * (LONG)sizeof(*t), OFFSET_BEGINNING) >= 0)
        ok = Read(fh, t, sizeof(*t)) == sizeof(*t) && t->used;
    Close(fh);
    return ok;
}

static BOOL vote_write(int i, struct VoteTopic *t)
{
    BPTR fh = Open((STRPTR)VOTE_FILE, MODE_READWRITE);
    BOOL ok = FALSE;
    if (!fh) return FALSE;
    /* grow the file with empty records up to this slot */
    Seek(fh, 0, OFFSET_END);
    {
        LONG size = Seek(fh, 0, OFFSET_CURRENT);
        static struct VoteTopic empty;
        while (size < i * (LONG)sizeof(*t)) {
            Write(fh, &empty, sizeof(empty));
            size += sizeof(empty);
        }
    }
    if (Seek(fh, i * (LONG)sizeof(*t), OFFSET_BEGINNING) >= 0)
        ok = Write(fh, t, sizeof(*t)) == sizeof(*t);
    Close(fh);
    return ok;
}

static BOOL has_voted(struct VoteTopic *t)
{
    ULONG u = N.user.id;
    return u < VOTE_USERS && (t->voted[u / 8] & (1 << (u % 8)));
}

static void vote_results(struct VoteTopic *t)
{
    ULONG total = 0, i;
    for (i = 0; i < t->nchoices; i++) total += t->count[i];
    tprintf("\n|15%s\n", t->question);
    tprintf(total == 1 ? L("comm.vote_results.asked_by_one", "|08asked by %s, %lu vote|07\n\n")
                       : L("comm.vote_results.asked_by_many", "|08asked by %s, %lu votes|07\n\n"), t->by, total);
    for (i = 0; i < t->nchoices; i++) {
        int pct = total ? (int)(t->count[i] * 100 / total) : 0;
        int bar = pct / 4, j;
        tprintf("  |15%2lu|08) |07%-30.30s |11%3d%% |03", i + 1, t->choice[i], pct);
        for (j = 0; j < bar; j++) tputs(N.charset == CS_ASCII || N.term == TT_ASCII ? "#" : "\xDB");
        tputs("|07\n");
    }
}

static void vote_on(int ti)
{
    struct VoteTopic *t = AllocVec(sizeof(*t), 0);
    char b[6];
    int c;
    BOOL ok;
    if (!t) return;
    ObtainSemaphore(&N.S->msglock);
    ok = vote_read(ti, t);
    ReleaseSemaphore(&N.S->msglock);
    if (!ok) { FreeVec(t); return; }
    vote_results(t);
    if (has_voted(t)) { tputs(L("comm.vote_on.youve_already_voted", "\n|08You've already voted on this one.|07\n")); FreeVec(t); tpause(); return; }
    if (N.user.id >= VOTE_USERS) { FreeVec(t); return; }
    tprintf(L("comm.vote_on.your_vote_1", "\n|07Your vote |08(1-%d, Enter = skip)|07: |15"), (int)t->nchoices);
    if (tgetline(b, 3, GL_DIGITS) <= 0) { FreeVec(t); return; }
    c = atoi(b) - 1;
    if (c < 0 || c >= t->nchoices) { FreeVec(t); return; }
    ObtainSemaphore(&N.S->msglock);
    if (vote_read(ti, t) && !has_voted(t)) {        /* re-read: another node may have voted */
        t->count[c]++;
        t->voted[N.user.id / 8] |= 1 << (N.user.id % 8);
        vote_write(ti, t);
    }
    ReleaseSemaphore(&N.S->msglock);
    tputs(L("comm.vote_on.thanks_your_vote", "|10Thanks - your vote is counted.|07\n"));
    vote_results(t);
    FreeVec(t);
}

static void vote_add(void)
{
    struct VoteTopic *t = AllocVec(sizeof(*t), MEMF_CLEAR);
    int i, slot = -1;
    if (!t) return;
    tputs(L("comm.vote_add.question", "\n|07Question: |15"));
    if (tgetline(t->question, 74, 0) <= 0) { FreeVec(t); return; }
    for (i = 0; i < VOTE_CHOICES; i++) {
        tprintf(L("comm.vote_add.choice_enter_when", "|07Choice %d |08(Enter when done)|07: |15"), i + 1);
        if (tgetline(t->choice[i], 42, 0) <= 0) break;
        t->nchoices++;
    }
    if (t->nchoices < 2) { tputs(L("comm.vote_add.vote_needs_at", "|12A vote needs at least two choices.|07\n")); FreeVec(t); return; }
    t->used = 1;
    t->created = bbs_now();
    str_copy(t->by, N.user.name, NAMELEN);
    ObtainSemaphore(&N.S->msglock);
    {
        struct VoteTopic *x = AllocVec(sizeof(*x), 0);
        for (i = 0; x && i < VOTE_TOPICS && slot < 0; i++) if (!vote_read(i, x)) slot = i;
        if (x) FreeVec(x);
    }
    if (slot >= 0) vote_write(slot, t);
    ReleaseSemaphore(&N.S->msglock);
    if (slot < 0) tputs(L("comm.vote_add.the_booth_is", "|12The booth is full - delete an old topic first.|07\n"));
    else {
        tputs(L("comm.vote_add.topic_added", "|10Topic added.|07\n"));
        bbs_log(BBS_SYSLOG, "node %d: %s added voting topic \"%s\"", N.node, N.user.name, t->question);
    }
    FreeVec(t);
}

void voting_booth(void)
{
    struct VoteTopic *t = AllocVec(sizeof(*t), 0);
    const char *addacs = cfg_str(N.cfg, "vote_add_acs", "S");
    if (!t) return;
    set_activity("Voting booth");
    for (;;) {
        int i, shown = 0;
        char b[6];
        tputs(L("comm.voting_booth.voting_booth", "\n|09-=[ |15Voting Booth|09 ]=-|07\n\n"));
        for (i = 0; i < VOTE_TOPICS; i++) {
            BOOL ok;
            ObtainSemaphore(&N.S->msglock);
            ok = vote_read(i, t);
            ReleaseSemaphore(&N.S->msglock);
            if (!ok) continue;
            tprintf("  |15%2d|08) |07%-60.60s %s|07\n", i + 1, t->question,
                    has_voted(t) ? "" : "|10*");
            shown++;
        }
        if (!shown) tputs(L("comm.voting_booth.no_topics_yet", "  |08No topics yet.|07\n"));
        else tputs(L("comm.voting_booth.you_havent_voted", "\n  |10*|08 = you haven't voted yet|07\n"));
        tputs(L("comm.voting_booth.topic_number", "\n|07Topic number"));
        if (acs_check(addacs)) tputs(L("comm.voting_booth.add", ", |15A|07 add"));
        if (N.sysop) tputs(L("comm.voting_booth.delete", ", |15D|07 delete"));
        tputs(L("comm.voting_booth.enter_quit", " |08(Enter = quit)|07: |15"));
        if (tgetline(b, 4, GL_UPPER) <= 0) break;
        if (b[0] == 'A' && acs_check(addacs)) vote_add();
        else if (b[0] == 'D' && N.sysop) {
            tputs(L("comm.voting_booth.delete_topic_number", "|07Delete topic number: |15"));
            if (tgetline(b, 4, GL_DIGITS) > 0 && atoi(b) >= 1 && atoi(b) <= VOTE_TOPICS &&
                tyesno(L("comm.voting_booth.delete_it_and", "|12Delete it and its votes?|07"), FALSE)) {
                memset(t, 0, sizeof(*t));
                ObtainSemaphore(&N.S->msglock);
                vote_write(atoi(b) - 1, t);
                ReleaseSemaphore(&N.S->msglock);
                tputs(L("comm.voting_booth.deleted", "|10Deleted.|07\n"));
            }
        } else if ((i = atoi(b)) >= 1 && i <= VOTE_TOPICS) vote_on(i - 1);
    }
    FreeVec(t);
    back_to_menu();
}

/* at logon: topics this caller hasn't voted on */
int voting_waiting(void)
{
    struct VoteTopic *t = AllocVec(sizeof(*t), 0);
    int i, n = 0;
    if (!t) return 0;
    ObtainSemaphore(&N.S->msglock);
    for (i = 0; i < VOTE_TOPICS; i++) if (vote_read(i, t) && !has_voted(t)) n++;
    ReleaseSemaphore(&N.S->msglock);
    FreeVec(t);
    return n;
}

/* ---- finger: a user's profile and plan ----------------------------------------------- */

void plan_path(ULONG id, char *buf)
{
    sprintf(buf, "BBS:Data/Plans/%lu.plan", id);
}

void plan_edit(void)
{
    char path[PATHLEN], lines[8][72];
    int n = 0, i;
    BPTR fh, l;
    tputs(L("comm.plan_edit.your_plan_is", "\n|07Your plan is what people see when they finger you: up to 8 lines.\n"
          "|08An empty first line removes it; an empty line later finishes.|07\n"));
    for (n = 0; n < 8; n++) {
        tprintf("|08%d: |15", n + 1);
        if (tgetline(lines[n], 70, 0) < 0) return;
        if (!lines[n][0]) break;
        for (i = 0; lines[n][i]; i++) if (lines[n][i] == '|') lines[n][i] = '!';   /* no colour codes */
    }
    if ((l = Lock((STRPTR)"BBS:Data/Plans", ACCESS_READ))) UnLock(l);
    else if ((l = CreateDir((STRPTR)"BBS:Data/Plans"))) UnLock(l);
    plan_path(N.user.id, path);
    if (!n) { DeleteFile((STRPTR)path); tputs(L("comm.plan_edit.plan_removed", "|10Plan removed.|07\n")); return; }
    if ((fh = Open((STRPTR)path, MODE_NEWFILE))) {
        for (i = 0; i < n; i++) { FPuts(fh, (STRPTR)lines[i]); FPutC(fh, '\n'); }
        Close(fh);
        tputs(L("comm.plan_edit.plan_saved", "|10Plan saved.|07\n"));
    }
}

void finger(void)
{
    struct UserRec *u = AllocVec(sizeof(struct UserRec), 0);
    char name[NAMELEN], d1[20], d2[20], path[PATHLEN];
    LONG id;
    int n, on = 0;
    if (!u) return;
    tputs(L("comm.finger.finger_who_enter", "\n|07Finger who? |08(Enter = who's online)|07: |15"));
    if (tgetline(name, NAMELEN, GL_NAME) < 0) { FreeVec(u); return; }
    if (!name[0]) { FreeVec(u); whos_online(); return; }
    ObtainSemaphore(&N.S->userlock);
    id = userdb_find(name, u);
    ReleaseSemaphore(&N.S->userlock);
    if (!id || (u->flags & UF_DELETED)) { tputs(L("comm.finger.no_such_user", "|12No such user.|07\n")); FreeVec(u); return; }
    shared_lock(N.S);
    for (n = 0; n < N.S->nodes; n++)
        if (N.S->node[n].state >= NS_ONLINE && N.S->node[n].userid == id) on = n + 1;
    shared_unlock(N.S);
    bbs_datestr(u->firstcall, d1);
    bbs_datetimestr(u->lastcall, d2);
    tprintf("\n|15%s|07", u->name);
    if (cfg_bool(N.cfg, "finger_realname", FALSE) && u->realname[0]) tprintf(" |08(%s)|07", u->realname);
    tputs("\n");
    if (u->location[0]) tprintf(L("comm.finger.from", "  |07From        |15%s\n"), u->location);
    tprintf(L("comm.finger.member_since_calls", "  |07Member since |15%s|07, |15%lu|07 calls, |15%lu|07 posts\n"), d1, u->calls, u->posts);
    tprintf(L("comm.finger.files_up_down", "  |07Files       |15%lu|07 up, |15%lu|07 down\n"), u->uploads, u->downloads);
    if (on) tprintf(L("comm.finger.online_now_on", "  |10Online now on node %d|07\n"), on);
    else tprintf(L("comm.finger.last_on", "  |07Last on     |15%s\n"), d2);
    plan_path(u->id, path);
    if (file_exists(path)) {
        tputs(L("comm.finger.plan", "|07Plan:\n|03"));
        tshowpath(path, CS_LATIN1);
        tputs("|07");
    } else tputs(L("comm.finger.no_plan", "|08No plan.|07\n"));
    FreeVec(u);
}
