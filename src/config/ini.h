/*
 * ini.h - an editable, comment-preserving model of an NilBBS config file.
 *
 * The file is kept as its original list of lines.  Setting a key rewrites
 * only that key's line (or adds one at the end of its section), so the
 * sysop's comments and layout survive a round trip through BBSConfig.
 */
#ifndef NILBBS_INI_H
#define NILBBS_INI_H
#include <exec/types.h>

#define INI_MAXLINES 1200
#define INI_LINELEN  256

struct Ini {
    char  path[256];
    int   n;
    char *line[INI_MAXLINES];
    BOOL  dirty;
};

struct Ini *ini_load(const char *path);         /* missing file = empty model */
void  ini_free(struct Ini *ini);
BOOL  ini_save(struct Ini *ini);                 /* writes path.new then renames */

/* sect NULL = the top level (before any [SECTION]) */
BOOL  ini_get(struct Ini *ini, const char *sect, const char *key, char *out, int size);
LONG  ini_getint(struct Ini *ini, const char *sect, const char *key, LONG def);
BOOL  ini_getbool(struct Ini *ini, const char *sect, const char *key, BOOL def);
void  ini_set(struct Ini *ini, const char *sect, const char *key, const char *val);
void  ini_setint(struct Ini *ini, const char *sect, const char *key, LONG v);
void  ini_setbool(struct Ini *ini, const char *sect, const char *key, BOOL v);

int   ini_sections(struct Ini *ini, char names[][32], int max);
BOOL  ini_add_section(struct Ini *ini, const char *name);
BOOL  ini_del_section(struct Ini *ini, const char *name);
BOOL  ini_move_section(struct Ini *ini, const char *name, int dir);   /* -1 up, +1 down */
BOOL  ini_rename_section(struct Ini *ini, const char *from, const char *to);

/* raw line access (IPFilter.cfg, menus) */
BOOL  ini_insert_line(struct Ini *ini, int at, const char *text);
void  ini_delete_line(struct Ini *ini, int at);
void  ini_replace_line(struct Ini *ini, int at, const char *text);

#endif
