/*
 * XIMProbe - checks NilBBS's /X door server message by message, without
 * AEDoor.library: it builds the JHMessages itself and PutMsg()s them to
 * AEDoorPort<node>, the way the library does underneath.  Each check prints
 * one line "CHECK <name>: <what came back>" for test/t48_ximprobe.py to judge.
 *
 *   XIMProbe <node>
 *
 * The caller (the test script) is asked for keys at a few points: an arrow
 * with RAWARROW on, an arrow then 'x' with it off, and nothing at all for the
 * timeout check.  Ends with RETURNCOMMAND "G" when the caller types G at the
 * last prompt (the BBS then logs them off), else just shuts down.
 */
#include <exec/types.h>
#include <exec/ports.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static const char __attribute__((used)) verstag[] = "$VER: XIMProbe 1.0 (29.9.2026)";

struct JHMessage {
    struct Message msg;
    char   String[200];
    LONG   Data;
    LONG   Command;
    LONG   NodeID;
    LONG   LineNum;
    ULONG  signal;
    struct Process *task;
    LONG   Semi;
    LONG   Filler1, Filler2;
    char  *strptr;
    LONG   filler3;
};

static struct MsgPort *bbs, *reply;
static struct JHMessage jm;

/* one round trip: command, data, string in; the reply is left in jm */
static void send(LONG cmd, LONG data, const char *s)
{
    jm.Command = cmd;
    jm.Data = data;
    jm.String[0] = 0;
    if (s) { strncpy(jm.String, s, 199); jm.String[199] = 0; }
    jm.msg.mn_Node.ln_Type = NT_MESSAGE;
    jm.msg.mn_ReplyPort = reply;
    jm.msg.mn_Length = sizeof(jm);
    PutMsg(bbs, (struct Message *)&jm);
    WaitPort(reply);
    GetMsg(reply);
}

static void say(const char *s)
{
    char b[210];
    strncpy(b, s, 199); b[199] = 0;
    send(4 /* JH_SM */, 1, b);
}

static void check(const char *name, const char *fmt, LONG a, LONG b, const char *str)
{
    char line[220], v[180];
    sprintf(v, fmt, a, b);
    sprintf(line, "CHECK %s: %s", name, v);
    say(line);
}

static void checks(const char *name, const char *str)
{
    char line[220];
    sprintf(line, "CHECK %s: %.180s", name, str);
    say(line);
}

int main(int argc, char **argv)
{
    char name[24], s[200];
    struct Task *bbstask;
    LONG sig, t0, k;

    sprintf(name, "AEDoorPort%s", argc > 1 ? argv[1] : "0");
    Forbid();
    bbs = FindPort((STRPTR)name);
    Permit();
    if (!bbs || !(reply = CreateMsgPort())) return 20;

    send(1 /* JH_REGISTER */, 0, NULL);
    check("register", "%ld", jm.Command, 0, NULL);                 /* the caller's lines */

    send(152 /* EXPRESS_VERSION */, 1, NULL);   checks("version", jm.String);
    send(517 /* BB_LOGONTYPE */, 1, NULL);      check("logontype", "%ld", jm.Data, 0, NULL);
    send(505 /* NODE_BAUD */, 1, NULL);         checks("baud", jm.String);
    send(1002 /* DT_QUERYBIT */, 3, NULL);      check("querybit_download", "%ld", jm.Command, 0, NULL);
    send(1002, 11, NULL);                       check("querybit_shell", "%ld", jm.Command, 0, NULL);
    send(532 /* LOAD_ACCOUNT */, 1, NULL);      check("load_account", "%ld", jm.Data, 0, NULL);
    jm.Semi = 12345;                            /* garbage, as a careless door leaves it */
    send(531 /* MULTICOM */, 0, NULL);          check("multicom", "%ld", jm.Semi, 0, NULL);
    send(614 /* CONF_ACCESS */, 0, NULL);       check("conf_access", "%ld/", jm.Data, 0, NULL);
    send(145 /* DT_CURR_TIME */, 1, NULL);      checks("curr_time", jm.String);
    send(144 /* DT_STAMP_CTIME */, 1, NULL);    checks("stamp_ctime", jm.String);

    /* ENVSTAT: read the default, write, read back; SV_NEWMSG sets WHO */
    send(163, 1, NULL);                         checks("envstat_default", jm.String);
    send(163, 0, "17");
    send(163, 1, NULL);                         checks("envstat_set", jm.String);
    send(177 /* SV_NEWMSG */, 0, "Probing the /X server");

    /* the time: add ten minutes through DT_TIMETOTAL */
    send(116 /* DT_TIMETOTAL */, 1, NULL);
    t0 = atol(jm.String);
    sprintf(s, "%ld", t0 + 600);
    send(116, 0, s);
    send(116, 1, NULL);
    check("timetotal_plus600", "%ld", atol(jm.String) - t0, 0, NULL);

    /* JH_SIGBIT + JH_ExtHK: our own signal wakes the wait with key 0 */
    send(16 /* JH_SIGBIT */, 1, NULL);
    sig = jm.Data;
    send(512 /* BB_GETTASK */, 1, NULL);
    bbstask = (struct Task *)jm.task;
    if (bbstask && sig > 0) Signal(bbstask, 1UL << sig);
    send(15 /* JH_ExtHK */, 0, NULL);
    check("exthk_signal", "%ld %ld", jm.Command, jm.Data, NULL);

    /* arrows with RAWARROW on: one key, 4 = up */
    send(501 /* RAWARROW */, 0, NULL);
    send(6 /* JH_HK */, 0, "PRESS-UP-ARROW: ");
    check("hk_rawarrow", "%ld", (UBYTE)jm.String[0], 0, NULL);
    send(501, 0, NULL);                                             /* toggled off again */
    send(6, 0, "PRESS-UP-ARROW-THEN-X: ");
    check("hk_arrow_swallowed", "%ld", (UBYTE)jm.String[0], 0, NULL);

    /* a 5 second timeout, and nobody types */
    send(125 /* DT_TIMEOUT */, 0, "5");
    send(125, 1, NULL);                         checks("timeout_set", jm.String);
    t0 = time(NULL);
    send(6, 0, "WAIT-FOR-TIMEOUT: ");
    k = jm.Data;
    check("hk_timeout", "%ld %ld", k, (LONG)(signed char)jm.String[0], NULL);
    check("hk_timeout_secs", "%ld", (LONG)(time(NULL) - t0), 0, NULL);
    send(125, 0, "300");

    /* the last key decides: G = RETURNCOMMAND G (log off), else just leave */
    send(6, 0, "LAST-KEY (G logs off): ");
    k = (UBYTE)jm.String[0];
    if (k == 'G' || k == 'g') send(136 /* RETURNCOMMAND */, 0, "G");
    say("PROBE DONE");
    send(2 /* JH_SHUTDOWN */, 0, NULL);
    DeleteMsgPort(reply);
    return 0;
}
