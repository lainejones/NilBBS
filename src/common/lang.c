/*
 * lang.c - language files: BBS:Text/Language/<name>.lng
 *
 *   ; comment            # comment too
 *   key = text
 *   msg.list.head = "  |15%d|07 messages, |15%d|07 new  "
 *
 * The text runs to the end of the line; surrounding blanks are dropped unless
 * the text is wrapped in double quotes.  Escapes: \n new line, \t tab, \\ \" and
 * \xHH (a raw byte of the internal CP437 set).  |07 colour/MCI codes and printf
 * conversions are kept as written.  The file may be UTF-8 or Latin-1 (a byte
 * that isn't part of a UTF-8 sequence counts as Latin-1).
 *
 * The file is read into one AllocVec'd block and decoded in place (decoding never
 * makes text longer); a second block holds the entries and a hash table.
 * Checking a translation's printf conversions against the English happens the
 * first time L() hands it out (the English isn't known before that).
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>

#include "bbs.h"
#include "lang.h"

#define HASH_SIZE 512                   /* power of two */

struct LangEnt {
    const char *key;
    const char *text;
    WORD  next;                         /* next entry in the bucket, -1 = end */
    UBYTE state;                        /* 0 = not checked yet, 1 = good, 2 = bad printf */
    UBYTE pad;
};

static char *g_buf;                     /* the file, decoded in place */
static struct LangEnt *g_ent;           /* g_ent[0..g_n-1], then the hash heads */
static WORD *g_hash;
static LONG g_n;
static char g_name[LANG_NAMELEN] = LANG_ENGLISH;

static ULONG hash_key(const char *s)
{
    ULONG h = 5381;
    while (*s) h = h * 33 + (UBYTE)*s++;
    return h & (HASH_SIZE - 1);
}

void lang_free(void)
{
    if (g_buf) FreeVec(g_buf);
    if (g_ent) FreeVec(g_ent);
    g_buf = NULL; g_ent = NULL; g_hash = NULL; g_n = 0;
    strcpy(g_name, LANG_ENGLISH);
}

const char *lang_current(void) { return g_name; }
LONG lang_count(void) { return g_n; }

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* decode the text in s[0..len) in place, 0-terminated; returns the end */
static char *decode(char *s, LONG len, UBYTE (*map)(UWORD))
{
    UBYTE *p = (UBYTE *)s, *e = p + len, *o = p;
    while (p < e) {
        UBYTE c = *p++;
        if (c == '\\' && p < e) {
            UBYTE d = *p++;
            if (d == 'n') *o++ = '\n';
            else if (d == 't') *o++ = '\t';
            else if (d == 'x' && p < e && hexval(*p) >= 0) {
                int v = hexval(*p++);
                if (p < e && hexval(*p) >= 0) v = v * 16 + hexval(*p++);
                if (v) *o++ = (UBYTE)v;         /* \x00 would end the text: dropped */
            }
            else *o++ = d;                      /* \\ \" and anything else: the character */
            continue;
        }
        if (c >= 0x80) {
            UWORD u = c;                        /* Latin-1 unless it is valid UTF-8 */
            int more = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : 0;
            if (more && p + more <= e) {
                UWORD v = c & (more == 1 ? 0x1F : 0x0F);
                int i;
                BOOL ok = TRUE;
                for (i = 0; i < more; i++) {
                    if ((p[i] & 0xC0) != 0x80) { ok = FALSE; break; }
                    v = (UWORD)((v << 6) | (p[i] & 0x3F));
                }
                if (ok && v >= 0x80) { u = v; p += more; }
            }
            *o++ = map ? map(u) : (u < 256 ? (UBYTE)u : '?');
            continue;
        }
        *o++ = c;
    }
    *o = 0;
    return (char *)o;
}

static BOOL key_char(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '.' || c == '_' || c == '-';
}

BOOL lang_exists(const char *name)
{
    char path[PATHLEN];
    if (!name[0] || !str_icmp(name, LANG_ENGLISH)) return TRUE;
    if (strlen(name) >= LANG_NAMELEN || strchr(name, '/') || strchr(name, ':')) return FALSE;
    sprintf(path, LANG_DIR "/%s.lng", name);
    return file_exists(path);
}

BOOL lang_load(const char *name, UBYTE (*map)(UWORD))
{
    char path[PATHLEN], *p, *end;
    LONG size, got, lines = 0, n = 0;
    BPTR fh;

    lang_free();
    if (!name || !name[0] || !str_icmp(name, LANG_ENGLISH)) return FALSE;
    if (strlen(name) >= LANG_NAMELEN || strchr(name, '/') || strchr(name, ':')) return FALSE;
    sprintf(path, LANG_DIR "/%s.lng", name);
    if ((size = file_size(path)) <= 0 || size > 1024 * 1024) return FALSE;
    if (!(g_buf = AllocVec(size + 1, MEMF_ANY))) return FALSE;
    if (!(fh = Open((STRPTR)path, MODE_OLDFILE))) { lang_free(); return FALSE; }
    got = Read(fh, g_buf, size);
    Close(fh);
    if (got <= 0) { lang_free(); return FALSE; }
    g_buf[got] = 0;
    end = g_buf + got;
    for (p = g_buf; p < end; p++) if (*p == '\n') lines++;
    lines++;
    if (lines > 30000) lines = 30000;           /* entry links are WORDs */
    g_ent = AllocVec(lines * sizeof(struct LangEnt) + HASH_SIZE * sizeof(WORD), MEMF_ANY);
    if (!g_ent) { lang_free(); return FALSE; }
    g_hash = (WORD *)(g_ent + lines);
    memset(g_hash, 0xFF, HASH_SIZE * sizeof(WORD));

    p = g_buf;
    if (got >= 3 && (UBYTE)p[0] == 0xEF && (UBYTE)p[1] == 0xBB && (UBYTE)p[2] == 0xBF) p += 3;  /* BOM */
    while (p < end && n < lines) {
        char *line = p, *le, *k, *ke, *v, *ve;
        while (p < end && *p != '\n') p++;
        le = p;
        if (p < end) p++;
        while (le > line && (le[-1] == '\r' || le[-1] == ' ' || le[-1] == '\t')) le--;
        k = line;
        while (k < le && (*k == ' ' || *k == '\t')) k++;
        if (k >= le || *k == ';' || *k == '#') continue;
        ke = k;
        while (ke < le && key_char((UBYTE)*ke)) ke++;
        v = ke;
        while (v < le && (*v == ' ' || *v == '\t')) v++;
        if (ke == k || v >= le || *v != '=') continue;      /* not "key = text" */
        v++;
        while (v < le && (*v == ' ' || *v == '\t')) v++;
        ve = le;
        if (ve - v >= 2 && *v == '"' && ve[-1] == '"') { v++; ve--; }
        *ke = 0;                                /* ke < v, so this can't clobber the text */
        decode(v, ve - v, map);
        {
            ULONG h = hash_key(k);
            g_ent[n].key = k;
            g_ent[n].text = v;
            g_ent[n].state = 0;
            g_ent[n].next = g_hash[h];
            g_hash[h] = (WORD)n;
            n++;
        }
    }
    g_n = n;
    if (!n) { lang_free(); return FALSE; }
    str_copy(g_name, name, sizeof(g_name));
    return TRUE;
}

/* the next printf conversion in *pp as a short code ("s", "ld", "*d", "?" = invalid),
   written to out; FALSE when there are no more */
static BOOL next_conv(const char **pp, char *out)
{
    const char *p = *pp;
    int n = 0;
    for (;;) {
        while (*p && *p != '%') p++;
        if (!*p) { *pp = p; return FALSE; }
        p++;
        if (*p == '%') { p++; continue; }
        break;
    }
    while (*p && strchr("-+ #0'", *p)) p++;
    if (*p == '*') { out[n++] = '*'; p++; } else while (*p >= '0' && *p <= '9') p++;
    if (*p == '.') {
        p++;
        if (*p == '*') { out[n++] = '*'; p++; } else while (*p >= '0' && *p <= '9') p++;
    }
    while (*p && strchr("hlLqjzt", *p) && n < 5) out[n++] = *p++;
    if (*p && strchr("diouxXcspeEfgGaA", *p)) out[n++] = *p++;
    else { out[n++] = '?'; if (*p) p++; }
    out[n] = 0;
    *pp = p;
    return TRUE;
}

BOOL lang_fmt_match(const char *a, const char *b)
{
    char ca[8], cb[8];
    for (;;) {
        BOOL ha = next_conv(&a, ca), hb = next_conv(&b, cb);
        if (ha != hb) return FALSE;
        if (!ha) return TRUE;
        if (strcmp(ca, cb) || ca[0] == '?') return FALSE;
    }
}

const char *L(const char *key, const char *english)
{
    WORD i;
    if (!g_n) return english;
    for (i = g_hash[hash_key(key)]; i >= 0; i = g_ent[i].next) {
        struct LangEnt *e = &g_ent[i];
        if (strcmp(e->key, key)) continue;
        if (!e->state) e->state = lang_fmt_match(english, e->text) ? 1 : 2;
        return e->state == 1 ? e->text : english;
    }
    return english;
}

int lang_list(char names[][LANG_NAMELEN], int max)
{
    struct FileInfoBlock *fib;
    BPTR lock;
    int n = 0, i, j;
    BOOL english = FALSE;
    if (max <= 0) return 0;
    if ((lock = Lock((STRPTR)LANG_DIR, ACCESS_READ))) {
        if ((fib = AllocDosObject(DOS_FIB, NULL))) {
            if (Examine(lock, fib)) {
                while (ExNext(lock, fib) && n < max) {
                    char *nm = (char *)fib->fib_FileName;
                    LONG len = strlen(nm);
                    if (fib->fib_DirEntryType >= 0 || len < 5 || str_icmp(nm + len - 4, ".lng")) continue;
                    if (len - 4 >= LANG_NAMELEN) continue;
                    memcpy(names[n], nm, len - 4);
                    names[n][len - 4] = 0;
                    if (!str_icmp(names[n], LANG_ENGLISH)) english = TRUE;
                    n++;
                }
            }
            FreeDosObject(DOS_FIB, fib);
        }
        UnLock(lock);
    }
    if (!english) {                             /* built in, file or not */
        if (n >= max) n = max - 1;
        strcpy(names[n++], LANG_ENGLISH);
    }
    for (i = 1; i < n; i++)                     /* a handful of names: insertion sort */
        for (j = i; j > 0 && str_icmp(names[j - 1], names[j]) > 0; j--) {
            char t[LANG_NAMELEN];
            strcpy(t, names[j]); strcpy(names[j], names[j - 1]); strcpy(names[j - 1], t);
        }
    return n;
}
