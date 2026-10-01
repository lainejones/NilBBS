/*
 * BBSMail - the binkp mailer for NilBBS: calls the FidoNet uplink, sends what BBSToss left in the
 * outbound, receives into the inbound, and tosses.
 *
 *   BBSMail [NOTOSS] [VERBOSE]
 *
 * One run = BBSToss SCAN (new posts -> a packet), one binkp session with the uplink, then
 * BBSToss TOSS if anything arrived.  NOTOSS leaves BBSToss out (just the session); VERBOSE prints
 * every frame.  Run it from Events.cfg every half hour or so.
 *
 * BBS:Config/Fido.cfg (the BBSToss settings, plus):
 *   binkp_host     = net4.fsxnet.nz:24560   ; the uplink's binkp address (port 24554 if left out)
 *   binkp_password = secret                 ; the session password the hub gave you
 *                                           ;   (left out: the packet password; none at all: "-")
 *   binkp_domain   = fidonet                ; the network's domain in our 5D address
 *   binkp_timeout  = 120                    ; seconds without a frame before giving up
 *
 * The outbound is BinkleyTerm-style, as BBSToss writes it: <net><node>.out (also .cut .dut .hut)
 * are packets, sent as <unique>.pkt and deleted once the hub has them; <net><node>.flo (.clo .dlo
 * .hlo) list files to send, one per line - "^path" deletes the file after, "#path" truncates it,
 * "~path" was already sent.  <net><node>.bsy locks the uplink while a session runs.
 *
 * binkp/1.0 (FTS-1026) as the calling side, with CRAM-MD5 (FTS-1027) when the hub offers it.
 * No resuming, compression or encryption: a broken transfer is simply sent again next time.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <proto/bsdsocket.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "../common/bbs.h"
#include "../common/cfg.h"

static const char __attribute__((used)) verstag[] = "$VER: BBSMail " BBS_VERSION " (" BBS_VERDATE ")";

struct Library *SocketBase;

#define MAILLOG   "BBS:Logs/BBSMail.log"
#define CHUNK     4096                  /* data frame we send (the protocol allows 32767) */
#define MAXOUT    64                    /* files in one session */

/* frame commands */
enum { M_NUL, M_ADR, M_PWD, M_FILE, M_OK, M_EOB, M_GOT, M_ERR, M_BSY, M_GET, M_SKIP };

struct FtnAddr { UWORD zone, net, node, point; };

static struct FtnAddr me, uplink;
static char inbound[PATHLEN], outbound[PATHLEN], host[128], pwd[64], domain[32];
static char sysname[80], sysop[64], location[80];
static LONG port = 24554, timeout_s = 120;
static BOOL verbose;
static LONG sock = -1;

/* ---- logging: everything to BBSMail.log (and the console), the outcome to System.log too ---- */
static void say(const char *fmt, ...)
{
    static char line[400];
    va_list ap;
    va_start(ap, fmt);
    vsprintf(line, fmt, ap);
    va_end(ap);
    PutStr((STRPTR)line); PutStr((STRPTR)"\n");
    bbs_log(MAILLOG, "%s", line);
}

/* ---- addresses ---------------------------------------------------------------------- */
static BOOL parse_addr(const char *s, struct FtnAddr *a)
{
    int z = 0, n = 0, f = 0, p = 0;
    if (sscanf(s, " %d:%d/%d.%d", &z, &n, &f, &p) < 3) return FALSE;
    a->zone = z; a->net = n; a->node = f; a->point = p;
    return TRUE;
}

static void addr_str(const struct FtnAddr *a, char *buf, BOOL dom)
{
    sprintf(buf, "%lu:%lu/%lu", (unsigned long)a->zone, (unsigned long)a->net, (unsigned long)a->node);
    if (a->point) sprintf(buf + strlen(buf), ".%lu", (unsigned long)a->point);
    if (dom && domain[0]) sprintf(buf + strlen(buf), "@%s", domain);
}

/* ---- MD5 (RFC 1321) and HMAC-MD5, for CRAM-MD5 --------------------------------------- */
struct Md5 { ULONG h[4]; ULONG lo, hi; UBYTE buf[64]; };

#define F1(x, y, z) ((z) ^ ((x) & ((y) ^ (z))))
#define F2(x, y, z) ((y) ^ ((z) & ((x) ^ (y))))
#define F3(x, y, z) ((x) ^ (y) ^ (z))
#define F4(x, y, z) ((y) ^ ((x) | ~(z)))
#define STEP(f, a, b, c, d, x, t, s) \
    (a) += f((b), (c), (d)) + (x) + (t); (a) = ((a) << (s)) | ((a) >> (32 - (s))); (a) += (b);

static void md5_block(ULONG h[4], const UBYTE *p)
{
    ULONG x[16], a = h[0], b = h[1], c = h[2], d = h[3];
    int i;
    for (i = 0; i < 16; i++)                    /* MD5 is little-endian */
        x[i] = p[i*4] | ((ULONG)p[i*4+1] << 8) | ((ULONG)p[i*4+2] << 16) | ((ULONG)p[i*4+3] << 24);
    STEP(F1,a,b,c,d,x[ 0],0xd76aa478, 7) STEP(F1,d,a,b,c,x[ 1],0xe8c7b756,12)
    STEP(F1,c,d,a,b,x[ 2],0x242070db,17) STEP(F1,b,c,d,a,x[ 3],0xc1bdceee,22)
    STEP(F1,a,b,c,d,x[ 4],0xf57c0faf, 7) STEP(F1,d,a,b,c,x[ 5],0x4787c62a,12)
    STEP(F1,c,d,a,b,x[ 6],0xa8304613,17) STEP(F1,b,c,d,a,x[ 7],0xfd469501,22)
    STEP(F1,a,b,c,d,x[ 8],0x698098d8, 7) STEP(F1,d,a,b,c,x[ 9],0x8b44f7af,12)
    STEP(F1,c,d,a,b,x[10],0xffff5bb1,17) STEP(F1,b,c,d,a,x[11],0x895cd7be,22)
    STEP(F1,a,b,c,d,x[12],0x6b901122, 7) STEP(F1,d,a,b,c,x[13],0xfd987193,12)
    STEP(F1,c,d,a,b,x[14],0xa679438e,17) STEP(F1,b,c,d,a,x[15],0x49b40821,22)
    STEP(F2,a,b,c,d,x[ 1],0xf61e2562, 5) STEP(F2,d,a,b,c,x[ 6],0xc040b340, 9)
    STEP(F2,c,d,a,b,x[11],0x265e5a51,14) STEP(F2,b,c,d,a,x[ 0],0xe9b6c7aa,20)
    STEP(F2,a,b,c,d,x[ 5],0xd62f105d, 5) STEP(F2,d,a,b,c,x[10],0x02441453, 9)
    STEP(F2,c,d,a,b,x[15],0xd8a1e681,14) STEP(F2,b,c,d,a,x[ 4],0xe7d3fbc8,20)
    STEP(F2,a,b,c,d,x[ 9],0x21e1cde6, 5) STEP(F2,d,a,b,c,x[14],0xc33707d6, 9)
    STEP(F2,c,d,a,b,x[ 3],0xf4d50d87,14) STEP(F2,b,c,d,a,x[ 8],0x455a14ed,20)
    STEP(F2,a,b,c,d,x[13],0xa9e3e905, 5) STEP(F2,d,a,b,c,x[ 2],0xfcefa3f8, 9)
    STEP(F2,c,d,a,b,x[ 7],0x676f02d9,14) STEP(F2,b,c,d,a,x[12],0x8d2a4c8a,20)
    STEP(F3,a,b,c,d,x[ 5],0xfffa3942, 4) STEP(F3,d,a,b,c,x[ 8],0x8771f681,11)
    STEP(F3,c,d,a,b,x[11],0x6d9d6122,16) STEP(F3,b,c,d,a,x[14],0xfde5380c,23)
    STEP(F3,a,b,c,d,x[ 1],0xa4beea44, 4) STEP(F3,d,a,b,c,x[ 4],0x4bdecfa9,11)
    STEP(F3,c,d,a,b,x[ 7],0xf6bb4b60,16) STEP(F3,b,c,d,a,x[10],0xbebfbc70,23)
    STEP(F3,a,b,c,d,x[13],0x289b7ec6, 4) STEP(F3,d,a,b,c,x[ 0],0xeaa127fa,11)
    STEP(F3,c,d,a,b,x[ 3],0xd4ef3085,16) STEP(F3,b,c,d,a,x[ 6],0x04881d05,23)
    STEP(F3,a,b,c,d,x[ 9],0xd9d4d039, 4) STEP(F3,d,a,b,c,x[12],0xe6db99e5,11)
    STEP(F3,c,d,a,b,x[15],0x1fa27cf8,16) STEP(F3,b,c,d,a,x[ 2],0xc4ac5665,23)
    STEP(F4,a,b,c,d,x[ 0],0xf4292244, 6) STEP(F4,d,a,b,c,x[ 7],0x432aff97,10)
    STEP(F4,c,d,a,b,x[14],0xab9423a7,15) STEP(F4,b,c,d,a,x[ 5],0xfc93a039,21)
    STEP(F4,a,b,c,d,x[12],0x655b59c3, 6) STEP(F4,d,a,b,c,x[ 3],0x8f0ccc92,10)
    STEP(F4,c,d,a,b,x[10],0xffeff47d,15) STEP(F4,b,c,d,a,x[ 1],0x85845dd1,21)
    STEP(F4,a,b,c,d,x[ 8],0x6fa87e4f, 6) STEP(F4,d,a,b,c,x[15],0xfe2ce6e0,10)
    STEP(F4,c,d,a,b,x[ 6],0xa3014314,15) STEP(F4,b,c,d,a,x[13],0x4e0811a1,21)
    STEP(F4,a,b,c,d,x[ 4],0xf7537e82, 6) STEP(F4,d,a,b,c,x[11],0xbd3af235,10)
    STEP(F4,c,d,a,b,x[ 2],0x2ad7d2bb,15) STEP(F4,b,c,d,a,x[ 9],0xeb86d391,21)
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
}

static void md5_init(struct Md5 *m)
{
    m->h[0] = 0x67452301; m->h[1] = 0xefcdab89; m->h[2] = 0x98badcfe; m->h[3] = 0x10325476;
    m->lo = m->hi = 0;
}

static void md5_add(struct Md5 *m, const UBYTE *p, ULONG n)
{
    ULONG used = m->lo & 63;
    if ((m->lo += n) < n) m->hi++;
    while (n) {
        ULONG k = 64 - used;
        if (k > n) k = n;
        memcpy(m->buf + used, p, k);
        used += k; p += k; n -= k;
        if (used == 64) { md5_block(m->h, m->buf); used = 0; }
    }
}

static void md5_end(struct Md5 *m, UBYTE out[16])
{
    static const UBYTE pad[64] = { 0x80 };
    UBYTE len[8];
    ULONG bits_lo = m->lo << 3, bits_hi = (m->hi << 3) | (m->lo >> 29), used = m->lo & 63;
    int i;
    for (i = 0; i < 4; i++) { len[i] = (UBYTE)(bits_lo >> (8 * i)); len[i + 4] = (UBYTE)(bits_hi >> (8 * i)); }
    md5_add(m, pad, used < 56 ? 56 - used : 120 - used);
    md5_add(m, len, 8);
    for (i = 0; i < 16; i++) out[i] = (UBYTE)(m->h[i / 4] >> (8 * (i % 4)));
}

static void hmac_md5(const UBYTE *key, ULONG klen, const UBYTE *msg, ULONG mlen, UBYTE out[16])
{
    UBYTE k[64], pad[64], inner[16];
    struct Md5 m;
    int i;
    memset(k, 0, sizeof(k));
    if (klen > 64) { md5_init(&m); md5_add(&m, key, klen); md5_end(&m, k); }
    else memcpy(k, key, klen);
    for (i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    md5_init(&m); md5_add(&m, pad, 64); md5_add(&m, msg, mlen); md5_end(&m, inner);
    for (i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    md5_init(&m); md5_add(&m, pad, 64); md5_add(&m, inner, 16); md5_end(&m, out);
}

/* ---- frames ----------------------------------------------------------------------------- */
static UBYTE *rbuf;                     /* what has come in and isn't a whole frame yet */
static LONG rlen;
#define RBUF (32768 + 2 + CHUNK)

static BOOL send_all(const UBYTE *p, LONG n)
{
    while (n > 0) {
        LONG k = send(sock, (APTR)p, n, 0);
        if (k <= 0) return FALSE;
        p += k; n -= k;
    }
    return TRUE;
}

static BOOL send_cmd(int cmd, const char *fmt, ...)
{
    static UBYTE f[600];
    va_list ap;
    LONG n;
    f[3] = 0;                           /* libnix's vsprintf("") writes nothing, not even the NUL */
    va_start(ap, fmt);
    if (*fmt) vsprintf((char *)f + 3, fmt, ap);
    va_end(ap);
    n = strlen((char *)f + 3) + 1;
    f[0] = (UBYTE)(0x80 | (n >> 8)); f[1] = (UBYTE)n; f[2] = (UBYTE)cmd;
    if (verbose) Printf((STRPTR)"  > %ld %s\n", (long)cmd, (LONG)(cmd == M_PWD ? "(password)" : (char *)f + 3));
    return send_all(f, n + 2);
}

static BOOL send_data(const UBYTE *p, LONG n)
{
    UBYTE h[2];
    h[0] = (UBYTE)(n >> 8); h[1] = (UBYTE)n;
    return send_all(h, 2) && send_all(p, n);
}

/* wait up to ms for more bytes; -1 = the connection is gone */
static LONG fill(LONG ms)
{
    fd_set r;
    struct timeval tv;
    LONG n;
    if (rlen >= RBUF) return 0;
    FD_ZERO(&r);
    FD_SET(sock, &r);
    tv.tv_secs = ms / 1000; tv.tv_micro = (ms % 1000) * 1000;
    n = WaitSelect(sock + 1, &r, NULL, NULL, &tv, NULL);
    if (n < 0) return -1;
    if (n == 0 || !FD_ISSET(sock, &r)) return 0;
    n = recv(sock, rbuf + rlen, RBUF - rlen, 0);
    if (n <= 0) return -1;
    rlen += n;
    return n;
}

/* the next whole frame, if there is one: *cmd = -1 for data, else the command; the text of a
 * command is NUL-terminated in place.  Returns the payload length, -1 = no whole frame yet. */
static UBYTE frame[32768 + 1];
static LONG next_frame(int *cmd)
{
    LONG whole, n;
    if (rlen < 2) return -1;
    whole = ((rbuf[0] & 0x7f) << 8) | rbuf[1];          /* the length after the 2-byte header */
    if (rlen < whole + 2) return -1;
    if (rbuf[0] & 0x80) {                               /* command: its id, then text */
        *cmd = whole ? rbuf[2] : M_NUL;
        n = whole ? whole - 1 : 0;
        memcpy(frame, rbuf + 3, n);
        frame[n] = 0;
    } else {
        *cmd = -1;
        n = whole;
        memcpy(frame, rbuf + 2, n);
    }
    rlen -= whole + 2;
    memmove(rbuf, rbuf + whole + 2, rlen);
    return n;
}

/* ---- the outbound ------------------------------------------------------------------------ */
struct OutItem {
    char path[PATHLEN];                 /* the file on disk */
    char name[64];                      /* as the hub sees it */
    LONG size;
    ULONG utime;
    UBYTE how;                          /* after it's sent: */
    WORD  flo, line;                    /* -1, or which .flo and which line of it */
    BOOL  got;                          /* the hub has it */
};
enum { H_DELETE, H_TRUNC, H_KEEP };

static struct OutItem *outq;
static LONG nout;
static char flopath[8][PATHLEN];
static LONG nflo;

static ULONG unixtime(const struct DateStamp *d)
{
    return 252460800UL + (ULONG)d->ds_Days * 86400UL + (ULONG)d->ds_Minute * 60UL + (ULONG)d->ds_Tick / 50UL;
}

static BOOL file_info(const char *path, LONG *size, ULONG *utime)
{
    BPTR l = Lock((STRPTR)path, ACCESS_READ);
    struct FileInfoBlock *fib;
    BOOL ok = FALSE;
    if (!l) return FALSE;
    if ((fib = AllocDosObject(DOS_FIB, NULL))) {
        if (Examine(l, fib) && fib->fib_DirEntryType < 0) {
            *size = fib->fib_Size; *utime = unixtime(&fib->fib_Date); ok = TRUE;
        }
        FreeDosObject(DOS_FIB, fib);
    }
    UnLock(l);
    return ok;
}

static void add_out(const char *path, const char *name, UBYTE how, WORD flo, WORD line)
{
    struct OutItem *o;
    if (nout >= MAXOUT) return;
    o = &outq[nout];
    if (!file_info(path, &o->size, &o->utime)) { say("BBSMail: %s is listed to send but isn't there - skipped", path); return; }
    str_copy(o->path, path, sizeof(o->path));
    str_copy(o->name, name, sizeof(o->name));
    o->how = how; o->flo = flo; o->line = line; o->got = FALSE;
    nout++;
}

/* <net><node>.out/.cut/.dut/.hut (packets) and .flo/.clo/.dlo/.hlo (file lists) for the uplink */
static void scan_outbound(void)
{
    static const char *pkt[] = { "out", "cut", "dut", "hut" }, *lst[] = { "flo", "clo", "dlo", "hlo" };
    static char path[PATHLEN], line[PATHLEN], name[64];
    char base[16];
    int i;
    ULONG uniq = bbs_now();
    sprintf(base, "%04lx%04lx", (unsigned long)uplink.net, (unsigned long)uplink.node);
    for (i = 0; i < 4; i++) {
        LONG sz; ULONG t;
        sprintf(path, "%s/%s.%s", outbound, base, pkt[i]);
        if (file_info(path, &sz, &t)) {
            sprintf(name, "%08lx.pkt", (unsigned long)(uniq + i));
            add_out(path, name, H_DELETE, -1, -1);
        }
    }
    for (i = 0; i < 4 && nflo < 8; i++) {
        BPTR fh;
        WORD n = 0;
        sprintf(path, "%s/%s.%s", outbound, base, lst[i]);
        if (!(fh = Open((STRPTR)path, MODE_OLDFILE))) continue;
        str_copy(flopath[nflo], path, PATHLEN);
        while (FGets(fh, (STRPTR)line, sizeof(line))) {
            char *p = line, *s;
            UBYTE how = H_KEEP;
            LONG l = strlen(p);
            while (l && (p[l - 1] == '\n' || p[l - 1] == '\r' || p[l - 1] == ' ')) p[--l] = 0;
            n++;
            if (!*p || *p == '~' || *p == ';') continue;        /* sent already / a comment */
            if (*p == '^' || *p == '-') { how = H_DELETE; p++; }
            else if (*p == '#') { how = H_TRUNC; p++; }
            s = (char *)FilePart((STRPTR)p);
            add_out(p, s, how, (WORD)nflo, (WORD)(n - 1));
        }
        Close(fh);
        nflo++;
    }
}

/* after the session: what the hub has goes (or gets truncated), the .flo keeps the rest */
static void finish_outbound(void)
{
    static char line[PATHLEN];
    LONG i, f;
    for (i = 0; i < nout; i++) {
        struct OutItem *o = &outq[i];
        if (!o->got) continue;
        if (o->how == H_DELETE) DeleteFile((STRPTR)o->path);
        else if (o->how == H_TRUNC) { BPTR fh = Open((STRPTR)o->path, MODE_NEWFILE); if (fh) Close(fh); }
    }
    for (f = 0; f < nflo; f++) {
        static char tmp[PATHLEN];
        BPTR in, out;
        WORD n = 0, kept = 0;
        sprintf(tmp, "%s.new", flopath[f]);
        if (!(in = Open((STRPTR)flopath[f], MODE_OLDFILE))) continue;
        if (!(out = Open((STRPTR)tmp, MODE_NEWFILE))) { Close(in); continue; }
        while (FGets(in, (STRPTR)line, sizeof(line))) {
            BOOL sent = FALSE;
            for (i = 0; i < nout; i++)
                if (outq[i].flo == f && outq[i].line == n && outq[i].got) sent = TRUE;
            n++;
            if (sent) continue;
            if (line[0] && line[0] != '\n') { FPuts(out, (STRPTR)line); kept++; }
        }
        Close(in); Close(out);
        DeleteFile((STRPTR)flopath[f]);
        if (kept) Rename((STRPTR)tmp, (STRPTR)flopath[f]); else DeleteFile((STRPTR)tmp);
    }
}

/* ---- the session -------------------------------------------------------------------------- */
static LONG sent_files, got_files, sent_bytes, got_bytes;

static void unescape(char *s)            /* binkp file names: \xHH */
{
    char *o = s;
    while (*s) {
        if (s[0] == '\\' && s[1] == 'x' && s[2] && s[3]) {
            char h[3] = { s[2], s[3], 0 };
            *o++ = (char)strtol(h, NULL, 16); s += 4;
        } else *o++ = *s++;
    }
    *o = 0;
}

static void safe_name(char *s)          /* a received name never leaves the inbound */
{
    char *p;
    for (p = s; *p; p++) if (*p == '/' || *p == ':' || *p == '\\' || *p < ' ') *p = '_';
    if (!s[0] || !strcmp(s, ".") || !strcmp(s, "..")) strcpy(s, "noname");
}

static BOOL session(void)
{
    static char rname[128], rtmp[PATHLEN], rfinal[PATHLEN], ourad[48], cram[130];
    static UBYTE chunk[CHUNK];
    BOOL authed = FALSE, remote_adr = FALSE, our_eob = FALSE, their_eob = FALSE, ok = FALSE, closed = FALSE;
    BPTR sfh = 0, rfh = 0;
    LONG cur = -1, rsize = 0, rgot = 0, waiting = 0, cramlen = 0;
    ULONG rtime = 0, idle = bbs_now();

    addr_str(&me, ourad, TRUE);
    send_cmd(M_NUL, "SYS %s", sysname);
    send_cmd(M_NUL, "ZYZ %s", sysop);
    send_cmd(M_NUL, "LOC %s", location);
    send_cmd(M_NUL, "NDL 115200,TCP,BINKP");
    send_cmd(M_NUL, "VER BBSMail/%s binkp/1.0", BBS_VERSION);
    send_cmd(M_ADR, "%s", ourad);

    for (;;) {
        int cmd;
        LONG n, got;

        /* send: the password once we've seen the hub's address, then the files, then EOB */
        if (authed && !our_eob) {
            if (!sfh) {
                while (++cur < nout && !(sfh = Open((STRPTR)outq[cur].path, MODE_OLDFILE)))
                    say("BBSMail: can't open %s - skipped", outq[cur].path);
                if (cur < nout) {
                    struct OutItem *o = &outq[cur];
                    send_cmd(M_FILE, "%s %ld %lu 0", o->name, (long)o->size, (unsigned long)o->utime);
                    say("BBSMail: sending %s as %s (%ld bytes)", o->path, o->name, (long)o->size);
                    waiting++;
                } else {
                    send_cmd(M_EOB, "");
                    our_eob = TRUE;
                }
            }
            if (sfh) {
                n = Read(sfh, chunk, CHUNK);
                if (n > 0) { if (!send_data(chunk, n)) break; sent_bytes += n; }
                else { Close(sfh); sfh = 0; }
            }
        }

        /* receive whatever is there (a short wait while we're sending, longer when we aren't) */
        got = fill(sfh ? 0 : 1000);
        if (got < 0) closed = TRUE;     /* the frames already here still count: the hub may hang up */
        if (got > 0) idle = bbs_now();  /* the moment it has sent its last GOT and EOB */
        else if (bbs_now() - idle > (ULONG)timeout_s) { say("BBSMail: no answer for %ld seconds - giving up", (long)timeout_s); break; }

        while ((n = next_frame(&cmd)) >= 0) {
            if (cmd < 0) {                                  /* data: the file being received */
                if (rfh && n) {
                    if (Write(rfh, frame, n) != n) { say("BBSMail: can't write %s - disk full?", rtmp); goto out; }
                    rgot += n;
                }
            } else {
                if (verbose) Printf((STRPTR)"  < %ld %s\n", (long)cmd, (LONG)frame);
                switch (cmd) {
                case M_NUL:
                    if (!strncmp((char *)frame, "OPT ", 4)) {
                        char *c = strstr((char *)frame, "CRAM-MD5-");
                        if (c) {
                            c += 9;
                            for (cramlen = 0; cramlen < (LONG)sizeof(cram) - 1 && c[cramlen] && c[cramlen] != ' '; cramlen++)
                                cram[cramlen] = c[cramlen];
                            cram[cramlen] = 0;
                        }
                    } else if (!strncmp((char *)frame, "SYS ", 4)) say("BBSMail: hub: %s", (char *)frame + 4);
                    break;
                case M_ADR: {
                    char want[48];
                    addr_str(&uplink, want, FALSE);
                    if (!strstr((char *)frame, want)) {
                        say("BBSMail: the hub is %s, not our uplink %s - hanging up", (char *)frame, want);
                        send_cmd(M_ERR, "Not the node I called");
                        goto out;
                    }
                    remote_adr = TRUE;
                    if (cramlen && pwd[0] && strcmp(pwd, "-")) {
                        static UBYTE ch[64];
                        UBYTE dig[16];
                        char hex[33];
                        LONG k, cl = cramlen / 2;
                        for (k = 0; k < cl && k < 64; k++) {
                            char h[3] = { cram[k * 2], cram[k * 2 + 1], 0 };
                            ch[k] = (UBYTE)strtol(h, NULL, 16);
                        }
                        hmac_md5((UBYTE *)pwd, strlen(pwd), ch, cl, dig);
                        for (k = 0; k < 16; k++) sprintf(hex + k * 2, "%02lx", (unsigned long)dig[k]);
                        send_cmd(M_PWD, "CRAM-MD5-%s", hex);
                    } else send_cmd(M_PWD, "%s", pwd[0] ? pwd : "-");
                    break;
                }
                case M_OK:
                    if (!remote_adr) break;
                    authed = TRUE;
                    say("BBSMail: logged in to %s (%s)", host, frame[0] ? (char *)frame : "ok");
                    break;
                case M_FILE: {
                    long sz = 0, off = 0;
                    unsigned long tm = 0;
                    rname[0] = 0;
                    if (rfh) { Close(rfh); rfh = 0; DeleteFile((STRPTR)rtmp); }     /* the last one broke off */
                    sscanf((char *)frame, "%127s %ld %lu %ld", rname, &sz, &tm, &off);
                    unescape(rname); safe_name(rname);
                    rsize = sz; rtime = tm; rgot = 0;
                    sprintf(rtmp, "%s/%s.bmtmp", inbound, rname);
                    if (off) say("BBSMail: %s offered from byte %ld - taking it whole", rname, off);
                    if (!(rfh = Open((STRPTR)rtmp, MODE_NEWFILE))) { say("BBSMail: can't create %s", rtmp); goto out; }
                    say("BBSMail: receiving %s (%ld bytes)", rname, (long)sz);
                    break;
                }
                case M_GOT: case M_SKIP: {
                    char nm[128];
                    LONG i;
                    nm[0] = 0;
                    sscanf((char *)frame, "%127s", nm);
                    unescape(nm);
                    for (i = 0; i < nout; i++)
                        if (!outq[i].got && !strcmp(outq[i].name, nm)) {
                            if (cmd == M_GOT) { outq[i].got = TRUE; sent_files++; }
                            else say("BBSMail: the hub skipped %s - it stays for next time", nm);
                            if (i == cur && sfh) { Close(sfh); sfh = 0; }    /* it has it already */
                            if (waiting) waiting--;
                            break;
                        }
                    break;
                }
                case M_EOB:
                    their_eob = TRUE;
                    break;
                case M_ERR:
                    say("BBSMail: the hub says: %s", (char *)frame);
                    goto out;
                case M_BSY:
                    say("BBSMail: the hub is busy: %s", (char *)frame);
                    goto out;
                case M_GET:
                    say("BBSMail: the hub asked for %s again (not supported) - sent whole next time", (char *)frame);
                    break;
                }
            }
            /* a received file is complete: into the inbound under its own name, then GOT */
            if (rfh && rgot >= rsize) {
                Close(rfh); rfh = 0;
                sprintf(rfinal, "%s/%s", inbound, rname);
                if (!Rename((STRPTR)rtmp, (STRPTR)rfinal)) {               /* the name is taken */
                    sprintf(rfinal, "%s/%08lx_%s", inbound, (unsigned long)bbs_now(), rname);
                    Rename((STRPTR)rtmp, (STRPTR)rfinal);
                }
                send_cmd(M_GOT, "%s %ld %lu", rname, (long)rsize, (unsigned long)rtime);
                got_files++; got_bytes += rsize;
            }
        }

        if (our_eob && their_eob && !waiting && !rfh) { ok = TRUE; break; }
        if (closed) {
            say("BBSMail: the hub hung up before the session was finished");
            break;
        }
    }
out:
    if (sfh) Close(sfh);
    if (rfh) { Close(rfh); DeleteFile((STRPTR)rtmp); }
    return ok;
}

/* ---- main --------------------------------------------------------------------------------- */
static BOOL run_toss(const char *what)
{
    static char cmd[64];
    sprintf(cmd, "BBS:BBSToss %s", what);
    return SystemTags((STRPTR)cmd, SYS_Input, Input(), SYS_Output, Output(), SYS_UserShell, TRUE,
                      NP_StackSize, 16384, TAG_DONE) == 0;
}

static BOOL connect_hub(void)
{
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((UWORD)port);
    sa.sin_addr.s_addr = inet_addr((STRPTR)host);
    if (sa.sin_addr.s_addr == (ULONG)-1) {
        struct hostent *he = gethostbyname((STRPTR)host);
        if (!he) { say("BBSMail: can't find %s", host); return FALSE; }
        memcpy(&sa.sin_addr, he->h_addr, 4);
    }
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) { say("BBSMail: no socket"); return FALSE; }
    if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        say("BBSMail: no answer from %s port %ld", host, (long)port);
        CloseSocket(sock); sock = -1;
        return FALSE;
    }
    return TRUE;
}

int main(void)
{
    struct RDArgs *rda;
    LONG args[2] = { 0, 0 };
    struct Cfg *c, *bc;
    static char bsy[PATHLEN], ua[48];
    BOOL toss, ok = FALSE;
    BPTR lk;
    int rc = RETURN_ERROR;

    rda = ReadArgs((STRPTR)"NOTOSS/S,VERBOSE/S", args, NULL);
    if (!rda) { PrintFault(IoErr(), (STRPTR)"BBSMail"); return RETURN_FAIL; }
    toss = !args[0];
    verbose = args[1] != 0;

    c = cfg_load("BBS:Config/Fido.cfg");
    if (!parse_addr(cfg_str(c, "address", ""), &me) || !parse_addr(cfg_str(c, "uplink", ""), &uplink)) {
        PutStr((STRPTR)"BBSMail: set address and uplink in BBS:Config/Fido.cfg\n");
        cfg_free(c); FreeArgs(rda);
        return RETURN_ERROR;
    }
    str_copy(host, cfg_str(c, "binkp_host", ""), sizeof(host));
    if (!host[0]) {
        PutStr((STRPTR)"BBSMail: set binkp_host (the uplink's binkp address) in BBS:Config/Fido.cfg\n");
        cfg_free(c); FreeArgs(rda);
        return RETURN_ERROR;
    }
    {   char *colon = strrchr(host, ':');
        if (colon) { port = atol(colon + 1); *colon = 0; } }
    if (port <= 0 || port > 65535) port = 24554;
    str_copy(pwd, cfg_str(c, "binkp_password", cfg_str(c, "password", "")), sizeof(pwd));
    str_copy(domain, cfg_str(c, "binkp_domain", "fidonet"), sizeof(domain));
    timeout_s = cfg_int(c, "binkp_timeout", 120);
    if (timeout_s < 10) timeout_s = 10;
    str_copy(inbound, cfg_str(c, "inbound", "BBS:Fido/Inbound"), sizeof(inbound));
    str_copy(outbound, cfg_str(c, "outbound", "BBS:Fido/Outbound"), sizeof(outbound));
    cfg_free(c);
    bc = cfg_load(bbs_config());
    str_copy(sysname, cfg_str(bc, "bbs_name", "A NilBBS system"), sizeof(sysname));
    str_copy(sysop, cfg_str(bc, "sysop_name", "Sysop"), sizeof(sysop));
    str_copy(location, cfg_str(bc, "qwk_location", "Somewhere"), sizeof(location));
    cfg_free(bc);

    if (toss) run_toss("SCAN");

    /* one session at a time with the uplink: <net><node>.bsy (a stale one, over an hour old, goes) */
    sprintf(bsy, "%s/%04lx%04lx.bsy", outbound, (unsigned long)uplink.net, (unsigned long)uplink.node);
    if ((lk = Lock((STRPTR)bsy, ACCESS_READ))) {
        LONG sz; ULONG t;
        UnLock(lk);
        /* t is Unix time.  Signed: a lock dated a little in the future (the clocks of whoever made
         * it and ours disagree - a host-dir drive takes the PC's clock) is fresh, not 136 years old */
        if (file_info(bsy, &sz, &t) && (LONG)(bbs_now() + 252460800UL - t) < 3600) {
            say("BBSMail: %s exists - another session is running", bsy);
            FreeArgs(rda);
            return RETURN_WARN;
        }
        DeleteFile((STRPTR)bsy);
    }
    { BPTR fh = Open((STRPTR)bsy, MODE_NEWFILE); if (fh) Close(fh); }

    outq = AllocVec(sizeof(struct OutItem) * MAXOUT, MEMF_CLEAR);
    rbuf = AllocVec(RBUF, MEMF_ANY);
    if (!outq || !rbuf) { say("BBSMail: out of memory"); goto done; }
    if (!(SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 4))) {
        say("BBSMail: no bsdsocket.library - start the TCP/IP stack first");
        goto done;
    }
    scan_outbound();
    addr_str(&uplink, ua, TRUE);
    say("BBSMail: calling %s at %s:%ld, %ld file(s) to send", ua, host, (long)port, (long)nout);
    if (connect_hub()) {
        ok = session();
        CloseSocket(sock); sock = -1;
    }
    finish_outbound();
    if (ok) {
        bbs_log(BBS_SYSLOG, "BBSMail: %s - sent %ld file(s) (%ld bytes), received %ld (%ld bytes)",
                ua, (long)sent_files, (long)sent_bytes, (long)got_files, (long)got_bytes);
        say("BBSMail: done - sent %ld file(s), received %ld", (long)sent_files, (long)got_files);
        rc = RETURN_OK;
    } else {
        bbs_log(BBS_SYSLOG, "BBSMail: the session with %s failed - see %s", ua, MAILLOG);
        rc = RETURN_WARN;
    }
    if (toss && got_files) run_toss("TOSS");

done:
    DeleteFile((STRPTR)bsy);
    if (SocketBase) CloseLibrary(SocketBase);
    if (rbuf) FreeVec(rbuf);
    if (outq) FreeVec(outq);
    FreeArgs(rda);
    return rc;
}
