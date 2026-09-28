/*
 * acs.c - access condition strings ("ACS") and conferences.
 *
 * An ACS decides who may see a menu item, door, area or conference:
 *
 *   L20        security level 20 or more (a bare number means the same)
 *   FA         access flag A set (flags A..Z, set by the sysop per user)
 *   Gstaff     in access group "staff"
 *   @Sysop     the user with that handle
 *   Cmain      currently in conference MAIN (tag, or its number)
 *   T10        at least 10 minutes left
 *   S          the sysop
 *   !term      not ...
 *
 * Terms separated by spaces must all match; "|" separates alternatives:
 *   "L50 FB | Gstaff"  =  (level 50+ and flag B) or group staff.
 * An empty ACS lets everyone in, and the sysop passes every ACS.
 *
 * Conferences (BBS:Config/Conferences.cfg) group message and file areas,
 * AmiExpress-style: an area with "conf = TAG" only shows inside that
 * conference; an area without one shows in all of them.  No Conferences.cfg
 * means no conferences at all - every area is always visible.
 */
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "node.h"

struct Conf {
    char tag[NAMELEN];
    char name[LONGNAME];
    char acs[64];
};

static struct Conf confs[MAX_CONFS];
static int nconfs;

static BOOL term_ok(const char *t, LONG len)
{
    char arg[NAMELEN];
    BOOL neg = FALSE, ok = FALSE;
    char c;
    if (len > 0 && *t == '!') { neg = TRUE; t++; len--; }
    if (len <= 0) return !neg;
    c = *t;
    if (c >= 'a' && c <= 'z') c -= 32;
    if (len >= NAMELEN) len = NAMELEN - 1;
    memcpy(arg, t + 1, len - 1);
    arg[len - 1] = 0;
    if (c >= '0' && c <= '9') {             /* bare level */
        memcpy(arg, t, len);
        arg[len] = 0;
        ok = N.user.level >= atol(arg);
    } else switch (c) {
    case 'L': ok = N.user.level >= atol(arg); break;
    case 'F': {
        char f = arg[0];
        if (f >= 'a' && f <= 'z') f -= 32;
        ok = f >= 'A' && f <= 'Z' && (N.user.aflags & (1UL << (f - 'A')));
        break;
    }
    case 'G': ok = N.user.group[0] && !str_icmp(N.user.group, arg); break;
    case '@': ok = !str_icmp(N.user.name, arg); break;
    case 'C': {
        int ci = (arg[0] >= '0' && arg[0] <= '9') ? atoi(arg) - 1 : conf_find(arg);
        ok = nconfs && ci == N.user.conf;
        break;
    }
    case 'T': { LONG left = time_left_mins(); ok = left < 0 || left >= atol(arg); break; }
    case 'S': ok = N.sysop; break;
    default:  ok = FALSE;                   /* unknown term: fail closed */
    }
    return neg ? !ok : ok;
}

BOOL acs_check(const char *acs)
{
    const char *p = acs;
    if (!acs || !*acs || N.sysop) return TRUE;
    while (*p) {                            /* each "|" alternative */
        BOOL all = TRUE, any = FALSE;
        while (*p && *p != '|') {
            const char *s;
            while (*p == ' ' || *p == '\t') p++;
            s = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '|') p++;
            if (p > s) { any = TRUE; if (!term_ok(s, p - s)) all = FALSE; }
        }
        if (any && all) return TRUE;
        if (*p == '|') p++;
    }
    return FALSE;
}

/* ---- conferences ---------------------------------------------------------- */

void conf_load(void)
{
    struct Cfg *c = cfg_load("BBS:Config/Conferences.cfg");
    LONG i, n = cfg_sections(c);
    nconfs = 0;
    for (i = 0; i < n && nconfs < MAX_CONFS; i++) {
        const char *tag = cfg_section(c, i);
        struct Conf *cf = &confs[nconfs++];
        str_copy(cf->tag, tag, NAMELEN);
        str_copy(cf->name, cfg_sget(c, tag, "name", tag), LONGNAME);
        str_copy(cf->acs, cfg_sget(c, tag, "acs", ""), sizeof(cf->acs));
    }
    cfg_free(c);
    /* back into the last conference, or the first one this user may enter */
    if (nconfs && (N.user.conf >= nconfs || !acs_check(confs[N.user.conf].acs))) {
        N.user.conf = 0;
        for (i = 0; i < nconfs; i++) if (acs_check(confs[i].acs)) { N.user.conf = (UBYTE)i; break; }
    }
}

int conf_count(void) { return nconfs; }

int conf_find(const char *tag)
{
    int i;
    for (i = 0; i < nconfs; i++) if (!str_icmp(confs[i].tag, tag)) return i;
    return -1;
}

const char *conf_name(void)
{
    return (nconfs && N.user.conf < nconfs) ? confs[N.user.conf].name : "";
}

const char *conf_tag(void)
{
    return (nconfs && N.user.conf < nconfs) ? confs[N.user.conf].tag : "";
}

/* is an area tagged `conf` (may be "" or a comma list) visible right now? */
BOOL conf_visible(const char *conf)
{
    char buf[80], *f[8];
    LONG n, i;
    if (!nconfs || !conf || !*conf) return TRUE;
    str_copy(buf, conf, sizeof(buf));
    n = str_split(buf, ',', f, 8);
    for (i = 0; i < n; i++) if (!str_icmp(str_trim(f[i]), confs[N.user.conf].tag)) return TRUE;
    return FALSE;
}

void conf_join(const char *arg)
{
    char b[6];
    int i, shown = 0;
    if (!nconfs) { tputs(L("acs.conf_join.this_bbs_has", "\n|08This BBS has no conferences - every area is always open.|07\n")); return; }
    if (arg && *arg) {                      /* menu item with a fixed conference */
        i = (arg[0] >= '0' && arg[0] <= '9') ? atoi(arg) - 1 : conf_find(arg);
    } else {
        tputs(L("acs.conf_join.conferences", "\n|09-=[ |15Conferences|09 ]=-|07\n\n"));
        for (i = 0; i < nconfs; i++) {
            if (!acs_check(confs[i].acs)) continue;
            tprintf("  |%s%2d|08) |07%s\n", i == N.user.conf ? "14" : "15", i + 1, confs[i].name);
            shown++;
        }
        if (!shown) { tputs(L("acs.conf_join.none_open_to", "  |08None open to you.|07\n")); return; }
        tputs(L("acs.conf_join.join_conference", "\n|07Join conference: |15"));
        if (tgetline(b, 4, GL_DIGITS) <= 0) return;
        i = atoi(b) - 1;
    }
    if (i < 0 || i >= nconfs || !acs_check(confs[i].acs)) { tputs(L("acs.conf_join.you_cant_join", "|12You can't join that one.|07\n")); return; }
    N.user.conf = (UBYTE)i;
    user_save();
    msg_areas_load();                       /* pick a current area inside it */
    file_areas_load();
    tprintf(L("acs.conf_join.joined", "|07Joined |15%s|07.\n"), confs[i].name);
    {
        char path[PATHLEN];
        sprintf(path, "conf_%s", confs[i].tag);   /* Text/conf_<TAG>.ans, if any */
        tshowfile(path);
    }
}
