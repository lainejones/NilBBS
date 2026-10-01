/*
 * doorcheck.c - Detect (guess a door's Doors.cfg settings from its program) and Check (will
 * these settings work on this Amiga?) for BBSConfig's Doors page.  dos.library only.
 *
 * Detect reads the program the command starts (up to 1 MB) and looks for what gives the door
 * kind away: an ARexx script (a comment first) is CNet ARexx, or AIM when it talks to
 * AERexxControl; a program that opens AEDoor.library is an AmiExpress /X door; one that says
 * "CNet" is probably a CNet C door; anything else is a CLI door.  It also finds CNet 1.x/2.x
 * backslash screen codes (old_mci), the assign its paths expect (PFILES:, DOORS:, IRONGATE:...),
 * the drop file it reads and the libraries it opens.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>

#include "bbs.h"
#include "doorcheck.h"

#define DC_READMAX (1024L * 1024L)

const char *dc_type_norm(const char *type)
{
    if (!str_icmp(type, "arexx")) return "cnetrexx";
    if (!str_icmp(type, "amiexpress")) return "xim";
    return type;
}

static BOOL exists(const char *path)
{
    BPTR l = Lock((STRPTR)path, ACCESS_READ);
    if (l) { UnLock(l); return TRUE; }
    return FALSE;
}

/* "NAME: target" -> name "NAME" + target */
static BOOL split_assign(const char *assign, char *name, int nsize, char *target, int tsize)
{
    const char *p = assign, *c;
    while (*p == ' ') p++;
    if (!(c = strchr(p, ':')) || c - p + 1 >= nsize) return FALSE;
    memcpy(name, p, c - p); name[c - p] = 0;
    c++;
    while (*c == ' ' || *c == '=') c++;
    str_copy(target, c, tsize);
    str_trim(target);
    return name[0] && target[0];
}

/* a path through the door's own assign when that name isn't mounted here (yet) */
static void via_assign(const char *path, const char *assign, char *out, int size)
{
    char name[40], target[200];
    const char *c = strchr(path, ':');
    str_copy(out, path, size);
    if (!c || !assign || !assign[0] || exists(path)) return;
    if (!split_assign(assign, name, sizeof(name), target, sizeof(target))) return;
    if ((int)strlen(name) != c - path || str_nicmp(path, name, c - path)) return;
    str_copy(out, target, size);
    if (c[1]) {
        int n = strlen(out);
        if (n && out[n - 1] != ':' && out[n - 1] != '/' && n + 1 < size) { out[n++] = '/'; out[n] = 0; }
        str_copy(out + n, c + 1, size - n);
    }
}

/* the first word of the command (quotes allowed), macros not expanded */
static void first_word(const char *cmd, char *out, int size)
{
    int n = 0;
    while (*cmd == ' ') cmd++;
    if (*cmd == '"') {
        cmd++;
        while (*cmd && *cmd != '"' && n < size - 1) out[n++] = *cmd++;
    } else
        while (*cmd && *cmd != ' ' && n < size - 1) out[n++] = *cmd++;
    out[n] = 0;
}

BOOL dc_resolve(const char *command, const char *dir, const char *assign, char *out, int size)
{
    char prog[200], d[256], p[300];
    first_word(command, prog, sizeof(prog));
    out[0] = 0;
    if (!prog[0]) return FALSE;
    if (strchr(prog, ':')) {
        via_assign(prog, assign, p, sizeof(p));
        if (exists(p)) { str_copy(out, p, size); return TRUE; }
        return FALSE;
    }
    if (dir && dir[0] && !strchr(dir, '%')) {       /* relative to the door's directory */
        via_assign(dir, assign, d, sizeof(d));
        str_copy(p, d, sizeof(p));
        AddPart((STRPTR)p, (STRPTR)prog, sizeof(p));
        if (exists(p)) { str_copy(out, p, size); return TRUE; }
    }
    sprintf(p, "C:%.190s", prog);                   /* a command in the path */
    if (exists(p)) { str_copy(out, p, size); return TRUE; }
    if (exists(prog)) { str_copy(out, prog, size); return TRUE; }
    return FALSE;
}

/* ---- looking inside ------------------------------------------------------------------ */

static const char *find_ci(const UBYTE *b, LONG n, const char *s)
{
    LONG i, k = strlen(s);
    for (i = 0; i + k <= n; i++) {
        LONG j;
        for (j = 0; j < k; j++) if (tolower(b[i + j]) != tolower((UBYTE)s[j])) break;
        if (j == k) return (const char *)b + i;
    }
    return NULL;
}

static const char *find_cs(const UBYTE *b, LONG n, const char *s)
{
    LONG i, k = strlen(s);
    for (i = 0; i + k <= n; i++) if (b[i] == (UBYTE)s[0] && !memcmp(b + i, s, k)) return (const char *)b + i;
    return NULL;
}

static const char *sys_names[] = {
    "SYS", "C", "S", "L", "LIBS", "DEVS", "FONTS", "RAM", "T", "ENV", "ENVARC", "CON", "RAW", "NIL",
    "PRT", "PAR", "SER", "BBS", "REXX", "LOCALE", "HELP", "KEYMAPS", "PRINTERS", "CLIPS", "SPEAK",
    "AUX", "PIPE", "WORKBENCH", "SYSTEM", "DH0", "DH1", "DF0", "DF1", "RAD", "HTTP", "HTTPS", "FTP",
    "MAILTO", "NEWS", "TCP", "UDP", "MAIL", "NOTE", "ERROR", "WARNING", "TIME", "DATE", "VER", NULL
};
static const char *sys_libs[] = {
    "exec", "dos", "intuition", "graphics", "gadtools", "utility", "icon", "diskfont", "layers",
    "mathffp", "mathtrans", "mathieeesingbas", "mathieeesingtrans", "mathieeedoubbas",
    "mathieeedoubtrans", "asl", "workbench", "locale", "expansion", "keymap", "iffparse",
    "commodities", "rexxsyslib", "datatypes", "console", "input", "timer", NULL
};

static BOOL in_list(const char *s, const char **list)
{
    int i;
    for (i = 0; list[i]; i++) if (!str_icmp(s, list[i])) return TRUE;
    return FALSE;
}

#define ASCII_ALNUM(c) (((c) >= 'a' && (c) <= 'z') || ((c) >= 'A' && (c) <= 'Z') || ((c) >= '0' && (c) <= '9'))

/* does name end with one of the system libraries?  In a program the name is often glued to
 * code bytes that happen to be printable ("Nu" = RTS): "Nudos", "XONudos", "_Numathffp" */
static BOOL ends_sys(const char *name)
{
    int i, n = strlen(name);
    for (i = 0; sys_libs[i]; i++) {
        int k = strlen(sys_libs[i]);
        if (k <= n && !str_icmp(name + n - k, sys_libs[i])) return TRUE;
    }
    return FALSE;
}

static void find_libs(const UBYTE *b, LONG n, struct DoorGuess *g)
{
    char found[16][32];
    int nf = 0, j, k;
    LONG i;
    for (i = 2; i + 8 <= n && nf < 16; i++) {
        LONG s = i;
        char name[32];
        if (b[i] != '.' || str_nicmp((const char *)b + i, ".library", 8)) continue;
        while (s > 0 && i - s < 24 && (ASCII_ALNUM(b[s - 1]) || b[s - 1] == '_' || b[s - 1] == '-')) s--;
        if (s == i) continue;
        memcpy(name, b + s, i - s); name[i - s] = 0;
        if (ends_sys(name)) continue;
        for (k = 0; k < nf && str_icmp(found[k], name); k++) ;
        if (k == nf) str_copy(found[nf++], name, sizeof(found[0]));
    }
    for (j = 0; j < nf && g->nlibs < DC_MAXLIBS; j++) {     /* "srexxserdev" when "rexxserdev" is there too */
        int n1 = strlen(found[j]);
        BOOL glued = FALSE;
        for (k = 0; k < nf && !glued; k++) {
            int n2 = strlen(found[k]);
            if (k != j && n2 < n1 && !str_icmp(found[j] + n1 - n2, found[k])) glued = TRUE;
        }
        if (!glued) sprintf(g->libs[g->nlibs++], "%.22s.library", found[j]);
    }
}

/* "BBS:..." rather than "WorkBench:BBS/..." when the path is under BBS: */
static void bbs_relative(char *path, int size)
{
    char bbs[256], rest[256];
    BPTR l = Lock((STRPTR)"BBS:", ACCESS_READ);
    int n;
    if (!l) return;
    if (!NameFromLock(l, (STRPTR)bbs, sizeof(bbs))) { UnLock(l); return; }
    UnLock(l);
    n = strlen(bbs);
    if (!str_nicmp(path, bbs, n) && (path[n] == 0 || path[n] == '/')) {
        str_copy(rest, path + n + (path[n] == '/'), sizeof(rest));
        sprintf(path, "BBS:%.*s", size - 5, rest);
    }
}

static BOOL canon(const char *path, char *out, int size)
{
    BPTR l = Lock((STRPTR)path, ACCESS_READ);
    BOOL ok;
    if (!l) return FALSE;
    ok = NameFromLock(l, (STRPTR)out, size);
    UnLock(l);
    if (ok) bbs_relative(out, size);
    return ok;
}

/* the assign the program's own paths expect.  Every NAME:path in it that isn't a system name
 * is a candidate; one only counts when the path really is there under the program's folder
 * or the one above ("PFILES:excal/_excal.docs" -> BBS:PFiles/excal/_excal.docs) - text like
 * "Cargo: 10" never matches a file.  The name with the most such paths wins. */
#define DC_CANDS 24
#define DC_RESTS 4
static void find_assign(const UBYTE *b, LONG n, const char *prog, struct DoorGuess *g)
{
    struct { char name[20], rest[DC_RESTS][64]; int nr; } *c;
    char own[256], up[256], probe[330], cand[20], rest[64];
    int nc = 0, j, r, bestn = 0;
    LONG i;

    str_copy(own, prog, sizeof(own));
    *PathPart((STRPTR)own) = 0;
    if (!canon(own, own, sizeof(own))) return;
    str_copy(up, own, sizeof(up));
    *PathPart((STRPTR)up) = 0;
    if (!(c = AllocVec(sizeof(*c) * DC_CANDS, MEMF_ANY | MEMF_CLEAR))) return;

    for (i = 1; i < n; i++) {
        LONG s = i, k;
        if (b[i] != ':') continue;
        while (s > 0 && i - s < 16 && (ASCII_ALNUM(b[s - 1]) || b[s - 1] == '_')) s--;
        if (i - s < 2 || !((b[s] >= 'a' && b[s] <= 'z') || (b[s] >= 'A' && b[s] <= 'Z'))) continue;
        if (s > 0 && (b[s - 1] >= 0x80 || ASCII_ALNUM(b[s - 1]) || b[s - 1] == '_' || b[s - 1] == 0x5C)) continue;
        memcpy(cand, b + s, i - s); cand[i - s] = 0;
        for (k = 0; cand[k]; k++) cand[k] = toupper((UBYTE)cand[k]);
        if (in_list(cand, sys_names) || !strcmp(cand, "PROGDIR")) continue;
        for (k = 0; k < 63 && i + 1 + k < n && (ASCII_ALNUM(b[i + 1 + k]) || strchr("_./-!#", b[i + 1 + k])); k++)
            rest[k] = b[i + 1 + k];
        rest[k] = 0;
        while (k > 0 && (rest[k - 1] == '/' || rest[k - 1] == '.')) rest[--k] = 0;
        if (!rest[0]) continue;
        for (j = 0; j < nc && strcmp(c[j].name, cand); j++) ;
        if (j == nc) {
            if (nc == DC_CANDS) continue;
            str_copy(c[nc++].name, cand, sizeof(c[0].name));
        }
        for (r = 0; r < c[j].nr && str_icmp(c[j].rest[r], rest); r++) ;
        if (r == c[j].nr && r < DC_RESTS) str_copy(c[j].rest[c[j].nr++], rest, sizeof(c[0].rest));
    }
    for (j = 0; j < nc; j++) {
        int hits[2] = { 0, 0 }, w, k2;
        BOOL glued = FALSE;                     /* "QWPFILES" when "PFILES" is there too */
        for (k2 = 0; k2 < nc && !glued; k2++) {
            int n1 = strlen(c[j].name), n2 = strlen(c[k2].name);
            if (k2 != j && n2 < n1 && !strcmp(c[j].name + n1 - n2, c[k2].name)) glued = TRUE;
        }
        if (glued) continue;
        for (r = 0; r < c[j].nr; r++) {
            sprintf(probe, "%.250s", own); AddPart((STRPTR)probe, (STRPTR)c[j].rest[r], sizeof(probe));
            if (exists(probe)) { hits[0]++; continue; }
            if (up[0]) {
                sprintf(probe, "%.250s", up); AddPart((STRPTR)probe, (STRPTR)c[j].rest[r], sizeof(probe));
                if (exists(probe)) hits[1]++;
            }
        }
        w = hits[1] > hits[0] ? 1 : 0;
        if (hits[w] > bestn) {
            bestn = hits[w];
            sprintf(g->assign, "%s: %.100s", c[j].name, w ? up : own);
        }
    }
    FreeVec(c);
}

BOOL dc_detect(const char *command, const char *dir, const char *assign, struct DoorGuess *g)
{
    BPTR fh;
    UBYTE *b;
    LONG n;
    BOOL hunk, rexx;

    memset(g, 0, sizeof(*g));
    if (!dc_resolve(command, dir, assign, g->program, sizeof(g->program))) return FALSE;
    if (!(fh = Open((STRPTR)g->program, MODE_OLDFILE))) return FALSE;
    if (!(b = AllocVec(DC_READMAX, MEMF_ANY))) { Close(fh); return FALSE; }
    n = Read(fh, b, DC_READMAX);
    Close(fh);
    if (n < 4) { FreeVec(b); return FALSE; }

    hunk = b[0] == 0 && b[1] == 0 && b[2] == 3 && b[3] == 0xF3;
    {
        LONG i = 0;
        while (i < n && (b[i] == ' ' || b[i] == '\t' || b[i] == '\n' || b[i] == '\r')) i++;
        rexx = !hunk && i + 1 < n && b[i] == '/' && b[i + 1] == '*';
    }
    if (rexx) {
        if (find_ci(b, n, "AERexxControl") || find_ci(b, n, "AEREXX")) {
            strcpy(g->type, "aim"); strcpy(g->why, "an ARexx script for AmiExpress (AERexxControl)");
        } else {
            strcpy(g->type, "cnetrexx"); strcpy(g->why, "an ARexx script (CNet ARexx)");
        }
        {   /* CNet 1.x/2.x: backslash + code letter + digit, and no ^Y codes */
            LONG i, bs = 0;
            BOOL ctly = FALSE;
            for (i = 0; i + 2 < n; i++) {
                if (b[i] == 0x19) ctly = TRUE;
                if (b[i] == 0x5C && strchr("cCnNrRfFoOuU@", b[i + 1]) && isdigit(b[i + 2])) bs++;
            }
            g->oldmci = bs >= 3 && !ctly;
        }
    } else if (hunk && (find_cs(b, n, "AEDoor.library") || find_cs(b, n, "AEDoorPort"))) {
        strcpy(g->type, "xim"); strcpy(g->why, "opens AEDoor.library (an AmiExpress /X door)");
    } else if (hunk && find_ci(b, n, "rexxplslib.library")) {
        if (find_ci(b, n, "AERexxControl")) { strcpy(g->type, "aim"); strcpy(g->why, "compiled ARexx (RexxPlus) for AmiExpress"); }
        else { strcpy(g->type, "cnetrexx"); strcpy(g->why, "compiled ARexx (RexxPlus) - a BBS ARexx door"); }
    } else if (hunk && (find_cs(b, n, "CNet") || find_ci(b, n, "C-NET") || find_cs(b, n, "CNET"))) {
        strcpy(g->type, "cnetc"); strcpy(g->why, "a program that mentions CNet (probably a CNet C door)");
        if (find_cs(b, n, "CNet/3") || find_cs(b, n, "CNet 3")) g->cnetver = 3;
    } else if (hunk) {
        strcpy(g->type, "cli"); strcpy(g->why, "an AmigaDOS program (a CLI door)");
    } else {
        strcpy(g->type, "cli"); strcpy(g->why, "not a program or an ARexx script - a script for the Shell?");
    }
    if (!strcmp(g->type, "cli")) {
        if (find_ci(b, n, "DOOR32.SYS")) strcpy(g->dropfile, "door32.sys");
        else if (find_ci(b, n, "DORINFO")) strcpy(g->dropfile, "dorinfo1.def");
        else if (find_ci(b, n, "DOOR.SYS")) strcpy(g->dropfile, "door.sys");
    }
    find_libs(b, n, g);
    find_assign(b, n, g->program, g);
    FreeVec(b);
    return TRUE;
}

/* ---- Check ----------------------------------------------------------------------------- */

static void add(char *report, int size, int *count, const char *line)
{
    int n = strlen(report);
    if (n + (int)strlen(line) + 2 < size) { strcat(report, line); strcat(report, "\n"); }
    (*count)++;
}

static void add_note(char *report, int size, const char *line)
{
    int n = strlen(report);
    if (n + (int)strlen(line) + 2 < size) { strcat(report, line); strcat(report, "\n"); }
}

static BOOL port_up(const char *name)
{
    struct MsgPort *p;
    Forbid();
    p = FindPort((STRPTR)name);
    Permit();
    return p != NULL;
}

/* AEDoor.library's $VER version, 0 = not there */
static int aedoor_version(void)
{
    BPTR fh = Open((STRPTR)"LIBS:AEDoor.library", MODE_OLDFILE);
    UBYTE *buf;
    LONG n, i;
    int v = 0;
    if (!fh) return 0;
    if (!(buf = AllocVec(16384, MEMF_ANY))) { Close(fh); return 1; }
    n = Read(fh, buf, 16384);
    Close(fh);
    /* ours: "AEDoor.library 3.0 (...)" (the ID string); the original: "$VER: AEDoorLib 2.7" */
    for (i = 0; i + 20 < n; i++)
        if (!memcmp(buf + i, "AEDoor", 6)) {
            LONG k;
            for (k = i + 6; k < i + 22 && k < n && buf[k] >= 32 && !isdigit(buf[k]); k++) ;
            if (k < n && isdigit(buf[k]) && atoi((const char *)buf + k) > v) v = atoi((const char *)buf + k);
        }
    FreeVec(buf);
    return v ? v : 1;
}

int dc_check(const char *type, const char *command, const char *dir, const char *assign,
             const char *host, char *report, int size)
{
    struct DoorGuess g;
    char line[320], p[256], name[40], target[200];
    int count = 0, i;
    BOOL net;

    report[0] = 0;
    type = dc_type_norm(type);
    net = !str_icmp(type, "rlogin") || !str_icmp(type, "telnet") || !str_icmp(type, "tcp");
    if (net) {
        if (!host || !host[0]) add(report, size, &count, "No host: set the server this door connects to.");
        return count;
    }
    if (!command || !command[0]) { add(report, size, &count, "No command: what should the door run?"); return count; }
    if (assign && assign[0]) {
        if (!split_assign(assign, name, sizeof(name), target, sizeof(target)))
            add(report, size, &count, "Assign should look like   NAME: folder   (e.g. PFILES: BBS:PFiles).");
        else if (!exists(target)) {
            sprintf(line, "Assign %s: points at %.100s, which doesn't exist.", name, target);
            add(report, size, &count, line);
        }
    }
    if (dir && dir[0] && !strchr(dir, '%')) {
        via_assign(dir, assign, p, sizeof(p));
        if (!exists(p)) { sprintf(line, "Directory %.100s doesn't exist.", dir); add(report, size, &count, line); }
    }
    if (!dc_detect(command, dir, assign, &g)) {
        char w[200];
        first_word(command, w, sizeof(w));
        sprintf(line, "Can't find the program %.100s%s.", w,
                strchr(w, ':') ? "" : " (in the door's directory or C:)");
        add(report, size, &count, line);
        return count;
    }
    if (g.type[0] && str_icmp(g.type, type)) {
        sprintf(line, "Type is %s, but the program looks like %s: %s.", type, g.type, g.why);
        add(report, size, &count, line);
    }
    for (i = 0; i < g.nlibs; i++) {
        sprintf(p, "LIBS:%s", g.libs[i]);
        if (!exists(p)) {       /* a note, not a problem: many doors open a library only for an extra */
            sprintf(line, "Note: it mentions %s, which isn't in LIBS: (if the door won't start, that's why).", g.libs[i]);
            add_note(report, size, line);
        }
    }
    if (!str_icmp(type, "xim")) {
        int v = aedoor_version();
        if (!v) add(report, size, &count, "/X doors need LIBS:AEDoor.library (NilBBS's own is in the archive's Libs).");
        else if (v < 3) add(report, size, &count, "LIBS:AEDoor.library is an old 2.x one - use NilBBS's own (3.x).");
    }
    if (!str_icmp(type, "cnetrexx") || !str_icmp(type, "aim")) {
        if (!port_up("REXX")) add(report, size, &count, "ARexx isn't running (RexxMast).");
        for (i = 0; i < g.nlibs; i++)
            if (!str_icmp(g.libs[i], "rexxplslib.library") && !port_up("RPREXX"))
                add(report, size, &count, "It's compiled with RexxPlus: run RPStart (at startup) first.");
    }
    if (g.assign[0] && (!assign || !assign[0])) {
        split_assign(g.assign, name, sizeof(name), target, sizeof(target));
        sprintf(p, "%s:", name);
        if (!exists(p)) {
            sprintf(line, "Its paths use %s:, which isn't assigned - set Assign to  %.100s", name, g.assign);
            add(report, size, &count, line);
        }
    }
    return count;
}
