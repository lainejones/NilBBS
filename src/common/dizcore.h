/*
 * dizcore.h - FILE_ID.DIZ handling shared by BBSNode and BBSMaint.
 * Set diz_env before the first call.
 */
#ifndef NILBBS_DIZCORE_H
#define NILBBS_DIZCORE_H
#include <exec/types.h>
#include <exec/semaphores.h>

#define DIZ_W   45              /* the FILE_ID.DIZ standard: 45 columns ... */
#define DIZ_H   10              /* ... by 10 lines (what we write) */
#define DIZ_READ_H 20           /* what we read: scene DIZs often run longer - a group logo, then
                                   the release's own name (e.g. DAT-SW21.LHA) */
#define DIZ_MAX 1024

struct DizEnv {
    struct Cfg *cfg;                    /* NilBBS.cfg (diz_* commands, bbs_name) */
    char   work[64];                    /* a private scratch drawer */
    struct SignalSemaphore *lock;       /* shared->filelock, or NULL */
    const char *uploader;               /* for {UPLOADER} in the template */
};
extern struct DizEnv diz_env;

/* read-only areas (a CD/DVD): the BBS writes nothing to the disc - the DIZ cache,
   files.bbs and the rest live in BBS:Data/CD/<tag> instead */
void area_readonly(const char *path, const char *tag);
const char *area_meta(const char *path);      /* where an area's own files go (its path, or that) */
void area_disc(const char *path, const char *tag, const char *disc);   /* a CD-ROM: this disc's cache */
BOOL area_is_ro(const char *path);

BOOL diz_is_archive(const char *name);
BOOL diz_extract(const char *archive, char *out, LONG max);
BOOL diz_get(const char *area, const char *name, char *out, LONG max);
struct DateStamp;
BOOL diz_get_at(const char *area, const char *name, const struct DateStamp *date, char *out, LONG max);
BOOL diz_add(const char *area, const char *name, const char *text);
void diz_cache_put(const char *area, const char *name, const char *text);
void diz_build(const char *prog, const char *version, char desc[][DIZ_W + 1], int ndesc,
               const char *area, char *out, LONG max);
/* take other boards' ads out of an archive (patterns in BBS:Config/StripAds.cfg; top-level files only,
   never FILE_ID.DIZ; the archive keeps its date).  Returns how many went; their names go in `removed`. */
LONG ads_strip(const char *archive, char *removed, LONG max);

/* the sysop's archive tools (BBSControl): what's in an LhA / Zip archive, and taking a file out of it
   (the archive keeps its date and protection bits).  arc_list returns the entries (-1 = can't be read,
   with the reason in err).  Zip needs UnZip and Zip installed; the commands are NilBBS.cfg settings. */
#define ARC_LHA 1
#define ARC_ZIP 2
struct ArcEntry { char name[108]; ULONG size; char date[20]; };
int  arc_kind(const char *name);                 /* ARC_LHA, ARC_ZIP or 0 */
LONG arc_list(const char *archive, struct ArcEntry *e, LONG max, char *err, LONG errmax);
BOOL arc_delete(const char *archive, const char *name, char *err, LONG errmax);
#endif
