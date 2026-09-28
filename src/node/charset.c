/*
 * charset.c - one printable byte in, the right bytes for this caller out.
 *
 * Source text is CP437 (ANSI art, most doors) or Latin-1 (Amiga-native
 * doors and text).  Callers get:
 *   CS_CP437   byte passes straight through (SyncTERM, NetRunner, ...)
 *   CS_UTF8    Unicode encoded as UTF-8 (PuTTY, xterm, macOS Terminal)
 *   CS_LATIN1  ISO-8859-1 - what an Amiga terminal program shows
 *   CS_ASCII   7-bit, with box drawing approximated by + - |
 * A VT100 caller without UTF-8 gets box drawing through the DEC Special
 * Graphics set (ESC ( 0) instead of ASCII approximations.
 */
#include <exec/types.h>
#include "node.h"

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

UWORD cp437_to_uni(UBYTE c)
{
    return c < 128 ? c : cp437_hi[c - 128];
}

/* Latin-1 letters 0xC0..0xFF without their accents, for 7-bit callers */
static const char latin_base[65] =
    "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPs"
    "aaaaaaaceeeeiiiidnooooo/ouuuuypy";

/* the closest single ASCII character */
static char uni_to_ascii(UWORD u)
{
    if (u < 128) return (char)u;
    if (u >= 0xC0 && u <= 0xFF) return latin_base[u - 0xC0];
    switch (u) {
    case 0x2500: case 0x2550: return '-';
    case 0x2502: case 0x2551: return '|';
    case 0x2591: return '.';
    case 0x2592: return ':';
    case 0x2593: case 0x2588: case 0x258C: case 0x2590: return '#';
    case 0x2584: return '_';
    case 0x2580: return '"';
    case 0x25A0: return '*';
    case 0x00B7: case 0x2219: return '.';
    case 0x00A0: return ' ';
    case 0x00AB: return '<';
    case 0x00BB: return '>';
    case 0x00A2: return 'c';
    case 0x00A3: return 'L';
    case 0x00A5: return 'Y';
    case 0x00B0: return 'o';
    case 0x00B1: return '+';
    case 0x00B2: return '2';
    case 0x00B5: return 'u';
    case 0x00BF: return '?';
    case 0x00A1: return '!';
    case 0x00F7: return '/';
    case 0x00AC: return '-';
    case 0x2264: return '<';
    case 0x2265: return '>';
    case 0x2248: return '~';
    case 0x2261: return '=';
    case 0x221E: return '8';
    case 0x03C0: return 'p';
    case 0x03A3: case 0x03C3: return 's';
    case 0x03B1: return 'a';
    case 0x00DF: return 'B';
    }
    if (u >= 0x2500 && u <= 0x257F) return '+';   /* corners, tees, crosses */
    return '?';
}

/* DEC Special Graphics letter for a box-drawing code point, 0 if none */
static char uni_to_dec(UWORD u)
{
    switch (u) {
    case 0x2500: case 0x2550: return 'q';
    case 0x2502: case 0x2551: return 'x';
    case 0x250C: case 0x2554: case 0x2552: case 0x2553: return 'l';
    case 0x2510: case 0x2557: case 0x2555: case 0x2556: return 'k';
    case 0x2514: case 0x255A: case 0x2558: case 0x2559: return 'm';
    case 0x2518: case 0x255D: case 0x255B: case 0x255C: return 'j';
    case 0x251C: case 0x2560: case 0x255E: case 0x255F: return 't';
    case 0x2524: case 0x2563: case 0x2561: case 0x2562: return 'u';
    case 0x2534: case 0x2569: case 0x2567: case 0x2568: return 'v';
    case 0x252C: case 0x2566: case 0x2564: case 0x2565: return 'w';
    case 0x253C: case 0x256C: case 0x256A: case 0x256B: return 'n';
    case 0x2591: case 0x2592: case 0x2593: case 0x2588: return 'a';
    case 0x00B0: return 'f';
    case 0x00B1: return 'g';
    case 0x2264: return 'y';
    case 0x2265: return 'z';
    case 0x03C0: return '{';
    case 0x00A3: return '}';
    case 0x00B7: case 0x2219: return '~';
    }
    return 0;
}

static void put1(UBYTE c) { tn_raw(&c, 1); }

static void dec_off(void)
{
    if (N.dec_gfx) {
        static const UBYTE g0b[3] = { 27, '(', 'B' };
        tn_raw(g0b, 3);
        N.dec_gfx = 0;
    }
}

/*
 * Emit one printable (non-escape) byte from a source in charset `srccs`
 * (CS_CP437 or CS_LATIN1).  Control characters < 32 are passed through.
 */
void emit_char(UBYTE c, UBYTE srccs)
{
    UWORD u;

    if (c < 128) {
        if (N.dec_gfx && c >= 32) dec_off();
        put1(c);
        return;
    }
    /* same charset on both ends: no work */
    if (srccs == CS_CP437 && N.charset == CS_CP437) { put1(c); return; }
    if (srccs == CS_LATIN1 && N.charset == CS_LATIN1) { put1(c); return; }

    u = (srccs == CS_LATIN1) ? c : cp437_to_uni(c);
    if (u >= 0x80 && u < 0xA0) return;          /* C1 controls: drop */

    switch (N.charset) {
    case CS_UTF8:
        if (u < 0x800) {
            put1((UBYTE)(0xC0 | (u >> 6)));
            put1((UBYTE)(0x80 | (u & 0x3F)));
        } else {
            put1((UBYTE)(0xE0 | (u >> 12)));
            put1((UBYTE)(0x80 | ((u >> 6) & 0x3F)));
            put1((UBYTE)(0x80 | (u & 0x3F)));
        }
        return;
    case CS_CP437: {                            /* Latin-1 source -> CP437 */
        int i;
        for (i = 0; i < 128; i++)
            if (cp437_hi[i] == u) { put1((UBYTE)(128 + i)); return; }
        put1((UBYTE)uni_to_ascii(u));
        return;
    }
    case CS_LATIN1:
        if (u < 256) { put1((UBYTE)u); return; }
        /* fall through: box drawing etc. */
    default:
        if (N.term == TT_VT100) {
            char d = uni_to_dec(u);
            if (d) {
                if (!N.dec_gfx) {
                    static const UBYTE g00[3] = { 27, '(', '0' };
                    tn_raw(g00, 3);
                    N.dec_gfx = 1;
                }
                put1((UBYTE)d);
                return;
            }
        }
        if (N.dec_gfx) dec_off();
        put1((UBYTE)uni_to_ascii(u));
        return;
    }
}
