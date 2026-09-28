/*
 * lang.h - the caller-facing text in another language (BBS:Text/Language/<name>.lng).
 *
 * Every string the node shows a caller goes through L("key", "English text").
 * A language file holds `key = text` lines; L() returns the translation when the
 * loaded file has the key, else the English text compiled into the program - so a
 * partial translation falls back line by line and no file at all = English.
 * A translation whose printf conversions (%s %ld ...) differ from the English
 * text's is never used (it could crash the node), the English is shown instead.
 */
#ifndef NILBBS_LANG_H
#define NILBBS_LANG_H

#include <exec/types.h>

#define LANG_DIR     "BBS:Text/Language"
#define LANG_NAMELEN 16                 /* = sizeof(UserRec.lang), incl. the 0 */
#define LANG_ENGLISH "English"          /* compiled in: English.lng is only the template */

/* Load <name>.lng (replacing the loaded one).  map turns a Unicode character of the
 * file (UTF-8, or a Latin-1 byte) into the internal character set; NULL = keep
 * Latin-1 bytes as they are.  "" / "English" / a missing file = English.  TRUE if
 * a file was loaded. */
BOOL lang_load(const char *name, UBYTE (*map)(UWORD));
void lang_free(void);
const char *lang_current(void);         /* the loaded language's name ("English" if none) */
LONG lang_count(void);                  /* entries in the loaded file */

const char *L(const char *key, const char *english);

/* the language files in LANG_DIR, names without .lng, sorted; returns how many */
int  lang_list(char names[][LANG_NAMELEN], int max);
BOOL lang_exists(const char *name);     /* "English" always exists */

/* TRUE if both strings have the same printf conversions in the same order */
BOOL lang_fmt_match(const char *a, const char *b);

#endif
