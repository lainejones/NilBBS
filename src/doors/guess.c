/*
 * Guess - a sample NilBBS door, and a template for writing your own.
 *
 *   Guess [DROP=<path to DOOR.SYS>]
 *
 * A door is an ordinary AmigaDOS CLI program: it prints to Output() and
 * reads Input(), and NilBBS wires both to the caller.  This one reads the
 * caller's name from the drop file, draws with ANSI colour, uses SetMode()
 * raw for single-key hotkeys and ordinary cooked input for typed numbers.
 */
#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char __attribute__((used)) verstag[] = "$VER: Guess 1.0 (22.9.2026) NilBBS sample door";

static BPTR in, out;
static BOOL ansi = TRUE;

static void say(const char *s) { Write(out, (APTR)s, strlen(s)); }

static void col(const char *code)
{
    if (ansi) { say("\x1b["); say(code); say("m"); }
}

/* one key, raw mode */
static int getkey(void)
{
    UBYTE c;
    SetMode(in, 1);
    if (Read(in, &c, 1) != 1) { SetMode(in, 0); return -1; }
    SetMode(in, 0);
    return c;
}

/* a line, cooked mode (the BBS does the editing and echo) */
static int getline_(char *buf, int max)
{
    LONG n = Read(in, buf, max - 1);
    if (n <= 0) return -1;
    buf[n] = 0;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    return (int)n;
}

/* DOOR.SYS line 36 is the user's alias, line 20 the graphics mode */
static void read_dropfile(const char *path, char *name, int max)
{
    BPTR fh = Open((STRPTR)path, MODE_OLDFILE);
    char line[80];
    int ln = 0;
    strcpy(name, "stranger");
    if (!fh) return;
    while (FGets(fh, (STRPTR)line, sizeof(line))) {
        char *e = line + strlen(line);
        while (e > line && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
        ln++;
        if (ln == 20) ansi = strcmp(line, "GR") == 0;
        if (ln == 36 && line[0]) { strncpy(name, line, max - 1); name[max - 1] = 0; }
    }
    Close(fh);
}

int main(void)
{
    struct RDArgs *rda;
    LONG args[1] = { 0 };
    char name[40], buf[20], msg[120];
    ULONG seed;
    struct DateStamp ds;

    in = Input();
    out = Output();
    rda = ReadArgs((STRPTR)"DROP", args, NULL);
    read_dropfile(args[0] ? (char *)args[0] : "DOOR.SYS", name, sizeof(name));
    DateStamp(&ds);
    seed = ds.ds_Tick * 7919 + ds.ds_Minute * 104729;

    for (;;) {
        int target, tries = 0, k;
        seed = seed * 1103515245UL + 12345UL;
        target = (int)((seed >> 16) % 100) + 1;

        if (ansi) say("\x1b[2J\x1b[H");
        col("1;33"); say("\r\n  G U E S S   T H E   N U M B E R\r\n");
        col("0;36"); sprintf(msg, "  Hello %s - I'm thinking of a number from 1 to 100.\r\n\r\n", name); say(msg);

        for (;;) {
            int g;
            col("0;37"); say("  Your guess: ");
            col("1;37");
            if (getline_(buf, sizeof(buf)) < 0) goto bye;     /* caller hung up */
            g = atoi(buf);
            if (g < 1 || g > 100) { col("0;31"); say("  1 to 100, please.\r\n"); continue; }
            tries++;
            if (g < target) { col("0;33"); say("  Higher!\r\n"); }
            else if (g > target) { col("0;33"); say("  Lower!\r\n"); }
            else {
                col("1;32");
                sprintf(msg, "  Got it in %d tr%s!\r\n", tries, tries == 1 ? "y" : "ies");
                say(msg);
                break;
            }
        }
        col("0;37"); say("\r\n  Play again? (Y/N) ");
        k = getkey();
        if (k < 0) goto bye;
        say("\r\n");
        if (k != 'y' && k != 'Y') break;
    }
bye:
    col("0");
    say("\r\n  Thanks for playing!\r\n");
    if (rda) FreeArgs(rda);
    return 0;
}
