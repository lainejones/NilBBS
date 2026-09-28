/*
 * msgbase.h - the message base: per area, BBS:Msgs/<TAG>.hdr holds fixed
 * 256-byte headers (message n is record n-1) and <TAG>.txt the bodies,
 * appended.  Bodies use '\n' line ends and CP437 text.
 * Writers hold shared->msglock.
 */
#ifndef NILBBS_MSGBASE_H
#define NILBBS_MSGBASE_H

#include <exec/types.h>

#define MF_DELETED   0x0001
#define MF_PRIVATE   0x0002
#define MF_READ      0x0004     /* private mail: recipient has read it */
#define MF_SENT      0x0008     /* exported to the network */
#define MF_LOCAL     0x0010     /* written on this system */
#define MF_IMPORTED  0x0020     /* came in from the network */
#define MF_HELD      0x0040     /* waiting for a moderator (hold_below / hold_first): only they + the author see it */
#define MF_REPORTED  0x0080     /* a caller reported it to the moderators (reportby says who) */

struct MsgHdr {
    ULONG num;
    ULONG date;
    ULONG replyto;
    ULONG textoff;
    ULONG textlen;
    UWORD flags;
    UWORD pad;
    char  from[36];
    char  to[36];
    char  subject[72];
    ULONG fromid, toid;         /* local user ids, 0 = network / unknown */
    char  origaddr[24];         /* FidoNet address "z:n/f.p" */
    char  destaddr[24];
    ULONG replies;              /* number of the first reply, 0 = none */
    char  reportby[24];         /* MF_REPORTED: who reported it */
    UBYTE reserved[4];
};
#define MSGHDR_SIZE 256
typedef char msghdr_size_check[(sizeof(struct MsgHdr) == MSGHDR_SIZE) ? 1 : -1];

#define MAX_MSGTEXT (32 * 1024)

LONG  msg_count(const char *tag);
BOOL  msg_read_hdr(const char *tag, ULONG num, struct MsgHdr *h);
BOOL  msg_write_hdr(const char *tag, struct MsgHdr *h);
LONG  msg_read_text(const char *tag, const struct MsgHdr *h, char *buf, LONG max);
ULONG msg_add(const char *tag, struct MsgHdr *h, const char *text, LONG len);

/* Reading many headers: a block of MSG_SCAN_BLOCK headers per Open/Seek/Read/Close
 * instead of one each.  The file is never left open between calls, so nothing is
 * held while a caller waits on a caller (paging) and BBSMaint can still swap it.
 * msg_scan_hdr() is msg_read_hdr() with the block cache; call it under msglock
 * wherever msg_read_hdr was.  The cache is a snapshot: use a fresh cursor for
 * each pass that must see other nodes' changes. */
#define MSG_SCAN_BLOCK 16
struct MsgScan {
    const char    *tag;
    struct MsgHdr *buf;         /* MSG_SCAN_BLOCK headers, or &one if out of memory */
    LONG           cap, n;      /* room, headers in buf */
    ULONG          first;       /* number of buf[0], 0 = nothing cached */
    struct MsgHdr  one;
};
void  msg_scan_init(struct MsgScan *ms, const char *tag);
BOOL  msg_scan_hdr(struct MsgScan *ms, ULONG num, struct MsgHdr *h);
void  msg_scan_done(struct MsgScan *ms);

/* every valid header first..last (last 0 = up to msg_count at the start), in order;
 * `lock` (may be NULL) is held only while a block is read, never during `cb`.
 * cb returns FALSE to stop.  Returns the number of the header it stopped on, or 0. */
struct SignalSemaphore;
ULONG msg_scan(const char *tag, ULONG first, ULONG last, struct SignalSemaphore *lock,
               BOOL (*cb)(const struct MsgHdr *h, void *ud), void *ud);

#endif
