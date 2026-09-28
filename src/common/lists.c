/*
 * lists.c - the one-liners and last-callers lists: load, save, trim, delete,
 * clear.  Shared by BBSNode (sysop menu), BBSMaint (nightly trim + commands)
 * and BBSControl (buttons).  Callers hold shared->msglock while the BBS runs.
 *
 *   BBS:Data/OneLiners.dat    "name|date|text" lines, newest last
 *   BBS:Data/LastCallers.dat  struct LastCall records, newest last (MAX_LASTCALL)
 */
#include "bbs.h"
#include <string.h>
#include <stdio.h>
#include <proto/exec.h>
#include <proto/dos.h>

/* all one-liners (the newest OL_MAX if there are more) into a new array;
   *out = NULL when there are none.  FreeVec(*out) afterwards. */
LONG oneliners_load(OneLine **out)
{
    struct LineReader lr;
    OneLine *a;
    char line[OL_LINE + 40];
    LONG n = 0;
    *out = NULL;
    if (!(a = AllocVec(sizeof(OneLine) * OL_MAX, MEMF_CLEAR))) return 0;
    if (lr_open(&lr, BBS_ONELINERS)) {
        while (lr_gets(&lr, line, sizeof(line)) >= 0) {
            if (!line[0]) continue;
            if (n == OL_MAX) { memmove(a[0], a[1], sizeof(OneLine) * (OL_MAX - 1)); n--; }
            str_copy(a[n++], line, OL_LINE);
        }
        lr_close(&lr);
    }
    if (!n) { FreeVec(a); return 0; }
    *out = a;
    return n;
}

/* rewrite the file with lines[0..n-1] (none: the file is deleted) */
BOOL oneliners_save(OneLine *lines, LONG n)
{
    BPTR fh;
    LONG i;
    if (n <= 0) { DeleteFile((STRPTR)BBS_ONELINERS); return TRUE; }
    if (!(fh = Open((STRPTR)BBS_ONELINERS, MODE_NEWFILE))) return FALSE;
    for (i = 0; i < n; i++) { FPuts(fh, (STRPTR)lines[i]); FPutC(fh, '\n'); }
    Close(fh);
    return TRUE;
}

/* keep only the newest `keep` one-liners; returns how many went (keep <= 0: none) */
LONG oneliners_trim(LONG keep)
{
    OneLine *a;
    LONG n, gone;
    if (keep <= 0) return 0;
    n = oneliners_load(&a);
    if (n <= keep) { if (a) FreeVec(a); return 0; }
    gone = n - keep;
    oneliners_save(a + gone, keep);
    FreeVec(a);
    return gone;
}

/* delete the one-liners whose number (1 = oldest) is set in del[]; returns count */
LONG oneliners_delete(const UBYTE *del, LONG ndel)
{
    OneLine *a;
    LONG n, i, j = 0, gone = 0;
    n = oneliners_load(&a);
    if (!n) return 0;
    for (i = 0; i < n; i++) {
        if (i + 1 < ndel && del[i + 1]) { gone++; continue; }
        if (j != i) memcpy(a[j], a[i], sizeof(OneLine));
        j++;
    }
    if (gone) oneliners_save(a, j);
    FreeVec(a);
    return gone;
}

LONG oneliners_clear(void)
{
    OneLine *a;
    LONG n = oneliners_load(&a);
    if (a) FreeVec(a);
    DeleteFile((STRPTR)BBS_ONELINERS);
    return n;
}

/* ---- last callers ------------------------------------------------------------ */

LONG lastcallers_load(struct LastCall *all)
{
    BPTR fh;
    LONG n = 0;
    if ((fh = Open((STRPTR)BBS_LASTCALLERS, MODE_OLDFILE))) {
        n = Read(fh, all, sizeof(struct LastCall) * MAX_LASTCALL) / (LONG)sizeof(struct LastCall);
        Close(fh);
        if (n < 0) n = 0;
    }
    return n;
}

BOOL lastcallers_save(struct LastCall *all, LONG n)
{
    BPTR fh;
    if (n <= 0) { DeleteFile((STRPTR)BBS_LASTCALLERS); return TRUE; }
    if (!(fh = Open((STRPTR)BBS_LASTCALLERS, MODE_NEWFILE))) return FALSE;
    Write(fh, all, n * sizeof(struct LastCall));
    Close(fh);
    return TRUE;
}

/* drop every entry for this handle (case-insensitive); returns how many */
LONG lastcallers_remove(const char *name)
{
    struct LastCall all[MAX_LASTCALL];
    LONG n = lastcallers_load(all), i, j = 0;
    for (i = 0; i < n; i++) {
        if (!str_icmp(all[i].name, name)) continue;
        if (j != i) all[j] = all[i];
        j++;
    }
    if (j != n) lastcallers_save(all, j);
    return n - j;
}

LONG lastcallers_clear(void)
{
    struct LastCall all[MAX_LASTCALL];
    LONG n = lastcallers_load(all);
    DeleteFile((STRPTR)BBS_LASTCALLERS);
    return n;
}

/* is this account kept off the last-callers list?  Its own Hidden flag, or
   (hide_sysop_calls) a level at or above hide_calls_level (default: sysop_level) */
BOOL lastcall_hidden(struct Cfg *c, const struct UserRec *u)
{
    LONG lvl;
    if (u->flags & UF_HIDDEN) return TRUE;
    if (!cfg_bool(c, "hide_sysop_calls", TRUE)) return FALSE;
    lvl = cfg_int(c, "hide_calls_level", cfg_int(c, "sysop_level", 255));
    return lvl > 0 && u->level >= lvl;
}
