/*
 * spy.c - the sysop watching a node (BBSCtl WATCH n, BBSControl: double-click a node).
 *
 * Every byte the node sends its caller passes through spy_feed() (from tn_flush), which keeps the
 * last SPY_HIST bytes.  While the node's slot says spy = 1 and the watcher's public port
 * NILBBS.WATCH.<n> exists, each chunk is copied to it as a SpyMsg - the history first, so the
 * watcher starts with the screen as it is.  The caller sees and hears nothing of it.  The watcher
 * frees the messages; the node never waits for it.  The bytes are the telnet stream as sent (IAC
 * doubled, in the caller's charset): the watcher sorts that out.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <proto/exec.h>
#include <string.h>
#include <stdio.h>

#include "node.h"

#define SPY_HIST 16384                      /* a whole ANSI art screen + its menu fits */

static UBYTE hist[SPY_HIST];
static LONG hist_len, hist_head;            /* a ring: hist_head = where the next byte goes */
static BOOL sent_hist;                      /* this watcher already has the history */

static void keep(const UBYTE *p, LONG len)
{
    if (len >= SPY_HIST) { p += len - SPY_HIST; len = SPY_HIST; }
    while (len > 0) {
        LONG n = SPY_HIST - hist_head;
        if (n > len) n = len;
        memcpy(hist + hist_head, p, n);
        hist_head = (hist_head + n) % SPY_HIST;
        hist_len = hist_len + n > SPY_HIST ? SPY_HIST : hist_len + n;
        p += n; len -= n;
    }
}

/* hand one chunk to the watcher's port (if it's still there) - FALSE = nobody is watching */
static BOOL post(const char *port, const UBYTE *p, LONG len)
{
    struct SpyMsg *m = AllocVec(sizeof(struct SpyMsg) + len, MEMF_PUBLIC | MEMF_CLEAR);
    struct MsgPort *mp;
    if (!m) return TRUE;
    m->msg.mn_Node.ln_Type = NT_MESSAGE;
    m->msg.mn_Length = sizeof(struct SpyMsg) + len;
    m->msg.mn_ReplyPort = NULL;             /* no reply: the watcher FreeVec()s it */
    m->len = len;
    memcpy(m->data, p, len);
    Forbid();
    if ((mp = FindPort((STRPTR)port))) PutMsg(mp, &m->msg);
    Permit();
    if (!mp) { FreeVec(m); return FALSE; }
    return TRUE;
}

/* the history for a new watcher, in order, starting at the last clear-screen if there is one
 * (so the watcher gets one whole screen, not the tail end of an art file) */
static BOOL post_history(const char *port)
{
    static UBYTE lin[SPY_HIST];
    LONG start = (hist_head - hist_len + SPY_HIST) % SPY_HIST, first = SPY_HIST - start, i, from = 0;
    if (first > hist_len) first = hist_len;
    memcpy(lin, hist + start, first);
    memcpy(lin + first, hist, hist_len - first);
    for (i = hist_len - 4; i >= 0; i--)
        if (lin[i] == 27 && lin[i + 1] == '[' && lin[i + 2] == '2' && lin[i + 3] == 'J') { from = i; break; }
    return post(port, lin + from, hist_len - from);
}

/* called while the node waits for its caller (at least once a second): a watcher who has just
 * started gets the recent screen straight away - otherwise an idle caller's watch stays blank
 * until the node next sends something */
void spy_poll(void)
{
    char port[32];
    if (!N.ni || !N.ni->spy || sent_hist || !hist_len) return;
    sprintf(port, SPY_PORTFMT, N.node);
    sent_hist = post_history(port);
}

void spy_feed(const UBYTE *p, LONG len)
{
    char port[32];
    if (len <= 0) return;
    if (!N.ni || !N.ni->spy) {
        sent_hist = FALSE;
        keep(p, len);
        return;
    }
    sprintf(port, SPY_PORTFMT, N.node);
    if (!sent_hist && hist_len && !post_history(port)) { keep(p, len); return; }   /* a new watcher: the screen first */
    sent_hist = post(port, p, len);
    keep(p, len);
}
