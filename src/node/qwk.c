/*
 * qwk.c - QWK offline mail: pack new messages into a .QWK packet, take
 * replies back from a .REP packet.
 *
 * A packet is an archive (LhA by default, Zip if you set it up) holding:
 *   CONTROL.DAT   BBS name, sysop, the conferences (= our message areas)
 *   MESSAGES.DAT  128-byte records: a copyright record, then per message a
 *                 header record and the text (lines end in 0xE3), space padded
 *   nnn.NDX       per conference: 5-byte records, the header's record number
 *                 as a Microsoft-BASIC float + the conference
 *   DOOR.ID       what we are
 * A .REP holds <BBSID>.MSG in the same record format, with the conference
 * number in the header's message-number field.
 *
 * NilBBS.cfg:
 *   qwk_bbsid      = MYBBS      ; packet name, up to 8 letters
 *   qwk_max        = 500        ; messages per packet
 *   qwk_pack       = LhA >NIL: -q -m a "%a" #?      ; run in the work drawer
 *   qwk_unpack_lha = LhA >NIL: -q -m x "%a"
 *   qwk_unpack_zip = UnZip >NIL: -o -q "%a"
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <utility/date.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "node.h"
#include "../common/msgbase.h"

/* msgui.c */
int  msg_area_count(void);
const char *msg_area_tag_n(int i);
const char *msg_area_name_n(int i);
BOOL msg_area_readable_n(int i);
BOOL msg_area_is_email(int i);
BOOL msg_visible(int i, struct MsgHdr *h);
ULONG msg_post_text(int ai, const char *to, const char *subj, ULONG replyto, const char *text);
/* fileui.c */

#define QWK_REC   128
#define QWK_EOL   0xE3

static char workdir[PATHLEN];
static char bbsid[10];

static void get_bbsid(void)
{
    const char *s = cfg_str(N.cfg, "qwk_bbsid", "");
    int n = 0;
    if (!*s) s = cfg_str(N.cfg, "bbs_name", "NILBBS");
    for (; *s && n < 8; s++) {
        char c = *s;
        if (c >= 'a' && c <= 'z') c -= 32;
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) bbsid[n++] = c;
    }
    if (!n) strcpy(bbsid, "NILBBS"); else bbsid[n] = 0;
}

/* BBS:Nodes/Node<n>/qwk, emptied */
static BOOL clean_workdir(void)
{
    struct FileInfoBlock *fib;
    BPTR l;
    sprintf(workdir, "BBS:Nodes/Node%d/qwk", N.node);
    if (!(l = Lock((STRPTR)workdir, ACCESS_READ))) {
        if (!(l = CreateDir((STRPTR)workdir))) return FALSE;
        UnLock(l);
        return TRUE;
    }
    if ((fib = AllocDosObject(DOS_FIB, NULL))) {
        char names[64][108];
        int n = 0, i;
        if (Examine(l, fib))
            while (n < 64 && ExNext(l, fib)) if (fib->fib_DirEntryType < 0) str_copy(names[n++], fib->fib_FileName, 108);
        FreeDosObject(DOS_FIB, fib);
        for (i = 0; i < n; i++) {
            char p[PATHLEN];
            sprintf(p, "%s/%s", workdir, names[i]);
            DeleteFile((STRPTR)p);
        }
    }
    UnLock(l);
    return TRUE;
}

/* run an archiver command with the work drawer as the current directory */
static LONG run_in_workdir(const char *tmpl, const char *archive)
{
    char cmd[PATHLEN * 2], *d = cmd;
    const char *s;
    BPTR lock, old, in, out;
    LONG rc;
    for (s = tmpl; *s && d < cmd + sizeof(cmd) - PATHLEN; s++) {
        if (s[0] == '%' && s[1] == 'a') { strcpy(d, archive); d += strlen(d); s++; }
        else *d++ = *s;
    }
    *d = 0;
    if (!(lock = Lock((STRPTR)workdir, ACCESS_READ))) return -1;
    old = CurrentDir(lock);
    in  = Open((STRPTR)"NIL:", MODE_OLDFILE);
    out = Open((STRPTR)"NIL:", MODE_NEWFILE);
    rc = SystemTags((STRPTR)cmd, SYS_Input, in, SYS_Output, out, NP_StackSize, 16384, TAG_END);
    Close(in);
    Close(out);
    CurrentDir(old);
    UnLock(lock);
    return rc;
}

static void fput_field(UBYTE *rec, int off, int len, const char *s)
{
    int i;
    for (i = 0; i < len; i++) rec[off + i] = (UBYTE)(*s ? *s++ : ' ');
}

/* an integer as a Microsoft-BASIC single, the NDX format */
static void msbin(ULONG n, UBYTE *out)
{
    int e = 0;
    ULONG m;
    if (!n) { out[0] = out[1] = out[2] = out[3] = 0; return; }
    while ((1UL << e) <= n) e++;                /* n = 0.1xxx (binary) * 2^e */
    m = (n << (24 - e)) & 0x7FFFFF;             /* drop the implied leading 1; sign 0 */
    out[0] = (UBYTE)m;
    out[1] = (UBYTE)(m >> 8);
    out[2] = (UBYTE)(m >> 16);
    out[3] = (UBYTE)(e + 128);
}

static void datetime(ULONG t, char *date, char *tm, BOOL fouryear)
{
    struct ClockData cd;
    Amiga2Date(t, &cd);
    if (fouryear) sprintf(date, "%02d-%02d-%04d", cd.month, cd.mday, cd.year);
    else sprintf(date, "%02d-%02d-%02d", cd.month, cd.mday, cd.year % 100);
    sprintf(tm, "%02d:%02d", cd.hour, cd.min);
}

/* ---- download: build and send the packet ---------------------------------------- */

void qwk_download(void)
{
    char path[PATHLEN], date[12], tm[8], qwkname[16], qwkpath[PATHLEN];
    ULONG newlast[MAX_MSGAREAS];
    struct MsgHdr h;
    char *text;
    BPTR mf, cf;
    UBYTE rec[QWK_REC];
    LONG recno = 1, total = 0, max = cfg_int(N.cfg, "qwk_max", 500);
    int na = msg_area_count(), ai, ncon = 0;

    get_bbsid();
    if (!clean_workdir()) { tputs(L("qwk.download.cant_make_the", "|12Can't make the work drawer.|07\n")); return; }
    if (!(text = AllocVec(MAX_MSGTEXT + 1, 0))) return;
    sprintf(path, "%s/MESSAGES.DAT", workdir);
    if (!(mf = Open((STRPTR)path, MODE_NEWFILE))) { FreeVec(text); return; }
    memset(rec, ' ', QWK_REC);
    fput_field(rec, 0, QWK_REC, "Produced by NilBBS " BBS_VERSION " - a native AmigaOS BBS");
    Write(mf, rec, QWK_REC);

    set_activity("Packing QWK mail");
    tputs(L("qwk.download.packing_new_messages", "\n|07Packing new messages"));
    for (ai = 0; ai < na && total < max; ai++) {
        LONG count, num;
        BPTR nf = 0;
        struct MsgScan ms;
        newlast[ai] = N.user.lastread[ai];
        if (!msg_area_readable_n(ai)) continue;
        ncon++;
        ObtainSemaphore(&N.S->msglock);
        count = msg_count(msg_area_tag_n(ai));
        ReleaseSemaphore(&N.S->msglock);
        msg_scan_init(&ms, msg_area_tag_n(ai));     /* headers 16 per read */
        for (num = N.user.lastread[ai] + 1; num <= count && total < max; num++) {
            LONG len, blocks, i;
            BOOL ok;
            ObtainSemaphore(&N.S->msglock);
            ok = msg_scan_hdr(&ms, num, &h);
            len = ok ? msg_read_text(msg_area_tag_n(ai), &h, text, MAX_MSGTEXT) : 0;
            ReleaseSemaphore(&N.S->msglock);
            newlast[ai] = num;
            if (!ok || !msg_visible(ai, &h)) continue;
            if (len < 0) len = 0;
            text[len] = 0;
            /* the body: \n -> 0xE3, FidoNet kludge lines (^A) left out */
            {
                char *s = text, *d = text;
                while (*s) {
                    if (*s == 1 && (s == text || s[-1] == '\n')) { while (*s && *s != '\n') s++; if (*s) s++; continue; }
                    *d++ = (*s == '\n') ? (char)QWK_EOL : *s;
                    s++;
                }
                *d = 0;
                len = d - text;
            }
            blocks = 1 + (len + QWK_REC - 1) / QWK_REC;
            memset(rec, ' ', QWK_REC);
            rec[0] = msg_area_is_email(ai) ? '+' : ' ';
            {
                char b[16];
                sprintf(b, "%lu", h.num); fput_field(rec, 1, 7, b);
                datetime(h.date, date, tm, FALSE);
                fput_field(rec, 8, 8, date);
                fput_field(rec, 16, 5, tm);
                fput_field(rec, 21, 25, h.to);
                fput_field(rec, 46, 25, h.from);
                fput_field(rec, 71, 25, h.subject);
                if (h.replyto) { sprintf(b, "%lu", h.replyto); fput_field(rec, 108, 8, b); }
                sprintf(b, "%ld", blocks); fput_field(rec, 116, 6, b);
            }
            rec[122] = 0xE1;
            rec[123] = (UBYTE)ai;           rec[124] = (UBYTE)(ai >> 8);
            rec[125] = (UBYTE)(total + 1);  rec[126] = (UBYTE)((total + 1) >> 8);
            rec[127] = ' ';
            recno++;
            Write(mf, rec, QWK_REC);
            /* index entry: this header's record number (1-based) */
            if (!nf) {
                sprintf(path, "%s/%03d.NDX", workdir, ai);
                nf = Open((STRPTR)path, MODE_NEWFILE);
            }
            if (nf) {
                UBYTE ndx[5];
                msbin((ULONG)recno, ndx);
                ndx[4] = (UBYTE)ai;
                Write(nf, ndx, 5);
            }
            for (i = 0; i < blocks - 1; i++) {
                LONG n = len - i * QWK_REC;
                memset(rec, ' ', QWK_REC);
                memcpy(rec, text + i * QWK_REC, n > QWK_REC ? QWK_REC : n);
                Write(mf, rec, QWK_REC);
                recno++;
            }
            total++;
            if (total % 10 == 0) { tputs("."); tn_flush(); }
        }
        msg_scan_done(&ms);
        if (nf) Close(nf);
    }
    for (; ai < na; ai++) newlast[ai] = N.user.lastread[ai];
    Close(mf);
    FreeVec(text);
    tprintf(total == 1 ? L("qwk.download.messages_one", " |15%ld|07 message.\n") : L("qwk.download.messages_many", " |15%ld|07 messages.\n"), total);

    /* CONTROL.DAT */
    sprintf(path, "%s/CONTROL.DAT", workdir);
    if ((cf = Open((STRPTR)path, MODE_NEWFILE))) {
        char line[120], uname[NAMELEN];
        int i;
        datetime(bbs_now(), date, tm, TRUE);
        for (i = 0; N.user.name[i] && i < NAMELEN - 1; i++)
            uname[i] = (N.user.name[i] >= 'a' && N.user.name[i] <= 'z') ? N.user.name[i] - 32 : N.user.name[i];
        uname[i] = 0;
        sprintf(line, "%s\r\n%s\r\n%s\r\n%s\r\n0,%s\r\n%s,%s:00\r\n%s\r\n\r\n0\r\n%ld\r\n%d\r\n",
                cfg_str(N.cfg, "bbs_name", "NilBBS"), cfg_str(N.cfg, "qwk_location", "The Amiga"),
                cfg_str(N.cfg, "qwk_phone", "telnet"), cfg_str(N.cfg, "sysop_name", "Sysop"),
                bbsid, date, tm, uname, total, ncon > 0 ? ncon - 1 : 0);
        FPuts(cf, (STRPTR)line);
        for (ai = 0; ai < na; ai++) {
            char cn[14];
            if (!msg_area_readable_n(ai)) continue;
            str_copy(cn, msg_area_tag_n(ai), sizeof(cn));
            sprintf(line, "%d\r\n%s\r\n", ai, cn);
            FPuts(cf, (STRPTR)line);
        }
        FPuts(cf, (STRPTR)"\r\n\r\n\r\n");         /* no hello / news / goodbye files */
        Close(cf);
    }
    sprintf(path, "%s/DOOR.ID", workdir);
    if ((cf = Open((STRPTR)path, MODE_NEWFILE))) {
        FPuts(cf, (STRPTR)"DOOR = NilBBS\r\nVERSION = " BBS_VERSION "\r\nSYSTEM = NilBBS " BBS_VERSION
                          " (AmigaOS)\r\nCONTROLNAME = NILBBS\r\nCONTROLTYPE = ADD\r\nCONTROLTYPE = DROP\r\n"
                          "MIXEDCASE = YES\r\n");
        Close(cf);
    }
    if (!total) {
        tputs(L("qwk.download.nothing_new_no", "|08Nothing new - no packet this time.|07\n"));
        back_to_menu();
        return;
    }

    sprintf(qwkname, "%s.QWK", bbsid);
    sprintf(qwkpath, "BBS:Nodes/Node%d/%s", N.node, qwkname);
    DeleteFile((STRPTR)qwkpath);
    run_in_workdir(cfg_str(N.cfg, "qwk_pack", "LhA >NIL: -q -m a \"%a\" #?"), qwkpath);
    if (file_size(qwkpath) <= 0) {
        tputs(L("qwk.download.the_archiver_failed", "|12The archiver failed - check qwk_pack in NilBBS.cfg.|07\n"));
        back_to_menu();
        return;
    }
    tprintf(L("qwk.download.packet_bytes", "|07Packet |15%s|07, |15%ld|07 bytes."), qwkname, file_size(qwkpath));
    if (file_send_path(qwkpath, qwkname)) {
        memcpy(N.user.lastread, newlast, sizeof(ULONG) * na);
        user_save();
        tputs(L("qwk.download.sent_those_messages", "|10Sent - those messages are now marked as read.|07\n"));
        bbs_log(BBS_SYSLOG, "node %d: %s downloaded a QWK packet (%ld messages)", N.node, N.user.name, total);
    } else tputs(L("qwk.download.the_packet_didnt", "|12The packet didn't go through - nothing was marked as read.|07\n"));
    DeleteFile((STRPTR)qwkpath);
    back_to_menu();
}

/* ---- upload: take a .REP and post its replies -------------------------------------- */

static void trim(char *s)
{
    int n = strlen(s);
    while (n > 0 && s[n - 1] == ' ') s[--n] = 0;
}

void qwk_upload(void)
{
    char names[4][32], rep[PATHLEN], msgf[PATHLEN];
    BPTR fh;
    UBYTE rec[QWK_REC], magic[4];
    LONG got, posted = 0, refused = 0;
    char *text;

    get_bbsid();
    if (!clean_workdir()) return;
    memset(names, 0, sizeof(names));
    {
        char xname[20];
        sprintf(xname, "%s.REP", bbsid);
        tprintf(L("qwk.upload.send_your_reply", "\n|07Send your reply packet |15%s|07."), xname);
        got = file_receive_dir(workdir, names, 1, xname);
    }
    if (got <= 0) { tputs(L("qwk.upload.nothing_was_received", "|12Nothing was received.|07\n")); return; }
    sprintf(rep, "%s/%s", workdir, names[0]);

    /* unpack it where it landed: Zip starts "PK", LhA has "-lh" at byte 2 */
    memset(magic, 0, sizeof(magic));
    if ((fh = Open((STRPTR)rep, MODE_OLDFILE))) { Read(fh, magic, 4); Close(fh); }
    run_in_workdir(magic[0] == 'P' && magic[1] == 'K' ?
                   cfg_str(N.cfg, "qwk_unpack_zip", "UnZip >NIL: -o -q \"%a\"") :
                   cfg_str(N.cfg, "qwk_unpack_lha", "LhA >NIL: -q -m x \"%a\""), rep);
    sprintf(msgf, "%s/%s.MSG", workdir, bbsid);
    if (!(fh = Open((STRPTR)msgf, MODE_OLDFILE))) {
        tprintf(L("qwk.upload.theres_no_msg", "|12There's no %s.MSG in that packet - is it for this BBS?|07\n"), bbsid);
        return;
    }
    if (!(text = AllocVec(MAX_MSGTEXT + QWK_REC, 0))) { Close(fh); return; }
    set_activity("Posting QWK replies");
    Read(fh, rec, QWK_REC);                        /* the BBS id record */
    while (Read(fh, rec, QWK_REC) == QWK_REC) {
        char f[30], to[26], from[26], subj[26];
        LONG conf, blocks, ref, len = 0, i;
        memcpy(f, rec + 1, 7); f[7] = 0; conf = atol(f);
        memcpy(f, rec + 108, 8); f[8] = 0; ref = atol(f);
        memcpy(f, rec + 116, 6); f[6] = 0; blocks = atol(f);
        memcpy(to, rec + 21, 25); to[25] = 0; trim(to);
        memcpy(from, rec + 46, 25); from[25] = 0; trim(from);
        memcpy(subj, rec + 71, 25); subj[25] = 0; trim(subj);
        if (blocks < 1 || blocks > MAX_MSGTEXT / QWK_REC) break;         /* corrupt */
        for (i = 1; i < blocks; i++) {
            if (Read(fh, text + len, QWK_REC) != QWK_REC) break;
            len += QWK_REC;
        }
        text[len] = 0;
        /* 0xE3 -> \n, trailing padding off */
        for (i = 0; i < len; i++) if ((UBYTE)text[i] == QWK_EOL) text[i] = '\n';
        while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == 0)) text[--len] = 0;
        if (len && text[len - 1] != '\n') { text[len++] = '\n'; text[len] = 0; }
        if (msg_post_text((int)conf, to, subj, (ULONG)ref, text)) {
            posted++;
            tprintf(L("qwk.upload.posted_to_in", "|10Posted|07 to |15%s|07 in |15%s|07: %s\n"), to[0] ? to : L("qwk.upload.all", "All"),
                    conf >= 0 && conf < msg_area_count() ? msg_area_name_n(conf) : "?", subj);
        } else {
            refused++;
            tprintf(L("qwk.upload.not_posted_conference", "|12Not posted|07 (conference %ld): %s\n"), conf, subj);
        }
        (void)from;                             /* replies are always from the caller */
    }
    Close(fh);
    FreeVec(text);
    tprintf(posted == 1 ? L("qwk.upload.posted_one", "|07%ld reply posted%s.\n") : L("qwk.upload.posted_many", "|07%ld replies posted%s.\n"), posted,
            refused ? L("qwk.upload.some_refused_no", ", some refused (no access, or no such user)") : "");
    bbs_log(BBS_SYSLOG, "node %d: %s uploaded a REP packet: %ld posted, %ld refused",
            N.node, N.user.name, posted, refused);
    back_to_menu();
}
