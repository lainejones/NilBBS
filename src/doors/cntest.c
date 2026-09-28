/*
 * CNTest - a CNet 4 C PFile door, written the way the CNet SDK glue does it,
 * used to test NilBBS's cnetc door type.
 *
 *   CNTest <portname>          (the BBS supplies the port name)
 */
#include <exec/types.h>
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

#define __saveds
#define __d0
#define __d1
#define __a0
#define __a1
#define __a2
#define __a3
#define __asm
#include <cnet.h>
#undef __asm
#undef __d0
#undef __d1
#undef __a0
#undef __a1
#undef __a2
#undef __a3

static const char __attribute__((used)) verstag[] = "$VER: CNTest 1.0 (22.9.2026)";

/* ---- the CNet SDK glue (same as Trolan's CNet3Lib) ---- */
struct MsgPort  *replyp;
struct MainPort *myp;
struct PortData *z;
struct CMessage  cmess;
struct CPort    *cport;

static void CallHost(UBYTE c)
{
    cmess.command = c;
    PutMsg((struct MsgPort *)cport, (struct Message *)&cmess);
    WaitPort(replyp);
    GetMsg(replyp);
}
static void PutText(char *t) { cmess.arg1 = (ULONG)t; CallHost(1); }
static int EnterLine(UBYTE len, USHORT flags, char *prompt)
{
    cmess.arg1 = len; cmess.arg2 = flags; cmess.arg3 = (ULONG)prompt;
    CallHost(2);
    return strlen(z->InBuffer);
}
static char OneKey(void) { CallHost(3); return (char)cmess.result; }
static void SetDoing(char *w) { cmess.arg1 = (ULONG)w; CallHost(7); }
static void ShutDown(char *spawn) { if (spawn) strcpy(z->CSpawn, spawn); CallHost(0); }
static void GetOut(void)
{
    ShutDown(NULL);
    DeleteMsgPort(replyp);
    exit(0);
}
static void checkcarrier(void)
{
    if (!z->Carrier || !z->TimeLeft || z->Dumped) GetOut();
}

int main(int argc, char **argv)
{
    char buf[200];
    if (argc < 2) return 20;
    Forbid();
    cport = (struct CPort *)FindPort((STRPTR)argv[1]);
    Permit();
    if (!cport) return 20;
    if (cport->ack != 40 && cport->ack != 30) { cport->ack = 1; return 20; }
    cport->ack = 0;                                     /* "we've arrived" */
    z = cport->zp;
    myp = cport->myp;
    if (!(replyp = CreateMsgPort())) return 20;
    cmess.cn_Message.mn_ReplyPort = replyp;
    cmess.cn_Message.mn_Length = sizeof(cmess);

    SetDoing("Playing CNTest");
    /* ^Q-style and ^Y-style MCI colours, like real CNet doors use */
    sprintf(buf, "\x11" "F1}\x11" "cb}CNET TEST DOOR\x11" "c7}\n"
                 "Hello \x19" "c3%s\x19" "c7 (%s), port %d, %d.%d min left.\n",
            z->user1.Handle, z->user1.CityState, (int)z->InPort,
            (int)(z->TimeLeft / 10), (int)(z->TimeLeft % 10));
    PutText(buf);

    for (;;) {
        char k;
        checkcarrier();
        PutText("\n\x11" "ce}[L]ine input  [K]ey test  [Q]uit\x11" "c7} > ");
        k = OneKey();
        checkcarrier();
        if (k == 'q' || k == 'Q') break;
        if (k == 'l' || k == 'L') {
            EnterLine(30, 16, "\nName a hero: ");
            checkcarrier();
            sprintf(buf, "InBuffer = [%s]\n", z->InBuffer);
            PutText(buf);
        } else {
            sprintf(buf, "\nYou pressed '%c'\n", k);
            PutText(buf);
        }
    }
    PutText("\nBye from CNTest.\n");
    GetOut();
    return 0;
}
