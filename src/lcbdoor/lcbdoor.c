/*
 * LCBDoor - run "Last Call BBS" JavaScript doors as native Amiga BBS doors.
 *
 *   LCBDoor <script.js> [USER=<handle>] [DROP=<DOOR.SYS>] [DATA=<file>]
 *
 * Last Call BBS (Zachtronics) doors are JavaScript programs written against
 * a tiny API: onConnect/onUpdate/onInput(key) callbacks, drawText(text,
 * colour, x, y) and clearScreen() on a 56x20 screen, plus a save hook.  This
 * door embeds the Duktape JavaScript engine, supplies that API, and turns the
 * screen into ANSI on Output() - so any AmigaDOS BBS (NilBBS as a cli door,
 * or anything that runs CLI doors) can offer these games.
 *
 * Doors that know the "_bbs_" save API (e.g. the LORD remake) keep one shared
 * world in DATA (default: the script's name + .dat) and each player's
 * character is keyed to their BBS handle, which the door enforces.
 */
#include <exec/types.h>
#include <exec/tasks.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "duktape.h"

static const char __attribute__((used)) verstag[] = "$VER: LCBDoor 1.0 (22.9.2026)";
/* Duktape recurses deeply (compiling a 250K script); main() makes sure it
 * has STACK_NEED bytes whatever stack the BBS or Shell gave us. */
#define STACK_NEED (192 * 1024)

#define SW 56
#define SH 20

static BPTR in, out;
static UBYTE scr_ch[SH][SW], scr_col[SH][SW];     /* what the game drew */
static UBYTE sent_ch[SH][SW], sent_col[SH][SW];   /* what the caller has */
static BOOL  first_frame = TRUE;
static char  user[40] = "SYSOP";
static char  datafile[256];
static char  outbuf[4096];
static BPTR  logfh;

static void logf_(const char *fmt, long a, long b)
{
    char s[120];
    if (!logfh) return;
    sprintf(s, fmt, a, b);
    FPuts(logfh, (STRPTR)s);
    Flush(logfh);
}
static int   outlen;

/* ---- output --------------------------------------------------------------- */

static void flush_out(void)
{
    if (outlen) Write(out, outbuf, outlen);
    outlen = 0;
}

static void put(const char *s, int n)
{
    while (n-- > 0) {
        if (outlen >= (int)sizeof(outbuf)) flush_out();
        outbuf[outlen++] = *s++;
    }
}

static void puts_(const char *s) { put(s, strlen(s)); }

/* Last Call colour index -> ANSI.  The LORD remake maps LORD's own colour
 * codes onto these numbers, so this restores the real LORD colours. */
static const char *sgr_for(UBYTE c)
{
    switch (c) {
    case 0:  return "\x1b[0;1;30m";     /* `8 dark grey */
    case 2:  return "\x1b[0;34m";       /* `1 blue */
    case 4:  return "\x1b[0;36m";       /* `3 cyan */
    case 5:  return "\x1b[0;31m";       /* `4 red */
    case 6:  return "\x1b[0;35m";       /* `5 magenta */
    case 7:  return "\x1b[0;33m";       /* `6 brown */
    case 8:  return "\x1b[0;37m";       /* `7 grey */
    case 9:  return "\x1b[0;32m";       /* `2 green (the default) */
    case 10: return "\x1b[0;1;34m";     /* `9 */
    case 11: return "\x1b[0;1;32m";     /* `0 */
    case 12: return "\x1b[0;1;36m";     /* `! */
    case 13: return "\x1b[0;1;31m";     /* `@ */
    case 14: return "\x1b[0;1;35m";     /* `# */
    case 15: return "\x1b[0;1;33m";     /* `$ */
    case 16: case 17: return "\x1b[0;1;37m";
    default: return "\x1b[0;37m";
    }
}

/* send only what changed since the last frame */
static void render(void)
{
    int y, x;
    char pos[16];
    int lastcol = -1;
    if (first_frame) {
        puts_("\x1b[0m\x1b[2J\x1b[H");
        memset(sent_ch, ' ', sizeof(sent_ch));
        memset(sent_col, 9, sizeof(sent_col));
        first_frame = FALSE;
    }
    for (y = 0; y < SH; y++) {
        x = 0;
        while (x < SW) {
            int start, end;
            if (scr_ch[y][x] == sent_ch[y][x] && (scr_col[y][x] == sent_col[y][x] || scr_ch[y][x] == ' ')) { x++; continue; }
            start = x;
            end = x;
            /* extend the run across small unchanged gaps */
            while (end < SW) {
                int gap = 0;
                while (end + gap < SW && scr_ch[y][end + gap] == sent_ch[y][end + gap] &&
                       (scr_col[y][end + gap] == sent_col[y][end + gap] || scr_ch[y][end + gap] == ' ')) gap++;
                if (end + gap >= SW || gap > 4) break;
                end += gap + 1;
            }
            sprintf(pos, "\x1b[%d;%dH", y + 1, start + 1);
            puts_(pos);
            for (x = start; x < end; x++) {
                if (scr_ch[y][x] != ' ' && scr_col[y][x] != lastcol) {
                    puts_(sgr_for(scr_col[y][x]));
                    lastcol = scr_col[y][x];
                }
                put((char *)&scr_ch[y][x], 1);
                sent_ch[y][x] = scr_ch[y][x];
                sent_col[y][x] = scr_col[y][x];
            }
        }
    }
    /* park the cursor on the status line, after its text */
    {
        int cx = SW - 1;
        while (cx > 0 && scr_ch[SH - 1][cx] == ' ') cx--;
        sprintf(pos, "\x1b[%d;%dH", SH, cx + 2);
        puts_(pos);
    }
    flush_out();
}

/* ---- Unicode -> CP437 ---------------------------------------------------------- */

static const UWORD cp437_hi[128] = {
    0x00C7,0x00FC,0x00E9,0x00E2,0x00E4,0x00E0,0x00E5,0x00E7,0x00EA,0x00EB,0x00E8,0x00EF,0x00EE,0x00EC,0x00C4,0x00C5,
    0x00C9,0x00E6,0x00C6,0x00F4,0x00F6,0x00F2,0x00FB,0x00F9,0x00FF,0x00D6,0x00DC,0x00A2,0x00A3,0x00A5,0x20A7,0x0192,
    0x00E1,0x00ED,0x00F3,0x00FA,0x00F1,0x00D1,0x00AA,0x00BA,0x00BF,0x2310,0x00AC,0x00BD,0x00BC,0x00A1,0x00AB,0x00BB,
    0x2591,0x2592,0x2593,0x2502,0x2524,0x2561,0x2562,0x2556,0x2555,0x2563,0x2551,0x2557,0x255D,0x255C,0x255B,0x2510,
    0x2514,0x2534,0x252C,0x251C,0x2500,0x253C,0x255E,0x255F,0x255A,0x2554,0x2569,0x2566,0x2560,0x2550,0x256C,0x2567,
    0x2568,0x2564,0x2565,0x2559,0x2558,0x2552,0x2553,0x256B,0x256A,0x2518,0x250C,0x2588,0x2584,0x258C,0x2590,0x2580,
    0x03B1,0x00DF,0x0393,0x03C0,0x03A3,0x03C3,0x00B5,0x03C4,0x03A6,0x0398,0x03A9,0x03B4,0x221E,0x03C6,0x03B5,0x2229,
    0x2261,0x00B1,0x2265,0x2264,0x2320,0x2321,0x00F7,0x2248,0x00B0,0x2219,0x00B7,0x221A,0x207F,0x00B2,0x25A0,0x00A0
};

static UBYTE to_cp437(ULONG u)
{
    int i;
    if (u < 128) return (UBYTE)u;
    for (i = 0; i < 128; i++) if (cp437_hi[i] == u) return (UBYTE)(128 + i);
    if (u == 0x2022 || u == 0x25CF) return 7;   /* bullets */
    return '?';
}

/* ---- the Last Call API ------------------------------------------------------------ */

static duk_ret_t js_drawText(duk_context *ctx)
{
    const unsigned char *s = (const unsigned char *)duk_safe_to_string(ctx, 0);
    int col = duk_to_int(ctx, 1), x = duk_to_int(ctx, 2), y = duk_to_int(ctx, 3);
    if (y < 0 || y >= SH) return 0;
    while (*s) {
        ULONG u = *s++;
        /* UTF-8 (Duktape's CESU-8 is the same for the BMP) */
        if (u >= 0xE0 && s[0] && s[1]) { u = ((u & 0x0F) << 12) | ((s[0] & 0x3F) << 6) | (s[1] & 0x3F); s += 2; }
        else if (u >= 0xC0 && s[0]) { u = ((u & 0x1F) << 6) | (s[0] & 0x3F); s += 1; }
        if (x >= 0 && x < SW) {
            /* a control character (LORD's stats screen has tabs) is one blank
             * cell: sent as-is, the terminal would jump to its own tab stop and
             * the screen would no longer match what we think is on it */
            scr_ch[y][x] = u < 32 ? ' ' : to_cp437(u);
            scr_col[y][x] = (UBYTE)col;
        }
        x++;
    }
    return 0;
}

static duk_ret_t js_clearScreen(duk_context *ctx)
{
    (void)ctx;
    memset(scr_ch, ' ', sizeof(scr_ch));
    return 0;
}

/* file helpers for the save API (the store itself is kept in JavaScript) */
static duk_ret_t js_readfile(duk_context *ctx)
{
    const char *path = duk_require_string(ctx, 0);
    BPTR fh = Open((STRPTR)path, MODE_OLDFILE);
    LONG size, n;
    char *buf;
    if (!fh) { duk_push_null(ctx); return 1; }
    Seek(fh, 0, OFFSET_END);
    size = Seek(fh, 0, OFFSET_BEGINNING);
    if (size < 0 || !(buf = malloc(size + 1))) { Close(fh); duk_push_null(ctx); return 1; }
    n = Read(fh, buf, size);
    Close(fh);
    duk_push_lstring(ctx, buf, n > 0 ? n : 0);
    free(buf);
    return 1;
}

static duk_ret_t js_writefile(duk_context *ctx)
{
    const char *path = duk_require_string(ctx, 0);
    duk_size_t len;
    const char *data = duk_require_lstring(ctx, 1, &len);
    char tmp[270];
    BPTR fh;
    BOOL ok = FALSE;
    sprintf(tmp, "%s.new", path);
    if ((fh = Open((STRPTR)tmp, MODE_NEWFILE))) {
        ok = Write(fh, (APTR)data, len) == (LONG)len;
        Close(fh);
        if (ok) { DeleteFile((STRPTR)path); ok = Rename((STRPTR)tmp, (STRPTR)path); }
    }
    duk_push_boolean(ctx, ok);
    return 1;
}

/* JavaScript run before the door: the Last Call helpers and the "_bbs_" save
 * API, which keeps one shared store per door and returns the caller's handle
 * for the 'handle' key. */
static const char *prelude =
    "var __store = {};\n"
    "function random(a, b) { return Math.floor(Math.random() * (b - a + 1)) + a; }\n"
    "function __reload() { var s = __readfile(__datafile); if (s) { try { __store = JSON.parse(s); } catch (e) { __store = {}; } } }\n"
    "function _bbs_load() { __reload(); return 1; }\n"
    "function _bbs_load_type(ns, def, key) {\n"
    "  if (key === 'handle') return __user;\n"
    "  var v = __store[key]; return (v === undefined) ? def : JSON.parse(JSON.stringify(v)); }\n"
    "var __pending = {};\n"
    "function _bbs_save_type(ns, key, val) { if (key !== 'handle') __pending[key] = JSON.parse(JSON.stringify(val)); }\n"
    "function _bbs_save() { __reload(); for (var k in __pending) __store[k] = __pending[k]; __pending = {};\n"
    "  __writefile(__datafile, JSON.stringify(__store)); }\n"
    "function saveData(s) { __writefile(__datafile, s); }\n"
    "function loadData() { var s = __readfile(__datafile); return s ? s : ''; }\n";

/* appended after the door: lets us see the game's state from C (top-level
 * let/const bindings aren't properties of the global object) */
static const char *postlude =
    "\n;globalThis.__lcb_state = function () {\n"
    "  var st = 0;\n"
    "  try { if (typeof current !== 'undefined' && typeof menu_exit !== 'undefined' && current === menu_exit) st |= 1; } catch (e) {}\n"
    "  try { if (typeof current !== 'undefined' && typeof menu_login !== 'undefined' && current === menu_login) st |= 2; } catch (e) {}\n"
    "  return st; };\n"
    "globalThis.__lcb_force_name = function (n) {\n"
    "  try { buffer = n; status = n; } catch (e) {} };\n";

/* ---- running it ------------------------------------------------------------------- */

static BOOL call0(duk_context *ctx, const char *fn)
{
    BOOL ok = TRUE;
    if (!duk_get_global_string(ctx, fn) || !duk_is_function(ctx, -1)) { duk_pop(ctx); return TRUE; }
    if (duk_pcall(ctx, 0) != DUK_EXEC_SUCCESS) {
        char msg[200];
        sprintf(msg, "\r\n\x1b[0;31m[door error in %s: %.150s]\x1b[0m\r\n", fn, duk_safe_to_string(ctx, -1));
        puts_(msg);
        flush_out();
        ok = FALSE;
    }
    duk_pop(ctx);
    return ok;
}

static BOOL call1(duk_context *ctx, const char *fn, int arg)
{
    BOOL ok = TRUE;
    if (!duk_get_global_string(ctx, fn) || !duk_is_function(ctx, -1)) { duk_pop(ctx); return TRUE; }
    duk_push_int(ctx, arg);
    if (duk_pcall(ctx, 1) != DUK_EXEC_SUCCESS) {
        char msg[200];
        sprintf(msg, "\r\n\x1b[0;31m[door error in %s: %.150s]\x1b[0m\r\n", fn, duk_safe_to_string(ctx, -1));
        puts_(msg);
        flush_out();
        ok = FALSE;
    }
    duk_pop(ctx);
    return ok;
}

static int game_state(duk_context *ctx)
{
    int st = 0;
    if (duk_get_global_string(ctx, "__lcb_state") && duk_pcall(ctx, 0) == DUK_EXEC_SUCCESS)
        st = duk_to_int(ctx, -1);
    duk_pop(ctx);
    return st;
}

static void force_name(duk_context *ctx)
{
    duk_get_global_string(ctx, "__lcb_force_name");
    duk_push_string(ctx, user);
    duk_pcall(ctx, 1);
    duk_pop(ctx);
}

/* handle = DOOR.SYS line 36 (the alias) */
static void user_from_dropfile(const char *path)
{
    BPTR fh = Open((STRPTR)path, MODE_OLDFILE);
    char line[80];
    int n = 0;
    if (!fh) return;
    while (FGets(fh, (STRPTR)line, sizeof(line))) {
        char *e = line + strlen(line);
        while (e > line && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
        if (++n == 36 && line[0]) { strncpy(user, line, sizeof(user) - 1); break; }
    }
    Close(fh);
}

/* next key from the caller, or -1 if none within `usecs` */
static LONG get_key(LONG usecs)
{
    UBYTE c;
    if (!WaitForChar(in, usecs)) return -1;
    if (Read(in, &c, 1) != 1) return -2;              /* carrier gone */
    if (c == 27) {                                    /* swallow ANSI key sequences */
        while (WaitForChar(in, 30000)) {
            UBYTE d;
            if (Read(in, &d, 1) != 1) return -2;
            if ((d >= 'A' && d <= 'Z') || (d >= 'a' && d <= 'z') || d == '~') break;
        }
        return -1;
    }
    if (c == '\r') {                                  /* CR LF -> one Enter */
        if (WaitForChar(in, 1000)) { UBYTE d; Read(in, &d, 1); if (d != '\n' && d != 0) { /* keep it */ } }
        return 10;
    }
    if (c == '\n') return 10;
    if (c == 127) return 8;
    return c;
}

static int real_main(void)
{
    struct RDArgs *rda;
    LONG args[6] = { 0, 0, 0, 0, 0, 0 };
    duk_context *ctx;
    char *src;
    LONG size, n;
    BPTR fh;
    int rc = RETURN_OK, idle = 0, exit_timer = -1;

    in = Input();
    out = Output();
    rda = ReadArgs((STRPTR)"SCRIPT/A,USER/K,DROP/K,DATA/K,LOG/K,MAINT/S", args, NULL);
    if (!rda) { PutStr((STRPTR)"Usage: LCBDoor <script.js> [USER=name] [DROP=DOOR.SYS] [DATA=file] [MAINT]\n"); return RETURN_FAIL; }
    if (args[4]) logfh = Open((STRPTR)args[4], MODE_NEWFILE);
    if (args[2]) user_from_dropfile((char *)args[2]);
    if (args[1]) strncpy(user, (char *)args[1], sizeof(user) - 1);
    if (args[3]) strncpy(datafile, (char *)args[3], sizeof(datafile) - 1);
    else {
        char *dot;
        strncpy(datafile, (char *)args[0], sizeof(datafile) - 5);
        if ((dot = strrchr(datafile, '.'))) *dot = 0;
        strcat(datafile, ".dat");
    }

    if (!(fh = Open((STRPTR)args[0], MODE_OLDFILE))) { PutStr((STRPTR)"LCBDoor: can't open the script\n"); FreeArgs(rda); return RETURN_FAIL; }
    Seek(fh, 0, OFFSET_END);
    size = Seek(fh, 0, OFFSET_BEGINNING);
    src = malloc(size + strlen(postlude) + 1);
    n = src ? Read(fh, src, size) : -1;
    Close(fh);
    if (n != size) { PutStr((STRPTR)"LCBDoor: can't read the script\n"); FreeArgs(rda); return RETURN_FAIL; }
    memcpy(src + size, postlude, strlen(postlude) + 1);

    if (!args[5]) {
        puts_("\x1b[0m\x1b[2J\x1b[H\x1b[0;32mLoading...\x1b[0m");
        flush_out();
    }

    if (!(ctx = duk_create_heap_default())) { PutStr((STRPTR)"LCBDoor: out of memory\n"); FreeArgs(rda); return RETURN_FAIL; }
    duk_push_c_function(ctx, js_drawText, 4);    duk_put_global_string(ctx, "drawText");
    duk_push_c_function(ctx, js_clearScreen, 0); duk_put_global_string(ctx, "clearScreen");
    duk_push_c_function(ctx, js_readfile, 1);    duk_put_global_string(ctx, "__readfile");
    duk_push_c_function(ctx, js_writefile, 2);   duk_put_global_string(ctx, "__writefile");
    duk_push_string(ctx, user);                  duk_put_global_string(ctx, "__user");
    duk_push_string(ctx, datafile);              duk_put_global_string(ctx, "__datafile");
    if (duk_peval_string(ctx, prelude) != 0) { puts_("prelude failed\r\n"); rc = RETURN_FAIL; goto out; }
    duk_pop(ctx);
    duk_push_string(ctx, (const char *)args[0]);
    if (duk_pcompile_string_filename(ctx, 0, src) != 0 || duk_pcall(ctx, 0) != 0) {
        char msg[300];
        sprintf(msg, "\r\n[LCBDoor: script error: %.250s]\r\n", duk_safe_to_string(ctx, -1));
        puts_(msg);
        flush_out();
        rc = RETURN_ERROR;
        goto out;
    }
    duk_pop(ctx);
    free(src);
    src = NULL;

    /* MAINT: nightly maintenance (BBSMaint runs it) - no caller, no screen;
     * the script's onMaint() does the work and returns a report */
    if (args[5]) {
        if (duk_get_global_string(ctx, "onMaint")) {
            if (duk_pcall(ctx, 0) != 0) {
                Printf((STRPTR)"LCBDoor: maintenance failed: %s\n", (LONG)duk_safe_to_string(ctx, -1));
                rc = RETURN_ERROR;
            } else if (duk_is_string(ctx, -1)) {
                PutStr((STRPTR)duk_get_string(ctx, -1));
                PutStr((STRPTR)"\n");
            }
        } else PutStr((STRPTR)"LCBDoor: this door has no onMaint() - nothing to do\n");
        duk_pop(ctx);
        goto out;
    }

    memset(scr_ch, ' ', sizeof(scr_ch));
    memset(scr_col, 9, sizeof(scr_col));
    call0(ctx, "onConnect");
    call0(ctx, "onUpdate");
    render();
    if (!IsInteractive(in)) goto out;       /* not a caller (e.g. <NIL:): one frame, then stop */
    SetMode(in, 1);

    for (;;) {
        LONG k = get_key(50000);                    /* ~20 frames a second */
        int st;
        if (k == -2) break;                         /* caller gone */
        if (CheckSignal(SIGBREAKF_CTRL_C)) break;   /* the BBS wants us out */
        st = game_state(ctx);
        if (k >= 0) {
            logf_("key %ld state %ld\n", k, st);
            idle = 0;
            if (st & 2) {                           /* login: the name is the BBS handle */
                force_name(ctx);
                if (k != 10) k = -1;
            }
            if (k >= 0 && exit_timer < 0) call1(ctx, "onInput", k);
        } else if (++idle > 20 * 60 * 15) break;    /* 15 minutes idle */
        /* Last Call doors redraw everything each frame (LORD draws its whole log
         * and the prompt line), so start each frame from an empty screen - else
         * the tail of a longer old line stays behind a shorter new one.  The
         * diff in render() still sends only what really changed. */
        memset(scr_ch, ' ', sizeof(scr_ch));
        memset(scr_col, 9, sizeof(scr_col));
        /* show the BBS handle as soon as the login prompt appears, not the
         * game's "SYSOP" placeholder until the next key */
        if (game_state(ctx) & 2) force_name(ctx);
        call0(ctx, "onUpdate");
        render();
        st = game_state(ctx);
        if ((st & 1) && exit_timer < 0) exit_timer = 60;   /* let them read the goodbye */
        if (exit_timer > 0 && --exit_timer == 0) break;
    }
    SetMode(in, 0);
    puts_("\x1b[0m\x1b[21;1H\r\n");
    flush_out();
out:
    duk_destroy_heap(ctx);
    if (logfh) Close(logfh);
    if (src) free(src);
    FreeArgs(rda);
    return rc;
}

/* The door can be started with a 4K Shell stack; running Duktape on that
 * overflows it and corrupts memory (guru 81000005).  So check, and if it is
 * too small switch to our own stack with exec's StackSwap(). */
static struct StackSwapStruct sss;
static APTR  newstack;
static int   main_rc;

int main(void)
{
    struct Task *me = FindTask(NULL);
    ULONG have = (ULONG)me->tc_SPUpper - (ULONG)me->tc_SPLower;
    if (have >= STACK_NEED) return real_main();
    if (!(newstack = AllocVec(STACK_NEED, MEMF_ANY))) {
        PutStr((STRPTR)"LCBDoor: not enough memory for its stack\n");
        return RETURN_FAIL;
    }
    sss.stk_Lower   = newstack;
    sss.stk_Upper   = (ULONG)newstack + STACK_NEED;
    sss.stk_Pointer = (APTR)sss.stk_Upper;
    StackSwap(&sss);
    main_rc = real_main();
    StackSwap(&sss);
    FreeVec(newstack);
    return main_rc;
}
