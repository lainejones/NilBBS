/*
 * userdb.c - the user file: fixed 768-byte records in BBS:Data/Users.dat.
 * Record n (0-based) holds user id n+1.  Deleted users keep their slot (so
 * message "from" ids stay meaningful) and are flagged UF_DELETED.
 *
 * Callers hold shared->userlock around read-modify-write sequences.
 */
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>

#include "bbs.h"

/* where Users.dat is; BBSCtl DIR= points it into a BBS drawer that isn't BBS: */
const char *userdb_path = BBS_USERS;

#define PW_ROUNDS 256       /* ~0.3s on a 68030/25 - enough to slow guessing */

LONG userdb_count(void)
{
    LONG sz = file_size(userdb_path);
    return sz > 0 ? sz / USERREC_SIZE : 0;
}

BOOL userdb_read(ULONG id, struct UserRec *u)
{
    BPTR fh;
    BOOL ok = FALSE;
    if (!id) return FALSE;
    if (!(fh = Open((STRPTR)userdb_path, MODE_OLDFILE))) return FALSE;
    if (Seek(fh, (id - 1) * USERREC_SIZE, OFFSET_BEGINNING) >= 0 &&
        Read(fh, u, USERREC_SIZE) == USERREC_SIZE && u->id == id)
        ok = TRUE;
    Close(fh);
    return ok;
}

BOOL userdb_write(struct UserRec *u)
{
    BPTR fh;
    BOOL ok = FALSE;
    if (!u->id) return FALSE;
    if (!(fh = Open((STRPTR)userdb_path, MODE_READWRITE))) return FALSE;
    if (Seek(fh, (u->id - 1) * USERREC_SIZE, OFFSET_BEGINNING) >= 0 &&
        Write(fh, u, USERREC_SIZE) == USERREC_SIZE)
        ok = TRUE;
    Close(fh);
    return ok;
}

#define FIND_BLOCK 16       /* records per Read() in userdb_find (12 KB) */

LONG userdb_find(const char *name, struct UserRec *u)
{
    BPTR fh;
    LONG found = 0;
    struct UserRec *blk;
    if (!(fh = Open((STRPTR)userdb_path, MODE_OLDFILE))) return 0;
    if ((blk = AllocVec(FIND_BLOCK * USERREC_SIZE, 0))) {
        LONG got, n, i;
        while (!found && (got = Read(fh, blk, FIND_BLOCK * USERREC_SIZE)) >= USERREC_SIZE) {
            n = got / USERREC_SIZE;             /* a short last record is ignored, as before */
            for (i = 0; i < n; i++) {
                struct UserRec *r = &blk[i];
                if (r->id && !(r->flags & UF_DELETED) && !str_icmp(r->name, name)) { found = r->id; break; }
            }
            memcpy(u, &blk[found ? i : n - 1], USERREC_SIZE);   /* as before: the match, or the last read */
            if (got < FIND_BLOCK * USERREC_SIZE) break;
        }
        FreeVec(blk);
    } else {
        while (Read(fh, u, USERREC_SIZE) == USERREC_SIZE) {
            if (u->id && !(u->flags & UF_DELETED) && !str_icmp(u->name, name)) {
                found = u->id;
                break;
            }
        }
    }
    Close(fh);
    return found;
}

LONG userdb_add(struct UserRec *u)
{
    BPTR fh;
    LONG n;
    if (!(fh = Open((STRPTR)userdb_path, MODE_READWRITE))) return 0;
    n = Seek(fh, 0, OFFSET_END);
    n = Seek(fh, 0, OFFSET_END);          /* second call returns the new end */
    u->id = n / USERREC_SIZE + 1;
    if (Write(fh, u, USERREC_SIZE) != USERREC_SIZE) u->id = 0;
    Close(fh);
    return u->id;
}

static void hash_pw(const UBYTE salt[16], const char *pw, UBYTE out[32])
{
    UBYTE buf[16 + 32 + 64];
    LONG pl = strlen(pw), i;
    if (pl > 64) pl = 64;
    memcpy(buf, salt, 16);
    memset(buf + 16, 0, 32);
    memcpy(buf + 48, pw, pl);
    sha256(buf, 48 + pl, out);
    for (i = 1; i < PW_ROUNDS; i++) {       /* h = SHA256(salt | h | pw) */
        memcpy(buf + 16, out, 32);
        sha256(buf, 48 + pl, out);
    }
}

void user_setpass(struct UserRec *u, const char *pw)
{
    struct DateStamp ds;
    ULONG seed;
    int i;
    DateStamp(&ds);
    seed = ds.ds_Days * 1440 + ds.ds_Minute;
    seed = seed * 50 + ds.ds_Tick + (ULONG)FindTask(NULL) + (ULONG)u;
    for (i = 0; i < 16; i++) {              /* salt only needs to be unique */
        seed = seed * 1103515245UL + 12345UL;
        u->salt[i] = (UBYTE)(seed >> 16);
    }
    hash_pw(u->salt, pw, u->pwhash);
}

BOOL user_checkpass(const struct UserRec *u, const char *pw)
{
    UBYTE h[32];
    UBYTE diff = 0;
    int i;
    hash_pw(u->salt, pw, h);
    for (i = 0; i < 32; i++) diff |= h[i] ^ u->pwhash[i];
    return diff == 0;
}
