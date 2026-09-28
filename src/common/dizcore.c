/*
 * dizcore.c - FILE_ID.DIZ: read it out of archives, cache it, write new ones.
 * Shared by BBSNode (listings, uploads, the sysop's DIZ tool) and BBSMaint
 * (importing files); each sets up diz_env first (see dizcore.h).
 *
 * Reading: an archive's own FILE_ID.DIZ is its description.  It is taken out
 * with the configured extractor (NilBBS.cfg diz_lha / diz_zip) once and
 * cached as <area path>/.diz/<filename>; an empty cache file means "this
 * archive has none".  The cache is refreshed when the archive is newer.
 *
 * Writing: a new DIZ is built from the BBS's house template BBS:Text/diz.tmpl
 * and added to the archive with diz_add_lha / diz_add_zip.  Template tokens:
 *   {NAMEVER} "Name v1.2" (or just Name)   {NAME} {VERSION} {BBS} {SYSOP}
 *   {DATE} {UPLOADER} {AREA}   and a line holding only {DESC} expands to the
 *   description lines.  A line starting with ^ is centred, ; is a comment.  The result is
 *   clipped to the DIZ standard of 10 lines x 45 columns.
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
#include "dizcore.h"

struct DizEnv diz_env;

static void lk(BOOL on)
{
    if (diz_env.lock) { if (on) ObtainSemaphore(diz_env.lock); else ReleaseSemaphore(diz_env.lock); }
}

static const char *default_tmpl =
    "\xDC\xDC\xDC\xDC\xDC\xDC\xDC\xDC\xDC\xDC\xDC\xDC\xDC\xDC\xDC\xDC\xDC\xDC\xDC\xDC\xDC\xDC\xDC"
    "\xDC\xDC\xDC\xDC\xDC\xDC\xDC\xDC\xDC\xDC\xDC\xDC\xDC\xDC\xDC\xDC\xDC\xDC\xDC\xDC\xDC\xDC\n"
    "^{NAMEVER}\n"
    "{DESC}\n"
    "^- {BBS} -\n"
    "\xDF\xDF\xDF\xDF\xDF\xDF\xDF\xDF\xDF\xDF\xDF\xDF\xDF\xDF\xDF\xDF\xDF\xDF\xDF\xDF\xDF\xDF\xDF"
    "\xDF\xDF\xDF\xDF\xDF\xDF\xDF\xDF\xDF\xDF\xDF\xDF\xDF\xDF\xDF\xDF\xDF\xDF\xDF\xDF\xDF\xDF\n";

BOOL diz_is_archive(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (!str_icmp(dot, ".lha") || !str_icmp(dot, ".lzh") || !str_icmp(dot, ".zip"));
}

static BOOL is_zip(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && !str_icmp(dot, ".zip");
}

static void work_dir(char *dir, const char *sub)
{
    BPTR l;
    path_join(dir, diz_env.work, sub);
    if ((l = Lock((STRPTR)dir, ACCESS_READ))) UnLock(l);
    else if ((l = CreateDir((STRPTR)dir))) UnLock(l);
}

/* run a configured command with %a (archive) and %d (directory) filled in */
static void run_cmd(const char *tmpl, const char *archive, const char *dir)
{
    char cmd[PATHLEN * 2 + 64], *c = cmd;
    const char *s;
    for (s = tmpl; *s && c < cmd + sizeof(cmd) - PATHLEN; ) {
        if (s[0] == '%' && s[1] == 'a') { c += sprintf(c, "%s", archive); s += 2; }
        else if (s[0] == '%' && s[1] == 'd') { c += sprintf(c, "%s", dir); s += 2; }
        else *c++ = *s++;
    }
    *c = 0;
    SystemTags((STRPTR)cmd, TAG_END);
}

/* read a DIZ file: at most DIZ_READ_H lines of 45 columns, trailing blanks trimmed */
static BOOL read_diz_file(const char *path, char *out, LONG max)
{
    struct LineReader lr;
    char line[LINELEN];
    LONG n = 0, lines = 0;
    out[0] = 0;
    if (!lr_open(&lr, path)) return FALSE;
    while (lines < DIZ_READ_H && lr_gets(&lr, line, sizeof(line)) >= 0) {
        LONG len = strlen(line);
        if (len > DIZ_W) len = DIZ_W;
        while (len > 0 && line[len - 1] == ' ') len--;
        if (n + len + 2 >= max) break;
        memcpy(out + n, line, len);
        n += len;
        out[n++] = '\n';
        lines++;
    }
    lr_close(&lr);
    while (n > 0 && out[n - 1] == '\n') n--;
    out[n] = 0;
    return n > 0;
}

/* take FILE_ID.DIZ out of an archive (no cache) */
BOOL diz_extract(const char *archive, char *out, LONG max)
{
    char dir[PATHLEN], diz[PATHLEN];
    out[0] = 0;
    if (!diz_is_archive(archive)) return FALSE;
    work_dir(dir, "diz");
    sprintf(diz, "%s/FILE_ID.DIZ", dir);
    DeleteFile((STRPTR)diz);
    if (is_zip(archive))
        run_cmd(cfg_str(diz_env.cfg, "diz_zip", "UnZip >NIL: -o -j -C \"%a\" FILE_ID.DIZ -d \"%d\""), archive, dir);
    else
        run_cmd(cfg_str(diz_env.cfg, "diz_lha", "LhA >NIL: -q -m e \"%a\" FILE_ID.DIZ \"%d/\""), archive, dir);
    if (!read_diz_file(diz, out, max)) { DeleteFile((STRPTR)diz); return FALSE; }
    DeleteFile((STRPTR)diz);
    return TRUE;
}

/* ---- read-only areas ------------------------------------------------------------- */

#define MAX_RO 16
static char ro_path[MAX_RO][PATHLEN], ro_meta[MAX_RO][PATHLEN];
static int n_ro;

static void mkdir1(const char *path)
{
    BPTR l;
    if ((l = Lock((STRPTR)path, ACCESS_READ))) UnLock(l);
    else if ((l = CreateDir((STRPTR)path))) UnLock(l);
}

/* every drawer on the way to path */
static void mkdirs(const char *path)
{
    char p[PATHLEN], *s;
    str_copy(p, path, PATHLEN);
    for (s = strchr(p, ':') ? strchr(p, ':') + 1 : p; ; s++) {
        char c = *s;
        if ((c == '/' || c == 0) && s > p && s[-1] != ':' && s[-1] != '/') { *s = 0; mkdir1(p); *s = c; }
        if (!c) break;
    }
}

void area_readonly(const char *path, const char *tag)
{
    int i;
    if (!path[0]) return;
    for (i = 0; i < n_ro; i++) if (!str_icmp(ro_path[i], path)) return;
    if (n_ro >= MAX_RO) return;
    str_copy(ro_path[n_ro], path, PATHLEN);
    sprintf(ro_meta[n_ro], "BBS:Data/CD/%.40s", tag);
    mkdir1("BBS:Data/CD");
    mkdir1(ro_meta[n_ro]);
    n_ro++;
}

/* the area's own drawer - or, for a read-only one and every drawer under it, the same place under
   BBS:Data/CD (a static buffer: use it at once) */
const char *area_meta(const char *path)
{
    static char buf[PATHLEN];
    int i;
    for (i = 0; i < n_ro; i++) {
        LONG n = strlen(ro_path[i]);
        const char *rest = path + n;
        if (!str_icmp(ro_path[i], path)) return ro_meta[i];
        if (str_nicmp(ro_path[i], path, n)) continue;
        if (ro_path[i][n - 1] != ':' && ro_path[i][n - 1] != '/') {
            if (*rest != '/') continue;
            rest++;
        }
        sprintf(buf, "%.120s/%.120s", ro_meta[i], rest);
        return buf;
    }
    return path;
}

/* a CD-ROM area: each disc gets its own cache, BBS:Data/CD/<tag>/<disc name> */
void area_disc(const char *path, const char *tag, const char *disc)
{
    int i;
    area_readonly(path, tag);
    for (i = 0; i < n_ro; i++)
        if (!str_icmp(ro_path[i], path)) {
            sprintf(ro_meta[i], "BBS:Data/CD/%.40s/%.60s", tag, disc);
            mkdirs(ro_meta[i]);
        }
}

BOOL area_is_ro(const char *path)
{
    return area_meta(path) != path;
}

/* ---- the per-area cache --------------------------------------------------------- */

static void cache_path(const char *area, const char *name, char *out)
{
    sprintf(out, "%s/.diz/%s", area_meta(area), name);
}

/* a cache file's date, and whether it was written by a reader that keeps long DIZs (its comment) */
#define CACHE_MARK "DIZ20"
static BOOL datestamp_of2(const char *path, struct DateStamp *ds, BOOL *marked)
{
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    BPTR l;
    BOOL ok = FALSE;
    if (marked) *marked = FALSE;
    if (!fib) return FALSE;
    if ((l = Lock((STRPTR)path, ACCESS_READ))) {
        if (Examine(l, fib)) {
            *ds = fib->fib_Date; ok = TRUE;
            if (marked) *marked = !strcmp((char *)fib->fib_Comment, CACHE_MARK);
        }
        UnLock(l);
    }
    FreeDosObject(DOS_FIB, fib);
    return ok;
}
static BOOL datestamp_of(const char *path, struct DateStamp *ds) { return datestamp_of2(path, ds, NULL); }

/* ---- other boards' ads (the old SStrip / LZXStrip / XPStrip job) ------------------------ */

#define STRIP_CFG  "BBS:Config/StripAds.cfg"
#define MAX_STRIP  48
#define STRIP_TOK  130

static BOOL is_lha(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (!str_icmp(dot, ".lha") || !str_icmp(dot, ".lzh"));
}

/* the patterns, parsed (AmigaDOS wildcards, no case) */
static int strip_patterns(char tok[][STRIP_TOK])
{
    struct LineReader lr;
    char line[LINELEN];
    int n = 0;
    if (!lr_open(&lr, STRIP_CFG)) return 0;
    while (n < MAX_STRIP && lr_gets(&lr, line, sizeof(line)) >= 0) {
        char *s = str_trim(line);
        if (!*s || *s == ';' || *s == '#') continue;
        if (strlen(s) > STRIP_TOK / 2 - 2) continue;
        if (ParsePatternNoCase((STRPTR)s, (STRPTR)tok[n], STRIP_TOK) >= 0) n++;
    }
    lr_close(&lr);
    return n;
}

LONG ads_strip(const char *archive, char *removed, LONG max)
{
    static char tok[MAX_STRIP][STRIP_TOK], victim[16][108];
    char dir[PATHLEN], list[PATHLEN], line[LINELEN], cmd[PATHLEN * 2 + 128];
    struct LineReader lr;
    struct DateStamp ds;
    int ntok, nv = 0, i;
    BPTR out, in;
    BOOL have_date;
    LONG prot = -1;
    removed[0] = 0;
    if (diz_env.cfg && !cfg_bool(diz_env.cfg, "strip_ads", TRUE)) return 0;
    if (!is_lha(archive) || !(ntok = strip_patterns(tok))) return 0;
    have_date = datestamp_of(archive, &ds);
    {   /* and its protection bits: LhA writes a new file */
        struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
        BPTR l;
        if (fib && (l = Lock((STRPTR)archive, ACCESS_READ))) {
            if (Examine(l, fib)) prot = fib->fib_Protection;
            UnLock(l);
        }
        if (fib) FreeDosObject(DOS_FIB, fib);
    }
    /* what's in it: LhA's verbose list has the full path of each file */
    work_dir(dir, "strip");
    sprintf(list, "%s/list", dir);
    if (!(out = Open((STRPTR)list, MODE_NEWFILE))) return 0;
    in = Open((STRPTR)"NIL:", MODE_OLDFILE);
    sprintf(cmd, "LhA v \"%s\"", archive);
    SystemTags((STRPTR)cmd, SYS_Input, in, SYS_Output, out, TAG_END);
    Close(out);
    if (in) Close(in);
    if (!lr_open(&lr, list)) return 0;
    while (nv < 16 && lr_gets(&lr, line, sizeof(line)) >= 0) {
        /*  "    1245     469 62.3% 07-May-97 18:22:26  UKLamers96"  */
        char *p = line, *name;
        int field;
        while (*p == ' ') p++;
        if (*p < '0' || *p > '9') continue;
        for (field = 0; field < 5 && *p; field++) {             /* original packed ratio date time */
            while (*p && *p != ' ') p++;
            while (*p == ' ') p++;
        }
        name = str_trim(p);
        if (!*name || strchr(name, '/') || strchr(name, ':') || !str_icmp(name, "FILE_ID.DIZ")) continue;
        {   /* the name comes from inside an uploaded archive and goes into an LhA
             * command line below: skip anything a Shell would act on */
            const char *q;
            for (q = name; *q && shell_char_ok((UBYTE)*q); q++) ;
            if (*q) continue;
        }
        for (i = 0; i < ntok; i++)
            if (MatchPatternNoCase((STRPTR)tok[i], (STRPTR)name)) { str_copy(victim[nv++], name, 108); break; }
    }
    lr_close(&lr);
    DeleteFile((STRPTR)list);
    for (i = 0; i < nv; i++) {
        const char *tmpl = diz_env.cfg ? cfg_str(diz_env.cfg, "strip_lha", "LhA >NIL: -q d \"%a\" \"%f\"")
                                       : "LhA >NIL: -q d \"%a\" \"%f\"";
        char *c = cmd;
        const char *s;
        for (s = tmpl; *s && c < cmd + sizeof(cmd) - PATHLEN; ) {
            if (s[0] == '%' && s[1] == 'a') { c += sprintf(c, "%s", archive); s += 2; }
            else if (s[0] == '%' && s[1] == 'f') { c += sprintf(c, "%s", victim[i]); s += 2; }
            else *c++ = *s++;
        }
        *c = 0;
        lk(TRUE);
        SystemTags((STRPTR)cmd, TAG_END);
        lk(FALSE);
        if ((LONG)(strlen(removed) + strlen(victim[i]) + 3) < max) {
            if (removed[0]) strcat(removed, ", ");
            strcat(removed, victim[i]);
        }
    }
    if (nv && have_date) SetFileDate((STRPTR)archive, &ds);   /* it isn't a new file */
    if (nv && prot >= 0) SetProtection((STRPTR)archive, prot);
    return nv;
}


void diz_cache_put(const char *area, const char *name, const char *text)
{
    char dir[PATHLEN], path[PATHLEN];
    BPTR fh, l;
    sprintf(dir, "%s/.diz", area_meta(area));
    mkdirs(dir);
    (void)l;
    cache_path(area, name, path);
    if ((fh = Open((STRPTR)path, MODE_NEWFILE))) {
        if (text && *text) { FPuts(fh, (STRPTR)text); FPutC(fh, '\n'); }
        Close(fh);
        SetComment((STRPTR)path, (STRPTR)CACHE_MARK);
    }
}

/* the DIZ for area/name, from the cache or (once) from the archive */
BOOL diz_get(const char *area, const char *name, char *out, LONG max)
{
    return diz_get_at(area, name, NULL, out, max);
}

/* the same, when the caller already has the archive's date (from a directory
   scan): saves a Lock/Examine of the archive per file */
BOOL diz_get_at(const char *area, const char *name, const struct DateStamp *date, char *out, LONG max)
{
    char path[PATHLEN], full[PATHLEN];
    struct DateStamp a, c;
    BOOL marked;
    out[0] = 0;
    if (!diz_is_archive(name)) return FALSE;
    cache_path(area, name, path);
    path_join(full, area, name);
    if (datestamp_of2(path, &c, &marked) && (date ? (a = *date, TRUE) : datestamp_of(full, &a)) &&
        CompareDates(&c, &a) <= 0) {
        BOOL got = read_diz_file(path, out, max);      /* cache is current ... */
        LONG lines = 0;
        const char *p;
        for (p = out; *p; p++) if (*p == '\n') lines++;
        if (marked || lines + 1 < DIZ_H) return got;   /* ... unless an old 10-line reader may have cut it */
    }
    lk(TRUE);
    diz_extract(full, out, max);
    diz_cache_put(area, name, out);
    lk(FALSE);
    return out[0] != 0;
}

/* write `text` into the archive as its FILE_ID.DIZ (replacing any old one) */
BOOL diz_add(const char *area, const char *name, const char *text)
{
    if (area_is_ro(area)) return FALSE;          /* the archive is on a disc */
    char dir[PATHLEN], diz[PATHLEN], full[PATHLEN], check[DIZ_MAX];
    BPTR fh, lock, old;
    work_dir(dir, "dizout");
    sprintf(diz, "%s/FILE_ID.DIZ", dir);
    if (!(fh = Open((STRPTR)diz, MODE_NEWFILE))) return FALSE;
    {
        /* DIZ files traditionally use CR LF */
        const char *p = text;
        while (*p) {
            if (*p == '\n') FPutC(fh, '\r');
            FPutC(fh, *p++);
        }
        FPuts(fh, (STRPTR)"\r\n");
    }
    Close(fh);
    path_join(full, area, name);
    /* the adders take a bare FILE_ID.DIZ from the current directory */
    if (!(lock = Lock((STRPTR)dir, ACCESS_READ))) return FALSE;
    old = CurrentDir(lock);
    lk(TRUE);
    /* "add" doesn't replace an entry that is already there (LhA keeps the
     * old one), so remove any old FILE_ID.DIZ first */
    if (is_zip(name)) {
        run_cmd(cfg_str(diz_env.cfg, "diz_del_zip", "Zip >NIL: -q -d \"%a\" FILE_ID.DIZ"), full, dir);
        run_cmd(cfg_str(diz_env.cfg, "diz_add_zip", "Zip >NIL: -q -j \"%a\" FILE_ID.DIZ"), full, dir);
    } else {
        run_cmd(cfg_str(diz_env.cfg, "diz_del_lha", "LhA >NIL: -q -m d \"%a\" FILE_ID.DIZ"), full, dir);
        run_cmd(cfg_str(diz_env.cfg, "diz_add_lha", "LhA >NIL: -q -m a \"%a\" FILE_ID.DIZ"), full, dir);
    }
    CurrentDir(old);
    UnLock(lock);
    DeleteFile((STRPTR)diz);
    /* believe it only if the archive now carries THIS text */
    diz_extract(full, check, sizeof(check));
    diz_cache_put(area, name, check);
    lk(FALSE);
    {
        char want[DIZ_H * (DIZ_W + 2) + 8];
        LONG wn;
        str_copy(want, text, sizeof(want));      /* normalise like read_diz_file() */
        wn = strlen(want);
        while (wn > 0 && (want[wn - 1] == '\n' || want[wn - 1] == ' ')) want[--wn] = 0;
        return !strcmp(want, check);
    }
}

/* ---- building one from the house template ------------------------------------------ */

static void put_line(char *out, LONG *n, LONG max, const char *s, BOOL centre)
{
    char line[DIZ_W + 1];
    LONG len = strlen(s), pad = 0;
    if (len > DIZ_W) len = DIZ_W;
    if (centre) pad = (DIZ_W - len) / 2;
    memset(line, ' ', pad);
    memcpy(line + pad, s, len);
    len += pad;
    while (len > 0 && line[len - 1] == ' ') len--;
    line[len] = 0;
    if (*n + len + 2 >= max) return;
    memcpy(out + *n, line, len);
    *n += len;
    out[(*n)++] = '\n';
}

void diz_build(const char *prog, const char *version, char desc[][DIZ_W + 1], int ndesc,
               const char *area, char *out, LONG max)
{
    char tmpl[1024], lines[16][96], *t, *e;
    int nl = 0, i, fixed = 0, budget;
    LONG n = 0;
    BPTR fh;

    /* the sysop's template, or the built-in one */
    tmpl[0] = 0;
    if ((fh = Open((STRPTR)"BBS:Text/diz.tmpl", MODE_OLDFILE))) {
        LONG r = Read(fh, tmpl, sizeof(tmpl) - 1);
        Close(fh);
        tmpl[r > 0 ? r : 0] = 0;
    }
    if (!tmpl[0]) str_copy(tmpl, default_tmpl, sizeof(tmpl));
    for (t = tmpl; *t && nl < 16; t = e) {
        e = strchr(t, '\n');
        if (e) *e++ = 0; else e = t + strlen(t);
        { char *cr = strchr(t, '\r'); if (cr) *cr = 0; }
        if (*t == ';') continue;                /* template comments */
        str_copy(lines[nl++], t, sizeof(lines[0]));
    }
    for (i = 0; i < nl; i++) if (!strstr(lines[i], "{DESC}")) fixed++;
    budget = DIZ_H - fixed;
    if (budget < 1) budget = 1;

    for (i = 0; i < nl; i++) {
        char expanded[160], *d = expanded;
        const char *s = lines[i];
        BOOL centre = FALSE;
        if (*s == '^') { centre = TRUE; s++; }
        if (strstr(s, "{DESC}")) {
            int k;
            for (k = 0; k < ndesc && k < budget; k++) put_line(out, &n, max, desc[k], centre);
            continue;
        }
        while (*s && d < expanded + sizeof(expanded) - 60) {
            if (*s == '{') {
                const char *end = strchr(s, '}');
                char tok[16], val[80];
                int tl = end ? end - s - 1 : 0;
                if (end && tl < 15) {
                    memcpy(tok, s + 1, tl);
                    tok[tl] = 0;
                    val[0] = 0;
                    if (!strcmp(tok, "NAMEVER")) {
                        if (version[0]) sprintf(val, "%.40s v%.12s", prog, version);
                        else str_copy(val, prog, sizeof(val));
                    }
                    else if (!strcmp(tok, "NAME")) str_copy(val, prog, sizeof(val));
                    else if (!strcmp(tok, "VERSION")) str_copy(val, version, sizeof(val));
                    else if (!strcmp(tok, "BBS")) str_copy(val, cfg_str(diz_env.cfg, "bbs_name", "NilBBS"), sizeof(val));
                    else if (!strcmp(tok, "SYSOP")) str_copy(val, cfg_str(diz_env.cfg, "sysop_name", "Sysop"), sizeof(val));
                    else if (!strcmp(tok, "DATE")) bbs_datestr(bbs_now(), val);
                    else if (!strcmp(tok, "UPLOADER")) str_copy(val, diz_env.uploader ? diz_env.uploader : "", sizeof(val));
                    else if (!strcmp(tok, "AREA")) str_copy(val, area ? area : "", sizeof(val));
                    else { *d++ = *s++; continue; }
                    d += sprintf(d, "%s", val);
                    s = end + 1;
                    continue;
                }
            }
            *d++ = *s++;
        }
        *d = 0;
        put_line(out, &n, max, expanded, centre);
    }
    while (n > 0 && out[n - 1] == '\n') n--;
    out[n] = 0;
}

