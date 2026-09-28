/*
 * term.c - everything between BBS text and the caller's screen/keyboard.
 *
 * Output:  tputs() understands pipe colour codes (|00..|15 foreground,
 *          |16..|23 background) and MCI codes (|UN user name, |TL time
 *          left, ...).  tputraw() takes a byte stream that may contain ANSI
 *          escapes and adapts it to the caller:
 *            TT_ANSI   escapes pass through
 *            TT_VT100  colour is filtered out of SGR (bold/underline/blink/
 *                      reverse survive), box drawing uses DEC graphics
 *            TT_ASCII  escapes are stripped; cursor-forward becomes spaces
 *          and every printable byte goes through the charset translator.
 * Input:   tgetkey() decodes arrow/function keys and UTF-8/Latin-1 into the
 *          internal CP437, and polices idle time and the time limit.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

#include "node.h"

static const UBYTE pc2ansi[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

const char *term_name(UBYTE t)
{
    return t == TT_ANSI ? "ANSI" : t == TT_VT100 ? "VT100" : "ASCII";
}

const char *charset_name(UBYTE c)
{
    return c == CS_UTF8 ? "UTF-8" : c == CS_LATIN1 ? "Amiga (ISO-8859-1)" :
           c == CS_CP437 ? "CP437 (IBM PC)" : "7-bit ASCII";
}

/* ---- paging -------------------------------------------------------------- */

void tpage_start(void) { N.paging = TRUE; N.page_abort = FALSE; N.lines_out = 0; }
void tpage_end(void)   { N.paging = FALSE; N.page_abort = FALSE; }

static void more_prompt(void)
{
    /* the keys stay Y / n / = in every language */
    const char *p = L("term.more", "-- More (Y/n/=) --");
    LONG k;
    tputs("|08");
    tputraw((const UBYTE *)p, strlen(p), CS_CP437);     /* no |codes: a translation's own characters */
    tputs("|07");
    k = tgetkey(0);
    tn_raw((const UBYTE *)"\r", 1);
    if (N.term != TT_ASCII) tn_raw((const UBYTE *)"\x1b[K", 3);
    else {
        char sp[80];
        LONG w = strlen(p) + 2;
        if (w < 20) w = 20;
        if (w > 78) w = 78;
        memset(sp, ' ', w); sp[w] = '\r';
        tn_raw((const UBYTE *)sp, w + 1);
    }
    if (k == 'n' || k == 'N' || k == 'q' || k == 'Q' || k == 27 || k == KEY_HANGUP)
        N.page_abort = TRUE;
    else if (k == '=' || k == 'c' || k == 'C')
        N.paging = FALSE;               /* continuous */
    N.lines_out = 0;
}

BOOL tmore(void)
{
    return N.online && !N.page_abort;
}

/* ---- the ANSI-aware output filter ---------------------------------------- */

static void emit_ctl(UBYTE c) { tn_raw(&c, 1); }

static void newline_out(void)
{
    static const UBYTE crlf[2] = { '\r', '\n' };
    tn_raw(crlf, 2);
    if (N.paging && !N.page_abort && ++N.lines_out >= N.rows - 1) more_prompt();
}

/* rewrite an SGR sequence for a VT100 (monochrome) terminal */
static void vt100_sgr(const char *params)
{
    char out[40];
    int n = 0, first = 1;
    const char *p = params;
    out[n++] = 27; out[n++] = '[';
    if (!*p) { out[n++] = 'm'; tn_raw((UBYTE *)out, n); return; }
    while (*p) {
        int v = atoi(p);
        if (v == 0 || v == 1 || v == 4 || v == 5 || v == 7 ||
            v == 22 || v == 24 || v == 25 || v == 27) {
            if (!first) out[n++] = ';';
            n += sprintf(out + n, "%d", v);
            first = 0;
        }
        if (v == 38 || v == 48) break;      /* extended colour: drop the rest */
        while (*p && *p != ';') p++;
        if (*p == ';') p++;
        if (n > 32) break;
    }
    if (first) return;                      /* nothing left worth sending */
    out[n++] = 'm';
    tn_raw((UBYTE *)out, n);
}

static void finish_csi(void)
{
    char *seq = N.esc_buf;                  /* ESC [ params final */
    UBYTE fin = (UBYTE)seq[N.esc_len - 1];
    char params[40];
    int plen = N.esc_len - 3;
    if (plen < 0) plen = 0;
    memcpy(params, seq + 2, plen);
    params[plen] = 0;

    switch (N.term) {
    case TT_ANSI:
        tn_raw((UBYTE *)seq, N.esc_len);
        break;
    case TT_VT100:
        if (fin == 'm') vt100_sgr(params);
        else tn_raw((UBYTE *)seq, N.esc_len);
        break;
    default:                                /* ASCII */
        if (fin == 'C') {
            int n = atoi(params), i;
            if (n < 1) n = 1;
            if (n > 132) n = 132;
            for (i = 0; i < n; i++) emit_ctl(' ');
        } else if (fin == 'J' && atoi(params) == 2) {
            newline_out();
        }
        break;
    }
}

void tputraw(const UBYTE *buf, LONG len, UBYTE srccs)
{
    while (len-- > 0) {
        UBYTE c = *buf++;
        if (!N.online || N.page_abort) return;

        switch (N.esc_state) {
        case 0:
            if (c == 27) {
                N.esc_state = 1; N.esc_len = 0;
                N.esc_buf[N.esc_len++] = 27;
                continue;
            }
            if (c == 0x9B && (srccs == CS_LATIN1 || N.door_csi8)) {  /* Amiga 8-bit CSI */
                N.esc_state = 2; N.esc_len = 0;
                N.esc_buf[N.esc_len++] = 27;
                N.esc_buf[N.esc_len++] = '[';
                continue;
            }
            if (c == '\n') { newline_out(); continue; }
            if (c == '\r') {
                if (len > 0 && *buf == '\n') continue;  /* CRLF: the LF emits both */
                emit_ctl(c);
                continue;
            }
            if (c < 32) {
                if (c == 7 || c == 8 || c == 9) emit_ctl(c);
                else if (c == 12) {                      /* form feed = clear */
                    if (N.term == TT_ASCII) newline_out();
                    else tn_raw((const UBYTE *)"\x1b[2J\x1b[H", 7);
                }
                continue;
            }
            emit_char(c, srccs);
            continue;

        case 1:                                 /* after ESC */
            N.esc_buf[N.esc_len++] = c;
            if (c == '[') { N.esc_state = 2; continue; }
            if (c == '(' || c == ')') { N.esc_state = 3; continue; }
            N.esc_state = 0;
            if (N.term != TT_ASCII) tn_raw((UBYTE *)N.esc_buf, N.esc_len);
            continue;

        case 2:                                 /* CSI body */
            if (N.esc_len < (UBYTE)(sizeof(N.esc_buf) - 1)) N.esc_buf[N.esc_len++] = c;
            if (c >= 0x40 && c <= 0x7E) {
                N.esc_state = 0;
                finish_csi();
            } else if (c < 0x20 || N.esc_len >= sizeof(N.esc_buf) - 1) {
                N.esc_state = 0;                /* garbage: drop it */
            }
            continue;

        case 3:                                 /* ESC ( x */
            N.esc_buf[N.esc_len++] = c;
            N.esc_state = 0;
            if (N.term != TT_ASCII) tn_raw((UBYTE *)N.esc_buf, N.esc_len);
            continue;
        }
    }
}

/* ---- colour --------------------------------------------------------------- */

static void apply_color(void)
{
    char seq[24];
    int n;
    if (N.term == TT_ASCII) return;
    if (N.term == TT_VT100) {
        n = sprintf(seq, "\x1b[0%sm", N.fg >= 8 ? ";1" : "");
    } else {
        n = sprintf(seq, "\x1b[0;%s3%d;4%dm", N.fg >= 8 ? "1;" : "",
                    pc2ansi[N.fg & 7], pc2ansi[N.bg & 7]);
    }
    tn_raw((UBYTE *)seq, n);
}

void tcolor(int fg)
{
    if (fg < 16) N.fg = (UBYTE)fg;
    else N.bg = (UBYTE)(fg - 16);
    apply_color();
}

void tcls(void)
{
    if (N.term == TT_ASCII) tn_raw((const UBYTE *)"\r\n\r\n", 4);
    else {
        N.fg = 7; N.bg = 0;
        tn_raw((const UBYTE *)"\x1b[0m\x1b[2J\x1b[H", 11);
    }
    N.lines_out = 0;
}

void tgotoxy(int x, int y)
{
    char s[16];
    if (N.term == TT_ASCII) return;
    tn_raw((UBYTE *)s, sprintf(s, "\x1b[%d;%dH", y, x));
}

void tcleol(void)
{
    if (N.term != TT_ASCII) tn_raw((const UBYTE *)"\x1b[K", 3);
}

void tnl(void) { newline_out(); }

/* ---- MCI codes -------------------------------------------------------------- */

void expand_mci(const char *code, char *out)
{
    char c0 = code[0], c1 = code[1];
    *out = 0;
#define IS(a,b) (c0 == (a) && c1 == (b))
    if (IS('U','N')) str_copy(out, N.loggedin ? N.user.name : L("term.mci.guest", "Guest"), NAMELEN);
    else if (IS('U','R')) strcpy(out, N.user.realname);
    else if (IS('U','L')) strcpy(out, N.user.location);
    else if (IS('B','N')) strcpy(out, cfg_str(N.cfg, "bbs_name", "NilBBS"));
    else if (IS('S','N')) strcpy(out, cfg_str(N.cfg, "sysop_name", "Sysop"));
    else if (IS('N','D')) sprintf(out, "%d", N.node);
    else if (IS('T','L')) {
        LONG t = time_left_mins();
        if (t < 0) strcpy(out, "--"); else sprintf(out, "%ld", t);
    }
    else if (IS('D','A')) bbs_datestr(bbs_now(), out);
    else if (IS('T','I')) bbs_timestr(bbs_now(), out);
    else if (IS('U','C')) sprintf(out, "%lu", N.user.calls);
    else if (IS('L','V')) sprintf(out, "%d", (int)N.user.level);
    else if (IS('I','P')) strcpy(out, N.ipstr);
    else if (IS('T','T')) strcpy(out, term_name(N.term));
    else if (IS('C','S')) strcpy(out, charset_name(N.charset));
    else if (IS('C','O')) sprintf(out, "%d", (int)N.cols);
    else if (IS('R','O')) sprintf(out, "%d", (int)N.rows);
    else if (IS('P','O')) sprintf(out, "%lu", N.user.posts);
    else if (IS('U','P')) sprintf(out, "%lu", N.user.uploads);
    else if (IS('D','N')) sprintf(out, "%lu", N.user.downloads);
    else if (IS('V','R')) strcpy(out, BBS_VERSION);
    else if (IS('C','L')) strcpy(out, "\x0C");
    else if (IS('C','R')) strcpy(out, "\n");
    else if (IS('P','A')) strcpy(out, "\x01");     /* pause marker */
    else if (IS('M','A') || IS('F','A')) {
        extern const char *msg_area_name(void);
        extern const char *file_area_name(void);
        strcpy(out, c0 == 'M' ? msg_area_name() : file_area_name());
    }
    else { out[0] = '|'; out[1] = c0; out[2] = c1; out[3] = 0; }
#undef IS
}

/* output with pipe/MCI codes; the rest goes through tputraw() */
static void tput_codes(const UBYTE *s, LONG len, UBYTE srccs)
{
    const UBYTE *run = s;
    while (len > 0) {
        if (*s == '|' && len >= 3) {
            UBYTE a = s[1], b = s[2];
            if (a >= '0' && a <= '2' && b >= '0' && b <= '9' && (a - '0') * 10 + (b - '0') <= 23) {
                if (s > run) tputraw(run, s - run, srccs);
                tcolor((a - '0') * 10 + (b - '0'));
                s += 3; len -= 3; run = s;
                continue;
            }
            if (a >= 'A' && a <= 'Z' && b >= 'A' && b <= 'Z') {
                char val[80];
                char code[2];
                code[0] = a; code[1] = b;
                expand_mci(code, val);
                if (val[0] != '|') {
                    if (s > run) tputraw(run, s - run, srccs);
                    if (val[0] == 1 && !val[1]) tpause();
                    else tputraw((UBYTE *)val, strlen(val), CS_CP437);
                    s += 3; len -= 3; run = s;
                    continue;
                }
            }
        }
        s++; len--;
    }
    if (s > run) tputraw(run, s - run, srccs);
}

void tputs(const char *s)
{
    tput_codes((const UBYTE *)s, strlen(s), CS_CP437);
}

void tprintf(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    tputs(buf);
}

/* ---- display files ------------------------------------------------------------- */

#define MAXSHOW (256 * 1024)

BOOL tshowpath(const char *path, UBYTE srccs)
{
    LONG size = file_size(path), n;
    UBYTE *buf, *eof;
    BPTR fh;
    if (size <= 0) return FALSE;
    if (size > MAXSHOW) size = MAXSHOW;
    if (!(buf = AllocVec(size, MEMF_ANY))) return FALSE;
    if (!(fh = Open((STRPTR)path, MODE_OLDFILE))) { FreeVec(buf); return FALSE; }
    n = Read(fh, buf, size);
    Close(fh);
    if (n > 0) {
        /* SAUCE metadata and anything after the DOS EOF marker isn't shown */
        if ((eof = memchr(buf, 0x1A, n))) n = eof - buf;
        tput_codes(buf, n, srccs);
    }
    FreeVec(buf);
    tcolor(7);
    return TRUE;
}

BOOL tshowfile(const char *name)
{
    static const char *ansi_ext[]  = { ".ans", ".asc", ".txt", NULL };
    static const char *vt_ext[]    = { ".vt", ".ans", ".asc", ".txt", NULL };
    static const char *ascii_ext[] = { ".asc", ".txt", ".ans", NULL };
    /* the Amiga console has no block graphics, so a local logon gets the
     * plain-text screens first */
    const char **ext = N.local ? ascii_ext : N.term == TT_ANSI ? ansi_ext : N.term == TT_VT100 ? vt_ext : ascii_ext;
    const char **e;
    char path[PATHLEN];
    /* a translation may bring its own screens in BBS:Text/<language>/; anything it
       doesn't have (the board's custom ANSI screens) comes from BBS:Text for everyone */
    if (lang_count() && strlen(name) < 100)
        for (e = ext; *e; e++) {
            sprintf(path, "BBS:Text/%s/%s%s", lang_current(), name, *e);
            if (file_exists(path)) return tshowpath(path, CS_CP437);
        }
    for (e = ext; *e; e++) {
        sprintf(path, "BBS:Text/%s%s", name, *e);
        if (file_exists(path)) return tshowpath(path, CS_CP437);
    }
    return FALSE;
}

/* ---- node messages ----------------------------------------------------------------- */

void tcheck_messages(void)
{
    struct NodeMsg m;
    BOOL any = FALSE;
    N.msg_waiting = FALSE;
    if (!N.ni) return;
    for (;;) {
        BOOL got = FALSE;
        shared_lock(N.S);
        if (N.ni->msg_tail != N.ni->msg_head) {
            m = N.ni->msgq[N.ni->msg_tail];
            N.ni->msg_tail = (N.ni->msg_tail + 1) % NODE_MSGQ;
            got = TRUE;
        }
        shared_unlock(N.S);
        if (!got) break;
        if (!any) { tputs("\n"); any = TRUE; }
        tputs("\x07");
        if (m.type == NM_CHATREQ && tele_chat_request(&m)) continue;   /* sysop chat / page */
        if (m.type == NM_SYSOP)
            tprintf(L("term.tcheck_messages.sysop_broadcast", "|12*** |15Sysop broadcast|12 ***|07 %s\n"), m.text);
        else if (m.type == NM_CHATREQ)
            tprintf(L("term.tcheck_messages.node_wants_to", "|14>> |15%s |07(node %d) |14wants to chat - use |15Chat|14 from the main menu.|07\n"),
                    m.fromname, (int)m.from);
        else if (!m.from)                   /* from the daemon (ARexx SEND) */
            tprintf("|11>> |15%s|11:|07 %s\n", m.fromname, m.text);
        else
            tprintf(L("term.tcheck_messages.node", "|11>> |15%s |07(node %d)|11:|07 %s\n"), m.fromname, (int)m.from, m.text);
    }
    if (any && N.reprompt) { tputs("\n"); tputs(N.reprompt); }
}

/* ---- input ------------------------------------------------------------------------ */

UBYTE uni_to_cp437(UWORD u)
{
    int i;
    if (u < 128) return (UBYTE)u;
    for (i = 128; i < 256; i++) if (cp437_to_uni((UBYTE)i) == u) return (UBYTE)i;
    return '?';
}

/* read one raw byte, waiting up to ms; -1 on timeout */
static LONG raw_byte(ULONG ms)
{
    LONG c = in_get();
    if (c >= 0) return c;
    tn_wait(ms, 0, NULL);
    return in_get();
}

static LONG decode_escape(void)
{
    LONG c = raw_byte(300), d, num = 0;
    if (c < 0) return 27;
    if (c != '[' && c != 'O') { in_unget((UBYTE)c); return 27; }
    d = raw_byte(300);
    while (d >= '0' && d <= '9') { num = num * 10 + (d - '0'); d = raw_byte(300); }
    switch (d) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': case 'K': return KEY_END;
    case '~':
        switch (num) {
        case 1: case 7: return KEY_HOME;
        case 2: return KEY_INS;
        case 3: return KEY_DEL;
        case 4: case 8: return KEY_END;
        case 5: return KEY_PGUP;
        case 6: return KEY_PGDN;
        }
    }
    return KEY_NONE;
}

/* idle_minutes, looked up once per config (tgetkey runs for every key typed and
   cfg_str walks the whole list); N.cfg is loaded once per node, but a new
   config pointer refreshes it anyway */
static LONG idle_limit_secs(void)
{
    static struct Cfg *seen;
    static LONG secs;
    static BOOL have;
    if (!have || seen != N.cfg) {
        secs = cfg_int(N.cfg, "idle_minutes", 10) * 60;
        seen = N.cfg;
        have = TRUE;
    }
    return secs;
}

LONG tgetkey(LONG timeout_secs)
{
    ULONG start = bbs_now();
    LONG idle_limit = idle_limit_secs();
    for (;;) {
        LONG c, t;
        ULONG now;
        if (!N.online) return KEY_HANGUP;
        if (N.msg_waiting) {
            tcheck_messages();
            return KEY_NONE;                /* callers redraw their line */
        }
        now = bbs_now();
        t = time_left_mins();
        if (N.loggedin && t == 0) {
            tputs(L("term.tgetkey.your_time_is", "\n|12Your time is up for today - thanks for calling!|07\n"));
            tn_flush();
            node_hangup("time limit");
            return KEY_HANGUP;
        }
        c = in_get();
        if (c < 0) {
            tn_wait(1000, 0, NULL);
            c = in_get();
        }
        now = bbs_now();
        if (c < 0) {
            if (timeout_secs && now - start >= (ULONG)timeout_secs) return KEY_TIMEOUT;
            if (idle_limit && !N.sysop) {
                ULONG idle = now - N.last_input;
                if (idle >= (ULONG)idle_limit) {
                    tputs(L("term.tgetkey.idle_too_long", "\n|12Idle too long - disconnecting.|07\n"));
                    tn_flush();
                    node_hangup("idle timeout");
                    return KEY_HANGUP;
                }
                if (idle >= (ULONG)idle_limit - 60 && !N.idle_warned) {
                    N.idle_warned = 1;
                    tputs(L("term.tgetkey.are_you_still", "\x07\n|14Are you still there? Press a key or you'll be disconnected in a minute.|07\n"));
                    return KEY_NONE;
                }
            }
            continue;
        }
        N.last_input = now;
        N.idle_warned = 0;
        if (c == 27) return decode_escape();
        if (c == 0x7F) return 8;
        if (c >= 0x80) {
            if (N.charset == CS_UTF8) {
                UWORD u;
                int more;
                if ((c & 0xE0) == 0xC0)      { u = c & 0x1F; more = 1; }
                else if ((c & 0xF0) == 0xE0) { u = c & 0x0F; more = 2; }
                else if ((c & 0xF8) == 0xF0) { u = 0xFFFF; more = 3; }
                else continue;               /* stray continuation byte */
                while (more-- > 0) {
                    LONG d = raw_byte(200);
                    if (d < 0 || (d & 0xC0) != 0x80) break;
                    u = (UWORD)((u << 6) | (d & 0x3F));
                }
                return uni_to_cp437(u);
            }
            if (N.charset == CS_LATIN1) return uni_to_cp437((UWORD)c);
        }
        return c;
    }
}

/* echo one internal (CP437) character */
static void echo_char(UBYTE c)
{
    tputraw(&c, 1, CS_CP437);
}

static void erase_chars(LONG n)
{
    while (n-- > 0) tn_raw((const UBYTE *)"\b \b", 3);
}

LONG tgetline(char *buf, LONG max, UWORD flags)
{
    LONG len = 0, k;
    if (flags & GL_EDIT) {
        len = strlen(buf);
        if (len >= max) len = max - 1;
        buf[len] = 0;
        if (flags & GL_MASK) { LONG i; for (i = 0; i < len; i++) echo_char('*'); }
        else tputraw((UBYTE *)buf, len, CS_CP437);
    } else buf[0] = 0;

    for (;;) {
        k = tgetkey(0);
        if (k == KEY_HANGUP) { buf[len] = 0; return -1; }
        if (k == KEY_NONE) {                        /* redraw after a message */
            buf[len] = 0;
            if (flags & GL_MASK) { LONG i; for (i = 0; i < len; i++) echo_char('*'); }
            else tputraw((UBYTE *)buf, len, CS_CP437);
            continue;
        }
        if (k == '\r') break;
        if (k == 8 || k == KEY_LEFT) {
            if (len > 0) { len--; erase_chars(1); }
            continue;
        }
        if (k == 21 || k == 24) {                   /* ^U / ^X: kill line */
            erase_chars(len);
            len = 0;
            continue;
        }
        if (k == 23) {                              /* ^W: delete word */
            while (len > 0 && buf[len - 1] == ' ') { len--; erase_chars(1); }
            while (len > 0 && buf[len - 1] != ' ') { len--; erase_chars(1); }
            continue;
        }
        if (k < 32 || k > 255) continue;
        if ((flags & GL_DIGITS) && (k < '0' || k > '9')) continue;
        if (len >= max - 1) { tn_raw((const UBYTE *)"\x07", 1); continue; }
        if ((flags & GL_NAME) && k == ' ' && (len == 0 || buf[len - 1] == ' ')) continue;
        if (flags & GL_UPPER) { if (k >= 'a' && k <= 'z') k -= 32; }
        else if (flags & GL_NAME) {
            BOOL start = len == 0 || buf[len - 1] == ' ';
            if (start && k >= 'a' && k <= 'z') k -= 32;
        }
        buf[len++] = (char)k;
        echo_char((flags & GL_MASK) ? '*' : (UBYTE)k);
    }
    buf[len] = 0;
    if (flags & GL_NAME) while (len > 0 && buf[len - 1] == ' ') buf[--len] = 0;
    if (!(flags & GL_NOCR)) newline_out();
    return len;
}

LONG tgethot(const char *valid)
{
    for (;;) {
        LONG k = tgetkey(0);
        if (k == KEY_HANGUP) return KEY_HANGUP;
        if (k >= 'a' && k <= 'z') k -= 32;
        if (k == '\r' && strchr(valid, '\r')) return '\r';
        if (k > 0 && k < 256 && k != '\r' && strchr(valid, (int)k)) return k;
    }
}

BOOL tyesno(const char *prompt, BOOL def)
{
    LONG k;
    tputs(prompt);
    tputs(def ? " |08(|15Y|07/n|08)|07 " : " |08(|07y/|15N|08)|07 ");
    k = tgethot("YN\r");
    if (k == '\r') k = def ? 'Y' : 'N';
    tputs(k == 'Y' ? L("term.tyesno.yes", "Yes\n") : L("term.tyesno.no", "No\n"));
    return k == 'Y';
}

void tpause(void)
{
    const char *p = L("term.pause", "[Press any key]");
    LONG k;
    BOOL paging = N.paging;
    N.paging = FALSE;
    tputs("|08");
    tputraw((const UBYTE *)p, strlen(p), CS_CP437);
    tputs("|07");
    do k = tgetkey(0); while (k == KEY_NONE);
    tn_raw((const UBYTE *)"\r", 1);
    if (N.term != TT_ASCII) tcleol();
    else {
        char sp[80];
        LONG w = strlen(p);
        if (w < 15) w = 15;
        if (w > 78) w = 78;
        memset(sp, ' ', w); sp[w] = '\r';
        tn_raw((const UBYTE *)sp, w + 1);
    }
    N.lines_out = 0;
    N.paging = paging;
}

/* ---- terminal detection -------------------------------------------------------------- */

/* Wait for a cursor position report ESC [ row ; col R.  Other input is
 * discarded.  Returns TRUE and fills row/col if one arrived in time. */
static BOOL read_cpr(LONG ms, int *row, int *col)
{
    ULONG start = bbs_now();
    int state = 0, a = 0, b = 0;
    while (N.online) {
        LONG c = in_get();
        if (c < 0) {
            if ((bbs_now() - start) * 1000 >= (ULONG)ms) return FALSE;
            tn_wait(200, 0, NULL);
            continue;
        }
        switch (state) {
        case 0: if (c == 27) state = 1; break;
        case 1: state = (c == '[') ? 2 : 0; a = b = 0; break;
        case 2:
            if (c >= '0' && c <= '9') a = a * 10 + (c - '0');
            else if (c == ';') state = 3;
            else state = 0;
            break;
        case 3:
            if (c >= '0' && c <= '9') b = b * 10 + (c - '0');
            else if (c == 'R') { *row = a; *col = b; return TRUE; }
            else state = 0;
            break;
        }
    }
    return FALSE;
}

static BOOL ttype_has(const char *what) { return str_istr(N.ttypes, what) != NULL; }

void tdetect(void)
{
    int row, col;
    BOOL ansi = FALSE;

    N.term = TT_ASCII;
    N.charset = CS_ASCII;
    tn_raw((const UBYTE *)"\r\nDetecting terminal... ", 24);

    /* 1. does it answer a cursor position request? */
    tn_rawflush((const UBYTE *)"\x1b[6n", 4);
    if (read_cpr(2500, &row, &col)) ansi = TRUE;
    else if (ttype_has("ANSI") || ttype_has("XTERM") || ttype_has("VT1") ||
             ttype_has("VT2") || ttype_has("SYNCTERM") || ttype_has("LINUX"))
        ansi = TRUE;                        /* claims it; some don't answer CPR */

    if (ansi) {
        N.term = (ttype_has("VT100") || ttype_has("VT102") || ttype_has("VT220")) &&
                 !ttype_has("ANSI") && !ttype_has("XTERM") ? TT_VT100 : TT_ANSI;
        N.charset = CS_CP437;

        /* 2. screen size if NAWS didn't tell us */
        if (!N.remote_naws) {
            tn_rawflush((const UBYTE *)"\x1b[s\x1b[255;255H\x1b[6n\x1b[u", 21);
            if (read_cpr(2000, &row, &col)) {
                if (col >= 20 && col <= 255) N.cols = col;
                if (row >= 5 && row <= 255)  N.rows = row;
            }
        }

        /* 3. UTF-8?  Print a 3-byte UTF-8 character from column 1: a UTF-8
         * terminal advances one column, an 8-bit one advances three. */
        tn_rawflush((const UBYTE *)"\r\xE2\x94\x80\x1b[6n", 8);
        if (read_cpr(2000, &row, &col) && col == 2) N.charset = CS_UTF8;
        else if (ttype_has("AMIGA")) N.charset = CS_LATIN1;
        /* a real VT100 has no CP437 glyphs: 7-bit, box drawing via DEC graphics */
        else if (N.term == TT_VT100) N.charset = CS_ASCII;
        tn_raw((const UBYTE *)"\r\x1b[K", 4);
    } else {
        /* no reply: probably a raw client - the CPR request may be on screen */
        tn_raw((const UBYTE *)"\r                              \r", 32);
        if (ttype_has("AMIGA")) N.charset = CS_LATIN1;
    }
    tn_flush();
    N.in_head = N.in_tail = 0;              /* stray replies */
    if (N.ni) N.ni->termtype = N.term;
}

void term_choose(void)
{
    LONG k;
    tputs(L("term.choose.terminal_type_ansi", "\n|15Terminal type|07\n"
          "  |15A|07) ANSI colour     |15V|07) VT100 (no colour)     |15T|07) Plain text\n"
          "|07Choice: "));
    k = tgethot("AVT\r");
    if (k == KEY_HANGUP) return;
    if (k == 'A') N.term = TT_ANSI;
    else if (k == 'V') N.term = TT_VT100;
    else if (k == 'T') N.term = TT_ASCII;
    tputs("\n");
    tputs(L("term.choose.character_set_ibm", "\n|15Character set|07\n"
          "  |15I|07) IBM PC / CP437 (SyncTERM, NetRunner)\n"
          "  |15U|07) UTF-8 (PuTTY, xterm, macOS/Linux terminals)\n"
          "  |15A|07) Amiga (NComm, Term, AmigaTerm - ISO-8859-1)\n"
          "  |157|07) 7-bit ASCII\n"
          "|07Choice: "));
    k = tgethot("IUA7\r");
    if (k == KEY_HANGUP) return;
    if (k == 'I') N.charset = CS_CP437;
    else if (k == 'U') N.charset = CS_UTF8;
    else if (k == 'A') N.charset = CS_LATIN1;
    else if (k == '7') N.charset = CS_ASCII;
    tputs("\n");
    if (N.ni) N.ni->termtype = N.term;
}
