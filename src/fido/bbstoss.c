/*
 * BBSToss - FidoNet echomail for NilBBS.
 *
 *   BBSToss [TOSS] [SCAN]        (no arguments = both: toss, then scan)
 *
 * TOSS  imports every Type 2+ packet (*.pkt) in the inbound directory into
 *       the message area whose echotag matches its AREA: line.  Netmail to a
 *       local user lands in private mail.  Duplicates (by MSGID) are dropped.
 *       Bundles (*.su0, *.mo1, ...) are unpacked first with `unpack`.
 * SCAN  exports new local posts in echo areas as one packet to the uplink,
 *       in a BinkleyTerm-style outbound (<net><node>.out) that binkd and
 *       similar binkp mailers pick up.
 *
 * BBS:Config/Fido.cfg:
 *   address  = 21:4/101           ; this system
 *   uplink   = 21:4/100           ; the hub you exchange mail with
 *   password =                     ; packet password (up to 8 chars)
 *   inbound  = BBS:Fido/Inbound
 *   outbound = BBS:Fido/Outbound
 *   origin   = My NilBBS - telnet bbs.example.org
 *   unpack   = UnZip -o -qq %a -d %d   ; for bundles (%a archive, %d dir)
 *
 * Echo areas are the message areas with  type = echo  and  echotag = NAME.
 * Run it from cron / your mailer's after-session script.
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

#include "../common/bbs.h"
#include "../common/cfg.h"
#include "../common/msgbase.h"

static const char __attribute__((used)) verstag[] = "$VER: BBSToss " BBS_VERSION " (" BBS_VERDATE ")";

struct FtnAddr { UWORD zone, net, node, point; };

struct EchoArea { char tag[NAMELEN]; char echotag[NAMELEN]; UBYTE type; };
#define AT_LOCAL 0
#define AT_ECHO  1
#define AT_EMAIL 2

static struct EchoArea areas[MAX_MSGAREAS];
static int nareas, email_area = -1;
static struct FtnAddr me, uplink;
static char password[9], origin[80], inbound[PATHLEN], outbound[PATHLEN], unpack[PATHLEN];
static struct BBSShared *S;

#define DUPES 4096
static ULONG dupes[DUPES];
static LONG dupe_next;
static BOOL dupes_dirty;
static ULONG crctab[256];

/* ---- helpers ---------------------------------------------------------------- */

static void mkcrc(void)
{
    int i, j;
    for (i = 0; i < 256; i++) {
        ULONG c = i;
        for (j = 0; j < 8; j++) c = (c & 1) ? (c >> 1) ^ 0xEDB88320UL : c >> 1;
        crctab[i] = c;
    }
}

static ULONG crc32s(const char *s)
{
    ULONG c = 0xFFFFFFFFUL;
    while (*s) c = crctab[(c ^ (UBYTE)*s++) & 0xFF] ^ (c >> 8);
    return ~c;
}

static BOOL parse_addr(const char *s, struct FtnAddr *a)
{
    memset(a, 0, sizeof(*a));
    if (!strchr(s, ':') || !strchr(s, '/')) return FALSE;
    a->zone = (UWORD)atoi(s);
    a->net  = (UWORD)atoi(strchr(s, ':') + 1);
    a->node = (UWORD)atoi(strchr(s, '/') + 1);
    if (strchr(s, '.')) a->point = (UWORD)atoi(strchr(s, '.') + 1);
    return TRUE;
}

static void addr_str(const struct FtnAddr *a, char *buf)
{
    if (a->point) sprintf(buf, "%u:%u/%u.%u", a->zone, a->net, a->node, a->point);
    else sprintf(buf, "%u:%u/%u", a->zone, a->net, a->node);
}

static LONG strnlen(const char *s, LONG max)
{
    LONG n = 0;
    while (n < max && s[n]) n++;
    return n;
}

static UWORD rd16(const UBYTE *p) { return (UWORD)(p[0] | (p[1] << 8)); }
static void wr16(UBYTE *p, UWORD v) { p[0] = (UBYTE)v; p[1] = (UBYTE)(v >> 8); }

static void load_areas(void)
{
    struct Cfg *c = cfg_load("BBS:Config/MsgAreas.cfg");
    LONG i, n = cfg_sections(c);
    for (i = 0; i < n && nareas < MAX_MSGAREAS; i++) {
        const char *tag = cfg_section(c, i), *type = cfg_sget(c, tag, "type", "local");
        struct EchoArea *a = &areas[nareas++];
        str_copy(a->tag, tag, NAMELEN);
        str_copy(a->echotag, cfg_sget(c, tag, "echotag", tag), NAMELEN);
        a->type = !str_icmp(type, "echo") ? AT_ECHO : !str_icmp(type, "email") ? AT_EMAIL : AT_LOCAL;
        if (a->type == AT_EMAIL && email_area < 0) email_area = nareas - 1;
    }
    cfg_free(c);
}

static void load_dupes(void)
{
    BPTR fh = Open((STRPTR)"BBS:Data/FidoDupes.dat", MODE_OLDFILE);
    if (fh) {
        Read(fh, &dupe_next, 4);
        Read(fh, dupes, sizeof(dupes));
        Close(fh);
        if (dupe_next < 0 || dupe_next >= DUPES) dupe_next = 0;
    }
}

static void save_dupes(void)
{
    BPTR fh;
    if (!dupes_dirty) return;
    if ((fh = Open((STRPTR)"BBS:Data/FidoDupes.dat", MODE_NEWFILE))) {
        Write(fh, &dupe_next, 4);
        Write(fh, dupes, sizeof(dupes));
        Close(fh);
    }
}

static BOOL is_dupe(const char *msgid)
{
    ULONG h = crc32s(msgid);
    LONG i;
    for (i = 0; i < DUPES; i++) if (dupes[i] == h) return TRUE;
    dupes[dupe_next] = h;
    dupe_next = (dupe_next + 1) % DUPES;
    dupes_dirty = TRUE;
    return FALSE;
}

/* "22 Sep 26  17:45:00" -> bbs time (local, like everything else) */
static ULONG ftn_date(const char *s)
{
    static const char *mon = "JanFebMarAprMayJunJulAugSepOctNovDec";
    int d = atoi(s), m, y, hh = 0, mm = 0, ss = 0, i;
    ULONG days = 0;
    const char *p = strchr(s, ' ');
    if (!p) return bbs_now();
    for (m = 0; m < 12; m++) if (!strncmp(p + 1, mon + m * 3, 3)) break;
    if (m == 12) return bbs_now();
    y = atoi(p + 5);
    y += (y < 77) ? 2000 : 1900;
    p = strchr(p + 5, ' ');
    if (p) sscanf(p, " %d:%d:%d", &hh, &mm, &ss);
    for (i = 1978; i < y; i++) days += ((i % 4 == 0 && i % 100 != 0) || i % 400 == 0) ? 366 : 365;
    {
        static const UBYTE md[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
        for (i = 0; i < m; i++) days += md[i] + (i == 1 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0));
    }
    days += d - 1;
    return days * 86400UL + hh * 3600UL + mm * 60UL + ss;
}

static void ftn_datestr(ULONG t, char *buf)
{
    char d[16], tm[8];
    bbs_datestr(t, d);                          /* "22-Sep-26" */
    bbs_timestr(t, tm);
    sprintf(buf, "%c%c %c%c%c %c%c  %s:%02lu", d[0], d[1], d[3], d[4], d[5], d[7], d[8], tm, t % 60);
}

/* ---- toss -------------------------------------------------------------------- */

static LONG tossed, dupecount, bad;

static int find_echo(const char *tag)
{
    int i;
    for (i = 0; i < nareas; i++)
        if (areas[i].type == AT_ECHO && !str_icmp(areas[i].echotag, tag)) return i;
    return -1;
}

static void store(int ai, struct MsgHdr *h, char *text, LONG len)
{
    ObtainSemaphore(&S->msglock);
    msg_add(areas[ai].tag, h, text, len);
    ReleaseSemaphore(&S->msglock);
}

/* one packed message; returns bytes consumed or 0 at the end / on garbage */
static LONG toss_msg(UBYTE *p, LONG left, const struct FtnAddr *pktorig)
{
    struct MsgHdr h;
    UBYTE *q, *end = p + left;
    char *dt, *to, *from, *subj, *body, *out, *o;
    char area[NAMELEN], msgid[80];
    int ai = -1;
    LONG blen;

    if (left < 14 || rd16(p) != 2) return 0;
    q = p + 14;
    dt = (char *)q;   q += strnlen((char *)q, end - q) + 1; if (q >= end) return 0;
    to = (char *)q;   q += strnlen((char *)q, end - q) + 1; if (q >= end) return 0;
    from = (char *)q; q += strnlen((char *)q, end - q) + 1; if (q >= end) return 0;
    subj = (char *)q; q += strnlen((char *)q, end - q) + 1; if (q > end) return 0;
    body = (char *)q; blen = strnlen((char *)q, end - q); q += blen + 1;

    memset(&h, 0, sizeof(h));
    str_copy(h.to, to, sizeof(h.to));
    str_copy(h.from, from, sizeof(h.from));
    str_copy(h.subject, subj, sizeof(h.subject));
    h.date = ftn_date(dt);
    h.flags = MF_IMPORTED | MF_SENT;
    {
        struct FtnAddr oa = *pktorig;
        oa.node = rd16(p + 2);
        oa.net = rd16(p + 6);
        addr_str(&oa, h.origaddr);
    }

    if (!(out = AllocVec(blen + 2, 0))) return q - p;
    area[0] = 0; msgid[0] = 0;
    o = out;
    {
        char *line = body, *bend = body + blen;
        BOOL first = TRUE;
        while (line < bend) {
            char *e = line;
            LONG ll;
            while (e < bend && *e != '\r' && *e != '\n') e++;
            ll = e - line;
            if (first && ll > 5 && !strncmp(line, "AREA:", 5)) {
                LONG n = ll - 5 > NAMELEN - 1 ? NAMELEN - 1 : ll - 5;
                memcpy(area, line + 5, n); area[n] = 0;
                str_trim(area);
            } else if (ll > 7 && line[0] == 1 && !strncmp(line + 1, "MSGID:", 6)) {
                LONG n = ll - 7 > 79 ? 79 : ll - 7;
                memcpy(msgid, line + 7, n); msgid[n] = 0;
                memcpy(o, line, ll); o += ll; *o++ = '\n';      /* keep kludges (hidden) */
            } else if (ll >= 8 && !strncmp(line, "SEEN-BY:", 8)) {
                /* dropped: we are a leaf, it's only useful for routing */
            } else {
                /* soft CR (0x8D) is a CP437 character we don't want */
                LONG i;
                for (i = 0; i < ll; i++) *o++ = (line[i] == (char)0x8D) ? ' ' : line[i];
                *o++ = '\n';
            }
            first = FALSE;
            if (e < bend && *e == '\r' && e + 1 < bend && e[1] == '\n') e++;
            line = e + 1;
        }
    }
    if (!msgid[0]) sprintf(msgid, "%s %s %s %s", h.origaddr, h.from, h.subject, dt);

    if (is_dupe(msgid)) { dupecount++; FreeVec(out); return q - p; }

    if (area[0]) {
        ai = find_echo(area);
        if (ai < 0) {
            bbs_log(BBS_SYSLOG, "BBSToss: no area for echo %s - message dropped", area);
            bad++;
        }
    } else if (email_area >= 0 && S) {
        /* netmail: deliver to a local user by name */
        struct UserRec u;
        LONG id;
        ObtainSemaphore(&S->userlock);
        id = userdb_find(h.to, &u);
        ReleaseSemaphore(&S->userlock);
        if (id) { h.toid = id; h.flags |= MF_PRIVATE; ai = email_area; }
        else { bbs_log(BBS_SYSLOG, "BBSToss: netmail for unknown user %s dropped", h.to); bad++; }
    }
    if (ai >= 0) { store(ai, &h, out, o - out); tossed++; }
    FreeVec(out);
    return q - p;
}

static BOOL toss_packet(const char *path)
{
    LONG size = file_size(path), n, pos;
    UBYTE *buf;
    BPTR fh;
    struct FtnAddr orig;
    if (size < 60) return FALSE;
    if (!(buf = AllocVec(size + 1, 0))) return FALSE;
    if (!(fh = Open((STRPTR)path, MODE_OLDFILE))) { FreeVec(buf); return FALSE; }
    n = Read(fh, buf, size);
    Close(fh);
    buf[n > 0 ? n : 0] = 0;
    if (n != size || rd16(buf + 18) != 2) { FreeVec(buf); return FALSE; }
    memset(&orig, 0, sizeof(orig));
    orig.node = rd16(buf + 0);
    orig.net  = rd16(buf + 20);
    orig.zone = rd16(buf + 46) ? rd16(buf + 46) : rd16(buf + 34);
    if (password[0] && strncmp((char *)buf + 26, password, 8) && str_nicmp((char *)buf + 26, password, strlen(password))) {
        bbs_log(BBS_SYSLOG, "BBSToss: bad password in %s", path);
        FreeVec(buf);
        return FALSE;
    }
    pos = 58;
    while (pos + 2 <= size) {
        LONG used;
        if (rd16(buf + pos) == 0) break;            /* end of packet */
        used = toss_msg(buf + pos, size - pos, &orig);
        if (!used) break;
        pos += used;
    }
    FreeVec(buf);
    return TRUE;
}

/* run `unpack` on bundles, then toss every .pkt */
static void do_toss(void)
{
    struct AnchorPath *ap = AllocVec(sizeof(struct AnchorPath) + PATHLEN, MEMF_CLEAR);
    char pat[PATHLEN];
    LONG err;
    if (!ap) return;
    ap->ap_Strlen = PATHLEN;

    if (unpack[0]) {
        static const char *days[] = { "su", "mo", "tu", "we", "th", "fr", "sa" };
        int d;
        for (d = 0; d < 7; d++) {
            sprintf(pat, "%s/#?.%s?", inbound, days[d]);
            for (err = MatchFirst((STRPTR)pat, ap); !err; err = MatchNext(ap)) {
                char cmd[PATHLEN * 3], *c = cmd;
                const char *s = unpack;
                while (*s && c < cmd + sizeof(cmd) - PATHLEN) {
                    if (s[0] == '%' && s[1] == 'a') { c += sprintf(c, "\"%s\"", (char *)ap->ap_Buf); s += 2; }
                    else if (s[0] == '%' && s[1] == 'd') { c += sprintf(c, "\"%s\"", inbound); s += 2; }
                    else *c++ = *s++;
                }
                *c = 0;
                if (SystemTags((STRPTR)cmd, TAG_END) == 0) DeleteFile(ap->ap_Buf);
                else bbs_log(BBS_SYSLOG, "BBSToss: unpack failed: %s", cmd);
            }
            MatchEnd(ap);
        }
    }

    sprintf(pat, "%s/#?.pkt", inbound);
    for (err = MatchFirst((STRPTR)pat, ap); !err; err = MatchNext(ap)) {
        char done[PATHLEN];
        if (toss_packet((char *)ap->ap_Buf)) DeleteFile(ap->ap_Buf);
        else {
            sprintf(done, "%s.bad", (char *)ap->ap_Buf);
            Rename(ap->ap_Buf, (STRPTR)done);
            bbs_log(BBS_SYSLOG, "BBSToss: bad packet renamed to %s", done);
        }
    }
    MatchEnd(ap);
    FreeVec(ap);
}

/* ---- scan / export --------------------------------------------------------------- */

static BPTR open_outpkt(char *path)
{
    BPTR fh;
    sprintf(path, "%s/%04x%04x.out", outbound, uplink.net, uplink.node);
    if ((fh = Open((STRPTR)path, MODE_OLDFILE))) {
        /* append to the packet already waiting: drop its 0x0000 terminator */
        LONG end;
        Seek(fh, 0, OFFSET_END);
        end = Seek(fh, 0, OFFSET_END);
        if (end >= 60) { Seek(fh, end - 2, OFFSET_BEGINNING); return fh; }
        Close(fh);
    }
    if (!(fh = Open((STRPTR)path, MODE_NEWFILE))) return 0;
    {
        UBYTE hdr[58];
        char d[16];
        int m;
        static const char *mon = "JanFebMarAprMayJunJulAugSepOctNovDec";
        ULONG now = bbs_now();
        memset(hdr, 0, sizeof(hdr));
        bbs_datestr(now, d);
        for (m = 0; m < 12; m++) if (!strncmp(mon + m * 3, d + 3, 3)) break;
        wr16(hdr + 0, me.node);
        wr16(hdr + 2, uplink.node);
        wr16(hdr + 4, (UWORD)(2000 + atoi(d + 7)));
        wr16(hdr + 6, (UWORD)m);
        wr16(hdr + 8, (UWORD)atoi(d));
        wr16(hdr + 10, (UWORD)((now % 86400) / 3600));
        wr16(hdr + 12, (UWORD)((now % 3600) / 60));
        wr16(hdr + 14, (UWORD)(now % 60));
        wr16(hdr + 18, 2);
        wr16(hdr + 20, me.net);
        wr16(hdr + 22, uplink.net);
        hdr[24] = 0xFE;                               /* product code: "no FTSC code" */
        hdr[25] = 0;
        strncpy((char *)hdr + 26, password, 8);
        wr16(hdr + 34, me.zone);
        wr16(hdr + 36, uplink.zone);
        hdr[40] = 0x00; hdr[41] = 0x01;               /* capability word, byte-swapped copy */
        wr16(hdr + 44, 0x0001);                       /* Type 2+ */
        wr16(hdr + 46, me.zone);
        wr16(hdr + 48, uplink.zone);
        wr16(hdr + 50, me.point);
        wr16(hdr + 52, uplink.point);
        Write(fh, hdr, 58);
    }
    return fh;
}

static LONG exported;

static void export_area(int ai, BPTR *fh, char *path)
{
    struct MsgHdr h;
    struct MsgScan ms;
    LONG total, i;
    char *text = AllocVec(MAX_MSGTEXT + 1024, 0);
    char myaddr[24];
    if (!text) return;
    msg_scan_init(&ms, areas[ai].tag);          /* headers 16 per read */
    addr_str(&me, myaddr);
    ObtainSemaphore(&S->msglock);
    total = msg_count(areas[ai].tag);
    ReleaseSemaphore(&S->msglock);
    for (i = 1; i <= total; i++) {
        UBYTE mh[14];
        char dt[24], *t;
        LONG len;
        BOOL ok;
        ObtainSemaphore(&S->msglock);
        ok = msg_scan_hdr(&ms, i, &h);
        if (ok) len = msg_read_text(areas[ai].tag, &h, text + 512, MAX_MSGTEXT);
        ReleaseSemaphore(&S->msglock);
        if (!ok || !(h.flags & MF_LOCAL) || (h.flags & (MF_SENT | MF_DELETED | MF_HELD))) continue;   /* held: once approved */

        if (!*fh && !(*fh = open_outpkt(path))) break;
        memset(mh, 0, sizeof(mh));
        wr16(mh + 0, 2);
        wr16(mh + 2, me.node);
        wr16(mh + 4, uplink.node);
        wr16(mh + 6, me.net);
        wr16(mh + 8, uplink.net);
        Write(*fh, mh, 14);
        ftn_datestr(h.date, dt);
        Write(*fh, dt, strlen(dt) + 1);
        Write(*fh, h.to, strlen(h.to) + 1);
        Write(*fh, h.from, strlen(h.from) + 1);
        Write(*fh, h.subject, strlen(h.subject) + 1);

        /* body: AREA, kludges, the text (LF -> CR), tear, origin, seen-by, path */
        t = text;
        {
            char id[64];
            sprintf(id, "%s %08lx", myaddr, crc32s(h.subject) ^ h.date ^ i);
            is_dupe(id);                        /* never re-import our own echo */
            t += sprintf(t, "AREA:%s\r\001MSGID: %s\r\001PID: NilBBS %s\r\001CHRS: CP437 2\r",
                         areas[ai].echotag, id, BBS_VERSION);
        }
        {
            char *src = text + 512, *e = src + len;
            for (; src < e; src++) *t++ = (*src == '\n') ? '\r' : *src;
        }
        t += sprintf(t, "\r--- NilBBS %s\r * Origin: %.50s (%s)\rSEEN-BY: %u/%u %u/%u\r\001PATH: %u/%u\r",
                     BBS_VERSION, origin, myaddr, me.net, me.node, uplink.net, uplink.node, me.net, me.node);
        Write(*fh, text, (t - text) + 1);          /* includes the NUL */

        h.flags |= MF_SENT;
        str_copy(h.origaddr, myaddr, sizeof(h.origaddr));
        ObtainSemaphore(&S->msglock);
        msg_write_hdr(areas[ai].tag, &h);
        ReleaseSemaphore(&S->msglock);
        exported++;
    }
    msg_scan_done(&ms);
    FreeVec(text);
}

static void do_scan(void)
{
    BPTR fh = 0;
    char path[PATHLEN];
    int i;
    for (i = 0; i < nareas; i++) if (areas[i].type == AT_ECHO) export_area(i, &fh, path);
    if (fh) {
        static const UBYTE term[2] = { 0, 0 };
        Write(fh, (APTR)term, 2);
        Close(fh);
    }
}

/* ---- main --------------------------------------------------------------------- */

static struct BBSShared dummy;      /* stands in when the BBS isn't running */

int main(void)
{
    struct RDArgs *rda;
    LONG args[2] = { 0, 0 };
    struct Cfg *c;
    BOOL toss, scan;

    rda = ReadArgs((STRPTR)"TOSS/S,SCAN/S", args, NULL);
    if (!rda) { PrintFault(IoErr(), (STRPTR)"BBSToss"); return RETURN_FAIL; }
    toss = args[0] || !args[1];
    scan = args[1] || !args[0];

    c = cfg_load("BBS:Config/Fido.cfg");
    if (!parse_addr(cfg_str(c, "address", ""), &me) || !parse_addr(cfg_str(c, "uplink", ""), &uplink)) {
        PutStr((STRPTR)"BBSToss: set address and uplink in BBS:Config/Fido.cfg\n");
        cfg_free(c); FreeArgs(rda);
        return RETURN_ERROR;
    }
    str_copy(password, cfg_str(c, "password", ""), sizeof(password));
    str_copy(origin, cfg_str(c, "origin", "An NilBBS system"), sizeof(origin));
    str_copy(inbound, cfg_str(c, "inbound", "BBS:Fido/Inbound"), sizeof(inbound));
    str_copy(outbound, cfg_str(c, "outbound", "BBS:Fido/Outbound"), sizeof(outbound));
    str_copy(unpack, cfg_str(c, "unpack", ""), sizeof(unpack));
    cfg_free(c);

    /* share the BBS's locks if it's running, otherwise use our own */
    if (!(S = shared_find())) {
        S = &dummy;
        InitSemaphore(&S->userlock);
        InitSemaphore(&S->msglock);
    }
    mkcrc();
    load_areas();
    load_dupes();

    if (toss) {
        do_toss();
        Printf((STRPTR)"BBSToss: tossed %ld, %ld dupes, %ld undeliverable\n", tossed, dupecount, bad);
        if (tossed || dupecount || bad)
            bbs_log(BBS_SYSLOG, "BBSToss: tossed %ld, %ld dupes, %ld undeliverable", tossed, dupecount, bad);
    }
    if (scan) {
        do_scan();
        Printf((STRPTR)"BBSToss: exported %ld\n", exported);
        if (exported) bbs_log(BBS_SYSLOG, "BBSToss: exported %ld messages", exported);
    }
    save_dupes();
    FreeArgs(rda);
    return RETURN_OK;
}
