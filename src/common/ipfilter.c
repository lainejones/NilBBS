/*
 * ipfilter.c - who may connect.
 *
 * Three layers, checked in this order:
 *   1. "allow" rules  - a whitelist.  A matching address skips everything
 *                       below (sysop's LAN never gets auto-banned).
 *   2. "deny" rules   - static blocks from Config/IPFilter.cfg.
 *                       With "default deny" anything not allowed is denied.
 *   3. dynamic bans   - added by the sysop (BBSCtl BAN / sysop menu) or
 *                       automatically: a connect flood, or too many failed
 *                       logins.  Timed or permanent; saved to Data/Bans.dat.
 *
 * Rule syntax (one per line, ; or # comments):
 *   default allow|deny
 *   allow 192.168.1.0/24
 *   deny  203.0.113.7
 *   deny  198.51.100.*          (a trailing * octet is a /8, /16 or /24)
 *
 * Every function here expects the caller to hold the shared semaphore.
 */
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "bbs.h"

static ULONG mask_of(UBYTE bits)
{
    return bits ? (0xFFFFFFFFUL << (32 - bits)) : 0;
}

static BOOL rule_match(const struct IPRule *r, ULONG ip)
{
    return (ip & mask_of(r->bits)) == r->net;
}

BOOL ipf_parse_rule(const char *line, struct IPRule *r)
{
    char buf[80], *p, *addr;
    ULONG v = 0;
    int bits = 32, oct;

    str_copy(buf, line, sizeof(buf));
    p = str_trim(buf);
    if (!str_nicmp(p, "allow", 5))      { r->type = IPR_ALLOW; p += 5; }
    else if (!str_nicmp(p, "deny", 4))  { r->type = IPR_DENY;  p += 4; }
    else return FALSE;
    addr = str_trim(p);

    /* dotted quad, possibly with trailing '*' octets or a /len */
    for (oct = 0; oct < 4; oct++) {
        if (*addr == '*') {
            bits = oct * 8;
            v = oct ? v << (8 * (4 - oct)) : 0;     /* "*" alone = everything */
            goto done;
        } else {
            ULONG n = 0;
            int d = 0;
            while (*addr >= '0' && *addr <= '9') { n = n * 10 + (*addr++ - '0'); d++; }
            if (!d || n > 255) return FALSE;
            v = (v << 8) | n;
        }
        if (oct < 3) {
            if (*addr != '.') return FALSE;
            addr++;
        }
    }
    if (*addr == '/') {
        bits = atoi(addr + 1);
        if (bits < 0 || bits > 32) return FALSE;
    }
done:
    r->bits = (UBYTE)bits;
    r->net  = v & mask_of((UBYTE)bits);
    return TRUE;
}

void ipf_rule_str(const struct IPRule *r, char *buf)
{
    char ip[16];
    ip_tostr(r->net, ip);
    sprintf(buf, "%s %s/%d", r->type == IPR_ALLOW ? "allow" : "deny ", ip, (int)r->bits);
}

LONG ipf_load_rules(struct BBSShared *s, const char *path)
{
    struct LineReader lr;
    char line[LINELEN];
    s->nrules = 0;
    s->default_deny = 0;
    if (!lr_open(&lr, path)) return 0;
    while (lr_gets(&lr, line, sizeof(line)) >= 0) {
        char *p = str_trim(line);
        if (!*p || *p == ';' || *p == '#') continue;
        if (!str_nicmp(p, "default", 7)) {
            s->default_deny = str_istr(p + 7, "deny") ? 1 : 0;
            continue;
        }
        if (s->nrules < MAX_IPRULES && ipf_parse_rule(p, &s->rules[s->nrules]))
            s->nrules++;
        else
            bbs_log(BBS_SYSLOG, "IPFilter: bad rule ignored: %s", p);
    }
    lr_close(&lr);
    return s->nrules;
}

BOOL ipf_whitelisted(struct BBSShared *s, ULONG ip)
{
    UWORD i;
    for (i = 0; i < s->nrules; i++)
        if (s->rules[i].type == IPR_ALLOW && rule_match(&s->rules[i], ip)) return TRUE;
    return FALSE;
}

struct IPBan *ipf_findban(struct BBSShared *s, ULONG ip)
{
    UWORD i;
    for (i = 0; i < MAX_BANS; i++)
        if (s->bans[i].ip == ip) return &s->bans[i];
    return NULL;
}

BOOL ipf_ban(struct BBSShared *s, ULONG ip, ULONG mins, const char *reason)
{
    struct IPBan *b = ipf_findban(s, ip);
    ULONG now = bbs_now();
    UWORD i;
    BOOL keep = FALSE;

    if (!ip) return FALSE;
    /* re-banning an address with no new reason keeps the one on file */
    if (b && (!reason || !*reason) && b->reason[0]) keep = TRUE;
    if (!b) {
        for (i = 0; i < MAX_BANS; i++) if (!s->bans[i].ip) { b = &s->bans[i]; break; }
    }
    if (!b) {
        /* table full: evict the timed ban closest to expiry */
        struct IPBan *victim = NULL;
        for (i = 0; i < MAX_BANS; i++)
            if (s->bans[i].expires && (!victim || s->bans[i].expires < victim->expires))
                victim = &s->bans[i];
        if (!victim) return FALSE;
        b = victim;
    }
    b->ip      = ip;
    b->since   = now;
    b->expires = mins ? now + mins * 60 : 0;
    if (!keep) str_copy(b->reason, reason && *reason ? reason : "sysop", sizeof(b->reason));
    s->bans_dirty = 1;
    {   /* the permanent record (System.log gets rotated; this never is) */
        char a[16], dur[24];
        ip_tostr(ip, a);
        if (mins) sprintf(dur, "for %lu min", mins); else strcpy(dur, "permanent");
        bbs_log(BBS_BANLOG, "BAN     %-15s %-13s %s", a, dur, b->reason);
    }
    return TRUE;
}

BOOL ipf_unban(struct BBSShared *s, ULONG ip)
{
    struct IPBan *b = ipf_findban(s, ip);
    UWORD i;
    if (!b) return FALSE;
    {
        char a[16];
        ip_tostr(ip, a);
        bbs_log(BBS_BANLOG, "UNBAN   %-15s               (was: %s)", a, b->reason);
    }
    memset(b, 0, sizeof(*b));
    for (i = 0; i < MAX_RECENT; i++)            /* start the counters over too */
        if (s->recent[i].ip == ip) memset(&s->recent[i], 0, sizeof(s->recent[i]));
    s->bans_dirty = 1;
    return TRUE;
}

int ipf_expire(struct BBSShared *s, ULONG now)
{
    UWORD i;
    int n = 0;
    for (i = 0; i < MAX_BANS; i++) {
        if (s->bans[i].ip && s->bans[i].expires && s->bans[i].expires <= now) {
            char a[16];
            ip_tostr(s->bans[i].ip, a);
            bbs_log(BBS_BANLOG, "EXPIRED %-15s               (was: %s)", a, s->bans[i].reason);
            memset(&s->bans[i], 0, sizeof(s->bans[i]));
            n++;
        }
    }
    if (n) s->bans_dirty = 1;
    return n;
}

static struct IPRecent *recent_slot(struct BBSShared *s, ULONG ip, ULONG now)
{
    struct IPRecent *oldest = &s->recent[0];
    UWORD i;
    for (i = 0; i < MAX_RECENT; i++) {
        if (s->recent[i].ip == ip) return &s->recent[i];
        if (s->recent[i].first < oldest->first) oldest = &s->recent[i];
    }
    memset(oldest, 0, sizeof(*oldest));         /* recycle the stalest entry */
    oldest->ip = ip;
    oldest->first = now;
    return oldest;
}

/* allow/deny rules and bans only - no flood counting (finger uses this) */
int ipf_screen(struct BBSShared *s, ULONG ip, ULONG now)
{
    struct IPBan *b;
    UWORD i;
    BOOL denied = FALSE;

    if (ipf_whitelisted(s, ip)) return IPV_ALLOW;

    for (i = 0; i < s->nrules; i++)
        if (s->rules[i].type == IPR_DENY && rule_match(&s->rules[i], ip)) denied = TRUE;
    if (denied || s->default_deny) return IPV_DENY;

    if ((b = ipf_findban(s, ip))) {
        if (!b->expires || b->expires > now) return IPV_BANNED;
        memset(b, 0, sizeof(*b));
        s->bans_dirty = 1;
    }
    return IPV_ALLOW;
}

int ipf_check(struct BBSShared *s, ULONG ip, ULONG now)
{
    struct IPRecent *r;
    int v = ipf_screen(s, ip, now);

    if (v != IPV_ALLOW || ipf_whitelisted(s, ip)) return v;

    /* connect-flood tracking */
    if (s->flood_conns) {
        r = recent_slot(s, ip, now);
        if (now - r->first > s->flood_secs) { r->first = now; r->conns = 0; }
        r->conns++;
        if (r->conns > s->flood_conns) {
            char why[40];
            sprintf(why, "flood: %u connects/%us", (unsigned)r->conns, (unsigned)s->flood_secs);
            ipf_ban(s, ip, s->flood_ban_mins, why);
            r->conns = 0;
            return IPV_FLOOD;
        }
    }
    return IPV_ALLOW;
}

BOOL ipf_login_failed(struct BBSShared *s, ULONG ip, ULONG now)
{
    struct IPRecent *r;
    if (!s->fail_logins || ipf_whitelisted(s, ip)) return FALSE;
    r = recent_slot(s, ip, now);
    /* failures older than an hour are forgiven */
    if (r->lastfail && now - r->lastfail > 3600) r->fails = 0;
    r->lastfail = now;
    if (++r->fails >= s->fail_logins) {
        char why[40];
        sprintf(why, "%u failed logins", (unsigned)r->fails);
        ipf_ban(s, ip, s->fail_ban_mins, why);
        r->fails = 0;
        return TRUE;
    }
    return FALSE;
}

void ipf_login_ok(struct BBSShared *s, ULONG ip)
{
    UWORD i;
    for (i = 0; i < MAX_RECENT; i++)
        if (s->recent[i].ip == ip) { s->recent[i].fails = 0; s->recent[i].lastfail = 0; }
}

/* Data/Bans.dat:  ip since expires reason...   (text, one per line) */
LONG ipf_load_bans(struct BBSShared *s, const char *path)
{
    struct LineReader lr;
    char line[LINELEN];
    LONG n = 0;
    ULONG now = bbs_now();
    if (!lr_open(&lr, path)) return 0;
    while (lr_gets(&lr, line, sizeof(line)) >= 0) {
        char *f[4];
        ULONG ip;
        struct IPBan *b = NULL;
        UWORD i;
        char *p = str_trim(line);
        if (!*p || *p == '#') continue;
        /* space-separated; reason is the rest of the line */
        f[0] = p;
        if (!(p = strchr(p, ' '))) continue;
        *p++ = 0;
        f[1] = p;
        if (!(p = strchr(p, ' '))) continue;
        *p++ = 0;
        f[2] = p;
        if ((p = strchr(p, ' '))) { *p++ = 0; f[3] = p; } else f[3] = "";
        if (!ip_parse(f[0], &ip)) continue;
        if (strtoul(f[2], NULL, 10) && strtoul(f[2], NULL, 10) <= now) continue; /* expired */
        for (i = 0; i < MAX_BANS; i++) if (!s->bans[i].ip) { b = &s->bans[i]; break; }
        if (!b) break;
        b->ip = ip;
        b->since = strtoul(f[1], NULL, 10);
        b->expires = strtoul(f[2], NULL, 10);
        str_copy(b->reason, f[3], sizeof(b->reason));
        n++;
    }
    lr_close(&lr);
    return n;
}

BOOL ipf_save_bans(struct BBSShared *s, const char *path)
{
    char tmp[PATHLEN], line[128], ip[16];
    BPTR fh;
    UWORD i;
    sprintf(tmp, "%s.new", path);
    if (!(fh = Open((STRPTR)tmp, MODE_NEWFILE))) return FALSE;
    FPuts(fh, (STRPTR)"# NilBBS dynamic bans: ip since expires(0=never) reason\n");
    for (i = 0; i < MAX_BANS; i++) {
        struct IPBan *b = &s->bans[i];
        if (!b->ip) continue;
        ip_tostr(b->ip, ip);
        sprintf(line, "%s %lu %lu %s\n", ip, b->since, b->expires, b->reason);
        FPuts(fh, (STRPTR)line);
    }
    Close(fh);
    DeleteFile((STRPTR)path);
    Rename((STRPTR)tmp, (STRPTR)path);
    s->bans_dirty = 0;
    return TRUE;
}
