/*
 * diz.c - the node's side of FILE_ID.DIZ: asking a caller (or the sysop) for a
 * new one.  Reading, caching and writing DIZs is in common/dizcore.c.
 */
#include <exec/types.h>
#include <string.h>
#include <stdio.h>

#include "node.h"

/* ask the caller for name/version/description and build the DIZ.
 * Returns FALSE if they gave up. */
BOOL diz_create_interactive(const char *filename, const char *areaname, char *out, LONG max)
{
    char prog[48], ver[16], desc[DIZ_H][DIZ_W + 1];
    int nd = 0;
    char guess[48], *p;

    /* guess the program name and version from "Name-1.2.lha" */
    str_copy(guess, filename, sizeof(guess));
    if ((p = strrchr(guess, '.'))) *p = 0;
    ver[0] = 0;
    if ((p = strrchr(guess, '-')) && p[1] >= '0' && p[1] <= '9') { str_copy(ver, p + 1, sizeof(ver)); *p = 0; }
    str_copy(prog, guess, sizeof(prog));

    tputs(L("diz.create_interactive.program_name", "\n|07Program name: |15"));
    if (tgetline(prog, 40, GL_EDIT) < 0) return FALSE;
    if (!prog[0]) return FALSE;
    tputs(L("diz.create_interactive.version_enter_for", "|07Version |08(Enter for none)|07: |15"));
    if (tgetline(ver, 12, GL_EDIT) < 0) return FALSE;
    tprintf(L("diz.create_interactive.describe_it_up", "|07Describe it |08(up to 6 lines of %d characters, blank line to finish)|07\n"), DIZ_W);
    while (nd < 6) {
        tprintf("|08%d>|15 ", nd + 1);
        if (tgetline(desc[nd], DIZ_W + 1, 0) < 0) return FALSE;
        if (!desc[nd][0]) break;
        nd++;
    }
    if (!nd) { str_copy(desc[0], "(no description)", sizeof(desc[0])); nd = 1; }
    diz_build(prog, ver, desc, nd, areaname, out, max);

    tputs(L("diz.create_interactive.here_is_the", "\n|07Here is the FILE_ID.DIZ:\n|08"));
    {
        int i;
        for (i = 0; i < DIZ_W; i++) tputs("-");
    }
    tputs("|07\n");
    tputraw((UBYTE *)out, strlen(out), CS_CP437);
    tputs("\n|08");
    {
        int i;
        for (i = 0; i < DIZ_W; i++) tputs("-");
    }
    tputs("|07\n");
    return tyesno(L("diz.create_interactive.add_this_to", "|07Add this to the archive?"), TRUE);
}
