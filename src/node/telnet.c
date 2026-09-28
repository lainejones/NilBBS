/*
 * telnet.c - the wire: RFC 854 telnet on the node's socket.
 *
 * We offer WILL ECHO + WILL SGA (character-at-a-time, server echo), ask for
 * NAWS (window size) and TTYPE (terminal names), and negotiate BINARY both
 * ways so 8-bit CP437 art and file transfers pass untouched.  Incoming data
 * is de-IAC'd into a ring buffer; CR LF and CR NUL collapse to a single CR.
 * Outgoing bytes are buffered and IAC-escaped.
 */
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>

#include <sys/socket.h>
#include <proto/bsdsocket.h>

#include "node.h"

#define IAC   255
#define DONT  254
#define DO    253
#define WONT  252
#define WILL  251
#define SB    250
#define SE    240

#define O_BINARY 0
#define O_ECHO   1
#define O_SGA    3
#define O_TTYPE  24
#define O_NAWS   31

#define TS_DATA 0
#define TS_IAC  1
#define TS_OPT  2       /* got WILL/WONT/DO/DONT, want the option byte */
#define TS_SB   3
#define TS_SBIAC 4

static UBYTE us[256];           /* option enabled on our side  */
static UBYTE them[256];         /* option enabled on their side */
static UBYTE asked[256];        /* we sent DO and await the answer */
static UBYTE optverb;

extern struct Library *SocketBase;

static void in_put(UBYTE c);

/* ---- local console (BBSNode LOCAL): a raw CON: window instead of a socket ----
 * The Amiga console speaks enough ANSI for our colours and cursor moves; the
 * differences are patched here: ESC[2J becomes a form feed (the console's clear
 * screen), and the console's 0x9B CSI in input becomes ESC [ for tgetkey(). */

static void local_write(const UBYTE *p, LONG len)
{
    UBYTE buf[512];
    LONG n = 0, i;
    for (i = 0; i < len; i++) {
        if (p[i] == 27 && i + 3 < len && p[i + 1] == '[' && p[i + 2] == '2' && p[i + 3] == 'J') {
            buf[n++] = 12;
            i += 3;
        } else buf[n++] = p[i];
        if (n >= (LONG)sizeof(buf) - 1) { Write(N.lcon, buf, n); n = 0; }
    }
    if (n) Write(N.lcon, buf, n);
}

/* read what the console has; the close gadget reports as CSI 11;...| */
static BOOL local_read(void)
{
    UBYTE buf[256];
    LONG n = 0, i;
    while (n < (LONG)sizeof(buf) && WaitForChar(N.lcon, n ? 20000 : 0)) {
        if (Read(N.lcon, &buf[n], 1) != 1) { node_hangup("local console gone"); return FALSE; }
        n++;
    }
    for (i = 0; i < n; i++) {
        if (buf[i] == 0x9B && i + 3 < n && buf[i + 1] == '1' && buf[i + 2] == '1' && buf[i + 3] == ';') {
            node_hangup("local window closed");
            return FALSE;
        }
        if (buf[i] == 0x9B) { in_put(27); in_put('['); }
        else in_put(buf[i]);
    }
    return n > 0;
}

static LONG local_wait(ULONG ms, ULONG extrasigs, ULONG *gotsigs)
{
    for (;;) {
        ULONG mask = SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_D | extrasigs;
        ULONG sigs = SetSignal(0, mask) & mask;
        ULONG slice = ms > 100 ? 100 : ms;
        node_heartbeat();
        if (sigs & SIGBREAKF_CTRL_C) {
            N.kicked = TRUE;
            node_hangup("disconnected by sysop");
            return 0;
        }
        if (sigs & SIGBREAKF_CTRL_D) { N.msg_waiting = TRUE; return in_avail(); }
        if (sigs & extrasigs) { if (gotsigs) *gotsigs = sigs & extrasigs; return in_avail(); }
        if (WaitForChar(N.lcon, slice * 1000) && local_read()) return in_avail();
        if (!N.online) return 0;
        if (ms <= slice) return in_avail();
        ms -= slice;
    }
}

/* window size from the console ("CSI 0 q" -> "CSI 1;1;rows;cols r"), close
 * gadget reports on */
static void local_start(void)
{
    UBYTE r[32];
    LONG n = 0;
    Write(N.lcon, (APTR)"\x9b" "0 q", 4);
    while (n < (LONG)sizeof(r) - 1 && WaitForChar(N.lcon, 500000)) {
        if (Read(N.lcon, &r[n], 1) != 1) break;
        if (r[n++] == 'r') break;
    }
    r[n] = 0;
    if (n > 4 && r[0] == 0x9B) {
        int a, b, rows, cols;
        if (sscanf((char *)r + 1, "%d;%d;%d;%d", &a, &b, &rows, &cols) == 4) {
            if (cols >= 20 && cols <= 255) N.cols = cols;
            if (rows >= 5 && rows <= 255) N.rows = rows;
        }
    }
    Write(N.lcon, (APTR)"\x9b" "11{", 4);        /* report the close gadget */
}

/* ---- output --------------------------------------------------------------- */

static BOOL wait_writable(LONG secs)
{
    while (secs-- > 0) {
        fd_set w;
        struct timeval tv;
        ULONG sigs = SIGBREAKF_CTRL_C;
        LONG r;
        FD_ZERO(&w);
        FD_SET(N.sock, &w);
        tv.tv_secs = 1; tv.tv_micro = 0;
        r = WaitSelect(N.sock + 1, NULL, &w, NULL, &tv, &sigs);
        if (sigs & SIGBREAKF_CTRL_C) { N.kicked = TRUE; return FALSE; }
        if (r > 0) return TRUE;
    }
    return FALSE;
}

BOOL tn_flush(void)
{
    UBYTE *p = N.out;
    LONG left = N.outlen;
    if (!N.online) { N.outlen = 0; return FALSE; }
    spy_feed(p, left);                          /* the sysop watching, if anyone is */
    if (N.local) { local_write(p, left); N.outlen = 0; return N.online; }
    if (N.serial) { ser_write(p, left); N.outlen = 0; return N.online; }
    while (left > 0) {
        LONG n;
        if (!wait_writable(30)) { node_hangup("send stalled"); break; }
        n = send(N.sock, p, left, 0);
        if (n <= 0) { node_hangup("send failed"); break; }
        p += n;
        left -= n;
    }
    N.outlen = 0;
    return N.online;
}

static void out_byte(UBYTE c)
{
    if (N.outlen >= OUTBUF_SIZE - 2) tn_flush();
    N.out[N.outlen++] = c;
}

void tn_raw(const UBYTE *buf, LONG len)
{
    if (!N.online) return;
    while (len-- > 0) {
        UBYTE c = *buf++;
        out_byte(c);
        if (c == IAC && !N.local && !N.serial) out_byte(IAC);
    }
}

void tn_rawflush(const UBYTE *buf, LONG len)
{
    tn_raw(buf, len);
    tn_flush();
}

static void send_cmd(UBYTE verb, UBYTE opt)
{
    if (!N.online || N.local || N.serial) return;
    if (N.outlen >= OUTBUF_SIZE - 4) tn_flush();
    N.out[N.outlen++] = IAC;
    N.out[N.outlen++] = verb;
    N.out[N.outlen++] = opt;
}

static void send_sb(UBYTE opt, const UBYTE *data, LONG len)
{
    if (N.local || N.serial) return;
    if (N.outlen >= OUTBUF_SIZE - len - 8) tn_flush();
    N.out[N.outlen++] = IAC;
    N.out[N.outlen++] = SB;
    N.out[N.outlen++] = opt;
    while (len-- > 0) {
        UBYTE c = *data++;
        N.out[N.outlen++] = c;
        if (c == IAC) N.out[N.outlen++] = IAC;
    }
    N.out[N.outlen++] = IAC;
    N.out[N.outlen++] = SE;
}

/* ---- input ring --------------------------------------------------------------- */

LONG in_avail(void)
{
    return (LONG)((N.in_head - N.in_tail) & (INBUF_SIZE - 1));
}

static void in_put(UBYTE c)
{
    UWORD next = (N.in_head + 1) & (INBUF_SIZE - 1);
    if (next == N.in_tail) return;          /* full: drop (typeahead flood) */
    N.in[N.in_head] = c;
    N.in_head = next;
}

LONG in_get(void)
{
    UBYTE c;
    if (N.in_head == N.in_tail) return -1;
    c = N.in[N.in_tail];
    N.in_tail = (N.in_tail + 1) & (INBUF_SIZE - 1);
    return c;
}

void in_unget(UBYTE c)
{
    UWORD prev = (N.in_tail - 1) & (INBUF_SIZE - 1);
    if (prev == N.in_head) return;
    N.in_tail = prev;
    N.in[prev] = c;
}

/* a byte from a serial line: no telnet, but the same CR LF / CR NUL -> CR as data */
static void serial_byte(UBYTE c)
{
    if (N.binary_raw) { in_put(c); return; }
    if (N.cr_last) {
        N.cr_last = 0;
        if (c == 0 || c == '\n') return;
    }
    if (c == '\r') N.cr_last = 1;
    else if (c == '\n') c = '\r';
    else if (c == 0) return;
    in_put(c);
}

/* ---- negotiation ------------------------------------------------------------ */

static BOOL we_support(UBYTE opt)   { return opt == O_ECHO || opt == O_SGA || opt == O_BINARY; }
static BOOL they_support(UBYTE opt) { return opt == O_NAWS || opt == O_TTYPE || opt == O_SGA || opt == O_BINARY; }

static void ask_ttype(void)
{
    static const UBYTE send1[1] = { 1 };    /* TTYPE SEND */
    send_sb(O_TTYPE, send1, 1);
}

static void handle_opt(UBYTE verb, UBYTE opt)
{
    switch (verb) {
    case DO:
        if (we_support(opt)) {
            if (!us[opt]) { us[opt] = 1; send_cmd(WILL, opt); }
            if (opt == O_BINARY) N.local_binary = 1;
        } else send_cmd(WONT, opt);
        break;
    case DONT:
        if (us[opt]) { us[opt] = 0; send_cmd(WONT, opt); }
        if (opt == O_BINARY) N.local_binary = 0;
        break;
    case WILL:
        if (they_support(opt)) {
            if (!them[opt]) {
                them[opt] = 1;
                if (!asked[opt]) send_cmd(DO, opt);
            }
            asked[opt] = 0;
            if (opt == O_TTYPE) { N.remote_ttype = 1; ask_ttype(); }
            if (opt == O_NAWS)   N.remote_naws = 1;
            if (opt == O_BINARY) N.remote_binary = 1;
        } else send_cmd(DONT, opt);
        break;
    case WONT:
        if (them[opt]) { them[opt] = 0; send_cmd(DONT, opt); }
        asked[opt] = 0;
        if (opt == O_BINARY) N.remote_binary = 0;
        break;
    }
}

static void handle_sb(void)
{
    if (N.sbopt == O_NAWS && N.sblen >= 4) {
        UWORD w = (N.sbbuf[0] << 8) | N.sbbuf[1];
        UWORD h = (N.sbbuf[2] << 8) | N.sbbuf[3];
        if (w >= 20 && w <= 255) N.cols = w;
        if (h >= 5 && h <= 255)  N.rows = h;
        if (N.ni) { N.ni->cols = N.cols; N.ni->rows = N.rows; }
    } else if (N.sbopt == O_TTYPE && N.sblen >= 1 && N.sbbuf[0] == 0) {  /* IS */
        char name[32];
        LONG n = N.sblen - 1, i;
        if (n > 31) n = 31;
        for (i = 0; i < n; i++) {
            UBYTE c = N.sbbuf[1 + i];
            name[i] = (c >= 'a' && c <= 'z') ? c - 32 : c;
        }
        name[n] = 0;
        if (!N.ttype[0]) str_copy(N.ttype, name, sizeof(N.ttype));
        /* clients cycle through their names; stop when one repeats */
        if (!str_istr(N.ttypes, name)) {
            if (strlen(N.ttypes) + n + 2 < sizeof(N.ttypes)) {
                if (N.ttypes[0]) strcat(N.ttypes, " ");
                strcat(N.ttypes, name);
            }
            if (++N.ttype_rounds < 4) ask_ttype();
        }
    }
}

static void parse_byte(UBYTE c)
{
    switch (N.tstate) {
    case TS_DATA:
        if (c == IAC) { N.tstate = TS_IAC; return; }
        if (N.binary_raw) { in_put(c); return; }
        if (N.cr_last) {
            N.cr_last = 0;
            if (c == 0 || c == '\n') return;        /* CR NUL / CR LF -> CR */
        }
        if (c == '\r') N.cr_last = 1;
        else if (c == '\n') c = '\r';               /* bare LF clients */
        else if (c == 0) return;
        in_put(c);
        return;
    case TS_IAC:
        N.tstate = TS_DATA;
        switch (c) {
        case IAC: in_put(IAC); return;
        case WILL: case WONT: case DO: case DONT:
            optverb = c; N.tstate = TS_OPT; return;
        case SB:
            N.tstate = TS_SB; N.sblen = 0; N.sbopt = 0xFF; return;
        case 244:           /* IP  - interrupt: pass a ^C to whoever reads */
            in_put(3); return;
        case 247:           /* EC  - erase char */
            in_put(8); return;
        default:            /* NOP, GA, AYT, ... */
            return;
        }
    case TS_OPT:
        N.tstate = TS_DATA;
        handle_opt(optverb, c);
        return;
    case TS_SB:
        if (c == IAC) { N.tstate = TS_SBIAC; return; }
        if (N.sbopt == 0xFF) { N.sbopt = c; return; }
        if (N.sblen < sizeof(N.sbbuf)) N.sbbuf[N.sblen++] = c;
        return;
    case TS_SBIAC:
        if (c == SE) { N.tstate = TS_DATA; handle_sb(); return; }
        if (c == IAC && N.sblen < sizeof(N.sbbuf)) N.sbbuf[N.sblen++] = IAC;
        N.tstate = TS_SB;
        return;
    }
}

/* ---- main wait ---------------------------------------------------------------- */

void node_heartbeat(void)
{
    node_check_slot();
    if (N.ni) N.ni->beat = bbs_now();
}

void node_hangup(const char *why)
{
    if (!N.online) return;
    N.online = FALSE;
    bbs_log(BBS_SYSLOG, "node %ld: %s (%s)", (LONG)N.node, why,
            N.loggedin ? N.user.name : N.ipstr);
}

/*
 * Wait up to `ms` for caller input (or one of `extrasigs`).  Returns the
 * number of decoded bytes waiting; 0 on timeout / signal.  Flushes pending
 * output first so prompts are on screen before we block.
 */
LONG tn_wait(ULONG ms, ULONG extrasigs, ULONG *gotsigs)
{
    UBYTE buf[512];
    if (gotsigs) *gotsigs = 0;
    if (!N.online) return 0;
    tn_flush();
    node_heartbeat();
    /* doors leave unread typeahead in the buffer; they only want to hear
     * about NEW bytes, or the wait would spin */
    if (in_avail() && !N.wait_new) return in_avail();
    if (N.local) return local_wait(ms, extrasigs, gotsigs);
    if (N.serial) return ser_wait(ms, extrasigs, gotsigs, serial_byte);

    for (;;) {
        fd_set r;
        struct timeval tv;
        ULONG sigs = SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_D | extrasigs;
        ULONG slice = ms > 1000 ? 1000 : ms;
        LONG rc;

        FD_ZERO(&r);
        FD_SET(N.sock, &r);
        tv.tv_secs  = slice / 1000;
        tv.tv_micro = (slice % 1000) * 1000;
        rc = WaitSelect(N.sock + 1, &r, NULL, NULL, &tv, &sigs);
        node_heartbeat();
        spy_poll();                     /* a new watcher gets the screen now, not at the next output */

        if (sigs & SIGBREAKF_CTRL_C) {
            N.kicked = TRUE;
            node_hangup("disconnected by sysop");
            return 0;
        }
        if (sigs & SIGBREAKF_CTRL_D) {      /* another node sent us a message */
            N.msg_waiting = TRUE;
            return in_avail();
        }
        if (sigs & extrasigs) {
            if (gotsigs) *gotsigs = sigs & extrasigs;
            return in_avail();
        }
        if (rc > 0 && FD_ISSET(N.sock, &r)) {
            LONG n = recv(N.sock, buf, sizeof(buf), 0);
            LONG i, before = in_avail();
            if (n <= 0) { node_hangup("carrier lost"); return 0; }
            for (i = 0; i < n; i++) parse_byte(buf[i]);
            tn_flush();                 /* negotiation replies */
            if (in_avail() != before) return in_avail();
            continue;                   /* only telnet chatter - keep waiting */
        }
        if (ms <= slice) return in_avail();
        ms -= slice;
    }
}

void tn_set_binary(BOOL on)
{
    N.binary_raw = on ? 1 : 0;
}

void tn_start(void)
{
    ULONG start;
    memset(us, 0, sizeof(us));
    memset(them, 0, sizeof(them));
    memset(asked, 0, sizeof(asked));
    N.tstate = TS_DATA;
    N.cols = 80;
    N.rows = 24;
    if (N.local) { local_start(); return; }
    if (N.serial) return;                  /* no telnet on a serial line: tdetect() asks the terminal */

    us[O_ECHO] = us[O_SGA] = us[O_BINARY] = 1;
    send_cmd(WILL, O_ECHO);
    send_cmd(WILL, O_SGA);
    send_cmd(WILL, O_BINARY);
    asked[O_SGA] = asked[O_NAWS] = asked[O_TTYPE] = asked[O_BINARY] = 1;
    send_cmd(DO, O_SGA);
    send_cmd(DO, O_NAWS);
    send_cmd(DO, O_TTYPE);
    send_cmd(DO, O_BINARY);
    tn_flush();

    /* give the client ~1.5s to answer; stop early once TTYPE and NAWS are in */
    start = bbs_now();
    while (N.online && bbs_now() - start < 2) {
        tn_wait(250, 0, NULL);
        if ((N.ttype[0] || !them[O_TTYPE]) && N.remote_naws && N.ttype_rounds >= 1) break;
    }
    /* anything typed during negotiation is noise ("\r" from impatient callers) */
    N.in_head = N.in_tail = 0;
}
