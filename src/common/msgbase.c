/*
 * msgbase.c - storage for message areas (see msgbase.h).
 */
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>

#include "bbs.h"
#include "msgbase.h"

static void hdr_path(const char *tag, char *buf)  { sprintf(buf, "BBS:Msgs/%s.hdr", tag); }
static void text_path(const char *tag, char *buf) { sprintf(buf, "BBS:Msgs/%s.txt", tag); }

LONG msg_count(const char *tag)
{
    char p[PATHLEN];
    LONG s;
    hdr_path(tag, p);
    s = file_size(p);
    return s > 0 ? s / MSGHDR_SIZE : 0;
}

BOOL msg_read_hdr(const char *tag, ULONG num, struct MsgHdr *h)
{
    char p[PATHLEN];
    BPTR fh;
    BOOL ok = FALSE;
    if (!num) return FALSE;
    hdr_path(tag, p);
    if (!(fh = Open((STRPTR)p, MODE_OLDFILE))) return FALSE;
    if (Seek(fh, (num - 1) * MSGHDR_SIZE, OFFSET_BEGINNING) >= 0 &&
        Read(fh, h, MSGHDR_SIZE) == MSGHDR_SIZE && h->num == num)
        ok = TRUE;
    Close(fh);
    return ok;
}

void msg_scan_init(struct MsgScan *ms, const char *tag)
{
    ms->tag = tag;
    ms->n = 0;
    ms->first = 0;
    if ((ms->buf = AllocVec(MSG_SCAN_BLOCK * MSGHDR_SIZE, 0))) ms->cap = MSG_SCAN_BLOCK;
    else { ms->buf = &ms->one; ms->cap = 1; }
}

void msg_scan_done(struct MsgScan *ms)
{
    if (ms->buf && ms->buf != &ms->one) FreeVec(ms->buf);
    ms->buf = NULL;
    ms->n = 0;
    ms->first = 0;
}

/* msg_read_hdr() through the block cache: same answers, 1/16th of the DOS calls */
BOOL msg_scan_hdr(struct MsgScan *ms, ULONG num, struct MsgHdr *h)
{
    if (!num || !ms->buf) return FALSE;
    if (!ms->first || num < ms->first || num >= ms->first + (ULONG)ms->n) {
        char p[PATHLEN];
        BPTR fh;
        LONG got = 0;
        ms->first = num;
        ms->n = 0;
        hdr_path(ms->tag, p);
        if (!(fh = Open((STRPTR)p, MODE_OLDFILE))) { ms->first = 0; return FALSE; }
        if (Seek(fh, (num - 1) * MSGHDR_SIZE, OFFSET_BEGINNING) >= 0)
            got = Read(fh, ms->buf, ms->cap * MSGHDR_SIZE);
        Close(fh);
        if (got > 0) ms->n = got / MSGHDR_SIZE;      /* a short last record counts as missing */
        if (!ms->n) { ms->first = 0; return FALSE; }
    }
    *h = ms->buf[num - ms->first];
    return h->num == num;
}

ULONG msg_scan(const char *tag, ULONG first, ULONG last, struct SignalSemaphore *lock,
               BOOL (*cb)(const struct MsgHdr *h, void *ud), void *ud)
{
    struct MsgScan ms;
    struct MsgHdr h;
    ULONG num, stop = 0;
    BOOL ok;
    if (!first) first = 1;
    if (!last) {
        if (lock) ObtainSemaphore(lock);
        last = msg_count(tag);
        if (lock) ReleaseSemaphore(lock);
    }
    msg_scan_init(&ms, tag);
    for (num = first; num <= last; num++) {
        if (lock) ObtainSemaphore(lock);
        ok = msg_scan_hdr(&ms, num, &h);
        if (lock) ReleaseSemaphore(lock);
        if (ok && !cb(&h, ud)) { stop = num; break; }
    }
    msg_scan_done(&ms);
    return stop;
}

BOOL msg_write_hdr(const char *tag, struct MsgHdr *h)
{
    char p[PATHLEN];
    BPTR fh;
    BOOL ok = FALSE;
    if (!h->num) return FALSE;
    hdr_path(tag, p);
    if (!(fh = Open((STRPTR)p, MODE_READWRITE))) return FALSE;
    if (Seek(fh, (h->num - 1) * MSGHDR_SIZE, OFFSET_BEGINNING) >= 0 &&
        Write(fh, h, MSGHDR_SIZE) == MSGHDR_SIZE)
        ok = TRUE;
    Close(fh);
    return ok;
}

LONG msg_read_text(const char *tag, const struct MsgHdr *h, char *buf, LONG max)
{
    char p[PATHLEN];
    BPTR fh;
    LONG n = 0, want = h->textlen;
    if (want > max - 1) want = max - 1;
    text_path(tag, p);
    if ((fh = Open((STRPTR)p, MODE_OLDFILE))) {
        if (Seek(fh, h->textoff, OFFSET_BEGINNING) >= 0) n = Read(fh, buf, want);
        Close(fh);
    }
    if (n < 0) n = 0;
    buf[n] = 0;
    return n;
}

ULONG msg_add(const char *tag, struct MsgHdr *h, const char *text, LONG len)
{
    char p[PATHLEN];
    BPTR fh;
    LONG off;

    text_path(tag, p);
    if (!(fh = Open((STRPTR)p, MODE_READWRITE))) return 0;
    Seek(fh, 0, OFFSET_END);
    off = Seek(fh, 0, OFFSET_END);          /* the second Seek returns the end */
    if (Write(fh, (APTR)text, len) != len) { Close(fh); return 0; }
    Close(fh);

    h->textoff = off;
    h->textlen = len;
    h->num = msg_count(tag) + 1;
    hdr_path(tag, p);
    if (!(fh = Open((STRPTR)p, MODE_READWRITE))) return 0;
    Seek(fh, (h->num - 1) * MSGHDR_SIZE, OFFSET_BEGINNING);
    if (Write(fh, h, MSGHDR_SIZE) != MSGHDR_SIZE) h->num = 0;
    Close(fh);
    return h->num;
}
