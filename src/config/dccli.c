/*
 * DoorCheck - check every door in Doors.cfg (or one) from the Shell: what each door's program
 * looks like (Detect) against its settings, and what would stop it working here (Check).
 *
 *   DoorCheck [FILE=BBS:Config/Doors.cfg] [TAG]
 */
#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>

#include "bbs.h"
#include "ini.h"
#include "doorcheck.h"

static const char __attribute__((used)) verstag[] = "$VER: DoorCheck " BBS_VERSION " (" BBS_VERDATE ")";

static char names[256][32];

static int real_main(void)
{
    LONG args[2] = { 0, 0 };
    struct RDArgs *rd = ReadArgs((STRPTR)"FILE,TAG", args, NULL);
    const char *file = args[0] ? (const char *)args[0] : "BBS:Config/Doors.cfg";
    struct Ini *ini;
    int n, i, problems = 0, doors = 0;

    if (!rd) { PrintFault(IoErr(), (STRPTR)"DoorCheck"); return RETURN_FAIL; }
    if (args[0] && !args[1] && !strchr(file, ':') && !strchr(file, '/') && !strchr(file, '.')) {
        args[1] = args[0];                          /* DoorCheck EXCAL: a tag, not a file */
        file = "BBS:Config/Doors.cfg";
    }
    if (!(ini = ini_load(file)) || !ini->n) { Printf((STRPTR)"Can't read %s\n", (LONG)file); FreeArgs(rd); return RETURN_FAIL; }
    n = ini_sections(ini, names, 256);
    for (i = 0; i < n; i++) {
        char type[24], cmd[256], dir[200], assign[120], host[80], old[8], rep[1200];
        struct DoorGuess g;
        int k;
        if (args[1] && str_icmp(names[i], (const char *)args[1])) continue;
        if (!ini_get(ini, names[i], "type", type, sizeof(type))) strcpy(type, "cli");
        if (!ini_get(ini, names[i], "command", cmd, sizeof(cmd))) cmd[0] = 0;
        if (!ini_get(ini, names[i], "dir", dir, sizeof(dir))) dir[0] = 0;
        if (!ini_get(ini, names[i], "assign", assign, sizeof(assign))) assign[0] = 0;
        if (!ini_get(ini, names[i], "host", host, sizeof(host))) host[0] = 0;
        if (!ini_get(ini, names[i], "old_mci", old, sizeof(old))) strcpy(old, "no");
        doors++;
        Printf((STRPTR)"[%s] type %s\n", (LONG)names[i], (LONG)type);
        if (dc_detect(cmd, dir, assign, &g)) {
            Printf((STRPTR)"  program  %s\n  detect   %s - %s\n", (LONG)g.program, (LONG)g.type, (LONG)g.why);
            if (g.oldmci) Printf((STRPTR)"  detect   old CNet codes (old_mci = yes; set: %s)\n", (LONG)old);
            if (g.cnetver) Printf((STRPTR)"  detect   cnet_version %ld\n", (LONG)g.cnetver);
            if (g.assign[0]) Printf((STRPTR)"  detect   assign %s   (set: %s)\n", (LONG)g.assign, (LONG)(assign[0] ? assign : "-"));
            if (g.dropfile[0]) Printf((STRPTR)"  detect   drop file %s\n", (LONG)g.dropfile);
            for (k = 0; k < g.nlibs; k++) Printf((STRPTR)"  detect   opens %s\n", (LONG)g.libs[k]);
        }
        k = dc_check(type, cmd, dir, assign, host, rep, sizeof(rep));
        problems += k;
        if (rep[0]) {
            char *p = rep, *e;
            while (*p && (e = strchr(p, '\n'))) {
                *e = 0;
                Printf((STRPTR)(strncmp(p, "Note:", 5) ? "  PROBLEM  %s\n" : "  %s\n"), (LONG)p);
                p = e + 1;
            }
        }
        if (!k) Printf((STRPTR)"  OK\n");
    }
    Printf((STRPTR)"%ld door%s, %ld problem%s\n", (LONG)doors, (LONG)(doors == 1 ? "" : "s"),
           (LONG)problems, (LONG)(problems == 1 ? "" : "s"));
    ini_free(ini);
    FreeArgs(rd);
    return problems ? RETURN_WARN : RETURN_OK;
}

int main(void) { return run_with_stack(32768, real_main); }
