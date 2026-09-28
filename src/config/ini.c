/*
 * ini.c - see ini.h.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "bbs.h"
#include "ini.h"

static char *dup_line(const char *s)
{
    LONG n = strlen(s) + 1;
    char *d = AllocVec(n, 0);
    if (d) memcpy(d, s, n);
    return d;
}

struct Ini *ini_load(const char *path)
{
    struct Ini *ini = AllocVec(sizeof(struct Ini), MEMF_CLEAR);
    struct LineReader lr;
    char buf[INI_LINELEN];
    if (!ini) return NULL;
    str_copy(ini->path, path, sizeof(ini->path));
    if (lr_open(&lr, path)) {
        while (ini->n < INI_MAXLINES && lr_gets(&lr, buf, sizeof(buf)) >= 0)
            ini->line[ini->n++] = dup_line(buf);
        lr_close(&lr);
    }
    return ini;
}

void ini_free(struct Ini *ini)
{
    int i;
    if (!ini) return;
    for (i = 0; i < ini->n; i++) if (ini->line[i]) FreeVec(ini->line[i]);
    FreeVec(ini);
}

BOOL ini_save(struct Ini *ini)
{
    char tmp[270];
    BPTR fh;
    int i;
    sprintf(tmp, "%s.new", ini->path);
    if (!(fh = Open((STRPTR)tmp, MODE_NEWFILE))) return FALSE;
    for (i = 0; i < ini->n; i++) {
        FPuts(fh, (STRPTR)ini->line[i]);
        FPutC(fh, '\n');
    }
    Close(fh);
    DeleteFile((STRPTR)ini->path);
    if (!Rename((STRPTR)tmp, (STRPTR)ini->path)) return FALSE;
    ini->dirty = FALSE;
    return TRUE;
}

/* ---- parsing helpers --------------------------------------------------------- */

static BOOL is_section(const char *l, char *name, int size)
{
    while (*l == ' ' || *l == '\t') l++;
    if (*l != '[') return FALSE;
    if (name) {
        const char *e = strchr(l, ']');
        int n = e ? e - l - 1 : (int)strlen(l + 1);
        if (n >= size) n = size - 1;
        memcpy(name, l + 1, n);
        name[n] = 0;
        str_trim(name);
    }
    return TRUE;
}

/* does line l set `key`?  returns a pointer to the value text */
static const char *key_line(const char *l, const char *key)
{
    int kl = strlen(key);
    while (*l == ' ' || *l == '\t') l++;
    if (*l == ';' || *l == '#' || !*l) return NULL;
    if (str_nicmp(l, key, kl)) return NULL;
    l += kl;
    while (*l == ' ' || *l == '\t') l++;
    if (*l != '=') return NULL;
    l++;
    while (*l == ' ' || *l == '\t') l++;
    return l;
}

/* the range of lines [start, end) belonging to a section (NULL = top level) */
static BOOL section_range(struct Ini *ini, const char *sect, int *start, int *end)
{
    int i;
    char name[64];
    if (!sect) {
        *start = 0;
        for (i = 0; i < ini->n && !is_section(ini->line[i], NULL, 0); i++) ;
        *end = i;
        return TRUE;
    }
    for (i = 0; i < ini->n; i++) {
        if (is_section(ini->line[i], name, sizeof(name)) && !str_icmp(name, sect)) {
            *start = i + 1;
            for (i = i + 1; i < ini->n && !is_section(ini->line[i], NULL, 0); i++) ;
            *end = i;
            return TRUE;
        }
    }
    return FALSE;
}

/* ---- values ------------------------------------------------------------------ */

BOOL ini_get(struct Ini *ini, const char *sect, const char *key, char *out, int size)
{
    int s, e, i;
    const char *v = NULL;
    out[0] = 0;
    if (!section_range(ini, sect, &s, &e)) return FALSE;
    for (i = s; i < e; i++) {
        const char *hit = key_line(ini->line[i], key);
        if (hit) v = hit;                   /* last one wins, like cfg.c */
    }
    if (!v) return FALSE;
    str_copy(out, v, size);
    /* drop an inline "  ; comment" the same way cfg.c does */
    {
        char *q;
        BOOL inq = FALSE;
        for (q = out; *q; q++) {
            if (*q == '"') inq = !inq;
            else if (!inq && *q == ';' && (q == out || q[-1] == ' ' || q[-1] == '\t')) { *q = 0; break; }
        }
    }
    str_trim(out);
    {
        int n = strlen(out);
        if (n >= 2 && out[0] == '"' && out[n - 1] == '"') { memmove(out, out + 1, n - 2); out[n - 2] = 0; }
    }
    return TRUE;
}

LONG ini_getint(struct Ini *ini, const char *sect, const char *key, LONG def)
{
    char b[40];
    if (!ini_get(ini, sect, key, b, sizeof(b)) || !b[0]) return def;
    return strtol(b, NULL, 10);
}

BOOL ini_getbool(struct Ini *ini, const char *sect, const char *key, BOOL def)
{
    char b[16];
    if (!ini_get(ini, sect, key, b, sizeof(b)) || !b[0]) return def;
    return b[0] == 'y' || b[0] == 'Y' || b[0] == '1' || b[0] == 't' || b[0] == 'T' || !str_icmp(b, "on");
}

void ini_set(struct Ini *ini, const char *sect, const char *key, const char *val)
{
    int s, e, i, last = -1;
    char buf[INI_LINELEN], comment[INI_LINELEN];
    if (!section_range(ini, sect, &s, &e)) {
        ini_add_section(ini, sect);
        section_range(ini, sect, &s, &e);
    }
    for (i = s; i < e; i++) if (key_line(ini->line[i], key)) last = i;

    comment[0] = 0;
    if (last >= 0) {
        /* keep the old line's inline comment, aligned where it was */
        const char *l = ini->line[last];
        const char *q;
        BOOL inq = FALSE;
        for (q = l; *q; q++) {
            if (*q == '"') inq = !inq;
            else if (!inq && *q == ';' && q > l && (q[-1] == ' ' || q[-1] == '\t') &&
                     strchr(l, '=') && q > strchr(l, '=')) {
                str_copy(comment, q, sizeof(comment));
                break;
            }
        }
    }
    if (last >= 0) {
        /* reuse the old "key   =" prefix so the file's alignment is kept */
        const char *eq = strchr(ini->line[last], '=');
        int pl = eq ? eq - ini->line[last] + 1 : 0;
        if (pl > 0 && pl < 60) {
            memcpy(buf, ini->line[last], pl);
            buf[pl] = 0;
            {
                const char *v0 = eq + 1;
                int sp = 0;
                while (v0[sp] == ' ' || v0[sp] == '\t') sp++;
                strncat(buf, eq + 1, sp ? sp : 0);
                if (!sp) strcat(buf, " ");
            }
            strncat(buf, val, INI_LINELEN - strlen(buf) - 1);
        } else sprintf(buf, "%-15s = %s", key, val);
    } else sprintf(buf, "%-15s = %s", key, val);
    if (comment[0]) {
        int pad = 32 - (int)strlen(buf);
        if (pad < 1) pad = 1;
        while (pad-- > 0 && strlen(buf) < INI_LINELEN - 2) strcat(buf, " ");
        strncat(buf, comment, INI_LINELEN - strlen(buf) - 1);
    }
    if (last >= 0) {
        if (strcmp(ini->line[last], buf)) { ini_replace_line(ini, last, buf); ini->dirty = TRUE; }
        return;
    }
    /* new key: after the last non-blank line of the section */
    for (i = e; i > s && !str_trim(strcpy(comment, ini->line[i - 1]))[0]; i--) ;
    ini_insert_line(ini, i, buf);
    ini->dirty = TRUE;
}

void ini_setint(struct Ini *ini, const char *sect, const char *key, LONG v)
{
    char b[20];
    sprintf(b, "%ld", v);
    ini_set(ini, sect, key, b);
}

void ini_setbool(struct Ini *ini, const char *sect, const char *key, BOOL v)
{
    ini_set(ini, sect, key, v ? "yes" : "no");
}

/* ---- sections ------------------------------------------------------------------- */

int ini_sections(struct Ini *ini, char names[][32], int max)
{
    int i, n = 0;
    for (i = 0; i < ini->n && n < max; i++)
        if (is_section(ini->line[i], names[n], 32)) n++;
    return n;
}

BOOL ini_add_section(struct Ini *ini, const char *name)
{
    char buf[80];
    int s, e;
    if (section_range(ini, name, &s, &e)) return FALSE;
    if (ini->n && ini->line[ini->n - 1][0]) ini_insert_line(ini, ini->n, "");
    sprintf(buf, "[%s]", name);
    ini->dirty = TRUE;
    return ini_insert_line(ini, ini->n, buf);
}

/* a section's block: its [header] and its lines, minus trailing blank lines */
static BOOL block(struct Ini *ini, const char *name, int *b, int *e)
{
    int s;
    if (!section_range(ini, name, &s, e)) return FALSE;
    *b = s - 1;
    return TRUE;
}

BOOL ini_del_section(struct Ini *ini, const char *name)
{
    int b, e;
    if (!block(ini, name, &b, &e)) return FALSE;
    while (e > b) ini_delete_line(ini, b), e--;
    ini->dirty = TRUE;
    return TRUE;
}

BOOL ini_move_section(struct Ini *ini, const char *name, int dir)
{
    char names[128][32], other[32];
    int n = ini_sections(ini, names, 128), i, b, e, ob, oe, j;
    char *save[INI_MAXLINES / 2];
    int cnt;
    for (i = 0; i < n && str_icmp(names[i], name); i++) ;
    if (i >= n || i + dir < 0 || i + dir >= n) return FALSE;
    strcpy(other, names[i + dir]);
    if (dir > 0) { strcpy(other, name); name = names[i + 1]; }  /* always move the later one up */
    if (!block(ini, name, &b, &e) || !block(ini, other, &ob, &oe)) return FALSE;
    /* lift `name`'s block out and insert it where `other`'s starts */
    cnt = e - b;
    if (cnt > (int)(sizeof(save) / sizeof(save[0]))) return FALSE;
    for (j = 0; j < cnt; j++) save[j] = ini->line[b + j];
    memmove(&ini->line[b], &ini->line[e], (ini->n - e) * sizeof(char *));
    ini->n -= cnt;
    memmove(&ini->line[ob + cnt], &ini->line[ob], (ini->n - ob) * sizeof(char *));
    for (j = 0; j < cnt; j++) ini->line[ob + j] = save[j];
    ini->n += cnt;
    ini->dirty = TRUE;
    return TRUE;
}

BOOL ini_rename_section(struct Ini *ini, const char *from, const char *to)
{
    int s, e;
    char buf[80];
    if (!section_range(ini, from, &s, &e)) return FALSE;
    if (str_icmp(from, to) && section_range(ini, to, &s, &e)) return FALSE;   /* taken */
    section_range(ini, from, &s, &e);
    sprintf(buf, "[%s]", to);
    ini_replace_line(ini, s - 1, buf);
    ini->dirty = TRUE;
    return TRUE;
}

/* ---- raw lines ---------------------------------------------------------------------- */

BOOL ini_insert_line(struct Ini *ini, int at, const char *text)
{
    char *d;
    if (ini->n >= INI_MAXLINES || at < 0 || at > ini->n) return FALSE;
    if (!(d = dup_line(text))) return FALSE;
    memmove(&ini->line[at + 1], &ini->line[at], (ini->n - at) * sizeof(char *));
    ini->line[at] = d;
    ini->n++;
    ini->dirty = TRUE;
    return TRUE;
}

void ini_delete_line(struct Ini *ini, int at)
{
    if (at < 0 || at >= ini->n) return;
    FreeVec(ini->line[at]);
    memmove(&ini->line[at], &ini->line[at + 1], (ini->n - at - 1) * sizeof(char *));
    ini->n--;
    ini->dirty = TRUE;
}

void ini_replace_line(struct Ini *ini, int at, const char *text)
{
    char *d;
    if (at < 0 || at >= ini->n || !(d = dup_line(text))) return;
    FreeVec(ini->line[at]);
    ini->line[at] = d;
    ini->dirty = TRUE;
}
