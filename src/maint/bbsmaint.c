/*
 * BBSMaint - NilBBS's nightly maintenance.
 *
 *   BBSMaint [NODOORS] [NOPACK] [IMPORT] [DOOR=<tag>] [STRIPADS [AREA=<tag>]]
 *
 * IMPORT on its own just imports new files (see import.c) and stops - that is
 * what BBSControl's Import button, the Import Files icon and the sysop menu run.
 *
 * Normally run by the NIGHTLY event in Config/Events.cfg (exclusive: callers
 * are warned and logged off first), or by hand / ARexx: "EVENT NIGHTLY".
 * Everything it does is logged to BBS:Logs/Maint.log, and a summary can be
 * mailed to the sysop.
 *
 *   1. Message bases: drop deleted messages; optionally messages older than
 *      N days or beyond a count (per area, or NilBBS.cfg defaults), and old
 *      read private mail.  Messages are renumbered, so reply links and every
 *      user's last-read pointer are rewritten to match.  Local echomail not
 *      yet exported by BBSToss is never dropped.
 *   2. Users: optionally retire callers who haven't called in N days.
 *   3. Files: empty old files out of each area's .trash drawer, and drop
 *      FILE_ID.DIZ cache entries whose archive is gone.
 *   4. Logs: logs over a size limit become <log>.old.
 *   5. Nodes: clear leftover drop files and QWK work files of idle nodes.
 *   6. Doors: each Doors.cfg entry with "maint = <command>" gets that
 *      command run in its directory (LORD: "LCBDoor LORD.js MAINT") - except a
 *      door with its own schedule in Events.cfg ("BBSMaint DOOR=<tag>", as
 *      BBSSchedule's "Add door maintenance" makes), which runs only then.
 *      "maint_rexx = <script>" runs a CNet game's ARexx maintenance with BBSMaint
 *      as its CNet host (rexxmaint.c): Wall Street's Date, Empire's Emp.Maint.
 *
 * DOOR=<tag> on its own runs just that door's maint command and stops.
 * STRIPADS takes other boards' ads (Config/StripAds.cfg) out of every archive already in the file
 * areas (or just AREA=<tag>) and stops; uploads and imports are stripped as they arrive.
 *
 * Packing and user retirement need an empty BBS; with callers online they are
 * skipped (and said so) and the rest still runs.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "bbs.h"
#include "cfg.h"
#include "msgbase.h"
#include "dizcore.h"

void import_files(void);               /* import.c */

static const char __attribute__((used)) verstag[] = "$VER: BBSMaint " BBS_VERSION " (" BBS_VERDATE ")";

#define MAINT_LOG "BBS:Logs/Maint.log"

struct BBSShared *S;                    /* shared with import.c */
struct Cfg *C;
static char report[3000];
static LONG reportlen;

/* a line to the console, Maint.log and the sysop's report */
void say(const char *fmt, ...)
{
    char line[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    PutStr((STRPTR)line);
    PutStr((STRPTR)"\n");
    bbs_log(MAINT_LOG, "%s", line);
    if (reportlen + (LONG)strlen(line) + 2 < (LONG)sizeof(report)) {
        strcpy(report + reportlen, line);
        reportlen += strlen(line);
        report[reportlen++] = '\n';
        report[reportlen] = 0;
    }
}

static void lock_users(BOOL on) { if (S) { if (on) ObtainSemaphore(&S->userlock); else ReleaseSemaphore(&S->userlock); } }
static void lock_msgs(BOOL on)  { if (S) { if (on) ObtainSemaphore(&S->msglock);  else ReleaseSemaphore(&S->msglock); } }

static int callers_online(void)
{
    int n, c = 0;
    if (!S) return 0;
    shared_lock(S);
    for (n = 0; n < S->nodes; n++) if (S->node[n].state != NS_FREE) c++;
    shared_unlock(S);
    return c;
}

static ULONG file_date(struct FileInfoBlock *fib)
{
    return fib->fib_Date.ds_Days * 86400UL + fib->fib_Date.ds_Minute * 60UL;
}

/* ---- 1. message bases ---------------------------------------------------------------- */

/* pack one area; `ai' is its index (the users' last-read slot) */
static void pack_area(int ai, const char *tag, const char *name, int type,
                      LONG days, LONG maxmsgs, LONG maildays)
{
    char hp[PATHLEN], tp[PATHLEN], hn[PATHLEN], tn[PATHLEN];
    LONG count = msg_count(tag), i, kept = 0, dropped = 0, keepable = 0;
    ULONG *map = NULL, now = bbs_now();
    UBYTE *keep = NULL;
    char *text = NULL;
    struct MsgHdr h;
    struct MsgScan ms;          /* the three passes read headers 16 at a time */
    BPTR nh = 0, nt = 0;
    BOOL ok = TRUE;

    if (count <= 0) return;
    msg_scan_init(&ms, tag);
    map  = AllocVec((count + 1) * sizeof(ULONG), MEMF_CLEAR);
    keep = AllocVec(count + 1, MEMF_CLEAR);
    text = AllocVec(MAX_MSGTEXT + 1, 0);
    if (!map || !keep || !text) { say("  %s: out of memory - skipped", tag); goto out; }

    /* decide: deleted always goes; unsent local echomail always stays */
    for (i = 1; i <= count; i++) {
        BOOL k = TRUE;
        if (!msg_scan_hdr(&ms, i, &h)) k = FALSE;
        else if (h.flags & MF_DELETED) k = FALSE;
        else if (type == 1 && (h.flags & MF_LOCAL) && !(h.flags & MF_SENT)) k = TRUE;
        else if (type == 2) {                   /* private mail: old and read */
            if (maildays > 0 && (h.flags & MF_READ) && now - h.date > (ULONG)maildays * 86400UL) k = FALSE;
        } else if (days > 0 && now - h.date > (ULONG)days * 86400UL) k = FALSE;
        keep[i] = k;
        if (k) keepable++;
    }
    /* over the count limit: the oldest kept ones go too */
    if (maxmsgs > 0 && type != 2 && keepable > maxmsgs) {
        LONG extra = keepable - maxmsgs;
        for (i = 1; i <= count && extra > 0; i++)
            if (keep[i] && msg_scan_hdr(&ms, i, &h) && !(type == 1 && (h.flags & MF_LOCAL) && !(h.flags & MF_SENT))) {
                keep[i] = 0;
                extra--;
            }
    }
    for (i = 1; i <= count; i++) if (keep[i]) map[i] = ++kept; else dropped++;
    if (!dropped) { say("  %-20s %5ld messages, nothing to pack", name, count); goto out; }

    /* write the packed copy, then swap it in */
    sprintf(hp, "BBS:Msgs/%s.hdr", tag);  sprintf(tp, "BBS:Msgs/%s.txt", tag);
    sprintf(hn, "BBS:Msgs/%s.hdr.new", tag); sprintf(tn, "BBS:Msgs/%s.txt.new", tag);
    if (!(nh = Open((STRPTR)hn, MODE_NEWFILE)) || !(nt = Open((STRPTR)tn, MODE_NEWFILE))) { ok = FALSE; goto done; }
    {
        LONG off = 0;
        BPTR ot = Open((STRPTR)tp, MODE_OLDFILE);
        for (i = 1; i <= count && ok; i++) {
            LONG left, len = 0;
            if (!keep[i] || !msg_scan_hdr(&ms, i, &h)) continue;
            /* copy the body in chunks: imported messages can be any size */
            left = h.textlen;
            if (ot && Seek(ot, h.textoff, OFFSET_BEGINNING) >= 0)
                while (left > 0) {
                    LONG n = Read(ot, text, left > MAX_MSGTEXT ? MAX_MSGTEXT : left);
                    if (n <= 0) break;
                    if (Write(nt, text, n) != n) { ok = FALSE; break; }
                    left -= n;
                    len += n;
                }
            h.num = map[i];
            h.textoff = off;
            h.textlen = len;
            off += len;
            h.replyto = h.replyto <= (ULONG)count ? map[h.replyto] : 0;
            h.replies = h.replies <= (ULONG)count ? map[h.replies] : 0;
            if (Write(nh, &h, MSGHDR_SIZE) != MSGHDR_SIZE) ok = FALSE;
        }
        if (ot) Close(ot);
    }
done:
    if (nh) Close(nh);
    if (nt) Close(nt);
    if (!ok) {
        DeleteFile((STRPTR)hn); DeleteFile((STRPTR)tn);
        say("  %s: couldn't write the packed copy - left as it was", tag);
        goto out;
    }
    DeleteFile((STRPTR)hp); DeleteFile((STRPTR)tp);
    Rename((STRPTR)hn, (STRPTR)hp);
    Rename((STRPTR)tn, (STRPTR)tp);

    /* every user's last-read pointer: the number of kept messages up to it */
    {
        struct UserRec *u = AllocVec(sizeof(struct UserRec), 0);
        LONG nu, id;
        if (u) {
            lock_users(TRUE);
            nu = userdb_count();
            for (id = 1; id <= nu; id++) {
                ULONG lr, j, n = 0;
                if (!userdb_read(id, u)) continue;
                lr = u->lastread[ai];
                if (!lr) continue;
                if (lr > (ULONG)count) lr = count;
                for (j = 1; j <= lr; j++) if (keep[j]) n++;
                if (n != u->lastread[ai]) { u->lastread[ai] = n; userdb_write(u); }
            }
            lock_users(FALSE);
            FreeVec(u);
        }
    }
    say("  %-20s %5ld messages, %ld packed out, %ld kept", name, count, dropped, kept);
out:
    msg_scan_done(&ms);
    if (map) FreeVec(map);
    if (keep) FreeVec(keep);
    if (text) FreeVec(text);
}

static void pack_messages(void)
{
    struct Cfg *mc = cfg_load("BBS:Config/MsgAreas.cfg");
    LONG i, n = cfg_sections(mc);
    LONG ddays = cfg_int(C, "maint_msg_days", 0), dmax = cfg_int(C, "maint_msg_max", 0);
    LONG maildays = cfg_int(C, "maint_mail_days", 180);
    say("Message bases:");
    lock_msgs(TRUE);
    for (i = 0; i < n && i < MAX_MSGAREAS; i++) {
        const char *tag = cfg_section(mc, i), *type = cfg_sget(mc, tag, "type", "local");
        int t = !str_icmp(type, "echo") ? 1 : !str_icmp(type, "email") ? 2 : 0;
        pack_area((int)i, tag, cfg_sget(mc, tag, "name", tag), t,
                  cfg_sint(mc, tag, "purge_days", ddays), cfg_sint(mc, tag, "max_msgs", dmax), maildays);
    }
    lock_msgs(FALSE);
    cfg_free(mc);
}

/* ---- 2. users ----------------------------------------------------------------------------- */

static void retire_users(void)
{
    LONG days = cfg_int(C, "maint_user_days", 0), keeplevel = cfg_int(C, "maint_user_keep_level", 100);
    struct UserRec *u;
    LONG nu, id, gone = 0, total = 0;
    ULONG now = bbs_now();
    if (days <= 0) { say("Users: retiring inactive callers is off (maint_user_days = 0)."); return; }
    if (!(u = AllocVec(sizeof(struct UserRec), 0))) return;
    lock_users(TRUE);
    nu = userdb_count();
    for (id = 1; id <= nu; id++) {
        if (!userdb_read(id, u) || (u->flags & UF_DELETED)) continue;
        total++;
        if (u->level >= keeplevel || !u->lastcall || now - u->lastcall <= (ULONG)days * 86400UL) continue;
        u->flags |= UF_DELETED;
        userdb_write(u);
        {
            char p[PATHLEN];
            sprintf(p, "BBS:Data/Plans/%lu.plan", u->id);
            DeleteFile((STRPTR)p);
        }
        say("  retired %s (last call %lu days ago)", u->name, (now - u->lastcall) / 86400UL);
        gone++;
    }
    lock_users(FALSE);
    FreeVec(u);
    say("Users: %ld active, %ld retired after %ld days away.", total - gone, gone, days);
}

/* ---- 3. files ------------------------------------------------------------------------------ */

/* delete files in dir: older than `days' days (0 = all of them), or, with a
 * `twin' drawer, those whose namesake there is gone */
static LONG sweep(const char *dir, LONG days, const char *twin)
{
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    char names[64][108];
    LONG n = 0, i, done = 0;
    ULONG now = bbs_now();
    BPTR l;
    if (!fib) return 0;
    if ((l = Lock((STRPTR)dir, ACCESS_READ))) {
        if (Examine(l, fib))
            while (n < 64 && ExNext(l, fib)) {
                if (fib->fib_DirEntryType > 0) continue;
                if (twin) {
                    char p[PATHLEN];
                    path_join(p, twin, fib->fib_FileName);
                    if (file_exists(p)) continue;
                } else if (days > 0 && now - file_date(fib) <= (ULONG)days * 86400UL) continue;
                str_copy(names[n++], fib->fib_FileName, 108);
            }
        UnLock(l);
    }
    FreeDosObject(DOS_FIB, fib);
    for (i = 0; i < n; i++) {
        char p[PATHLEN];
        path_join(p, dir, names[i]);
        if (DeleteFile((STRPTR)p)) done++;
    }
    return done;
}

static void tidy_files(void)
{
    struct Cfg *fc = cfg_load("BBS:Config/FileAreas.cfg");
    LONG i, n = cfg_sections(fc), trash = 0, diz = 0, tdays = cfg_int(C, "maint_trash_days", 14);
    for (i = 0; i < n; i++) {
        const char *tag = cfg_section(fc, i);
        char path[PATHLEN], sub[PATHLEN];
        str_copy(path, cfg_sget(fc, tag, "path", ""), PATHLEN);
        if (!path[0]) continue;                 /* a heading for sub-areas: no files */
        if (cfg_sbool(fc, tag, "readonly", FALSE) || cfg_sbool(fc, tag, "cdrom", FALSE)) continue;  /* a disc: its cache is kept (it may be out) */
        sprintf(sub, "%s/.trash", path);
        if (tdays > 0) trash += sweep(sub, tdays, NULL);
        sprintf(sub, "%s/.diz", path);
        diz += sweep(sub, 0, path);
    }
    cfg_free(fc);
    say("Files: %ld old file%s emptied from the trash, %ld stale DIZ cache entr%s.",
        trash, trash == 1 ? "" : "s", diz, diz == 1 ? "y" : "ies");
}

/* ---- 4. logs ---------------------------------------------------------------------------- */

/* one-liners: keep the newest oneliners_keep (0 = keep them all) */
static void trim_lists(LONG keep)
{
    LONG gone;
    if (keep <= 0) return;
    lock_msgs(TRUE);
    gone = oneliners_trim(keep);
    lock_msgs(FALSE);
    if (gone) say("One-liners: %ld old one%s removed, the newest %ld kept.", gone, gone == 1 ? "" : "s", keep);
}

static void rotate_logs(void)
{
    /* not Bans.log: it's the permanent ban history, never rotated */
    static const char *logs[] = { "System", "Callers", "Maint", "Event-NIGHTLY", NULL };
    LONG limit = cfg_int(C, "maint_log_kb", 256) * 1024, rotated = 0;
    const char **l;
    for (l = logs; *l; l++) {
        char p[PATHLEN], old[PATHLEN];
        sprintf(p, "BBS:Logs/%s.log", *l);
        if (file_size(p) <= limit) continue;
        sprintf(old, "BBS:Logs/%s.log.old", *l);
        DeleteFile((STRPTR)old);
        if (Rename((STRPTR)p, (STRPTR)old)) rotated++;
    }
    say("Logs: %ld over %ld KB rotated to .old.", rotated, limit / 1024);
}

/* ---- 5. nodes -------------------------------------------------------------------------------- */

static void tidy_nodes(void)
{
    int n, nodes = S ? S->nodes : cfg_int(C, "nodes", 4), cleaned = 0;
    for (n = 1; n <= nodes; n++) {
        char dir[40];
        if (S && S->node[n - 1].state != NS_FREE) continue;
        sprintf(dir, "BBS:Nodes/Node%d/qwk", n);
        sweep(dir, 0, NULL);
        sprintf(dir, "BBS:Nodes/Node%d", n);
        cleaned += sweep(dir, 0, NULL);
    }
    say("Nodes: %d leftover file%s cleared.", cleaned, cleaned == 1 ? "" : "s");
}

/* ---- other boards' ads, in the files already here -------------------------------------------- */
static void strip_all(const char *only)
{
    struct Cfg *fc = cfg_load("BBS:Config/FileAreas.cfg");
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    LONG i, n = fc ? cfg_sections(fc) : 0, files = 0, hit = 0, gone = 0;
    for (i = 0; i < n && fib; i++) {
        const char *tag = cfg_section(fc, i), *path = cfg_sget(fc, tag, "path", "");
        BPTR l;
        if (!path[0] || (only && str_icmp(tag, only))) continue;
        if (cfg_sbool(fc, tag, "readonly", FALSE) || cfg_sbool(fc, tag, "cdrom", FALSE)) continue;  /* a CD/DVD can't be changed */
        if (!(l = Lock((STRPTR)path, ACCESS_READ))) continue;
        if (Examine(l, fib))
            while (ExNext(l, fib)) {
                char full[PATHLEN], rm[300];
                LONG k;
                if (fib->fib_DirEntryType > 0) continue;
                path_join(full, path, (char *)fib->fib_FileName);
                files++;
                if ((k = ads_strip(full, rm, sizeof(rm))) > 0) {
                    say("  %-10s %-16s %s", tag, (char *)fib->fib_FileName, rm);
                    hit++; gone += k;
                }
            }
        UnLock(l);
    }
    if (fib) FreeDosObject(DOS_FIB, fib);
    if (fc) cfg_free(fc);
    say("Ads: %ld file%s taken out of %ld archive%s (%ld files looked at).", gone, gone == 1 ? "" : "s",
        hit, hit == 1 ? "" : "s", files);
}

/* ---- 6. doors ---------------------------------------------------------------------------------- */

/* a door's "assign = NAME: path" (as BBSNode makes it for the door) - so a
   maint command such as rx PFILES:Realm/REALM_MAINT.rexx works on a system
   that has no PFILES: of its own */
static void maint_assign(const char *spec)
{
    char name[40], *sp;
    const char *path;
    BPTR l;
    if (!spec[0]) return;
    str_copy(name, spec, sizeof(name));
    if (!(sp = strchr(name, ' ')) && !(sp = strchr(name, '='))) return;
    *sp = 0;
    path = str_trim((char *)spec + (sp - name) + 1);
    if ((sp = strchr(name, ':'))) *sp = 0;
    {
        char dev[44];
        sprintf(dev, "%s:", name);
        if ((l = Lock((STRPTR)dev, ACCESS_READ))) { UnLock(l); return; }   /* already there */
    }
    if ((l = Lock((STRPTR)path, ACCESS_READ)) && !AssignLock((STRPTR)name, l)) UnLock(l);
}

/* does Events.cfg give this door a schedule of its own ("BBSMaint DOOR=<tag>")? */
static BOOL own_schedule(struct Cfg *ev, const char *tag)
{
    LONG i, n = ev ? cfg_sections(ev) : 0;
    char want[NAMELEN + 8];
    sprintf(want, "DOOR=%s", tag);
    for (i = 0; i < n; i++) {
        const char *cmd = cfg_sget(ev, cfg_section(ev, i), "command", "");
        const char *hit = str_istr(cmd, want);
        if (hit && str_istr(cmd, "BBSMaint") && (hit[strlen(want)] == 0 || hit[strlen(want)] == ' ')) return TRUE;
    }
    return FALSE;
}

/* every door's maint command (the nightly run), or just one door's (only = its tag) */
LONG rexx_maint(const char *script);

static void door_maint(const char *only)
{
    struct Cfg *dc = cfg_load("BBS:Config/Doors.cfg");
    struct Cfg *ev = only ? NULL : cfg_load("BBS:Config/Events.cfg");
    LONG i, n = cfg_sections(dc), ran = 0;
    for (i = 0; i < n; i++) {
        const char *tag = cfg_section(dc, i), *cmd = cfg_sget(dc, tag, "maint", "");
        const char *rx = cfg_sget(dc, tag, "maint_rexx", "");
        const char *dir = cfg_sget(dc, tag, "dir", "");
        BPTR lock = 0, old = 0, in, out;
        LONG rc;
        if (only && str_icmp(tag, only)) continue;
        if (!cmd[0] && !rx[0]) { if (only) say("Door %s has no maint command in Doors.cfg.", tag); continue; }
        if (!only && own_schedule(ev, tag)) { say("Door %s: on its own schedule (Events.cfg) - not tonight.", tag); continue; }
        maint_assign(cfg_sget(dc, tag, "assign", ""));
        if (dir[0] && (lock = Lock((STRPTR)dir, ACCESS_READ))) old = CurrentDir(lock);
        if (cmd[0]) {
            say("Door %s: %s", tag, cmd);
            in = Open((STRPTR)"NIL:", MODE_OLDFILE);
            if ((out = Open((STRPTR)MAINT_LOG, MODE_READWRITE))) Seek(out, 0, OFFSET_END);
            else out = Open((STRPTR)"NIL:", MODE_NEWFILE);
            rc = SystemTags((STRPTR)cmd, SYS_Input, in, SYS_Output, out, NP_StackSize, 65536, TAG_END);
            Close(in);
            Close(out);
            say("  finished, rc %ld", rc);
        }
        if (rx[0]) {                    /* a CNet game's ARexx script: we're its host (rexxmaint.c) */
            say("Door %s: ARexx %s", tag, rx);
            rc = rexx_maint(rx);
            say("  finished, rc %ld", rc);
        }
        if (lock) { CurrentDir(old); UnLock(lock); }
        ran++;
    }
    cfg_free(dc);
    if (ev) cfg_free(ev);
    if (!ran && !only) say("Doors: none to run.");
    if (only && !ran && !n) say("No Doors.cfg.");
}

/* ---- the report --------------------------------------------------------------------------------- */

static void mail_report(void)
{
    struct Cfg *mc;
    struct UserRec *u;
    struct MsgHdr h;
    LONG i, n, id, nu, slevel = cfg_int(C, "sysop_level", 255);
    const char *mailtag = NULL;
    if (!cfg_bool(C, "maint_report", TRUE)) return;
    mc = cfg_load("BBS:Config/MsgAreas.cfg");
    n = cfg_sections(mc);
    for (i = 0; i < n && !mailtag; i++)
        if (!str_icmp(cfg_sget(mc, cfg_section(mc, i), "type", ""), "email")) mailtag = cfg_section(mc, i);
    if (!mailtag || !(u = AllocVec(sizeof(struct UserRec), 0))) { cfg_free(mc); return; }
    memset(&h, 0, sizeof(h));
    lock_users(TRUE);
    nu = userdb_count();
    for (id = 1; id <= nu && !h.toid; id++)
        if (userdb_read(id, u) && !(u->flags & UF_DELETED) && u->level >= slevel) {
            h.toid = u->id;
            str_copy(h.to, u->name, sizeof(h.to));
        }
    lock_users(FALSE);
    FreeVec(u);
    if (h.toid) {
        strcpy(h.from, "NilBBS Maintenance");
        strcpy(h.subject, "Nightly maintenance report");
        h.date = bbs_now();
        h.flags = MF_LOCAL | MF_PRIVATE;
        lock_msgs(TRUE);
        msg_add(mailtag, &h, report, reportlen);
        lock_msgs(FALSE);
    }
    cfg_free(mc);
}

/* ---- main ----------------------------------------------------------------------------------------- */

static int real_main(void)
{
    struct RDArgs *rda;
    LONG args[9] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    char when[20];
    int online;

    rda = ReadArgs((STRPTR)"NODOORS/S,NOPACK/S,IMPORT/S,CLEARONELINERS/S,CLEARCALLERS/S,TRIMONELINERS/K/N,DOOR/K,STRIPADS/S,AREA/K", args, NULL);
    if (!rda) { PrintFault(IoErr(), (STRPTR)"BBSMaint"); return RETURN_FAIL; }
    {   /* BBS: must exist - default to our own drawer, like NilBBS does */
        BPTR l = Lock((STRPTR)"BBS:", ACCESS_READ);
        if (l) UnLock(l);
        else if ((l = DupLock(GetProgramDir()))) { if (!AssignLock((STRPTR)"BBS", l)) UnLock(l); }
    }
    C = cfg_load(bbs_config());
    S = shared_find();
    diz_env.cfg = C;                        /* FILE_ID.DIZ handling (dizcore.c) */
    strcpy(diz_env.work, "BBS:Nodes/Maint");
    {
        BPTR l = CreateDir((STRPTR)diz_env.work);
        if (l) UnLock(l);
    }
    diz_env.lock = S ? &S->filelock : NULL;
    diz_env.uploader = cfg_str(C, "sysop_name", "Sysop");
    if (args[3] || args[4] || args[5]) {    /* the list commands (BBSControl, sysops): just those */
        LONG n;
        lock_msgs(TRUE);
        if (args[3]) { n = oneliners_clear(); say("One-liners cleared (%ld removed).", n); }
        if (args[4]) { n = lastcallers_clear(); say("Last callers cleared (%ld removed).", n); }
        if (args[5]) {
            LONG keep = *(LONG *)args[5];
            n = oneliners_trim(keep);
            say("One-liners trimmed to the newest %ld (%ld removed).", keep, n);
        }
        lock_msgs(FALSE);
        bbs_log(BBS_SYSLOG, "BBSMaint: one-liners/last callers cleanup");
        cfg_free(C);
        FreeArgs(rda);
        return RETURN_OK;
    }
    if (args[7]) {                          /* STRIPADS: other boards' ads out of the archives here */
        char when[20];
        bbs_datetimestr(bbs_now(), when);
        say("Taking other boards' ads out of the file areas, %s", when);
        strip_all((const char *)args[8]);
        cfg_free(C);
        FreeArgs(rda);
        return RETURN_OK;
    }
    if (args[6]) {                          /* DOOR=<tag>: just that door's maintenance */
        char when[20];
        bbs_datetimestr(bbs_now(), when);
        say("Door maintenance for %s, %s", (char *)args[6], when);
        door_maint((const char *)args[6]);
        cfg_free(C);
        FreeArgs(rda);
        return RETURN_OK;
    }
    if (args[2]) {                          /* IMPORT: just that */
        import_files();
        cfg_free(C);
        FreeArgs(rda);
        return RETURN_OK;
    }
    bbs_datetimestr(bbs_now(), when);
    say("NilBBS nightly maintenance, %s%s", when, S ? "" : " (NilBBS isn't running)");

    online = callers_online();
    if (args[1]) say("Message bases and users: skipped (NOPACK).");
    else if (online) say("Message bases and users: skipped - %d caller%s online.", online, online == 1 ? " is" : "s are");
    else {
        pack_messages();
        retire_users();
    }
    import_files();
    tidy_files();
    tidy_nodes();
    trim_lists(cfg_int(C, "oneliners_keep", 50));
    if (!args[0]) door_maint(NULL);
    rotate_logs();
    say("Done.");
    mail_report();
    cfg_free(C);
    FreeArgs(rda);
    return RETURN_OK;
}

int main(void)
{
    return run_with_stack(16384, real_main);
}
