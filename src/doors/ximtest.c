/*
 * XIMTest - an AmiExpress-style XIM door used to test NilBBS's XIM server.
 *
 *   XIMTest <node>
 *
 * Talks to the BBS only through the real AEDoor.library (called here through
 * its jump table), exactly like a third-party /X door: user data with GetDT,
 * output with WriteStr, a hotkey with HotKey and a line with Prompt.  Stops
 * when the caller presses Q or the BBS reports loss of carrier (-1 / NULL).
 */
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <inline/macros.h>
#include <string.h>
#include <stdio.h>

static const char __attribute__((used)) verstag[] = "$VER: XIMTest 1.0 (22.9.2026)";

struct Library *AEDBase;

#define CreateComm(n)       LP1(0x1e, APTR, CreateComm, ULONG, n, d0, , AEDBase)
#define DeleteComm(d)       LP1NR(0x24, DeleteComm, APTR, d, a1, , AEDBase)
#define GetString(d)        LP1(0x48, char *, GetString, APTR, d, a1, , AEDBase)
#define WriteStr(d, s, f)   LP3NR(0x54, WriteStr, APTR, d, a1, char *, s, a0, ULONG, f, d1, , AEDBase)
#define Prompt(d, l, s)     LP3(0x4e, char *, Prompt, APTR, d, a1, ULONG, l, d1, char *, s, a0, , AEDBase)
#define GetDT(d, c, s)      LP3NR(0x6c, GetDT, APTR, d, a1, ULONG, c, d0, char *, s, a0, , AEDBase)
#define Hotkey(d, s)        LP2(0x7e, LONG, Hotkey, APTR, d, a1, char *, s, a0, , AEDBase)

#define DT_NAME      100
#define DT_LOCATION  102
#define DT_TIMETOTAL 116
#define DT_LINELENGTH 122
#define JH_BBSNAME   11

int main(int argc, char **argv)
{
    APTR d;
    char *str, name[40], bbs[48], line[120];
    LONG loc_ok, k;

    if (!(AEDBase = OpenLibrary((STRPTR)"AEDoor.library", 0))) return 20;
    if (!(d = CreateComm(argc > 1 ? (ULONG)argv[1][0] : '0'))) {
        CloseLibrary(AEDBase);
        return 20;
    }
    str = GetString(d);

    GetDT(d, DT_NAME, NULL);       strncpy(name, str, 39); name[39] = 0;
    GetDT(d, JH_BBSNAME, NULL);    strncpy(bbs, str, 47); bbs[47] = 0;
    WriteStr(d, "\x1b[1;33mXIM TEST DOOR\x1b[0m (via AEDoor.library)", 1);
    sprintf(line, "Hello %s, welcome to %s.", name, bbs);
    WriteStr(d, line, 1);
    GetDT(d, DT_LOCATION, NULL);
    sprintf(line, "Location: %s", str);
    WriteStr(d, line, 1);
    GetDT(d, DT_TIMETOTAL, NULL);
    sprintf(line, "Time allowed today: %s seconds", str);
    WriteStr(d, line, 1);
    loc_ok = 1;

    for (;;) {
        char *in;
        k = Hotkey(d, "\nPress a key (Q quits): ");
        if (k < 0) break;                                   /* carrier lost */
        sprintf(line, "\nYou pressed '%c' (%ld)", (int)k, k);
        WriteStr(d, line, 1);
        if (k == 'q' || k == 'Q') break;
        in = Prompt(d, 30, "Say something: ");
        if (!in) break;                                     /* carrier lost */
        sprintf(line, "You said: %s", in);
        WriteStr(d, line, 1);
    }
    if (k >= 0) WriteStr(d, "Bye from XIMTest!", 1);
    DeleteComm(d);
    CloseLibrary(AEDBase);
    return loc_ok ? 0 : 5;
}
