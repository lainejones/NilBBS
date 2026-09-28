/*
 * serial.c - a caller on a serial port instead of a socket (BBSNode SERIAL).
 *
 * NilBBS.cfg:
 *   serial_device  = serial.device   ; "" = no serial line (the default)
 *   serial_unit    = 0
 *   serial_baud    = 19200           ; the Amiga <-> modem speed
 *   serial_rtscts  = yes             ; hardware (7-wire) handshake
 *   modem_init     = ATZ             ; "" = a direct line: null-modem cable, or a WiFi
 *                                    ;      modem left in transparent mode
 *   modem_answer   = ATA             ; "" = the modem answers by itself (ATS0=1)
 *   serial_carrier = auto            ; watch the CD line: auto = yes with a modem, no direct
 *   serial_node    = 0               ; the node calls land on (0 = any free one)
 *
 * With a modem we wait for RING, answer, and take the CONNECT line's speed; with
 * a direct line the first byte a caller sends is the call.  The session then runs
 * through the same tn_* calls as telnet (telnet.c routes them here while
 * N.serial is set), without the telnet parts: no IAC, no option talk.  A call
 * ends when the node ends it, CD drops (if watched), or the modem says NO CARRIER.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <devices/serial.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "node.h"

static struct MsgPort  *g_port;
static struct IOExtSer *g_io;
static BOOL  g_open, g_modem, g_watch_cd;
static char  g_nocar[16];               /* the last bytes seen, for "NO CARRIER" */

static void ser_put(const UBYTE *p, LONG n)
{
    if (!g_open || n <= 0) return;
    g_io->IOSer.io_Command = CMD_WRITE;
    g_io->IOSer.io_Data = (APTR)p;
    g_io->IOSer.io_Length = n;
    DoIO((struct IORequest *)g_io);
}

static void ser_puts(const char *s) { ser_put((const UBYTE *)s, strlen(s)); }

/* bytes waiting in serial.device's buffer, and the CD line (TRUE = carrier) */
static LONG ser_query(BOOL *cd)
{
    g_io->IOSer.io_Command = SDCMD_QUERY;
    DoIO((struct IORequest *)g_io);
    if (cd) *cd = !(g_io->io_Status & (1 << 5));        /* CD is active low */
    return (LONG)g_io->IOSer.io_Actual;
}

/* whatever is waiting now, up to max bytes; never blocks */
static LONG ser_get(UBYTE *buf, LONG max)
{
    LONG n = ser_query(NULL);
    if (n <= 0) return 0;
    if (n > max) n = max;
    g_io->IOSer.io_Command = CMD_READ;
    g_io->IOSer.io_Data = buf;
    g_io->IOSer.io_Length = n;
    if (DoIO((struct IORequest *)g_io)) return 0;
    return (LONG)g_io->IOSer.io_Actual;
}

BOOL ser_open(void)
{
    const char *dev = cfg_str(N.cfg, "serial_device", "");
    LONG unit = cfg_int(N.cfg, "serial_unit", 0);
    if (!dev[0]) return FALSE;
    if (!(g_port = CreateMsgPort())) return FALSE;
    if (!(g_io = (struct IOExtSer *)CreateIORequest(g_port, sizeof(struct IOExtSer)))) return FALSE;
    g_io->io_SerFlags = cfg_bool(N.cfg, "serial_rtscts", TRUE) ? SERF_7WIRE : 0;
    if (OpenDevice((STRPTR)dev, unit, (struct IORequest *)g_io, 0)) {
        bbs_log(BBS_SYSLOG, "serial: can't open %s unit %ld", dev, unit);
        return FALSE;
    }
    g_open = TRUE;
    g_io->io_Baud = cfg_int(N.cfg, "serial_baud", 19200);
    g_io->io_RBufLen = 8192;
    g_io->io_ReadLen = g_io->io_WriteLen = 8;
    g_io->io_StopBits = 1;
    g_io->io_SerFlags = (cfg_bool(N.cfg, "serial_rtscts", TRUE) ? SERF_7WIRE : 0) | SERF_XDISABLED;
    g_io->IOSer.io_Command = SDCMD_SETPARAMS;
    if (DoIO((struct IORequest *)g_io))
        bbs_log(BBS_SYSLOG, "serial: %s refused %ld baud", dev, (LONG)g_io->io_Baud);
    g_modem = cfg_str(N.cfg, "modem_init", "")[0] != 0;
    {
        const char *cd = cfg_str(N.cfg, "serial_carrier", "auto");
        g_watch_cd = !strcmp(cd, "yes") || (!strcmp(cd, "auto") && g_modem);
    }
    return TRUE;
}

/* hang up (a modem: +++ ATH, then DTR drops as the device closes) and let go */
void ser_close(void)
{
    if (g_open) {
        if (g_modem) {
            Delay(60); ser_puts("+++"); Delay(60); ser_puts("ATH0\r"); Delay(25);
        }
        CloseDevice((struct IORequest *)g_io);
        g_open = FALSE;
    }
    if (g_io) { DeleteIORequest((struct IORequest *)g_io); g_io = NULL; }
    if (g_port) { DeleteMsgPort(g_port); g_port = NULL; }
}

/* send a modem command and wait (up to secs) for a line containing `want` */
static BOOL modem_cmd(const char *cmd, const char *want, LONG secs, char *line, int max)
{
    UBYTE b[64];
    char acc[80];
    int an = 0;
    LONG ticks = secs * 50;
    ser_puts(cmd);
    ser_puts("\r");
    while (ticks > 0) {
        LONG n = ser_get(b, sizeof(b)), i;
        if (SetSignal(0, 0) & SIGBREAKF_CTRL_C) return FALSE;
        for (i = 0; i < n; i++) {
            if (b[i] == '\r' || b[i] == '\n') {
                acc[an] = 0;
                if (an && strstr(acc, want)) { if (line) str_copy(line, acc, max); return TRUE; }
                an = 0;
            } else if (an < (int)sizeof(acc) - 1) acc[an++] = b[i];
        }
        if (!n) { Delay(2); ticks -= 2; }
    }
    return FALSE;
}

/*
 * Wait for a call.  TRUE = someone is on the line (how: "CONNECT 19200" or "direct");
 * FALSE = CTRL-C (the BBS is shutting down) or the port failed.
 */
BOOL ser_wait_call(char *how, int max)
{
    UBYTE b[64];
    char acc[80];
    int an = 0;
    const char *answer = cfg_str(N.cfg, "modem_answer", "ATA");
    if (g_modem) {
        if (!modem_cmd(cfg_str(N.cfg, "modem_init", "ATZ"), "OK", 5, NULL, 0))
            bbs_log(BBS_SYSLOG, "serial: the modem didn't answer its init string with OK");
    }
    for (;;) {
        LONG n, i;
        if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) return FALSE;
        n = ser_get(b, sizeof(b));
        if (!n) { Delay(5); continue; }
        if (!g_modem) {                         /* direct: the caller's first key is the call */
            str_copy(how, "direct", max);
            return TRUE;
        }
        for (i = 0; i < n; i++) {
            if (b[i] != '\r' && b[i] != '\n') { if (an < (int)sizeof(acc) - 1) acc[an++] = b[i]; continue; }
            acc[an] = 0;
            an = 0;
            if (!strncmp(acc, "RING", 4) && answer[0]) {
                if (modem_cmd(answer, "CONNECT", 60, how, max)) return TRUE;
                bbs_log(BBS_SYSLOG, "serial: answered, but no CONNECT");
            } else if (!strncmp(acc, "CONNECT", 7)) {       /* the modem answered by itself */
                str_copy(how, acc, max);
                return TRUE;
            }
        }
    }
}

/* ---- the session side: what telnet.c calls while N.serial is set ----------------- */

void ser_write(const UBYTE *p, LONG n) { ser_put(p, n); }

static void nocarrier_feed(UBYTE c)
{
    int l = strlen(g_nocar);
    if (l >= (int)sizeof(g_nocar) - 1) { memmove(g_nocar, g_nocar + 1, l); l--; }
    g_nocar[l] = (char)c; g_nocar[l + 1] = 0;
}

/* like tn_wait for a socket: read what arrives into the input ring (via put), watch
 * the line, answer the node's signals */
LONG ser_wait(ULONG ms, ULONG extrasigs, ULONG *gotsigs, void (*put)(UBYTE))
{
    UBYTE b[256];
    for (;;) {
        ULONG mask = SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_D | extrasigs;
        ULONG sigs = SetSignal(0, mask) & mask;
        BOOL cd = TRUE;
        LONG n, i;
        node_heartbeat();
        spy_poll();
        if (sigs & SIGBREAKF_CTRL_C) { N.kicked = TRUE; node_hangup("disconnected by sysop"); return 0; }
        if (sigs & SIGBREAKF_CTRL_D) { N.msg_waiting = TRUE; return in_avail(); }
        if (sigs & extrasigs) { if (gotsigs) *gotsigs = sigs & extrasigs; return in_avail(); }
        ser_query(&cd);
        if (g_watch_cd && !cd) { node_hangup("carrier lost"); return 0; }
        if ((n = ser_get(b, sizeof(b))) > 0) {
            for (i = 0; i < n; i++) {
                put(b[i]);
                if (g_modem && !N.binary_raw) {
                    nocarrier_feed(b[i]);
                    if (strstr(g_nocar, "NO CARRIER")) { node_hangup("carrier lost (NO CARRIER)"); return 0; }
                }
            }
            return in_avail();
        }
        if (!N.online) return 0;
        if (ms < 40) return in_avail();
        Delay(2);                               /* 40 ms: plenty for a serial line */
        ms -= 40;
    }
}
