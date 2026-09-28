/*
 * cnetc.c - CNet C-language PFile doors (type = cnetc).
 *
 * A CNet C door is started as  "<door> <portname>"  and does
 *     cport = FindPort(argv[1]);  cport->ack = 0;
 *     z = cport->zp;  myp = cport->myp;
 * then talks to the BBS with CallHost(n): a struct CMessage (command number
 * + four args) PutMsg'd to that port.  Doors also read the caller's state
 * straight out of z - z->user1.Handle, z->TimeLeft, z->Carrier, z->Dumped
 * - and write their input results to z->InBuffer.
 *
 * So NilBBS "catches" the calls by being that port: we build a CPort whose
 * zp/myp point at a PortData/MainPort laid out byte-for-byte as in the
 * CNet 4 SDK headers (src/cnetsdk, the SDK that shipped with CNet), fill in
 * the fields doors read from our own user record, and answer each CallHost
 * command.  Output goes through the CNet MCI translator.
 *
 * Supported CallHost commands: 0 ShutDown, 1 PutText, 2 EnterLine, 3 OneKey,
 * 4 EnterPassword, 5 CommonCommands, 6 ReadFile, 7 SetDoing, 9 ReadGraphics,
 * 10 MakeDate, 11 ReadAccount, 12 SaveAccount (refused), 17/21 no-ops,
 * 20 FindAccount, 31 WaitForInput.  Doors that call cnet.library's own
 * database routines (LockAccount, ZGetItem, ...) need a real CNet and are
 * not supported.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <exec/semaphores.h>
#include <devices/serial.h>
#include <graphics/gfx.h>
#include <graphics/rastport.h>
#include <intuition/intuition.h>
#include <dos/dos.h>
#include <dos/dosasl.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "node.h"

/* the SDK was written for SAS/C: neutralise its register keywords */
#undef __saveds
#define __saveds
#define __d0
#define __d1
#define __a0
#define __a1
#define __a2
#define __a3
#define __asm
#include <cnet.h>
/* ...and put them back: the NDK's inline library calls use __asm("a0") */
#undef __asm
#undef __d0
#undef __d1
#undef __a0
#undef __a1
#undef __a2
#undef __a3

/* the layout must match CNet 4 exactly - these are the SDK's own numbers */
typedef char chk1[(offsetof(struct PortData, Carrier) == 38) ? 1 : -1];
typedef char chk2[(offsetof(struct PortData, user1) == 68) ? 1 : -1];
typedef char chk3[(offsetof(struct PortData, Dumped) == 1742) ? 1 : -1];
typedef char chk4[(offsetof(struct CPort, zp) == 40 && offsetof(struct CPort, ack) == 44) ? 1 : -1];
typedef char chk5[(offsetof(struct CMessage, command) == 40) ? 1 : -1];

struct CNetState {
    struct CPort *cp;
    struct PortData *z;
    struct MainPort *myp;
    char   portname[24];
    struct CMessage *waiting;   /* EnterLine / OneKey / EnterPassword / WaitForInput */
    UBYTE  wait_kind;           /* 2 line, 3 key, 4 password, 31 timed key */
    ULONG  wait_until;          /* 31: bbs_now() deadline */
    UWORD  flags;               /* EnterLine flags */
    int    maxlen;
    int    len;
    struct Task *door;
    UBYTE  srccs;
    LONG   unknown[8];
    int    nunknown;
    BOOL   acked;
};

static void to_isdate(ULONG t, struct IsDate *d)
{
    /* bbs time is seconds since 1-Jan-1978; CNet wants year-1900, month 1-12 */
    char buf[20];
    static const char *mon = "JanFebMarAprMayJunJulAugSepOctNovDec";
    int m;
    bbs_datestr(t, buf);                        /* "22-Sep-26" */
    for (m = 0; m < 12; m++) if (!strncmp(mon + m * 3, buf + 3, 3)) break;
    d->Year   = (UBYTE)(atoi(buf + 7) + 100);
    d->Month  = (UBYTE)(m + 1);
    d->Date   = (UBYTE)atoi(buf);
    d->Hour   = (UBYTE)((t % 86400) / 3600);
    d->Minute = (UBYTE)((t % 3600) / 60);
    d->Second = (UBYTE)(t % 60);
}

static void fill_user(struct UserData *u, const struct UserRec *r)
{
    memset(u, 0, sizeof(*u));
    u->IDNumber = r->id;
    str_copy(u->Handle, r->name, sizeof(u->Handle));
    str_copy(u->RealName, r->realname, sizeof(u->RealName));
    str_copy(u->CityState, r->location, sizeof(u->CityState));
    str_copy(u->PassWord, "*", sizeof(u->PassWord));
    to_isdate(r->firstcall, &u->FirstCall);
    to_isdate(r->lastcall, &u->LastCall);
    u->Access     = (BYTE)(r->level * 32 / 256);   /* CNet access groups 0..31 */
    u->Colors     = r->termtype == TT_ANSI;
    u->ANSI       = r->termtype != TT_ASCII;
    u->TermWidth  = r->cols ? r->cols : 80;
    u->TermLength = r->rows ? r->rows : 24;
    u->TotalCalls = r->calls;
    u->PubMessages = r->posts;
    u->UpBytes    = r->ulkb;
    u->UpFiles    = r->uploads;
    u->DownBytes  = r->dlkb;
    u->DownFiles  = r->downloads;
}

/* keep the fields doors poll in step with reality */
static void update_z(struct CNetState *s)
{
    struct PortData *z = s->z;
    LONG left = time_left_mins();
    to_isdate(bbs_now(), &z->Today);
    z->TimeLeft   = (short)(left < 0 ? 9990 : left * 10);
    z->TimeOnLine = (short)(((bbs_now() - N.logon) / 60) * 10);
    z->Carrier    = N.online ? 1 : 0;
    z->OnLine     = N.online ? 1 : 0;
    if (!N.online) { z->Dumped = 1; z->getout = 1; z->TimeLeft = 0; }
}

static BOOL setup(struct CNetState *s, UBYTE ack)
{
    struct MsgPort *mp;
    BYTE sb;
    if (!(s->cp = AllocVec(sizeof(struct CPort), MEMF_PUBLIC | MEMF_CLEAR))) return FALSE;
    if (!(s->z = AllocVec(sizeof(struct PortData), MEMF_PUBLIC | MEMF_CLEAR))) return FALSE;
    if (!(s->myp = AllocVec(sizeof(struct MainPort), MEMF_PUBLIC | MEMF_CLEAR))) return FALSE;
    if ((sb = AllocSignal(-1)) < 0) return FALSE;

    sprintf(s->portname, "crun-%d.0", N.node - 1);
    mp = &s->cp->mport;
    mp->mp_Node.ln_Type = NT_MSGPORT;
    mp->mp_Node.ln_Name = s->portname;
    mp->mp_Flags = PA_SIGNAL;
    mp->mp_SigBit = sb;
    mp->mp_SigTask = FindTask(NULL);
    mp->mp_MsgList.lh_Head = (struct Node *)&mp->mp_MsgList.lh_Tail;
    mp->mp_MsgList.lh_Tail = NULL;
    mp->mp_MsgList.lh_TailPred = (struct Node *)&mp->mp_MsgList.lh_Head;
    mp->mp_MsgList.lh_Type = NT_MESSAGE;
    s->cp->myp = s->myp;
    s->cp->zp  = s->z;
    s->cp->ack = ack;                   /* 40 = CNet 4, 30 = CNet 3; door writes 0 */

    /* the caller as a CNet door sees them */
    fill_user(&s->z->user1, &N.user);
    s->z->id       = (short)N.user.id;
    s->z->InPort   = (short)(N.node - 1);
    s->z->CurrentCPS = 11520;
    s->z->OnType   = 1;
    s->z->ANSIon   = N.term != TT_ASCII;
    s->z->ThisTask = FindTask(NULL);
    s->z->cnp      = s->myp;
    s->z->MyMail   = 0;
    s->z->WWidth   = N.cols;
    s->z->WLength  = N.rows;
    s->z->TermLength = N.rows;
    s->z->loaded   = 1;
    str_copy(s->z->Doing, "In a door", sizeof(s->z->Doing));
    update_z(s);
    AddPort(mp);
    return TRUE;
}

static void cleanup(struct CNetState *s, BOOL leak)
{
    if (s->cp) {
        Forbid();
        if (s->cp->mport.mp_Node.ln_Name) RemPort(&s->cp->mport);
        {
            struct Message *m;
            while ((m = GetMsg(&s->cp->mport))) ReplyMsg(m);
        }
        Permit();
        FreeSignal(s->cp->mport.mp_SigBit);
    }
    if (leak) return;       /* a door that won't die may still read z: keep it */
    if (s->myp) FreeVec(s->myp);
    if (s->z) FreeVec(s->z);
    if (s->cp) FreeVec(s->cp);
}

/* ---- input ------------------------------------------------------------------------- */

static LONG key_in(struct CNetState *s)
{
    LONG c = in_get();
    if (c == 0x7F) c = 8;
    if (c >= 0x80 && N.charset == CS_UTF8) {
        UWORD u; int more;
        if ((c & 0xE0) == 0xC0) { u = c & 0x1F; more = 1; }
        else if ((c & 0xF0) == 0xE0) { u = c & 0x0F; more = 2; }
        else return '?';
        while (more-- > 0) { LONG d = in_get(); if (d < 0) break; u = (UWORD)((u << 6) | (d & 0x3F)); }
        c = uni_to_cp437(u);
        if (s->srccs == CS_LATIN1) c = cp437_to_uni((UBYTE)c) < 256 ? cp437_to_uni((UBYTE)c) : '?';
    }
    return c;
}

static void finish(struct CNetState *s, ULONG result)
{
    s->waiting->result = result;
    ReplyMsg((struct Message *)s->waiting);
    s->waiting = NULL;
}

static void service_input(struct CNetState *s)
{
    LONG c;
    struct PortData *z = s->z;
    if (!s->waiting) return;

    if (s->wait_kind == 3 || s->wait_kind == 31) {
        if ((c = key_in(s)) >= 0) finish(s, (ULONG)(c & 0xFF));
        else if (s->wait_kind == 31 && bbs_now() >= s->wait_until) finish(s, 0);
        return;
    }
    while ((c = key_in(s)) >= 0) {
        if (c == '\r' || c == '\n') {
            z->InBuffer[s->len] = 0;
            tnl();
            finish(s, (ULONG)s->len);
            return;
        }
        if (c == 8) {
            if (s->len > 0) { s->len--; tn_raw((const UBYTE *)"\b \b", 3); }
            continue;
        }
        if (c == 24 || c == 21) {
            while (s->len > 0) { s->len--; tn_raw((const UBYTE *)"\b \b", 3); }
            continue;
        }
        if (c < 32 || s->len >= s->maxlen || s->len >= 255) continue;
        if ((s->flags & 64) && (c < '0' || c > '9')) continue;              /* numbers only */
        if ((s->flags & 16384) && c == ' ') continue;                       /* no spaces */
        if ((s->flags & 2) && strchr("=:;\"/*", (int)c)) continue;          /* filename */
        if ((s->flags & 512) && strchr("^_`{|}~@", (int)c)) continue;       /* handle */
        if ((s->flags & 8) && c == ' ' && s->len == 0) continue;            /* chop leading */
        if (s->flags & 1) { if (c >= 'a' && c <= 'z') c -= 32; }
        else if (s->flags & 16) {
            BOOL start = s->len == 0 || z->InBuffer[s->len - 1] == ' ';
            if (start && c >= 'a' && c <= 'z') c -= 32;
            else if (!start && (s->flags & 32) && c >= 'A' && c <= 'Z') c += 32;
        }
        z->InBuffer[s->len++] = (char)c;
        if (s->wait_kind == 4) tn_raw((const UBYTE *)"*", 1);
        else { UBYTE b = (UBYTE)c; tputraw(&b, 1, s->srccs); }
    }
}

/* ---- CallHost ------------------------------------------------------------------------ */

static void make_date(struct IsDate *d, char *out)
{
    static const char *mon[] = { "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" };
    int m = d->Month >= 1 && d->Month <= 12 ? d->Month - 1 : 0;
    sprintf(out, "%02d-%s-%02d %02d:%02d", d->Date, mon[m], d->Year % 100, d->Hour, d->Minute);
}

static void call_host(struct CNetState *s, struct CMessage *m, const char *tag)
{
    struct PortData *z = s->z;
    BOOL gone = !N.online;
    ULONG r = 0;

    if (m->cn_Message.mn_ReplyPort && m->cn_Message.mn_ReplyPort->mp_SigTask)
        s->door = (struct Task *)m->cn_Message.mn_ReplyPort->mp_SigTask;
    if (!s->acked) {
        s->acked = TRUE;
        if (s->cp->ack == 1)
            bbs_log(BBS_SYSLOG, "node %d: CNet door %s says it can't handle this CNet version "
                    "(try cnet_version = 3 or 4)", N.node, tag);
    }
    update_z(s);

    switch (m->command) {
    case 0:                                     /* ShutDown */
        if (z->CSpawn[0]) bbs_log(BBS_SYSLOG, "node %d: CNet door %s asked to spawn %s (ignored)",
                                  N.node, tag, z->CSpawn);
        break;
    case 1:                                     /* PutText */
        if (!gone && m->arg1) cnet_mci_write((const UBYTE *)m->arg1, strlen((char *)m->arg1), s->srccs);
        break;
    case 2:                                     /* EnterLine(len, flags, prompt) */
    case 4:                                     /* EnterPassword(len) */
        if (gone) { z->InBuffer[0] = 0; break; }
        s->wait_kind = m->command;
        s->maxlen = m->arg1 ? (int)(m->arg1 & 0xFF) : 40;
        s->flags = m->command == 2 ? (UWORD)m->arg2 : 0;
        s->len = 0;
        if (m->command == 2 && (s->flags & 0x0004) == 0) z->InBuffer[0] = 0;   /* not USEINBUFF */
        if (m->command == 2 && m->arg3)
            cnet_mci_write((const UBYTE *)m->arg3, strlen((char *)m->arg3), s->srccs);
        if (m->command == 2 && (s->flags & 0x0004)) {                   /* edit existing text */
            s->len = strlen(z->InBuffer);
            if (s->len > s->maxlen) s->len = s->maxlen;
            tputraw((UBYTE *)z->InBuffer, s->len, s->srccs);
        }
        s->waiting = m;
        service_input(s);
        return;
    case 3:                                     /* OneKey */
    case 31:                                    /* WaitForInput(microseconds) */
        if (gone) break;
        s->wait_kind = m->command;
        s->wait_until = bbs_now() + (m->command == 31 ? (m->arg1 + 999999) / 1000000 : 0);
        s->waiting = m;
        service_input(s);
        return;
    case 5:                                     /* CommonCommands */
        r = 0;
        break;
    case 6:                                     /* ReadFile(path, flags) */
    case 9:                                     /* ReadGraphics(path, flags) */
        if (m->arg1 && !gone) {
            r = tshowpath((const char *)m->arg1, s->srccs);
            if (!r && (m->arg2 & 1)) tputs(L("cnetc.call_host.file_not_found", "\n|12File not found.|07\n"));
        }
        break;
    case 7:                                     /* SetDoing */
        if (m->arg1) {
            char act[LONGNAME];
            str_copy(act, (const char *)m->arg1, sizeof(act));
            set_activity(act);
            str_copy(z->Doing, act, sizeof(z->Doing));
        }
        break;
    case 10:                                    /* MakeDate(date, out) */
        if (m->arg1 && m->arg2) make_date((struct IsDate *)m->arg1, (char *)m->arg2);
        break;
    case 11: {                                  /* ReadAccount(id, user) */
        struct UserRec *u = AllocVec(sizeof(struct UserRec), 0);
        if (u && m->arg2) {
            ObtainSemaphore(&N.S->userlock);
            if (userdb_read((ULONG)(m->arg1 & 0xFFFF), u) && !(u->flags & UF_DELETED)) {
                fill_user((struct UserData *)m->arg2, u);
                r = 1;
            }
            ReleaseSemaphore(&N.S->userlock);
        }
        if (u) FreeVec(u);
        break;
    }
    case 12:                                    /* SaveAccount: doors don't get to */
        r = 0;
        break;
    case 20: {                                  /* FindAccount(name, user[, flags]) */
        struct UserRec *u = AllocVec(sizeof(struct UserRec), 0);
        if (u && m->arg1) {
            LONG id;
            ObtainSemaphore(&N.S->userlock);
            id = userdb_find((const char *)m->arg1, u);
            ReleaseSemaphore(&N.S->userlock);
            if (id) { if (m->arg2) fill_user((struct UserData *)m->arg2, u); r = id; }
        }
        if (u) FreeVec(u);
        break;
    }
    case 13: case 14: case 16: case 17: case 21:
        r = (m->command == 14) ? 1 : 0;         /* CheckBalance: "ok" */
        break;
    default: {
        int i;
        BOOL seen = FALSE;
        for (i = 0; i < s->nunknown; i++) if (s->unknown[i] == m->command) seen = TRUE;
        if (!seen && s->nunknown < 8) {
            s->unknown[s->nunknown++] = m->command;
            bbs_log(BBS_SYSLOG, "node %d: CNet CallHost %d not emulated (door %s)",
                    N.node, (int)m->command, tag);
        }
        break;
    }
    }
    m->result = r;
    ReplyMsg((struct Message *)m);
}

void run_cnetc(const char *tag, const char *cmd, const char *dir, LONG stack,
               UBYTE ack, UBYTE srccs, LONG grace)
{
    struct CNetState s;
    char line[PATHLEN * 2 + 32];
    ULONG donesig, sigs, hung_at = 0, broke_at = 0;
    BOOL leak = FALSE;

    memset(&s, 0, sizeof(s));
    s.srccs = srccs;
    if (!N.S || !setup(&s, ack)) {
        tputs(L("cnetc.run_cnetc.out_of_memory", "|12Out of memory for the CNet door.|07\n"));
        cleanup(&s, FALSE);
        return;
    }
    sprintf(line, "%s %s", cmd, s.portname);
    if (!(donesig = door_launch_sync(line, dir, stack))) {
        cleanup(&s, FALSE);
        tputs(L("cnetc.run_cnetc.the_door_could", "|12The door could not be started.|07\n"));
        return;
    }
    sigs = donesig | (1UL << s.cp->mport.mp_SigBit);
    N.wait_new = 1;

    for (;;) {
        struct CMessage *m;
        if (N.online) {
            ULONG got;
            tn_wait(s.waiting && s.wait_kind == 31 ? 200 : 1000, sigs, &got);
            if (N.msg_waiting) N.msg_waiting = FALSE;
            if (N.loggedin && time_left_mins() == 0) {
                tputs(L("cnetc.run_cnetc.your_time_is", "\n\n|12*** Your time is up ***|07\n"));
                tn_flush();
                node_hangup("time limit in door");
            }
        } else Delay(5);

        while ((m = (struct CMessage *)GetMsg(&s.cp->mport))) {
            if (s.waiting) {                    /* a door only has one call out at a time */
                m->result = 0;
                ReplyMsg((struct Message *)m);
            } else call_host(&s, m, tag);
        }
        update_z(&s);

        if (!N.online && !hung_at) {
            hung_at = bbs_now();
            if (s.waiting) { s.z->InBuffer[0] = 0; finish(&s, 0); }
            bbs_log(BBS_SYSLOG, "node %d: caller dropped in CNet door %s", N.node, tag);
        }
        if (hung_at) {
            if (node_reset_wanted()) node_release_slot();   /* sysop RESET: free the node now */
            /* Carrier=0/Dumped=1 is how CNet tells a door to leave; break it
             * once after the grace period if it ignores that */
            if (bbs_now() - hung_at >= (ULONG)grace && s.door &&
                (!broke_at || bbs_now() - broke_at >= 30)) {
                Signal(s.door, SIGBREAKF_CTRL_C);
                broke_at = bbs_now();
            }
        } else service_input(&s);

        if (door_launch_done()) break;
    }
    door_launch_finish();
    N.wait_new = 0;
    /* the door process is gone: nothing can touch z any more */
    cleanup(&s, leak);
    tcolor(7);
}
