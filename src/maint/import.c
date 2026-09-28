/*
 * import.c - BBSMaint's file import: files the sysop drops in, made ready
 * for callers.
 *
 * Where to drop them:
 *   BBS:Files/Import/<AREA TAG>/   into that file area (e.g. Import/AMIGA/)
 *   BBS:Files/Import/              into import_area (default: the first area)
 *   or straight into an area's own drawer - any file the BBS hasn't seen yet
 *   (no files.bbs line and no DIZ cache entry) is processed where it lies.
 *
 * For each file:
 *   - archives are tested (import_test); a bad one goes to Import/Bad
 *   - a file already in the area goes to Import/Duplicates
 *   - it moves into the area and is dated today, so "new files" shows it
 *   - an archive's own FILE_ID.DIZ becomes its description (and is cached)
 *   - an Aminet-style <name>.readme next to it gives the description from
 *     its "Short:" line; with no DIZ in the archive, one is built in the
 *     house style (Text/diz.tmpl) and added to it (import_make_diz); the
 *     readme is kept in the area's .readme drawer
 *   - a files.bbs line is written
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

#include "bbs.h"
#include "cfg.h"
#include "dizcore.h"

extern void say(const char *fmt, ...);
extern struct Cfg *C;
extern struct BBSShared *S;

#define MAXNAMES 200

struct FArea {
    char tag[NAMELEN];
    char name[LONGNAME];
    char path[PATHLEN];
};

static struct FArea fa[MAX_FILEAREAS];
static int nfa;
static LONG n_done, n_bad, n_dupe, n_diz, n_made, n_stripped;

static void load_areas(void)
{
    struct Cfg *c = cfg_load("BBS:Config/FileAreas.cfg");
    LONG i, n = cfg_sections(c);
    nfa = 0;
    for (i = 0; i < n && nfa < MAX_FILEAREAS; i++) {
        const char *tag = cfg_section(c, i);
        str_copy(fa[nfa].tag, tag, NAMELEN);
        str_copy(fa[nfa].name, cfg_sget(c, tag, "name", tag), LONGNAME);
        str_copy(fa[nfa].path, cfg_sget(c, tag, "path", ""), PATHLEN);
        if (!fa[nfa].path[0]) continue;         /* a heading for sub-areas: nothing to import into */
        if (cfg_sbool(c, tag, "readonly", FALSE) || cfg_sbool(c, tag, "cdrom", FALSE)) continue;   /* a CD/DVD */
        nfa++;
    }
    cfg_free(c);
}

static struct FArea *area_by_tag(const char *tag)
{
    int i;
    for (i = 0; i < nfa; i++) if (!str_icmp(fa[i].tag, tag)) return &fa[i];
    return NULL;
}

static void make_dir(const char *path)
{
    BPTR l;
    if ((l = Lock((STRPTR)path, ACCESS_READ))) UnLock(l);
    else if ((l = CreateDir((STRPTR)path))) UnLock(l);
}

/* the plain files in dir (names only) - or its subdirectories */
static int list_dir(const char *dir, char names[][108], BOOL dirs)
{
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    BPTR l;
    int n = 0;
    if (!fib) return 0;
    if ((l = Lock((STRPTR)dir, ACCESS_READ))) {
        if (Examine(l, fib))
            while (n < MAXNAMES && ExNext(l, fib)) {
                if ((fib->fib_DirEntryType > 0) != dirs) continue;
                str_copy(names[n++], fib->fib_FileName, 108);
            }
        UnLock(l);
    }
    FreeDosObject(DOS_FIB, fib);
    return n;
}

static BOOL skip_name(const char *name)
{
    LONG n = strlen(name);
    return name[0] == '.' || !str_nicmp(name, "files.bbs", 9) ||   /* and its backups */
           (n > 5 && !str_icmp(name + n - 5, ".info"));
}

/* "Foo-1.2.lha" -> "Foo-1.2" */
static void base_of(const char *name, char *base)
{
    char *dot;
    str_copy(base, name, 108);
    if ((dot = strrchr(base, '.'))) *dot = 0;
}

static BOOL is_readme(const char *name)
{
    LONG n = strlen(name);
    return n > 7 && !str_icmp(name + n - 7, ".readme");
}

/* a .readme that belongs to an archive in the same list */
/* (on disk, not just in `names`: a drawer can hold more than one run's MAXNAMES) */
static BOOL companion_readme(const char *dir, const char *name, char names[][108], int n)
{
    static const char *ext[] = { "lha", "lzh", "lzx", "zip", NULL };
    char base[108], other[108], path[PATHLEN];
    int i;
    if (!is_readme(name)) return FALSE;
    base_of(name, base);
    for (i = 0; i < n; i++) {
        if (!diz_is_archive(names[i])) continue;
        base_of(names[i], other);
        if (!str_icmp(base, other)) return TRUE;
    }
    for (i = 0; ext[i]; i++) {
        sprintf(path, "%s/%s.%s", dir, base, ext[i]);
        if (file_exists(path)) return TRUE;
    }
    return FALSE;
}

/* ---- files.bbs ------------------------------------------------------------------------ */

static BOOL in_filesbbs(const char *dir, const char *name)
{
    struct LineReader lr;
    char path[PATHLEN], line[LINELEN];
    LONG nl = strlen(name);
    BOOL found = FALSE;
    sprintf(path, "%s/files.bbs", dir);
    if (!lr_open(&lr, path)) return FALSE;
    while (!found && lr_gets(&lr, line, sizeof(line)) >= 0)
        if (!str_nicmp(line, name, nl) && (line[nl] == ' ' || line[nl] == '\t' || !line[nl])) found = TRUE;
    lr_close(&lr);
    return found;
}

static void add_filesbbs(const char *dir, const char *name, const char *desc)
{
    char path[PATHLEN], line[200];
    BPTR fh;
    sprintf(path, "%s/files.bbs", dir);
    if (!(fh = Open((STRPTR)path, MODE_READWRITE))) return;
    Seek(fh, 0, OFFSET_END);
    sprintf(line, "%-12s %.120s\n", name, desc);
    FPuts(fh, (STRPTR)line);
    Close(fh);
}

/* ---- Aminet readmes: "Short:", "Version:", "Author:" ------------------------------------- */

static void readme_field(const char *line, const char *key, char *out, LONG max)
{
    LONG kl = strlen(key);
    const char *v;
    if (str_nicmp(line, key, kl) || line[kl] != ':') return;
    v = line + kl + 1;
    while (*v == ' ' || *v == '\t') v++;
    str_copy(out, v, max);
    str_trim(out);
}

static BOOL read_readme(const char *path, char *shortd, char *version)
{
    struct LineReader lr;
    char line[LINELEN];
    int n = 0;
    shortd[0] = version[0] = 0;
    if (!lr_open(&lr, path)) return FALSE;
    while (n++ < 40 && lr_gets(&lr, line, sizeof(line)) >= 0) {
        readme_field(line, "Short", shortd, 120);
        readme_field(line, "Version", version, 16);
    }
    lr_close(&lr);
    return shortd[0] != 0;
}

/* ---- moving, testing, dating ------------------------------------------------------------ */

static BOOL run_ok(const char *tmpl, const char *file)
{
    char cmd[PATHLEN * 2], *d = cmd;
    const char *s;
    BPTR in, out;
    LONG rc;
    for (s = tmpl; *s && d < cmd + sizeof(cmd) - PATHLEN; s++) {
        if (s[0] == '%' && s[1] == 'a') { d += sprintf(d, "%s", file); s++; }
        else *d++ = *s;
    }
    *d = 0;
    in = Open((STRPTR)"NIL:", MODE_OLDFILE);
    out = Open((STRPTR)"NIL:", MODE_NEWFILE);
    rc = SystemTags((STRPTR)cmd, SYS_Input, in, SYS_Output, out, NP_StackSize, 32768, TAG_END);
    Close(in);
    Close(out);
    return rc == 0;
}

static BOOL archive_ok(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!cfg_bool(C, "import_test", TRUE) || !diz_is_archive(path)) return TRUE;
    if (dot && !str_icmp(dot, ".zip"))
        return run_ok(cfg_str(C, "import_test_zip", "UnZip >NIL: -tqq \"%a\""), path);
    return run_ok(cfg_str(C, "import_test_lha", "LhA >NIL: -q t \"%a\""), path);
}

/* Rename, or copy + delete across volumes */
static BOOL move_file(const char *from, const char *to)
{
    char cmd[PATHLEN * 2 + 40];
    if (Rename((STRPTR)from, (STRPTR)to)) return TRUE;
    sprintf(cmd, "Copy \"%s\" TO \"%s\" CLONE QUIET", from, to);
    if (!run_ok(cmd, "") || !file_exists(to)) return FALSE;
    DeleteFile((STRPTR)from);
    return TRUE;
}

static void touch(const char *path)
{
    struct DateStamp ds;
    DateStamp(&ds);
    SetFileDate((STRPTR)path, &ds);
}

/* the first line of a DIZ that has letters in it, for files.bbs */
static void diz_summary(const char *diz, char *out, LONG max)
{
    const char *p = diz;
    out[0] = 0;
    while (*p) {
        const char *e = strchr(p, '\n'), *q;
        LONG len = e ? e - p : (LONG)strlen(p);
        for (q = p; q < p + len; q++)
            if ((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z')) {
                while (*p == ' ') { p++; len--; }
                if (len >= max) len = max - 1;
                memcpy(out, p, len);
                out[len] = 0;
                str_trim(out);
                return;
            }
        if (!e) break;
        p = e + 1;
    }
}

/* build and add a house-style DIZ from an Aminet readme */
static BOOL make_diz(struct FArea *a, const char *name, const char *shortd, const char *version,
                     char *out, LONG max)
{
    char prog[48], ver[16], desc[DIZ_H][DIZ_W + 1], *p;
    const char *s = shortd;
    int nd = 0;
    str_copy(prog, name, sizeof(prog));
    if ((p = strrchr(prog, '.'))) *p = 0;
    str_copy(ver, version, sizeof(ver));
    if ((p = strrchr(prog, '-')) && p[1] >= '0' && p[1] <= '9') { if (!ver[0]) str_copy(ver, p + 1, sizeof(ver)); *p = 0; }
    /* word-wrap the Short: line into DIZ lines */
    while (*s && nd < 4) {
        LONG len = strlen(s), cut = len;
        if (len > DIZ_W) {
            cut = DIZ_W;
            while (cut > DIZ_W / 2 && s[cut] != ' ') cut--;
            if (s[cut] != ' ') cut = DIZ_W;
        }
        memcpy(desc[nd], s, cut);
        desc[nd][cut] = 0;
        nd++;
        s += cut;
        while (*s == ' ') s++;
    }
    diz_build(prog, ver, desc, nd, a->name, out, max);
    return diz_add(a->path, name, out);
}

/* one file: from srcdir into area a (srcdir == a->path: in place) */
static void import_one(struct FArea *a, const char *srcdir, const char *name, char names[][108], int nn)
{
    char src[PATHLEN], dst[PATHLEN], readme[PATHLEN], base[108], diz[DIZ_MAX], desc[130];
    char shortd[130], version[20], how[60], importdir[PATHLEN];
    BOOL inplace = !str_icmp(srcdir, a->path), have_readme;
    int i;

    str_copy(importdir, cfg_str(C, "import_dir", "BBS:Files/Import"), PATHLEN);
    path_join(src, srcdir, name);
    path_join(dst, a->path, name);
    base_of(name, base);
    sprintf(readme, "%s/%s.readme", srcdir, base);
    have_readme = FALSE;
    for (i = 0; i < nn; i++) if (!str_icmp(names[i], readme + strlen(srcdir) + 1)) have_readme = TRUE;
    if (!have_readme && file_exists(readme)) have_readme = TRUE;       /* past this run's MAXNAMES */

    if (!inplace && file_exists(dst)) {
        char to[PATHLEN];
        sprintf(to, "%s/Duplicates", importdir); make_dir(to);
        sprintf(to, "%s/Duplicates/%s", importdir, name);
        DeleteFile((STRPTR)to);
        move_file(src, to);
        say("  %-8s %-28s already there - moved to Import/Duplicates", a->tag, name);
        n_dupe++;
        return;
    }
    if (!archive_ok(src)) {
        char to[PATHLEN];
        sprintf(to, "%s/Bad", importdir); make_dir(to);
        sprintf(to, "%s/Bad/%s", importdir, name);
        DeleteFile((STRPTR)to);
        move_file(src, to);
        say("  %-8s %-28s FAILED the archive test - moved to Import/Bad", a->tag, name);
        n_bad++;
        return;
    }
    if (!inplace && !move_file(src, dst)) {
        say("  %-8s %-28s couldn't be moved into %s", a->tag, name, a->path);
        return;
    }
    {
        char rm[200];
        if (ads_strip(dst, rm, sizeof(rm)) > 0) { say("  %-8s %-28s other boards' ads taken out: %s", a->tag, name, rm); n_stripped++; }
    }
    if (cfg_bool(C, "import_touch", TRUE)) touch(dst);

    /* the description */
    desc[0] = 0;
    strcpy(how, "no description yet (sysop menu Z)");
    shortd[0] = version[0] = 0;
    if (have_readme) read_readme(readme, shortd, version);
    if (diz_is_archive(name) && diz_get(a->path, name, diz, sizeof(diz))) {
        diz_summary(diz, desc, sizeof(desc));
        strcpy(how, "described by its FILE_ID.DIZ");
        n_diz++;
    } else if (shortd[0] && diz_is_archive(name) && cfg_bool(C, "import_make_diz", TRUE) &&
               make_diz(a, name, shortd, version, diz, sizeof(diz))) {
        strcpy(how, "new DIZ made from its readme");
        n_made++;
    } else if (shortd[0]) strcpy(how, "described from its readme");
    if (shortd[0]) str_copy(desc, shortd, sizeof(desc));
    if (!desc[0]) str_copy(desc, cfg_str(C, "import_desc", "(no description yet)"), sizeof(desc));
    if (!in_filesbbs(a->path, name)) add_filesbbs(a->path, name, desc);
    if (!diz_is_archive(name)) diz_cache_put(a->path, name, "");   /* seen */

    /* the readme stays with it, out of the listing */
    if (have_readme) {
        char to[PATHLEN];
        sprintf(to, "%s/.readme", a->path); make_dir(to);
        sprintf(to, "%s/.readme/%s.readme", a->path, base);
        DeleteFile((STRPTR)to);
        move_file(readme, to);
    }
    say("  %-8s %-28s %s", a->tag, name, how);
    n_done++;
}

/* every file waiting in dir, into area a */
static void import_dir(struct FArea *a, const char *dir)
{
    static char names[MAXNAMES][108];
    int n = list_dir(dir, names, FALSE), i;
    BOOL inplace = !str_icmp(dir, a->path);
    for (i = 0; i < n; i++) {
        char seen[PATHLEN];
        if (skip_name(names[i]) || companion_readme(dir, names[i], names, n)) continue;
        if (inplace) {
            /* in an area's own drawer: only what the BBS hasn't seen yet */
            sprintf(seen, "%s/.diz/%s", a->path, names[i]);
            if (in_filesbbs(a->path, names[i]) || file_exists(seen)) continue;
        }
        if (S) ObtainSemaphore(&S->filelock);
        import_one(a, dir, names[i], names, n);
        if (S) ReleaseSemaphore(&S->filelock);
    }
}

void import_files(void)
{
    static char subs[MAXNAMES][108];
    char importdir[PATHLEN], path[PATHLEN];
    struct FArea *def;
    int n, i;

    n_done = n_bad = n_dupe = n_diz = n_made = n_stripped = 0;
    load_areas();
    if (!nfa) { say("Import: no file areas are set up."); return; }
    str_copy(importdir, cfg_str(C, "import_dir", "BBS:Files/Import"), PATHLEN);
    make_dir(importdir);
    def = area_by_tag(cfg_str(C, "import_area", ""));
    if (!def) def = &fa[0];
    say("Import (drop files in %s, or %s/<area tag>):", importdir, importdir);

    import_dir(def, importdir);
    n = list_dir(importdir, subs, TRUE);
    for (i = 0; i < n; i++) {
        struct FArea *a;
        if (!str_icmp(subs[i], "Bad") || !str_icmp(subs[i], "Duplicates")) continue;
        if (!(a = area_by_tag(subs[i]))) { say("  %s/%s: there is no file area with that tag", importdir, subs[i]); continue; }
        path_join(path, importdir, subs[i]);
        import_dir(a, path);
    }
    for (i = 0; i < nfa; i++) import_dir(&fa[i], fa[i].path);      /* dropped straight in */

    if (!n_done && !n_bad && !n_dupe) say("  nothing new.");
    else say("Import: %ld file%s added (%ld with a DIZ, %ld DIZ%s made), %ld bad, %ld duplicate%s.",
             n_done, n_done == 1 ? "" : "s", n_diz, n_made, n_made == 1 ? "" : "s",
             n_bad, n_dupe, n_dupe == 1 ? "" : "s");
    if (n_stripped) say("Import: other boards' ads taken out of %ld archive%s.", n_stripped, n_stripped == 1 ? "" : "s");
}
