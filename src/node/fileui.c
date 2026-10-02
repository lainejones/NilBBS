/*
 * fileui.c - file areas: list, new files, search, ZMODEM download/upload.
 *
 * Areas come from BBS:Config/FileAreas.cfg, one [TAG] section each:
 *   [AMIGA]
 *   name     = Amiga Software
 *   path     = BBS:Files/Amiga
 *   download = 10         ; level to download
 *   upload   = 10         ; level to upload (255 = nobody)
 *   acs      = FA         ; optional extra condition to see/download (acs.c)
 *   upload_acs =           ; optional extra condition to upload
 *   cdrom    = no          ; yes: browse the disc in the drive, drawers and all (implies readonly)
 *   readonly = no          ; yes: a CD/DVD (CD0:...) - download only; nothing is written
 *                          ; to it (descriptions + DIZ cache go to BBS:Data/CD/<tag>)
 *   subop    = @Sysop     ; who looks after it (may delete files, write DIZs)
 *   conf     = MAIN       ; conference(s) it belongs to
 *   free     = yes        ; downloads here don't count against the ratio
 *   parent   = AMIGA      ; a sub-area of [AMIGA] (shown under it, listed with it)
 * An area without a path is just a heading for its sub-areas.
 * Descriptions live in files.bbs in each area's directory, the classic
 * format:  FILENAME.EXT  description   (indented lines continue it).
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

#include "node.h"
#include "zmodem.h"

struct FileArea {
    char  tag[NAMELEN];
    char  name[LONGNAME];
    char  path[PATHLEN];
    UBYTE dlevel, ulevel;
    UBYTE free;
    UBYTE depth;         /* 0 = top level, 1 = sub-area, 2 = sub-sub-area */
    char  acs[64], uacs[64], subop[64], conf[48];
    char  parent[NAMELEN];
    UBYTE ro;            /* a CD/DVD: never written to */
    UBYTE cdrom;         /* browse the disc in the drive (L), drawers and all */
};

static struct FileArea fareas[MAX_FILEAREAS];
static int nfareas;
static UBYTE order[MAX_FILEAREAS];      /* display order: each parent, then its sub-areas */
static int norder;

#define HAS_FILES(a) ((a)->path[0] != 0)

/* may see and download: level, ACS, and it belongs to this conference */
static BOOL farea_open(struct FileArea *a)
{
    return a->dlevel <= N.user.level && acs_check(a->acs) && conf_visible(a->conf);
}

static BOOL farea_upload(struct FileArea *a)
{
    return HAS_FILES(a) && !a->ro && a->ulevel <= N.user.level && acs_check(a->uacs) && conf_visible(a->conf);
}

/* say so and TRUE if the area is a read-only disc */
static BOOL ro_refuse(struct FileArea *a)
{
    if (!a->ro) return FALSE;
    tputs(L("file.ro_refuse.this_area_is", "|12This area is on a read-only disc (CD/DVD) - nothing on it can be changed.|07\n"));
    return TRUE;
}

static BOOL farea_subop(struct FileArea *a)
{
    return N.sysop || (a->subop[0] && acs_check(a->subop));
}

static void order_areas(void);
static void cd_browse(struct FileArea *a);

void file_areas_load(void)
{
    struct Cfg *c = cfg_load("BBS:Config/FileAreas.cfg");
    LONG i, n = cfg_sections(c);
    nfareas = 0;
    for (i = 0; i < n && nfareas < MAX_FILEAREAS; i++) {
        const char *tag = cfg_section(c, i);
        struct FileArea *a = &fareas[nfareas];
        memset(a, 0, sizeof(*a));
        str_copy(a->tag, tag, NAMELEN);
        str_copy(a->name, cfg_sget(c, tag, "name", tag), LONGNAME);
        str_copy(a->path, cfg_sget(c, tag, "path", ""), PATHLEN);
        str_copy(a->parent, cfg_sget(c, tag, "parent", ""), NAMELEN);
        a->dlevel = (UBYTE)cfg_sint(c, tag, "download", 10);
        a->ulevel = (UBYTE)cfg_sint(c, tag, "upload", 10);
        a->free   = (UBYTE)cfg_sbool(c, tag, "free", FALSE);
        str_copy(a->acs, cfg_sget(c, tag, "acs", ""), sizeof(a->acs));
        str_copy(a->uacs, cfg_sget(c, tag, "upload_acs", ""), sizeof(a->uacs));
        str_copy(a->subop, cfg_sget(c, tag, "subop", ""), sizeof(a->subop));
        str_copy(a->conf, cfg_sget(c, tag, "conf", ""), sizeof(a->conf));
        a->cdrom = (UBYTE)cfg_sbool(c, tag, "cdrom", FALSE);
        a->ro = (UBYTE)(cfg_sbool(c, tag, "readonly", FALSE) || a->cdrom);
        if (a->ro) area_readonly(a->path, a->tag);
        nfareas++;
    }
    cfg_free(c);
    if (!nfareas) {
        strcpy(fareas[0].tag, "UPLOADS");
        strcpy(fareas[0].name, "Uploads");
        strcpy(fareas[0].path, "BBS:Files/Uploads");
        fareas[0].dlevel = fareas[0].ulevel = 10;
        nfareas = 1;
    }
    order_areas();
    N.cur_filearea = (UBYTE)nfareas;           /* none open: an invalid index */
    for (i = 0; i < norder; i++)
        if (farea_open(&fareas[order[i]]) && HAS_FILES(&fareas[order[i]])) { N.cur_filearea = order[i]; break; }
}

/* the areas whose parent is `tag`, in config order, each followed by its own */
static void order_add(int i, int depth, UBYTE *seen)
{
    int j;
    if (seen[i] || norder >= MAX_FILEAREAS) return;
    seen[i] = 1;
    fareas[i].depth = (UBYTE)depth;
    order[norder++] = (UBYTE)i;
    if (depth >= 2) return;
    for (j = 0; j < nfareas; j++)
        if (!str_icmp(fareas[j].parent, fareas[i].tag)) order_add(j, depth + 1, seen);
}

static int find_tag(const char *tag)
{
    int i;
    for (i = 0; i < nfareas; i++) if (!str_icmp(fareas[i].tag, tag)) return i;
    return -1;
}

static void order_areas(void)
{
    UBYTE seen[MAX_FILEAREAS];
    int i;
    memset(seen, 0, sizeof(seen));
    norder = 0;
    /* a missing or looping parent makes the area a top-level one */
    for (i = 0; i < nfareas; i++)
        if (!fareas[i].parent[0] || find_tag(fareas[i].parent) < 0 || !str_icmp(fareas[i].parent, fareas[i].tag))
            order_add(i, 0, seen);
    for (i = 0; i < nfareas; i++) if (!seen[i]) order_add(i, 0, seen);
    /* an area with no path and nothing under it is useless: give it the default */
    for (i = 0; i < nfareas; i++) {
        int j, kids = 0;
        for (j = 0; j < nfareas; j++) if (!str_icmp(fareas[j].parent, fareas[i].tag) && j != i) kids++;
        if (!HAS_FILES(&fareas[i]) && !kids) str_copy(fareas[i].path, "BBS:Files/Uploads", PATHLEN);
    }
}

/* index in `order` of area i */
static int order_pos(int i)
{
    int k;
    for (k = 0; k < norder; k++) if (order[k] == i) return k;
    return -1;
}

/* "Amiga Software / Games" */
static const char *farea_title(int i)
{
    static char t[LONGNAME * 2 + 8];
    int p = fareas[i].parent[0] ? find_tag(fareas[i].parent) : -1;
    if (p >= 0 && p != i && fareas[i].depth) {
        sprintf(t, "%.40s / %.40s", fareas[p].name, fareas[i].name);
        return t;
    }
    return fareas[i].name;
}

const char *file_area_name(void)
{
    return N.cur_filearea < nfareas ? fareas[N.cur_filearea].name : "";
}

/* ---- directory + descriptions ------------------------------------------------ */

struct FEntry {
    char  name[32];
    LONG  size;
    ULONG date;
    struct DateStamp ds; /* the file's full date (the DIZ cache check wants the ticks too) */
    char *desc;          /* points into the description buffer, may be NULL */
    char *diz;           /* the archive's own FILE_ID.DIZ (AllocVec'd), wins over desc */
    BOOL  dizdone;       /* diz fetched yet?  (only for entries actually shown) */
};

struct FList {
    struct FEntry *e;
    LONG n, max;
    char *descbuf;       /* files.bbs, loaded whole */
    const char *dir;     /* the area's path, for the DIZ cache */
};

static void flist_free(struct FList *fl)
{
    LONG i;
    for (i = 0; fl->e && i < fl->n; i++) if (fl->e[i].diz) FreeVec(fl->e[i].diz);
    if (fl->e) FreeVec(fl->e);
    if (fl->descbuf) FreeVec(fl->descbuf);
    memset(fl, 0, sizeof(*fl));
}

/* an archive's own FILE_ID.DIZ (cached per area) - read the first time an entry
   is shown, not for every archive in the area at load time */
static const char *entry_diz(struct FList *fl, struct FEntry *e)
{
    if (!e->dizdone) {
        char dz[DIZ_MAX];
        e->dizdone = TRUE;
        if (fl->dir && diz_is_archive(e->name) && diz_get_at(fl->dir, e->name, &e->ds, dz, sizeof(dz)) &&
            (e->diz = AllocVec(strlen(dz) + 1, 0)))
            strcpy(e->diz, dz);
    }
    return e->diz;
}

/* find the files.bbs description for a name: a pointer to its first line
 * inside descbuf (continuation lines follow it, indented) */
static char *find_desc(char *buf, const char *name)
{
    char *p = buf;
    LONG nl = strlen(name);
    while (p && *p) {
        char *e;
        if (!str_nicmp(p, name, nl) && (p[nl] == ' ' || p[nl] == '\t')) {
            char *d = p + nl;
            while (*d == ' ' || *d == '\t') d++;
            return d;
        }
        e = strchr(p, '\n');
        p = e ? e + 1 : NULL;
    }
    return NULL;
}

/* files.bbs indexed in one pass (find_desc per file was O(files x lines)).
 * Every line whose first word is followed by a space or tab is a key; a lookup
 * gives exactly what find_desc would: the earliest such line, any case. */
struct DescKey {
    char *key;           /* line start in descbuf */
    char *desc;          /* where find_desc's answer starts */
    LONG  len;
    LONG  next;          /* bucket chain, -1 = end */
};
struct DescIdx {
    struct DescKey *k;
    LONG *bucket;
    ULONG mask;
    char *buf;
};

static ULONG desc_hash(const char *s, LONG n)
{
    ULONG h = 0;
    while (n-- > 0) {
        UBYTE c = (UBYTE)*s++;
        if (c >= 'A' && c <= 'Z') c += 32;      /* the same folding as str_nicmp */
        h = h * 31 + c;
    }
    return h;
}

static void desc_index_free(struct DescIdx *x)
{
    if (x->k) FreeVec(x->k);
    x->k = NULL;
    x->bucket = NULL;
}

/* no buffer or no memory leaves x->k NULL: desc_lookup then falls back to the
   linear find_desc */
static void desc_index(struct DescIdx *x, char *buf)
{
    char *p;
    LONG lines = 1, n = 0, i;
    ULONG size = 16;
    x->k = NULL; x->bucket = NULL; x->mask = 0; x->buf = buf;
    if (!buf) return;
    for (p = buf; *p; p++) if (*p == '\n') lines++;
    while (size < (ULONG)lines) size <<= 1;
    if (!(x->k = AllocVec(lines * sizeof(struct DescKey) + size * sizeof(LONG), 0))) return;
    x->bucket = (LONG *)(x->k + lines);
    x->mask = size - 1;
    for (i = 0; i < (LONG)size; i++) x->bucket[i] = -1;
    p = buf;
    while (p && *p && n < lines) {
        char *t = p, *e;
        while (*t && *t != ' ' && *t != '\t' && *t != '\n') t++;
        if (t > p && (*t == ' ' || *t == '\t')) {
            struct DescKey *k = &x->k[n];
            ULONG h = desc_hash(p, t - p) & x->mask;
            k->key = p;
            k->len = t - p;
            while (*t == ' ' || *t == '\t') t++;
            k->desc = t;
            k->next = x->bucket[h];
            x->bucket[h] = n++;
        }
        e = strchr(p, '\n');
        p = e ? e + 1 : NULL;
    }
}

static char *desc_lookup(struct DescIdx *x, const char *name)
{
    LONG nl = strlen(name), i;
    struct DescKey *best = NULL;
    if (!x->buf) return NULL;
    /* a name with a blank in it can match across words: keep the old scan for it */
    if (!x->k || !nl || strpbrk(name, " \t\n")) return find_desc(x->buf, name);
    for (i = x->bucket[desc_hash(name, nl) & x->mask]; i >= 0; i = x->k[i].next) {
        struct DescKey *k = &x->k[i];
        if (k->len == nl && !str_nicmp(k->key, name, nl) && (!best || k->key < best->key)) best = k;
    }
    return best ? best->desc : NULL;
}


/* FILE_ID.DIZ handling lives in diz.c */

/* append a (possibly multi-line) description to an area's files.bbs */
static void add_description(struct FileArea *a, const char *name, const char *desc)
{
    char path[PATHLEN], line[160];
    const char *p = desc;
    BPTR fh;
    BOOL first = TRUE;
    sprintf(path, "%s/files.bbs", area_meta(a->path));
    ObtainSemaphore(&N.S->filelock);
    if ((fh = Open((STRPTR)path, MODE_READWRITE))) {
        Seek(fh, 0, OFFSET_END);
        while (*p || first) {
            const char *e = strchr(p, '\n');
            LONG len = e ? e - p : (LONG)strlen(p);
            if (len > 120) len = 120;
            if (first) sprintf(line, "%-12s %.*s\n", name, (int)len, p);
            else sprintf(line, "             %.*s\n", (int)len, p);
            FPuts(fh, (STRPTR)line);
            first = FALSE;
            if (!e) break;
            p = e + 1;
        }
        Close(fh);
    }
    ReleaseSemaphore(&N.S->filelock);
}
/* qsort: newest first; equal dates keep their directory order (the pointers
   point into the unsorted array, so the lower address came first) */
static int fentry_newer(const void *x, const void *y)
{
    const struct FEntry *a = *(const struct FEntry * const *)x, *b = *(const struct FEntry * const *)y;
    if (a->date != b->date) return a->date > b->date ? -1 : 1;
    return a < b ? -1 : a > b ? 1 : 0;
}

static BOOL flist_load(struct FileArea *a, struct FList *fl)
{
    struct FileInfoBlock *fib;
    struct DescIdx idx;
    char path[PATHLEN];
    BPTR lock;
    LONG sz;

    memset(fl, 0, sizeof(*fl));
    {
        /* the BBS's own files.bbs; a disc's printed one after it (the first match wins) */
        char disc[PATHLEN];
        LONG sz2 = 0, n = 0, r;
        sprintf(path, "%s/files.bbs", area_meta(a->path));
        sprintf(disc, "%s/files.bbs", a->path);
        if ((sz = file_size(path)) < 0) sz = 0;
        if (a->ro && (sz2 = file_size(disc)) < 0) sz2 = 0;
        if (sz + sz2 > 0 && (fl->descbuf = AllocVec(sz + sz2 + 2, 0))) {
            BPTR fh;
            if (sz && (fh = Open((STRPTR)path, MODE_OLDFILE))) { if ((r = Read(fh, fl->descbuf, sz)) > 0) n = r; Close(fh); }
            if (n && fl->descbuf[n - 1] != '\n') fl->descbuf[n++] = '\n';
            if (sz2 && (fh = Open((STRPTR)disc, MODE_OLDFILE))) { if ((r = Read(fh, fl->descbuf + n, sz2)) > 0) n += r; Close(fh); }
            fl->descbuf[n] = 0;
        }
    }
    if (!(lock = Lock((STRPTR)a->path, ACCESS_READ))) { flist_free(fl); return FALSE; }
    if (!(fib = AllocDosObject(DOS_FIB, NULL))) { UnLock(lock); flist_free(fl); return FALSE; }
    fl->dir = a->path;
    desc_index(&idx, fl->descbuf);
    fl->max = 64;
    fl->e = AllocVec(fl->max * sizeof(struct FEntry), MEMF_CLEAR);
    if (fl->e && Examine(lock, fib)) {
        while (ExNext(lock, fib)) {
            struct FEntry *e;
            if (fib->fib_DirEntryType > 0) continue;
            {
                const char *fn = (const char *)fib->fib_FileName;
                LONG fl_len = strlen(fn);
                if (!str_nicmp(fn, "files.bbs", 9) || fn[0] == '.' ||
                    (fl_len > 5 && !str_icmp(fn + fl_len - 5, ".info")))
                    continue;
            }
            if (fl->n == fl->max) {
                struct FEntry *ne = AllocVec(fl->max * 2 * sizeof(struct FEntry), MEMF_CLEAR);
                if (!ne) break;
                memcpy(ne, fl->e, fl->max * sizeof(struct FEntry));
                FreeVec(fl->e);
                fl->e = ne;
                fl->max *= 2;
            }
            e = &fl->e[fl->n++];
            str_copy(e->name, fib->fib_FileName, sizeof(e->name));
            e->size = fib->fib_Size;
            e->date = fib->fib_Date.ds_Days * 86400UL + fib->fib_Date.ds_Minute * 60UL;
            e->ds = fib->fib_Date;
            e->desc = desc_lookup(&idx, e->name);
        }
    }
    FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    desc_index_free(&idx);

    /* an archive's own FILE_ID.DIZ is its description: entry_diz() reads it
       when the entry is shown */
    /* newest first (ties keep directory order, as the old insertion sort did) */
    if (fl->n > 1) {
        struct FEntry **pp = AllocVec(fl->n * sizeof(struct FEntry *), 0);
        struct FEntry *ne = pp ? AllocVec(fl->max * sizeof(struct FEntry), MEMF_CLEAR) : NULL;
        LONG i, j;
        if (ne) {
            for (i = 0; i < fl->n; i++) pp[i] = &fl->e[i];
            qsort(pp, fl->n, sizeof(pp[0]), fentry_newer);
            for (i = 0; i < fl->n; i++) ne[i] = *pp[i];
            FreeVec(fl->e);
            fl->e = ne;
        } else {
            for (i = 1; i < fl->n; i++) {           /* no memory: sort in place */
                struct FEntry t = fl->e[i];
                for (j = i; j > 0 && fl->e[j - 1].date < t.date; j--) fl->e[j] = fl->e[j - 1];
                fl->e[j] = t;
            }
        }
        if (pp) FreeVec(pp);
    }
    return TRUE;
}

/* classic layout: 14-char name, size, date, then the description at column
 * 32 - so a full 45-column FILE_ID.DIZ fits on an 80-column screen.  Longer
 * names get a line of their own. */
#define DESC_COL 31

static void show_entry(struct FList *fl, struct FEntry *e)
{
    char d[16], sz[12];
    const char *diz = entry_diz(fl, e);
    const char *desc = diz ? diz : e->desc;
    int lines = 0;
    LONG room = (LONG)N.cols - DESC_COL - 1;
    bbs_datestr(e->date, d);
    if (e->size >= 10L * 1024 * 1024) sprintf(sz, "%ldM", e->size / (1024 * 1024));
    else if (e->size >= 10000) sprintf(sz, "%ldK", e->size / 1024);
    else sprintf(sz, "%ld", e->size);
    if (strlen(e->name) > 14)
        tprintf("|15%s\n%-14s |11%5s |08%s ", e->name, "", sz, d);
    else
        tprintf("|15%-14s |11%5s |08%s ", e->name, sz, d);
    if (!desc) { tputs(L("file.show_entry.no_description", "|08(no description)|07\n")); return; }
    tputs(diz ? "|07" : "|03");
    for (;;) {
        const char *nl = strchr(desc, '\n');
        LONG len = nl ? nl - desc : (LONG)strlen(desc);
        if (len > 0 && desc[len - 1] == '\r') len--;
        if (lines) tprintf("%*s", DESC_COL, "");
        if (len > room) len = room;
        if (len > 0) tputraw((const UBYTE *)desc, len, CS_CP437);
        tputs("\n");
        if (!nl || ++lines >= DIZ_READ_H) break;
        desc = nl + 1;
        if (!diz) {
            /* files.bbs: only indented lines continue the entry */
            if (!(*desc == ' ' || *desc == '\t')) break;
            while (*desc == ' ' || *desc == '\t' || *desc == '|' || *desc == '+') desc++;
        }
    }
    tputs("|07");
}
/* ---- listing -------------------------------------------------------------------- */

void file_list(BOOL newonly)
{
    /* one area and its sub-areas (they follow it in `order`), or everything */
    int first = newonly ? 0 : order_pos(N.cur_filearea);
    int last = first, k, i;
    LONG shown = 0;
    ULONG since = N.user.lastfscan ? N.user.lastfscan : N.prevcall;
    ULONG newest = N.user.lastfscan;

    if (first < 0) { tputs(L("file.list.pick_file_area", "\n|08Pick a file area first.|07\n")); return; }
    if (!newonly && fareas[order[first]].cdrom) { cd_browse(&fareas[order[first]]); return; }
    if (newonly) last = norder - 1;
    else while (last + 1 < norder && fareas[order[last + 1]].depth > fareas[order[first]].depth) last++;
    set_activity("Listing files");
    tpage_start();
    for (k = first; k <= last && tmore(); k++) {
        struct FList fl;
        LONG j, areashown = 0;
        i = order[k];
        if (!farea_open(&fareas[i]) || !HAS_FILES(&fareas[i])) continue;
        if (fareas[i].cdrom) {
            if (!newonly) tprintf(L("file.list.cd_rom_pick", "\n|09-=[ |15%s|09 ]=-|07\n\n  |08A CD-ROM - pick it (A) and L to browse the disc.|07\n"), farea_title(i));
            continue;
        }
        if (!flist_load(&fareas[i], &fl)) {
            if (fareas[i].ro && !newonly)
                tprintf(L("file.list.the_disc_for", "\n|09-=[ |15%s|09 ]=-|07\n\n  |08The disc for this area isn't in the drive.|07\n"), farea_title(i));
            continue;
        }
        for (j = 0; j < fl.n && tmore(); j++) {
            if (newonly && fl.e[j].date <= since) continue;
            if (!areashown++) tprintf("\n|09-=[ |15%s|09 ]=-|07\n\n", farea_title(i));
            show_entry(&fl, &fl.e[j]);
            if (fl.e[j].date > newest) newest = fl.e[j].date;
            shown++;
        }
        flist_free(&fl);
    }
    tpage_end();
    if (!shown) tputs(newonly ? L("file.list.no_new_files", "\n|08No new files since your last scan.|07\n") :
                      last > first ? L("file.list.nothing_here_yet", "\n|08Nothing here yet, in this area or under it.|07\n") : L("file.list.this_area_is", "\n|08This area is empty.|07\n"));
    if (newonly && newest > N.user.lastfscan) { N.user.lastfscan = newest; user_save(); }
    back_to_menu();
}

/* the area list as a tree; returns the chosen area index, -1 = none.
   only_files: leave out headings that hold no files (for moving files) */
static int pick_area(const char *prompt, BOOL only_files)
{
    int k, shown = 0;
    char b[6];
    for (k = 0; k < norder; k++) {
        struct FileArea *a = &fareas[order[k]];
        int kids = 0, j;
        if (!farea_open(a)) continue;
        for (j = k + 1; j < norder && fareas[order[j]].depth > a->depth; j++) kids++;
        if (only_files && !HAS_FILES(a)) {
            tprintf("  %*s    |08%s|07\n", a->depth * 4, "", a->name);
            continue;
        }
        /* a sub-area: the whole line moves in, number and all */
        tprintf("  %*s|%s%2d|08) |07%s", a->depth * 4, "", order[k] == N.cur_filearea ? "14" : "15", k + 1,
                a->name);
        if (kids && !only_files) tprintf(kids == 1 ? L("file.pick_area.subareas_one", " |08(+%d sub-area)") : L("file.pick_area.subareas_many", " |08(+%d sub-areas)"), kids);
        tputs("|07\n");
        shown++;
    }
    if (!shown) { tputs(L("file.pick_area.no_file_areas", "  |08No file areas available.|07\n")); return -1; }
    tprintf("\n|07%s: |15", prompt);
    if (tgetline(b, 4, GL_DIGITS) <= 0) return -1;
    k = atoi(b) - 1;
    if (k < 0 || k >= norder || !farea_open(&fareas[order[k]])) return -1;
    if (only_files && !HAS_FILES(&fareas[order[k]])) return -1;
    return order[k];
}

void file_select_area(void)
{
    int i;
    tputs(L("file.select_area.file_areas", "\n|09-=[ |15File Areas|09 ]=-|07\n\n"));
    if ((i = pick_area(L("file.select_area.prompt", "Area number"), FALSE)) < 0) return;
    N.cur_filearea = (UBYTE)i;
    back_to_menu();
    tprintf(L("file.select_area.now_in", "|07Now in |15%s|07.%s\n"), farea_title(i),
            HAS_FILES(&fareas[i]) ? "" : L("file.select_area.lists_everything_under", " |08(L lists everything under it)|07"));
}

void file_search(void)
{
    char pat[48], parsed[100], word[40];
    int i, k;
    LONG hits = 0;
    tputs(L("file.search.search_for_wildcards", "\n|07Search for |08(wildcards ok, e.g. #?.lha)|07: |15"));
    if (tgetline(pat, 38, 0) <= 0) return;
    word[0] = 0;
    if (!strchr(pat, '#') && !strchr(pat, '?') && !strchr(pat, '*')) {
        str_copy(word, pat, sizeof(word));      /* also look in descriptions */
        /* plain word: match it anywhere in the name */
        char tmp[48];
        sprintf(tmp, "#?%s#?", pat);
        strcpy(pat, tmp);
    }
    if (ParsePatternNoCase((STRPTR)pat, (STRPTR)parsed, sizeof(parsed)) < 0) return;
    tpage_start();
    for (k = 0; k < norder && tmore(); k++) {
        struct FList fl;
        LONG j;
        i = order[k];
        if (!farea_open(&fareas[i]) || !HAS_FILES(&fareas[i]) || fareas[i].cdrom || !flist_load(&fareas[i], &fl)) continue;
        for (j = 0; j < fl.n && tmore(); j++) {
            BOOL match = MatchPatternNoCase((STRPTR)parsed, (STRPTR)fl.e[j].name);
            if (!match && word[0] && entry_diz(&fl, &fl.e[j])) match = str_istr(fl.e[j].diz, word) != NULL;
            if (!match && word[0] && fl.e[j].desc) {
                /* plain word: also search the first line of the description */
                char *nl = strchr(fl.e[j].desc, '\n');
                char save = 0;
                if (nl) { save = *nl; *nl = 0; }
                match = str_istr(fl.e[j].desc, word) != NULL;
                if (nl) *nl = save;
            }
            if (!match) continue;
            if (!hits) tputs("\n");
            tprintf("|08%-12.12s ", fareas[i].name);
            show_entry(&fl, &fl.e[j]);
            hits++;
        }
        flist_free(&fl);
    }
    tpage_end();
    if (!hits) tputs(L("file.search.nothing_found", "|08Nothing found.|07\n"));
}

/* ---- transfers ---------------------------------------------------------------------- */

#define MAXBATCH 10

/* ---- ratios: NilBBS.cfg ratio = N means N KB down per KB up ---------------------
 *   ratio         = 0       ; 0 = no ratio
 *   ratio_free_kb = 1024    ; what a new caller may download before uploading
 *   ratio_exempt  = L100    ; ACS of callers the ratio doesn't apply to
 * The sysop can also grant credits (extra KB) per user.  -1 = unlimited. */
LONG ratio_allowed_kb(void)
{
    LONG ratio = cfg_int(N.cfg, "ratio", 0), left;
    const char *ex = cfg_str(N.cfg, "ratio_exempt", "");
    if (ratio <= 0) return -1;
    user_refresh();                         /* the sysop may just have granted credits */
    if (N.sysop || (ex[0] && acs_check(ex))) return -1;
    left = cfg_int(N.cfg, "ratio_free_kb", 1024) + (LONG)N.user.ulkb * ratio +
           N.user.credits - (LONG)N.user.dlkb;
    return left > 0 ? left : 0;
}

void file_stats(void)
{
    LONG left = ratio_allowed_kb();
    tputs(L("file.stats.your_transfers", "\n|09-=[ |15Your Transfers|09 ]=-|07\n\n"));
    tprintf(L("file.stats.uploads_files_kb", "  |07Uploads     |15%lu|07 files, |15%lu|07 KB\n"), N.user.uploads, N.user.ulkb);
    tprintf(L("file.stats.downloads_files_kb", "  |07Downloads   |15%lu|07 files, |15%lu|07 KB\n"), N.user.downloads, N.user.dlkb);
    if (N.user.credits) tprintf(L("file.stats.credits_kb", "  |07Credits     |15%ld|07 KB\n"), N.user.credits);
    if (left < 0) tputs(L("file.stats.ratio_none_download", "  |07Ratio       |15none|07 - download as much as you like\n"));
    else {
        tprintf(L("file.stats.ratio_1_kb", "  |07Ratio       |15%ld:1|07 (%ld KB down for every KB up)\n"),
                cfg_int(N.cfg, "ratio", 0), cfg_int(N.cfg, "ratio", 0));
        tprintf(L("file.stats.you_may_download", "  |07You may download |15%ld|07 KB more.\n"), left);
    }
}

/* send a batch with the caller's protocol and keep the books (downloads, KB against the ratio) */
static void send_batch(const char **pp, const char **np, LONG count, ULONG bytes, BOOL allfree)
{
    struct ZStats st;
    int proto = N.user.proto <= PROTO_MAX ? N.user.proto : PROTO_Z;
    LONG i, sent;
    if (!allfree) {
        LONG left = ratio_allowed_kb();
        if (left >= 0 && (LONG)((bytes + 1023) / 1024) > left) {
            tprintf(L("file.send_batch.thats_kb_but", "\n|12That's %lu KB, but your ratio allows %ld KB more.|07\n"
                    "|07Upload something to earn more - |15%ld|07 KB down for every KB up.\n"),
                    (bytes + 1023) / 1024, left, cfg_int(N.cfg, "ratio", 0));
            return;
        }
    }
    tprintf(count == 1 ? L("file.send_batch.files_one", "\n|07%ld file, |15%lu|07 bytes:") : L("file.send_batch.files_many", "\n|07%ld files, |15%lu|07 bytes:"), count, bytes);
    for (i = 0; i < count; i++) tprintf(" |11%s", np[i]);
    if (proto == PROTO_X || proto == PROTO_X1K) {
        if (count > 1) tprintf(L("file.send_batch.sends_one_file", "\n|14%s sends one file at a time - just %s this time.|07"), proto_name(proto), np[0]);
        count = 1;
    }
    tprintf(L("file.send_batch.start_your_download", "|07\n\n|14Start your %s download now.|07 |08(Ctrl-X a few times to abort)|07\n"), proto_name(proto));
    tn_flush();
    set_activity("Downloading");
    sent = proto == PROTO_Z ? zm_send(pp, np, count, &st) : xy_send(pp, np, count, proto, &st);
    tputs("\r\n");
    if (sent > 0) {
        N.user.downloads += sent;
        if (!allfree) N.user.dlkb += st.total / 1024;     /* free areas don't count */
        user_save();
        tprintf(sent == 1 ? L("file.send_batch.sent_one", "|10%ld file sent.|07\n") : L("file.send_batch.sent_many", "|10%ld files sent.|07\n"), sent);
        for (i = 0; i < count; i++)
            if (st.ok_mask & (1UL << (i & 31)))
                bbs_log(BBS_SYSLOG, "node %d: %s downloaded %s", N.node, N.user.name, pp[i]);
    } else tputs(L("file.send_batch.transfer_failed_or", "|12Transfer failed or was cancelled.|07\n"));
}

void file_download(void)
{
    struct FileArea *a = &fareas[N.cur_filearea];
    char line[160], *f[MAXBATCH];
    char paths[MAXBATCH][PATHLEN], names[MAXBATCH][32];
    const char *pp[MAXBATCH], *np[MAXBATCH];
    LONG n, i, count = 0;
    ULONG bytes = 0;

    if (N.local) { tputs(L("file.download.file_transfers_need", "|08File transfers need a telnet session - at the console, just copy the file.|07\n")); return; }
    if (N.cur_filearea >= nfareas || !farea_open(a)) { tputs(L("file.download.you_cant_download", "|12You can't download from this area.|07\n")); return; }
    if (a->cdrom) { cd_browse(a); return; }                 /* pick files on the disc */
    BOOL allfree = TRUE;
    tputs(L("file.download.file_name_to", "\n|07File name(s) to download |08(space separated, wildcards ok)|07:\n|15> "));
    if (tgetline(line, sizeof(line), 0) <= 0) return;
    n = str_split(line, ' ', f, MAXBATCH);

    for (i = 0; i < n && count < MAXBATCH; i++) {
        char parsed[100];
        struct FList fl;
        LONG j;
        if (!f[i][0]) continue;
        if (ParsePatternNoCase((STRPTR)f[i], (STRPTR)parsed, sizeof(parsed)) < 0) continue;
        {
            /* this area and the sub-areas under it */
            int k0 = order_pos(N.cur_filearea), k;
            for (k = k0; k >= 0 && k < norder && count < MAXBATCH; k++) {
                struct FileArea *s = &fareas[order[k]];
                if (k > k0 && s->depth <= a->depth) break;
                if (!farea_open(s) || !HAS_FILES(s) || !flist_load(s, &fl)) continue;
                for (j = 0; j < fl.n && count < MAXBATCH; j++) {
                    if (!MatchPatternNoCase((STRPTR)parsed, (STRPTR)fl.e[j].name)) continue;
                    sprintf(paths[count], "%s/%s", s->path, fl.e[j].name);
                    str_copy(names[count], fl.e[j].name, 32);
                    pp[count] = paths[count];
                    np[count] = names[count];
                    bytes += fl.e[j].size;
                    if (!s->free) allfree = FALSE;
                    count++;
                }
                flist_free(&fl);
            }
        }
    }
    if (!count) { tputs(L("file.download.no_matching_files", "|12No matching files in this area.|07\n")); return; }
    send_batch(pp, np, count, bytes, allfree);
    back_to_menu();
}

/* ==== the CD-ROM browser =============================================================
 * cdrom = yes: L (or D) in the area opens the disc in the drive - its name, then its
 * drawers and files, fifteen to a page.  A number picks one: a file shows its DIZ or
 * readme and downloads; a drawer opens, or goes down as one .lha packed in T:.  U goes
 * up (never above the area's path).  Swap the disc and the browser starts at the top of
 * the new one.  Descriptions are read only for the page on screen, and cached per disc
 * in BBS:Data/CD/<tag>/<disc name>_<fingerprint of its top drawer>.
 *   NilBBS.cfg: cd_pack_max_kb = 30720   ; the biggest drawer packed for a download
 */
#define CD_MAX  500
#define CD_PAGE 15

struct CdEnt { char name[108]; LONG size; ULONG date; UBYTE dir, readme; char desc[48]; UBYTE got; };

/* the disc in the drive: its volume name (vol, to show) and who it is (id = name_<8 hex>, a hash of
 * the names and sizes at the top of the area's path).  The name alone isn't enough: an emulator's
 * folder-as-a-drive keeps the name its config gives it whatever disc is in, so a swap would look
 * like the same disc. */
static BOOL cd_disc(const char *path, char *vol, LONG max, char *id, LONG idmax)
{
    char buf[PATHLEN], *c;
    struct FileInfoBlock *fib;
    ULONG h = 2166136261UL;                 /* FNV-1a */
    BPTR l = Lock((STRPTR)path, ACCESS_READ);
    if (!l) return FALSE;
    buf[0] = 0;
    NameFromLock(l, (STRPTR)buf, sizeof(buf));
    if ((fib = AllocDosObject(DOS_FIB, NULL))) {
        if (Examine(l, fib)) {
            int k = 0;
            while (k++ < 64 && ExNext(l, fib)) {        /* directory order: stable for a given disc */
                const UBYTE *p;
                LONG sz = fib->fib_DirEntryType > 0 ? -1 : fib->fib_Size;
                int j;
                for (p = (const UBYTE *)fib->fib_FileName; *p; p++) { h ^= *p; h *= 16777619UL; }
                for (j = 0; j < 4; j++) { h ^= (UBYTE)(sz >> (8 * j)); h *= 16777619UL; }
            }
        }
        FreeDosObject(DOS_FIB, fib);
    }
    UnLock(l);
    if ((c = strchr(buf, ':'))) *c = 0;
    str_copy(vol, buf, max);
    if (id) { buf[40] = 0; sprintf(buf + strlen(buf), "_%08lx", (unsigned long)h); str_copy(id, buf, idmax); }
    return TRUE;
}

static int cd_cmp(const void *a, const void *b)
{
    const struct CdEnt *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return str_icmp(x->name, y->name);
}

/* the drawer's entries (icons out; an Aminet .readme is folded into its archive) */
static LONG cd_read(const char *dir, struct CdEnt *e)
{
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    BPTR l;
    LONG n = 0, i, j;
    if (!fib) return -1;
    if (!(l = Lock((STRPTR)dir, ACCESS_READ))) { FreeDosObject(DOS_FIB, fib); return -1; }
    if (Examine(l, fib))
        while (n < CD_MAX && ExNext(l, fib)) {
            const char *fn = (const char *)fib->fib_FileName;
            LONG len = strlen(fn);
            if (len > 5 && !str_icmp(fn + len - 5, ".info")) continue;
            memset(&e[n], 0, sizeof(e[n]));
            str_copy(e[n].name, fn, sizeof(e[n].name));
            e[n].dir = fib->fib_DirEntryType > 0;
            e[n].size = fib->fib_Size;
            e[n].date = fib->fib_Date.ds_Days * 86400UL + fib->fib_Date.ds_Minute * 60UL;
            n++;
        }
    UnLock(l);
    FreeDosObject(DOS_FIB, fib);
    /* X.readme next to X.lha: keep it with the archive, out of the list */
    for (i = 0; i < n; i++) {
        LONG li = strlen(e[i].name);
        if (e[i].dir || li <= 7 || str_icmp(e[i].name + li - 7, ".readme")) continue;
        for (j = 0; j < n; j++) {
            LONG lj = strlen(e[j].name);
            char *dot;
            if (j == i || e[j].dir || !diz_is_archive(e[j].name)) continue;
            dot = strrchr(e[j].name, '.');
            if (dot && dot - e[j].name == li - 7 && !str_nicmp(e[j].name, e[i].name, li - 7)) { e[j].readme = 1; e[i].name[0] = 0; break; }
            (void)lj;
        }
    }
    for (i = j = 0; i < n; i++) if (e[i].name[0]) e[j++] = e[i];
    n = j;
    qsort(e, n, sizeof(e[0]), cd_cmp);
    return n;
}

/* an Aminet readme's Short: line */
static BOOL cd_short(const char *dir, const char *name, char *out, LONG max)
{
    char path[PATHLEN], base[108], line[160], *dot;
    BPTR fh;
    BOOL got = FALSE;
    str_copy(base, name, sizeof(base));
    if ((dot = strrchr(base, '.'))) *dot = 0;
    strcat(base, ".readme");
    path_join(path, dir, base);
    if (!(fh = Open((STRPTR)path, MODE_OLDFILE))) return FALSE;
    while (!got && FGets(fh, (STRPTR)line, sizeof(line))) {
        if (str_nicmp(line, "Short:", 6)) continue;
        {
            char *s = line + 6, *e;
            while (*s == ' ' || *s == '\t') s++;
            if ((e = strpbrk(s, "\r\n"))) *e = 0;
            str_copy(out, s, max);
            got = TRUE;
        }
    }
    Close(fh);
    return got;
}

/* the first line of a DIZ with words in it */
static void cd_first_line(const char *diz, char *out, LONG max)
{
    const char *d = diz;
    out[0] = 0;
    while (*d) {
        const char *e = d;
        BOOL words = FALSE;
        while (*e && *e != '\n') { if ((*e >= 'A' && *e <= 'Z') || (*e >= 'a' && *e <= 'z')) words = TRUE; e++; }
        if (words) {
            LONG n = 0;
            while (*d == ' ') d++;
            while (d < e && n < max - 1) { out[n++] = (*d >= 32 && (UBYTE)*d < 127) ? *d : ' '; d++; }
            out[n] = 0;
            return;
        }
        d = *e ? e + 1 : e;
    }
}

static void cd_describe(const char *dir, struct CdEnt *e)
{
    static char diz[DIZ_MAX];
    if (e->got) return;
    e->got = 1;
    if (e->dir) return;
    if (e->readme && cd_short(dir, e->name, e->desc, sizeof(e->desc))) return;
    if (diz_is_archive(e->name) && diz_get(dir, e->name, diz, sizeof(diz))) cd_first_line(diz, e->desc, sizeof(e->desc));
}

/* a drawer's size in bytes (0 on error), stopping past a limit */
static ULONG cd_size(const char *dir, ULONG limit, LONG *files)
{
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    BPTR l;
    ULONG total = 0;
    if (!fib) return 0;
    if ((l = Lock((STRPTR)dir, ACCESS_READ))) {
        if (Examine(l, fib))
            while (total <= limit && ExNext(l, fib)) {
                if (fib->fib_DirEntryType > 0) {
                    char sub[PATHLEN];
                    path_join(sub, dir, (char *)fib->fib_FileName);
                    total += cd_size(sub, limit - total, files);
                } else { total += fib->fib_Size; (*files)++; }
            }
        UnLock(l);
    }
    FreeDosObject(DOS_FIB, fib);
    return total;
}

static void cd_up(char *cur, const char *root)
{
    char *s;
    if (!str_icmp(cur, root)) return;
    if ((s = strrchr(cur, '/'))) *s = 0;
    else if ((s = strchr(cur, ':'))) s[1] = 0;
    if (strlen(cur) < strlen(root)) strcpy(cur, root);
}

/* show a text file (a readme) a page at a time */
static void cd_view(const char *path)
{
    BPTR fh = Open((STRPTR)path, MODE_OLDFILE);
    char line[200];
    if (!fh) return;
    tputs("\n");
    tpage_start();
    while (tmore() && FGets(fh, (STRPTR)line, sizeof(line))) {
        char *e = strpbrk(line, "\r\n");
        if (e) *e = 0;
        tputraw((UBYTE *)line, strlen(line), CS_CP437);
        tputs("\n");
    }
    tpage_end();
    Close(fh);
}

static void cd_file(struct FileArea *a, const char *dir, struct CdEnt *e)
{
    char path[PATHLEN], rpath[PATHLEN];
    static char diz[DIZ_MAX];
    BOOL text, hasreadme = FALSE;
    LONG len = strlen(e->name);
    path_join(path, dir, e->name);
    text = str_istr(e->name, ".txt") || str_istr(e->name, "readme") || str_istr(e->name, "read_me") ||
           (len > 4 && (!str_icmp(e->name + len - 4, ".doc") || !str_icmp(e->name + len - 4, ".nfo") ||
                        !str_icmp(e->name + len - 4, ".diz")));
    if (e->readme) {
        char base[108], *dot;
        str_copy(base, e->name, sizeof(base));
        if ((dot = strrchr(base, '.'))) *dot = 0;
        strcat(base, ".readme");
        path_join(rpath, dir, base);
        hasreadme = TRUE;
    }
    for (;;) {
        LONG k;
        tprintf(L("file.cd_file.bytes", "\n|15%s|07  %ld bytes\n"), e->name, e->size);
        if (diz_is_archive(e->name) && diz_get(dir, e->name, diz, sizeof(diz))) {
            tputraw((UBYTE *)diz, strlen(diz), CS_CP437);
            tputs("\n");
        } else if (e->desc[0]) tprintf("|07%s\n", e->desc);
        tputs(L("file.cd_file.download", "\n  |08[|15D|08] |07Download"));
        if (hasreadme) tputs(L("file.cd_file.read_its_readme", "   |08[|15R|08] |07Read its readme"));
        else if (text && e->size <= 65536) tputs(L("file.cd_file.read_it", "   |08[|15R|08] |07Read it"));
        tputs(L("file.cd_file.back", "   |08[|15Q|08] |07Back: "));
        k = tgethot("DRQ\r");
        if (k == KEY_HANGUP) return;
        tprintf("%c\n", (int)(k == '\r' ? 'Q' : k));
        if (k == 'D') {
            const char *pp[1], *np[1];
            if (N.local) { tputs(L("file.cd_file.file_transfers_need", "|08File transfers need a telnet session.|07\n")); continue; }
            pp[0] = path; np[0] = e->name;
            send_batch(pp, np, 1, (ULONG)e->size, a->free);
            return;
        }
        if (k == 'R' && hasreadme) cd_view(rpath);
        else if (k == 'R' && text && e->size <= 65536) cd_view(path);
        else return;
    }
}

static void cd_drawer(struct FileArea *a, char *cur, struct CdEnt *e)
{
    char sub[PATHLEN];
    LONG k, files = 0;
    ULONG max = (ULONG)cfg_int(N.cfg, "cd_pack_max_kb", 30720) * 1024UL, size;
    path_join(sub, cur, e->name);
    tprintf(L("file.cd_drawer.drawer_open_it", "\n|15%s|07 - a drawer.\n\n  |08[|15O|08] |07Open it   |08[|15D|08] |07Download it all as one .lha   |08[|15Q|08] |07Back: "), e->name);
    k = tgethot("ODQ\r");
    if (k == KEY_HANGUP) return;
    tprintf("%c\n", (int)(k == '\r' ? 'O' : k));
    if (k == 'O' || k == '\r') { str_copy(cur, sub, PATHLEN); return; }
    if (k != 'D') return;
    if (N.local) { tputs(L("file.cd_drawer.file_transfers_need", "|08File transfers need a telnet session.|07\n")); return; }
    tputs(L("file.cd_drawer.measuring_the_drawer", "|07Measuring the drawer..."));
    tn_flush();
    size = cd_size(sub, max, &files);
    if (size > max) { tprintf(L("file.cd_drawer.over_kb_too", "\n|12Over %ld KB - too big to pack. Open it and take the files you want.|07\n"), (LONG)(max / 1024)); return; }
    tprintf(L("file.cd_drawer.files_kb_packing", " |15%ld|07 files, |15%lu|07 KB.\n|07Packing it (this can take a minute)..."), files, (size + 1023) / 1024);
    tn_flush();
    {
        char arc[64], nm[112], cmd[PATHLEN * 2 + 80], icon[PATHLEN], lname[112];
        BPTR out, l;
        const char *pp[1], *np[1];
        LONG asize;
        sprintf(arc, "T:NuzCD%d.lha", N.node);
        DeleteFile((STRPTR)arc);
        path_join(icon, cur, e->name);
        strcat(icon, ".info");
        l = Lock((STRPTR)icon, ACCESS_READ);
        if (l) UnLock(l);
        sprintf(cmd, "LhA >NIL: -r -x -q a \"%s\" \"%s\" \"%s\"%s%s%s", arc, cur, e->name,
                l ? " \"" : "", l ? e->name : "", l ? ".info\"" : "");
        out = Open((STRPTR)"NIL:", MODE_NEWFILE);
        SystemTags((STRPTR)cmd, SYS_Output, (ULONG)out, SYS_Input, 0UL, TAG_END);
        if (out) Close(out);
        asize = file_size(arc);
        if (asize <= 0) { tputs(L("file.cd_drawer.packing_failed", "\n|12Packing failed.|07\n")); bbs_log(BBS_SYSLOG, "node %d: CD pack failed: %s", N.node, cmd); return; }
        tprintf(L("file.cd_drawer.kb", " |15%ld|07 KB.\n"), (asize + 1023) / 1024);
        str_copy(nm, e->name, 100);
        sprintf(lname, "%s.lha", nm);
        pp[0] = arc; np[0] = lname;
        send_batch(pp, np, 1, (ULONG)asize, a->free);
        bbs_log(BBS_SYSLOG, "node %d: %s took the drawer %s as %s", N.node, N.user.name, sub, lname);
        DeleteFile((STRPTR)arc);
    }
}

static void cd_browse(struct FileArea *a)
{
    static char cur[PATHLEN], vol[64], did[64], seen_id[64];
    char sz[12];
    struct CdEnt *e;
    LONG n = -1, page = 0, i, k;
    int idx = (int)(a - fareas);
    if (!farea_open(a)) { tputs(L("file.cd_browse.you_cant_browse", "|12You can't browse this disc.|07\n")); return; }
    if (!(e = AllocVec(CD_MAX * sizeof(struct CdEnt), MEMF_CLEAR))) return;
    set_activity("Browsing the CD-ROM");
    str_copy(cur, a->path, PATHLEN);
    seen_id[0] = 0;
    for (;;) {
        if (!cd_disc(a->path, vol, sizeof(vol), did, sizeof(did))) {
            tprintf(L("file.cd_browse.no_disc_in", "\n|09-=[ |15%s|09 ]=-|07\n\n  |08No disc in the drive.|07\n"), farea_title(idx));
            break;
        }
        if (strcmp(did, seen_id)) {                        /* a new disc (or the first look) */
            if (seen_id[0]) tprintf(L("file.cd_browse.the_disc_has", "\n|14The disc has changed: now |15%s|14.|07\n"), vol);
            str_copy(seen_id, did, sizeof(seen_id));
            str_copy(cur, a->path, PATHLEN);
            area_disc(a->path, a->tag, did);        /* its own cache, even when the name repeats */
            n = -1;
        }
        if (n < 0) {
            n = cd_read(cur, e);
            page = 0;
            if (n < 0) { str_copy(cur, a->path, PATHLEN); n = cd_read(cur, e); if (n < 0) break; }
        }
        {   /* the path, its tail if it's long (the line stays inside 79 columns) */
            LONG room = 58 - (LONG)strlen(vol), cl = strlen(cur);
            if (room < 12) room = 12;
            if (cl > room) tprintf(L("file.cd_browse.cd_rom", "\n|09-=[ |15CD-ROM: %.24s|09 ]=-|07  |08...%s|07\n\n"), vol, cur + cl - room + 3);
            else tprintf(L("file.cd_browse.cd_rom_2", "\n|09-=[ |15CD-ROM: %.24s|09 ]=-|07  |08%s|07\n\n"), vol, cur);
        }
        if (!n) tputs(L("file.cd_browse.an_empty_drawer", "  |08(an empty drawer)|07\n"));
        for (i = page * CD_PAGE; i < n && i < (page + 1) * CD_PAGE; i++) {
            struct CdEnt *x = &e[i];
            if (x->dir) { tprintf(L("file.cd_browse.dir", "  |15%3ld|08) |11%-28.28s |08<DIR>|07\n"), i + 1, x->name); continue; }
            cd_describe(cur, x);
            if (x->size >= 10L * 1024 * 1024) sprintf(sz, "%ldM", x->size / (1024 * 1024));
            else if (x->size >= 10000) sprintf(sz, "%ldK", x->size / 1024);
            else sprintf(sz, "%ld", x->size);
            tprintf("  |15%3ld|08) |07%-24.24s |15%7s |08%-37.37s|07\n", i + 1, x->name, sz, x->desc);
        }
        tprintf(L("file.cd_browse.page_of_number", "\n|08  Page %ld of %ld.  |07Number |08= open/download   "), page + 1, (n + CD_PAGE - 1) / CD_PAGE ? (n + CD_PAGE - 1) / CD_PAGE : 1);
        if ((page + 1) * CD_PAGE < n) tputs(L("file.cd_browse.next", "|15N|08)ext  "));
        if (page) tputs(L("file.cd_browse.prev", "|15P|08)rev  "));
        if (str_icmp(cur, a->path)) tputs("|15U|08)p  ");
        tputs(L("file.cd_browse.quit_browse", "|15Q|08)uit|07\n|07Browse: |15"));
        {
            char b[8];
            LONG got = tgetline(b, 5, 0);
            if (got < 0 || !N.online) break;
            k = b[0] & 0xDF;
            if (b[0] >= '0' && b[0] <= '9') {
                LONG pick = atol(b) - 1;
                if (pick < 0 || pick >= n) continue;
                if (e[pick].dir) {
                    char was[PATHLEN];
                    str_copy(was, cur, PATHLEN);
                    cd_drawer(a, cur, &e[pick]);
                    if (str_icmp(was, cur)) n = -1;
                } else cd_file(a, cur, &e[pick]);
                continue;
            }
            if (!b[0] || k == 'Q') break;
            if (k == 'N' && (page + 1) * CD_PAGE < n) page++;
            else if (k == 'P' && page) page--;
            else if (k == 'U' || b[0] == '.') { cd_up(cur, a->path); n = -1; }
        }
    }
    FreeVec(e);
    back_to_menu();
}

/* ---- which uploads the sysop has looked at: <area>/.reviewed, one name per line ---- */

static BOOL is_reviewed(struct FileArea *a, const char *name)
{
    char path[PATHLEN], line[40];
    BPTR fh;
    BOOL yes = FALSE;
    sprintf(path, "%s/.reviewed", area_meta(a->path));
    if (!(fh = Open((STRPTR)path, MODE_OLDFILE))) return FALSE;
    while (!yes && FGets(fh, (STRPTR)line, sizeof(line))) {
        char *e = strpbrk(line, "\r\n");
        if (e) *e = 0;
        if (!str_icmp(line, name)) yes = TRUE;
    }
    Close(fh);
    return yes;
}

static void mark_reviewed(struct FileArea *a, const char *name)
{
    char path[PATHLEN];
    BPTR fh;
    if (is_reviewed(a, name)) return;
    sprintf(path, "%s/.reviewed", area_meta(a->path));
    ObtainSemaphore(&N.S->filelock);
    if ((fh = Open((STRPTR)path, MODE_READWRITE))) {
        Seek(fh, 0, OFFSET_END);
        FPuts(fh, (STRPTR)name);
        FPuts(fh, (STRPTR)"\n");
        Close(fh);
    }
    ReleaseSemaphore(&N.S->filelock);
}

static void upload_into(struct FileArea *a);

void file_upload(void)
{
    struct FileArea *a = &fareas[N.cur_filearea];
    LONG i;

    if (N.local) { tputs(L("file.upload.file_transfers_need", "|08File transfers need a telnet session - at the console, just copy the file.|07\n")); return; }
    if (N.cur_filearea >= nfareas || !farea_upload(a)) {
        /* fall back to the first area that takes uploads */
        a = NULL;
        for (i = 0; i < norder; i++) if (farea_upload(&fareas[order[i]])) { a = &fareas[order[i]]; break; }
        if (!a) { tputs(L("file.upload.you_cant_upload", "|12You can't upload here.|07\n")); return; }
        tprintf(L("file.upload.uploads_go_to", "|07Uploads go to |15%s|07.\n"), a->name);
    }
    upload_into(a);
}

/* receive into `a` (the caller has already been allowed to) and describe what came */
static void upload_into(struct FileArea *a)
{
    if (ro_refuse(a)) return;
    char names[MAXBATCH][32];
    struct ZStats st;
    LONG got, i;
    int proto = N.user.proto <= PROTO_MAX ? N.user.proto : PROTO_Z;

    if (N.local) { tputs(L("file.upload_into.file_transfers_need", "|08File transfers need a telnet session - at the console, just copy the file.|07\n")); return; }
    if (proto == PROTO_X || proto == PROTO_X1K) {
        /* XMODEM carries no file name: ask for it */
        tputs(L("file.upload_into.file_name", "|07File name: |15"));
        if (tgetline(names[0], 30, 0) <= 0) return;
        if (strchr(names[0], '/') || strchr(names[0], ':')) { tputs(L("file.upload_into.just_name_please", "|12Just a name, please.|07\n")); return; }
    }
    tprintf(L("file.upload_into.start_your_upload", "\n|14Start your %s upload now.|07 |08(Ctrl-X a few times to abort)|07\n"), proto_name(proto));
    tn_flush();
    set_activity("Uploading");
    got = proto == PROTO_Z ? zm_receive(a->path, names, MAXBATCH, &st) : xy_receive(a->path, names, MAXBATCH, proto, &st);
    tputs("\r\n");
    if (got <= 0) {
        if (st.skipped)
            tputs(L("file.upload_into.nothing_was_received", "|12Nothing was received|07 - |15that file is already here|07, so it was skipped.\n"
                  "|08Rename it if it's a different file.|07\n"));
        else
            tputs(L("file.upload_into.nothing_was_received_2", "|12Nothing was received.|07\n"));
        back_to_menu();
        return;
    }
    tprintf(got == 1 ? L("file.upload_into.received_one", "|10Received %ld file|07%s.\n") : L("file.upload_into.received_many", "|10Received %ld files|07%s.\n"), got,
            st.skipped ? L("file.upload_into.duplicates_were_skipped", " |08(duplicates were skipped)|07") : "");
    for (i = 0; i < got && N.online; i++) {
        char desc[DIZ_MAX];
        BOOL have = FALSE;
        tprintf("\n|15%s|07\n", names[i]);
        {
            char full[PATHLEN], rm[200];
            path_join(full, a->path, names[i]);
            if (ads_strip(full, rm, sizeof(rm)) > 0) {
                tprintf(L("file.upload_into.another_boards_ad", "|08Another board's ad taken out of it: %s|07\n"), rm);
                bbs_log(BBS_SYSLOG, "node %d: stripped ads from %s: %s", N.node, full, rm);
            }
        }
        if (diz_is_archive(names[i])) {
            if (diz_get(a->path, names[i], desc, sizeof(desc))) {
                /* it describes itself: that's what callers will see */
                tputs(L("file.upload_into.it_has_file", "|07It has a FILE_ID.DIZ:\n"));
                tputraw((UBYTE *)desc, strlen(desc), CS_CP437);
                tputs("\n");
                have = TRUE;
            } else if (cfg_bool(N.cfg, "diz_offer_upload", TRUE) &&
                       tyesno(L("file.upload_into.it_has_no", "|07It has no FILE_ID.DIZ. Create one now?"), TRUE)) {
                if (diz_create_interactive(names[i], a->name, desc, sizeof(desc))) {
                    if (diz_add(a->path, names[i], desc)) {
                        tputs(L("file.upload_into.file_id_diz", "|10FILE_ID.DIZ added to the archive.|07\n"));
                        have = TRUE;
                    } else tputs(L("file.upload_into.couldnt_add_it", "|12Couldn't add it to the archive - using it as the description.|07\n"));
                    if (!have) { add_description(a, names[i], desc); have = TRUE; }
                }
            }
        }
        if (!have) {
            tputs(L("file.upload_into.describe_it", "|07Describe it: |15"));
            if (tgetline(desc, 60, 0) < 0) break;
            if (!desc[0]) strcpy(desc, "(no description)");
            add_description(a, names[i], desc);
        }
        bbs_log(BBS_SYSLOG, "node %d: %s uploaded %s/%s", N.node, N.user.name, a->path, names[i]);
        if (farea_subop(a)) mark_reviewed(a, names[i]);     /* the sysop's own: nothing to review */
    }
    N.user.uploads += got;
    N.user.ulkb += st.total / 1024;
    user_save();
    tputs(L("file.upload_into.thanks_for_the", "|10Thanks for the upload!|07\n"));
    back_to_menu();
}

/* ---- sysop: create or replace an archive's FILE_ID.DIZ in the house style ---- */
static void area_diz(struct FileArea *a)
{
    struct FList fl;
    char name[40], desc[DIZ_MAX];
    LONG j;
    BOOL found = FALSE;

    if (ro_refuse(a)) return;
    if (!flist_load(a, &fl)) { tputs(L("file.area_diz.cant_read_that", "|12Can't read that area.|07\n")); return; }
    tputs(L("file.area_diz.archives_in_this", "\n|07Archives in this area:\n"));
    for (j = 0; j < fl.n; j++)
        if (diz_is_archive(fl.e[j].name))
            tprintf("  |15%-30s |08%s|07\n", fl.e[j].name, entry_diz(&fl, &fl.e[j]) ? L("file.area_diz.has_diz", "has a DIZ") : L("file.area_diz.no_diz", "no DIZ"));
    flist_free(&fl);
    tputs(L("file.area_diz.file_name", "|07File name: |15"));
    if (tgetline(name, 32, 0) <= 0) return;
    if (!flist_load(a, &fl)) return;
    for (j = 0; j < fl.n; j++) if (!str_icmp(fl.e[j].name, name)) { str_copy(name, fl.e[j].name, sizeof(name)); found = TRUE; }
    flist_free(&fl);
    if (!found || !diz_is_archive(name)) { tputs(L("file.area_diz.no_such_archive", "|12No such archive (.lha / .lzh / .zip) here.|07\n")); return; }
    if (diz_get(a->path, name, desc, sizeof(desc))) {
        tputs(L("file.area_diz.current_file_id", "|07Current FILE_ID.DIZ:\n"));
        tputraw((UBYTE *)desc, strlen(desc), CS_CP437);
        tputs("\n");
        if (!tyesno(L("file.area_diz.replace_it_with", "|07Replace it with one in the BBS style?"), FALSE)) return;
    }
    if (!diz_create_interactive(name, a->name, desc, sizeof(desc))) return;
    if (diz_add(a->path, name, desc)) {
        tputs(L("file.area_diz.done_the_archive", "|10Done - the archive now carries the new FILE_ID.DIZ.|07\n"));
        bbs_log(BBS_SYSLOG, "node %d: sysop %s wrote a FILE_ID.DIZ for %s/%s", N.node, N.user.name, a->path, name);
    } else tputs(L("file.area_diz.the_archiver_failed", "|12The archiver failed - check diz_add_lha / diz_add_zip in NilBBS.cfg.|07\n"));
}
void file_sysop_diz(void)
{
    file_select_area();
    if (N.cur_filearea < nfareas) area_diz(&fareas[N.cur_filearea]);
}

/* ---- sysop / sub-op tools for the current file area -------------------------------- */

/* take a file's entry out of files.bbs; its text (continuation lines unindented,
   joined with \n) goes to `out`.  TRUE if there was one. */
static BOOL desc_take(struct FileArea *a, const char *name, char *out, LONG outsize)
{
    char path[PATHLEN];
    LONG sz, nl = strlen(name), n = 0;
    char *buf, *p, *keep, *w;
    BPTR fh;
    BOOL found = FALSE;
    out[0] = 0;
    sprintf(path, "%s/files.bbs", area_meta(a->path));
    ObtainSemaphore(&N.S->filelock);
    if ((sz = file_size(path)) <= 0 || !(buf = AllocVec(sz + 1, 0))) { ReleaseSemaphore(&N.S->filelock); return FALSE; }
    if ((fh = Open((STRPTR)path, MODE_OLDFILE))) { n = Read(fh, buf, sz); Close(fh); }
    if (n < 0) n = 0;
    buf[n] = 0;
    if (!(keep = AllocVec(n + 1, 0))) { FreeVec(buf); ReleaseSemaphore(&N.S->filelock); return FALSE; }
    w = keep;
    for (p = buf; *p; ) {
        char *e = strchr(p, '\n');
        LONG len = e ? e - p + 1 : (LONG)strlen(p);
        if (!str_nicmp(p, name, nl) && (p[nl] == ' ' || p[nl] == '\t')) {
            /* the entry, then its indented continuation lines; every copy goes
               (re-uploads used to add one each), the first one's text is kept */
            char *d = p + nl;
            LONG o = found ? -1 : 0;
            found = TRUE;
            if (o < 0) {
                for (;;) {
                    char *de = strchr(d, '\n');
                    p = de ? de + 1 : d + strlen(d);
                    if (!(*p == ' ' || *p == '\t')) break;
                    d = p;
                }
                continue;
            }
            for (;;) {
                LONG dl;
                char *de;
                while (*d == ' ' || *d == '\t' || *d == '|' || *d == '+') d++;
                de = strchr(d, '\n');
                dl = de ? de - d : (LONG)strlen(d);
                if (dl && d[dl - 1] == '\r') dl--;
                if (o && o < outsize - 1) out[o++] = '\n';
                if (dl > outsize - 1 - o) dl = outsize - 1 - o;
                memcpy(out + o, d, dl); o += dl;
                p = de ? de + 1 : d + strlen(d);
                if (!(*p == ' ' || *p == '\t')) break;
                d = p;
            }
            out[o] = 0;
            continue;
        }
        memcpy(w, p, len); w += len;
        p += len;
    }
    if (found && (fh = Open((STRPTR)path, MODE_NEWFILE))) {
        Write(fh, keep, w - keep);
        Close(fh);
    }
    FreeVec(keep);
    FreeVec(buf);
    ReleaseSemaphore(&N.S->filelock);
    return found;
}

/* the cached FILE_ID.DIZ goes with the file */
static void diz_forget(struct FileArea *a, const char *name)
{
    char p[PATHLEN];
    sprintf(p, "%s/.diz/%s", area_meta(a->path), name);
    DeleteFile((STRPTR)p);
}

/* ---- one file at a time: remove / move --------------------------------------------- */

static BOOL remove_one(struct FileArea *a, const char *name, BOOL forever)
{
    char from[PATHLEN], to[PATHLEN], junk[DIZ_MAX];
    BPTR l;
    BOOL ok;
    if (ro_refuse(a)) return FALSE;
    path_join(from, a->path, name);
    if (forever) ok = DeleteFile((STRPTR)from) != 0;
    else {
        sprintf(to, "%s/.trash", a->path);
        if ((l = Lock((STRPTR)to, ACCESS_READ))) UnLock(l);
        else if ((l = CreateDir((STRPTR)to))) UnLock(l);
        sprintf(to, "%s/.trash/%s", a->path, name);
        DeleteFile((STRPTR)to);                             /* an older copy in the trash */
        ok = Rename((STRPTR)from, (STRPTR)to) != 0;
    }
    if (!ok) return FALSE;
    desc_take(a, name, junk, sizeof(junk));
    diz_forget(a, name);
    bbs_log(BBS_SYSLOG, "node %d: %s %s %s", N.node, N.user.name, forever ? "deleted" : "trashed", from);
    return TRUE;
}

/* a plain copy (off a disc) */
static BOOL copy_file(const char *from, const char *to)
{
    BPTR in, out;
    UBYTE *buf;
    LONG n;
    BOOL ok = TRUE;
    if (!(buf = AllocVec(16384, 0))) return FALSE;
    if (!(in = Open((STRPTR)from, MODE_OLDFILE))) { FreeVec(buf); return FALSE; }
    if (!(out = Open((STRPTR)to, MODE_NEWFILE))) { Close(in); FreeVec(buf); return FALSE; }
    while ((n = Read(in, buf, 16384)) > 0) if (Write(out, buf, n) != n) { ok = FALSE; break; }
    if (n < 0) ok = FALSE;
    Close(out);
    Close(in);
    FreeVec(buf);
    if (!ok) DeleteFile((STRPTR)to);
    return ok;
}

/* 0 = moved (copied, off a disc), 1 = already there, 2 = failed, 3 = onto a disc */
static int move_one(struct FileArea *a, const char *name, struct FileArea *d)
{
    char from[PATHLEN], to[PATHLEN], desc[DIZ_MAX];
    path_join(from, a->path, name);
    path_join(to, d->path, name);
    if (d->ro) return 3;
    if (file_exists(to)) return 1;
    if (a->ro) {
        /* the disc keeps its copy; the description (the BBS's, or the disc's first line) goes along */
        struct FList fl;
        LONG j;
        if (!copy_file(from, to)) return 2;
        desc[0] = 0;
        if (flist_load(a, &fl)) {
            for (j = 0; j < fl.n; j++)
                if (!str_icmp(fl.e[j].name, name) && fl.e[j].desc) {
                    const char *e = strpbrk(fl.e[j].desc, "\r\n");
                    LONG len = e ? e - fl.e[j].desc : (LONG)strlen(fl.e[j].desc);
                    if (len > 120) len = 120;
                    memcpy(desc, fl.e[j].desc, len); desc[len] = 0;
                }
            flist_free(&fl);
        }
        if (desc[0]) add_description(d, name, desc);
        bbs_log(BBS_SYSLOG, "node %d: %s copied %s to %s", N.node, N.user.name, from, d->path);
        return 0;
    }
    if (!Rename((STRPTR)from, (STRPTR)to)) return 2;
    if (desc_take(a, name, desc, sizeof(desc)) && desc[0]) add_description(d, name, desc);
    diz_forget(a, name);
    bbs_log(BBS_SYSLOG, "node %d: %s moved %s to %s", N.node, N.user.name, from, d->path);
    return 0;
}

/* ask where to, move it; the result goes to msg */
#define MOVE_MSG 159
static void move_ask(struct FileArea *a, const char *name, char *msg)
{
    int t, r;
    tprintf(L("file.move_ask.move_to_which", "\n|07Move |15%s|07 to which area?\n\n"), name);
    /* msg is 160 bytes (the callers'); one is left for the "\n" one caller adds */
    if ((t = pick_area(L("file.move_ask.prompt", "Area number (Enter = cancel)"), TRUE)) < 0) { str_copy(msg, L("file.move_ask.not_moved", "|08Not moved.|07"), MOVE_MSG); return; }
    if (&fareas[t] == a) { str_copy(msg, L("file.move_ask.already_here", "|08It's already here.|07"), MOVE_MSG); return; }
    r = move_one(a, name, &fareas[t]);
    if (r == 0) snprintf(msg, MOVE_MSG, a->ro ? L("file.move_ask.copied", "|10%.30s copied off the disc to %.40s.|07") : L("file.move_ask.moved", "|10%.30s moved to %.40s.|07"),
                         name, farea_title(t));
    else snprintf(msg, MOVE_MSG, r == 1 ? L("file.move_ask.there_already", "|12%.30s is already there - not moved.|07") :
                  r == 3 ? L("file.move_ask.read_only", "|12%.30s can't go there - that area is a read-only disc.|07") : L("file.move_ask.failed", "|12%.30s couldn't be moved.|07"), name);
}

/* the first line of a file's description, cleaned for a one-line listing */
static void first_line(struct FList *fl, struct FEntry *e, char *out, LONG max)
{
    const char *d = entry_diz(fl, e);
    if (!d) d = e->desc;
    LONG n = 0;
    out[0] = 0;
    if (!d) { str_copy(out, "(no description)", max); return; }
    /* skip decoration (a DIZ often opens with a line of box-drawing or dashes):
       the first line with a letter or digit in it */
    for (;;) {
        const char *e = d;
        BOOL words = FALSE;
        while (*e && *e != '\n') {
            if ((*e >= 'A' && *e <= 'Z') || (*e >= 'a' && *e <= 'z') || (*e >= '0' && *e <= '9')) words = TRUE;
            e++;
        }
        if (words || !*e) break;
        d = e + 1;
    }
    while (*d == ' ' || *d == '\t' || *d == '|' || *d == '+') d++;
    while (*d && *d != '\n' && *d != '\r' && n < max - 1) out[n++] = *d++;
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '|')) n--;
    out[n] = 0;
}

static void size_str(LONG size, char *sz)
{
    if (size >= 10L * 1024 * 1024) sprintf(sz, "%ldM", size / (1024 * 1024));
    else if (size >= 10000) sprintf(sz, "%ldK", size / 1024);
    else sprintf(sz, "%ld", size);
}

/* ---- the file browser: arrow keys, Del deletes, M moves ------------------------------ */

#define LIST_TOP 4

static void browse_row(struct FList *fl, LONG i, BOOL hl, int y)
{
    char line[300], d[16], sz[12], desc[200];
    LONG w = (LONG)N.cols - 1;
    struct FEntry *e = &fl->e[i];
    if (w > 250) w = 250;
    bbs_datestr(e->date, d);
    size_str(e->size, sz);
    first_line(fl, e, desc, sizeof(desc));
    sprintf(line, " %-22.22s %6s %9s  %s", e->name, sz, d, desc);
    if ((LONG)strlen(line) > w) line[w] = 0;
    while ((LONG)strlen(line) < w) strcat(line, " ");
    tgotoxy(1, y);
    tputs(hl ? "|17|15" : "|16|07");
    tputraw((const UBYTE *)line, strlen(line), CS_CP437);
    tputs("|16|07");
}

static void browse_draw(struct FileArea *a, struct FList *fl, LONG top, LONG cur, int rows, const char *msg)
{
    LONG i;
    tcls();
    tgotoxy(1, 1);
    tprintf(L("file.browse_draw.browse", "|09-=[ |15Browse: %s|09 ]=-|07"), farea_title(N.cur_filearea));
    tgotoxy(1, 2);
    tputs(L("file.browse_draw.up_down_pick", "|08Up/Down pick   PgUp/PgDn page   |15Del|08 delete   |15M|08 move   |15Q|08 back|07"));
    for (i = 0; i < rows; i++) {
        if (top + i < fl->n) browse_row(fl, top + i, top + i == cur, LIST_TOP + i);
    }
    tgotoxy(1, LIST_TOP + rows);
    tprintf(L("file.browse_draw.file_of", "|08File %ld of %ld|07"), cur + 1, fl->n);
    tcleol();
    tgotoxy(1, LIST_TOP + rows + 1);
    tputs(msg);
    tcleol();
    (void)a;
}

/* plain-text callers: a numbered list, one file at a time */
static void browse_numbered(struct FileArea *a)
{
    for (;;) {
        struct FList fl;
        LONG j, n;
        char b[8], msg[160];
        if (!flist_load(a, &fl)) return;
        if (!fl.n) { flist_free(&fl); tputs(L("file.browse_numbered.no_files_here", "|08No files here.|07\n")); return; }
        tpage_start();
        for (j = 0; j < fl.n && tmore(); j++) {
            char d[16], sz[12];
            bbs_datestr(fl.e[j].date, d);
            size_str(fl.e[j].size, sz);
            tprintf("|15%3ld|08) |07", j + 1);
            tputraw((const UBYTE *)fl.e[j].name, strlen(fl.e[j].name), CS_CP437);
            tprintf("  |08%s %s|07\n", sz, d);
        }
        tpage_end();
        tputs(L("file.browse_numbered.file_number_enter", "\n|07File number |08(Enter = back)|07: |15"));
        if (tgetline(b, 5, GL_DIGITS) <= 0) { flist_free(&fl); return; }
        n = atol(b) - 1;
        if (n < 0 || n >= fl.n) { flist_free(&fl); continue; }
        tprintf(L("file.browse_numbered.delete_move_enter", "|15%s|07: |15D|07)elete, |15M|07)ove, |15Enter|07 = nothing: "), fl.e[n].name);
        msg[0] = 0;
        switch (tgethot("DM\r")) {
        case 'D': {
            LONG k;
            tputs(L("file.browse_numbered.delete_trash_drawer", "Delete\n|07T)rash drawer, D)elete for good, N)o: "));
            k = tgethot("TDN\r");
            if (k == 'T' || k == 'D') {
                BOOL ok = remove_one(a, fl.e[n].name, k == 'D');
                snprintf(msg, sizeof(msg), !ok ? L("file.browse_numbered.remove_failed", "|12Couldn't remove %s.|07\n") :
                         k == 'D' ? L("file.browse_numbered.deleted", "|10%s deleted.|07\n") : L("file.browse_numbered.trashed", "|10%s moved to the trash drawer.|07\n"),
                         fl.e[n].name);
            } else str_copy(msg, L("file.browse_numbered.no", "No\n"), sizeof(msg));
            break;
        }
        case 'M': tputs(L("file.browse_numbered.move", "Move\n")); move_ask(a, fl.e[n].name, msg); strcat(msg, "\n"); break;
        default: tputs("\n");
        }
        flist_free(&fl);
        tputs(msg);
    }
}

static void browse(struct FileArea *a)
{
    LONG cur = 0, top = 0;
    char msg[160];
    msg[0] = 0;
    if (N.term == TT_ASCII) { browse_numbered(a); return; }
    for (;;) {                                      /* (re)load after every change */
        struct FList fl;
        int rows = (int)N.rows - LIST_TOP - 2;
        BOOL reload = FALSE;
        if (rows < 3) rows = 3;
        if (!flist_load(a, &fl)) { tputs(L("file.browse.cant_read_this", "|12Can't read this area.|07\n")); return; }
        if (!fl.n) { flist_free(&fl); tcls(); tputs(L("file.browse.no_files_here", "|08No files here.|07\n")); return; }
        if (cur >= fl.n) cur = fl.n - 1;
        if (cur < top) top = cur;
        if (cur >= top + rows) top = cur - rows + 1;
        browse_draw(a, &fl, top, cur, rows, msg);
        msg[0] = 0;
        while (!reload) {
            LONG k, was = cur;
            tgotoxy(1, LIST_TOP + rows + 1);
            tn_flush();
            k = tgetkey(0);
            switch (k) {
            case KEY_HANGUP: flist_free(&fl); return;
            case KEY_NONE: browse_draw(a, &fl, top, cur, rows, ""); continue;   /* a message came in */
            case KEY_UP:   if (cur > 0) cur--; break;
            case KEY_DOWN: if (cur < fl.n - 1) cur++; break;
            case KEY_PGUP: cur -= rows; if (cur < 0) cur = 0; break;
            case KEY_PGDN: cur += rows; if (cur > fl.n - 1) cur = fl.n - 1; break;
            case KEY_HOME: cur = 0; break;
            case KEY_END:  cur = fl.n - 1; break;
            case 'q': case 'Q': case 27:
                flist_free(&fl);
                tcls();
                return;
            case KEY_DEL: case 8: case 'd': case 'D': {
                char name[32];
                LONG c;
                str_copy(name, fl.e[cur].name, sizeof(name));
                tgotoxy(1, LIST_TOP + rows + 1);
                tputs(L("file.browse.delete", "|12Delete |15"));
                tputraw((const UBYTE *)name, strlen(name), CS_CP437);
                tputs(L("file.browse.trash_drawer_delete", "|12?  |15T|07)rash drawer  |15D|07)elete for good  |15N|07)o |08(Enter = No)|07 "));
                tcleol();
                c = tgethot("TDN\r");
                if (c == KEY_HANGUP) { flist_free(&fl); return; }
                if (c == 'T' || c == 'D') {
                    if (remove_one(a, name, c == 'D'))
                        snprintf(msg, sizeof(msg), c == 'D' ? L("file.browse.deleted", "|10%.30s deleted for good.|07") : L("file.browse.trashed", "|10%.30s moved to the trash drawer.|07"), name);
                    else snprintf(msg, sizeof(msg), L("file.browse.remove_failed", "|12Couldn't remove %.30s.|07"), name);
                } else str_copy(msg, L("file.browse.kept", "|08Kept.|07"), sizeof(msg));
                reload = TRUE;
                continue;
            }
            case 'm': case 'M': {
                char name[32];
                str_copy(name, fl.e[cur].name, sizeof(name));
                tcls();
                move_ask(a, name, msg);
                reload = TRUE;
                continue;
            }
            default: continue;
            }
            /* the bar moved: redraw two rows, or the page if it scrolled */
            if (cur < top || cur >= top + rows) {
                if (cur < top) top = cur; else top = cur - rows + 1;
                browse_draw(a, &fl, top, cur, rows, "");
            } else if (cur != was) {
                browse_row(&fl, was, FALSE, LIST_TOP + (int)(was - top));
                browse_row(&fl, cur, TRUE, LIST_TOP + (int)(cur - top));
                tgotoxy(1, LIST_TOP + rows);
                tprintf(L("file.browse.file_of", "|08File %ld of %ld|07"), cur + 1, fl.n);
                tcleol();
            }
        }
        flist_free(&fl);
    }
}

static void tool_trash(struct FileArea *a)
{
    char dir[PATHLEN], p[PATHLEN + 40];
    struct FileInfoBlock *fib;
    BPTR l;
    LONG n = 0, bytes = 0, k;
    if (ro_refuse(a)) return;
    sprintf(dir, "%s/.trash", a->path);
    if (!(l = Lock((STRPTR)dir, ACCESS_READ))) { tputs(L("file.tool_trash.the_trash_drawer", "|08The trash drawer is empty.|07\n")); return; }
    if (!(fib = AllocDosObject(DOS_FIB, NULL))) { UnLock(l); return; }
    tputs(L("file.tool_trash.in_the_trash", "\n|07In the trash drawer:\n"));
    if (Examine(l, fib))
        while (ExNext(l, fib))
            if (fib->fib_DirEntryType < 0) {
                tprintf("  |15%-30s |11%ld|07\n", (char *)fib->fib_FileName, (LONG)fib->fib_Size);
                n++; bytes += fib->fib_Size;
            }
    if (!n) { tputs(L("file.tool_trash.empty", "  |08(empty)|07\n")); FreeDosObject(DOS_FIB, fib); UnLock(l); return; }
    tprintf(n == 1 ? L("file.tool_trash.menu_one", "|07%ld file, %ld bytes. |15R|07)estore one, |15E|07)mpty it for good, |15Enter|07 = leave: ")
                   : L("file.tool_trash.menu_many", "|07%ld files, %ld bytes. |15R|07)estore one, |15E|07)mpty it for good, |15Enter|07 = leave: "),
            n, bytes);
    k = tgethot("RE\r");
    if (k == 'E') {
        tputs(L("file.tool_trash.empty_2", "Empty\n"));
        if (tyesno(L("file.tool_trash.delete_everything_in", "|12Delete everything in the trash for good?|07"), FALSE)) {
            /* ExNext can't go on after a delete: collect the names first */
            char (*nm)[32] = AllocVec(n * 32, 0);
            LONG c = 0, i;
            if (nm && Examine(l, fib))
                while (ExNext(l, fib) && c < n)
                    if (fib->fib_DirEntryType < 0) str_copy(nm[c++], (char *)fib->fib_FileName, 32);
            for (i = 0; i < c; i++) { path_join(p, dir, nm[i]); DeleteFile((STRPTR)p); }
            if (nm) FreeVec(nm);
            bbs_log(BBS_SYSLOG, "node %d: %s emptied the trash of %s", N.node, N.user.name, a->path);
            tprintf(c == 1 ? L("file.tool_trash.emptied_one", "|10Emptied (%ld file).|07\n") : L("file.tool_trash.emptied_many", "|10Emptied (%ld files).|07\n"), c);
        }
    } else if (k == 'R') {
        char name[40], from[PATHLEN + 40], to[PATHLEN + 40];
        tputs(L("file.tool_trash.restore_file_name", "Restore\n|07File name: |15"));
        if (tgetline(name, 32, 0) > 0 && !strchr(name, '/') && !strchr(name, ':')) {
            path_join(from, dir, name);
            path_join(to, a->path, name);
            if (file_exists(to)) tputs(L("file.tool_trash.file_by_that", "|12A file by that name is back in the area already.|07\n"));
            else if (Rename((STRPTR)from, (STRPTR)to)) {
                add_description(a, name, "(restored from the trash)");
                tputs(L("file.tool_trash.restored", "|10Restored.|07\n"));
            } else tputs(L("file.tool_trash.not_in_the", "|12Not in the trash.|07\n"));
        }
    } else tputs("\n");
    FreeDosObject(DOS_FIB, fib);
    UnLock(l);
}

/* ---- reviewing what callers uploaded ------------------------------------------------ */

/* go through one area's unreviewed files, newest first; FALSE = the sysop quit */
static BOOL review_area(struct FileArea *a, int ai)
{
    char skipped[32][32];
    int nskip = 0;
    for (;;) {
        struct FList fl;
        LONG j, left = 0, pick = -1;
        char msg[160];
        if (!flist_load(a, &fl)) return TRUE;
        for (j = 0; j < fl.n; j++) {
            int k;
            BOOL sk = FALSE;
            for (k = 0; k < nskip; k++) if (!str_icmp(skipped[k], fl.e[j].name)) sk = TRUE;
            if (sk || is_reviewed(a, fl.e[j].name)) continue;
            if (pick < 0) pick = j;
            left++;
        }
        if (pick < 0) { flist_free(&fl); return TRUE; }
        tprintf(L("file.review_area.review_waiting", "\n|09-=[ |15Review: %s|09 ]=- |08(%ld waiting)|07\n\n"), farea_title(ai), left);
        show_entry(&fl, &fl.e[pick]);
        tputs(L("file.review_area.keep_it_here", "\n|15K|07)eep it here  |15M|07)ove it  |15D|07)elete it  |15S|07)kip for now  |15Q|07)uit: "));
        msg[0] = 0;
        switch (tgethot("KMDSQ")) {
        case KEY_HANGUP: flist_free(&fl); return FALSE;
        case 'K':
            tputs(L("file.review_area.keep", "Keep\n"));
            mark_reviewed(a, fl.e[pick].name);
            break;
        case 'M': {
            tputs(L("file.review_area.move", "Move\n"));
            move_ask(a, fl.e[pick].name, msg);
            tprintf("%s\n", msg);
            break;
        }
        case 'D': {
            LONG k;
            tputs(L("file.review_area.delete_delete_it", "Delete\n|12Delete it?|07  |15T|07)rash drawer  |15D|07)elete for good  |15N|07)o |08(Enter = No)|07 "));
            k = tgethot("TDN\r");
            if (k == 'T' || k == 'D') {
                BOOL ok = remove_one(a, fl.e[pick].name, k == 'D');
                tprintf(ok ? "|10%s.|07\n" : L("file.review_area.couldnt_remove_it", "|12Couldn't remove it.|07\n"), k == 'D' ? L("file.review_area.deleted_for_good", "Deleted for good") : L("file.review_area.in_the_trash", "In the trash drawer"));
            } else tputs(L("file.review_area.no", "No\n"));
            break;
        }
        case 'S':
            tputs(L("file.review_area.skip", "Skip\n"));
            if (nskip < 32) str_copy(skipped[nskip++], fl.e[pick].name, 32);
            break;
        default:
            tputs(L("file.review_area.quit", "Quit\n"));
            flist_free(&fl);
            return FALSE;
        }
        flist_free(&fl);
    }
}

/* the sysop: every area callers can upload to; a sub-op: the current area */
void file_review_uploads(void)
{
    int k, areas = 0;
    set_activity("Reviewing uploads");
    for (k = 0; k < norder; k++) {
        int i = order[k];
        struct FileArea *a = &fareas[i];
        if (!HAS_FILES(a) || a->ro || !farea_subop(a)) continue;
        if (!N.sysop && i != N.cur_filearea) continue;
        if (N.sysop && a->ulevel > 254) continue;            /* nobody but the sysop uploads there */
        areas++;
        if (!review_area(a, i)) { back_to_menu(); return; }
    }
    tputs(areas ? L("file.review_uploads.nothing_left_to", "\n|10Nothing left to review.|07\n") : L("file.review_uploads.no_area_here", "\n|08No area here takes uploads from callers.|07\n"));
    back_to_menu();
}

void file_subop(void)
{
    struct FileArea *a;
    if (N.cur_filearea >= nfareas) return;
    a = &fareas[N.cur_filearea];
    if (!farea_subop(a)) { tputs(L("file.subop.only_the_sysop", "|12Only the sysop and this area's sub-ops can do that.|07\n")); return; }
    if (!HAS_FILES(a)) { tputs(L("file.subop.this_is_heading", "|08This is a heading - pick one of the areas under it (A).|07\n")); return; }
    for (;;) {
        LONG k;
        tprintf(L("file.subop.file_tools", "\n|09-=[ |15File tools: %s|09 ]=-|07\n\n"), farea_title(N.cur_filearea));
        if (a->ro) tputs(L("file.subop.read_only_disc", "  |08A read-only disc (CD/DVD): browse, and M copies a file off it.|07\n\n"));
        tputs(L("file.subop.browse_the_files", "  |08[|15B|08] |07Browse the files: pick one, Del deletes it, M moves it\n"
              "  |08[|15U|08] |07Upload new files into this area\n"
              "  |08[|15R|08] |07Review new uploads (keep, move or delete each one)\n"
              "  |08[|15T|08] |07The trash drawer (restore, empty)\n"
              "  |08[|15Z|08] |07Create/replace a FILE_ID.DIZ\n"
              "  |08[|15Q|08] |07Back\n\n|07File tools: "));
        k = tgethot("BURTZQ\r");
        if (k == KEY_HANGUP) return;
        tprintf("%c\n", (int)(k == '\r' ? 'Q' : k));
        switch (k) {
        case 'B': browse(a); break;
        case 'U': upload_into(a); break;
        case 'R': file_review_uploads(); break;
        case 'T': tool_trash(a); break;
        case 'Z': area_diz(a); break;
        default: return;
        }
    }
}

/* ---- single transfers for other modules (QWK packets) -------------------------- */

/* send one file with the caller's protocol; TRUE if it went through */
BOOL file_send_path(const char *path, const char *name)
{
    struct ZStats st;
    const char *pp[1], *np[1];
    int proto = N.user.proto <= PROTO_MAX ? N.user.proto : PROTO_Z;
    LONG sent;
    if (N.local) { tprintf(L("file.send_path.local_session_the", "|08Local session: the packet is %s|07\n"), path); return FALSE; }
    pp[0] = path; np[0] = name;
    tprintf(L("file.send_path.start_your_download", "|07\n|14Start your %s download of |15%s|14 now.|07 |08(Ctrl-X a few times to abort)|07\n"),
            proto_name(proto), name);
    tn_flush();
    sent = proto == PROTO_Z ? zm_send(pp, np, 1, &st) : xy_send(pp, np, 1, proto, &st);
    tputs("\r\n");
    return sent > 0;
}

/* receive into dir with the caller's protocol; XMODEM gets `xname` as its name */
LONG file_receive_dir(const char *dir, char names[][32], LONG max, const char *xname)
{
    struct ZStats st;
    int proto = N.user.proto <= PROTO_MAX ? N.user.proto : PROTO_Z;
    LONG got;
    if (N.local) { tputs(L("file.receive_dir.file_transfers_need", "|08File transfers need a telnet session.|07\n")); return 0; }
    if (proto == PROTO_X || proto == PROTO_X1K) str_copy(names[0], xname, 32);
    tprintf(L("file.receive_dir.start_your_upload", "|07\n|14Start your %s upload now.|07 |08(Ctrl-X a few times to abort)|07\n"), proto_name(proto));
    tn_flush();
    got = proto == PROTO_Z ? zm_receive(dir, names, max, &st) : xy_receive(dir, names, max, proto, &st);
    tputs("\r\n");
    return got;
}
