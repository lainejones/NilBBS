/* doorcheck.h - look at a door's program and guess its Doors.cfg settings (Detect), and check
 * that a door's settings will work on this Amiga (Check).  Used by BBSConfig's Doors page. */
#ifndef DOORCHECK_H
#define DOORCHECK_H

#include <exec/types.h>

#define DC_MAXLIBS 8

struct DoorGuess {
    char  program[256];         /* where the command's program was found ("" = not found) */
    char  type[16];             /* cli cnetrexx aim cnetc xim ("" = can't tell) */
    char  why[80];              /* what gave the type away */
    BOOL  oldmci;               /* CNet 1.x/2.x backslash screen codes */
    int   cnetver;              /* 3 / 4, 0 = no idea */
    char  assign[120];          /* "NAME: path" the program expects, "" = none */
    char  dropfile[16];         /* door.sys dorinfo1.def door32.sys, "" = no idea */
    int   nlibs;
    char  libs[DC_MAXLIBS][32]; /* non-system libraries it opens */
};

/* the command's program as a path this Amiga can open (the door's own assign and dir used) */
BOOL dc_resolve(const char *command, const char *dir, const char *assign, char *out, int size);

/* look at the program; FALSE if it can't be found or read */
BOOL dc_detect(const char *command, const char *dir, const char *assign, struct DoorGuess *g);

/* problems with these settings, one per line in report (fits size); returns how many */
int dc_check(const char *type, const char *command, const char *dir, const char *assign,
             const char *host, char *report, int size);

/* the type names BBSConfig shows, for the names NilBBS also accepts (arexx, amiexpress) */
const char *dc_type_norm(const char *type);

#endif
