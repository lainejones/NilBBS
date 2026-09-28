/*
 * fse.c - the full-screen message editor, for ANSI and VT100 callers.
 *
 *   rows 1-3   who it's to, the subject, the keys
 *   rows 4..   the text, scrolling
 *   last row   the status / Esc menu
 *
 * Typing inserts; a line that runs past the width wraps its last word onto a
 * new line.  Enter splits the line, Backspace/Del join lines, the arrows, Home,
 * End, PgUp and PgDn move.  Ctrl-Z saves, Ctrl-A aborts, Ctrl-Q quotes the
 * message being answered, Ctrl-Y deletes a line, Ctrl-R redraws.  Esc opens a
 * menu for terminals where control keys are awkward.
 */
#include <exec/types.h>
#include <proto/exec.h>
#include <string.h>
#include <stdio.h>

#include "node.h"

#define TOP 4                       /* first text row */

static struct Editor *E;
static int cx, cy, top;             /* cursor column/line, first line on screen */
static int W, H;                    /* text width and height */
static const char *q_text, *q_from, *h_to, *h_subj;

static int  llen(int i) { return (int)strlen(E->line[i]); }
static char *EL(int i)   { return E->line[i]; }

static void draw_line(int i)
{
    int row = TOP + i - top;
    if (row < TOP || row >= TOP + H) return;
    tgotoxy(1, row);
    tputs("|07");
    if (i < E->n) tputraw((const UBYTE *)EL(i), llen(i), CS_CP437);
    tcleol();
}

static void draw_text(void)
{
    int r;
    for (r = 0; r < H; r++) draw_line(top + r);
}

static void status(const char *msg)
{
    tgotoxy(1, N.rows);
    tputs(msg);
    tcleol();
}

static void draw_all(void)
{
    int i;
    char bar[140];
    tcls();
    tprintf(L("fse.draw_all.to_subj", "|09To: |15%-30.30s |09Subj: |15%.40s\n"), h_to ? h_to : "", h_subj ? h_subj : "");
    tputs(L("fse.draw_all.save_abort_quote", "|08^Z |07save  |08^A |07abort  |08^Q |07quote  |08^Y |07delete line  |08^R |07redraw  |08Esc |07menu\n|08"));
    for (i = 0; i < W && i < (int)sizeof(bar) - 1; i++) bar[i] = (N.term == TT_ASCII || N.charset == CS_ASCII) ? '-' : (char)0xC4;
    bar[i] = 0;
    tputs(bar);
    draw_text();
    status(L("fse.status.hint", "|08Full-screen editor - Esc for the menu|07"));
}

static void place(void)
{
    tgotoxy(cx + 1, TOP + cy - top);
    tn_flush();
}

/* keep the cursor line on screen; TRUE if the view scrolled */
static BOOL scroll(void)
{
    if (cy < top) { top = cy; return TRUE; }
    if (cy >= top + H) { top = cy - H + 1; return TRUE; }
    return FALSE;
}

static BOOL insert_line(int at, const char *text)
{
    if (E->n >= ED_MAXLINES) return FALSE;
    memmove(E->line[at + 1], E->line[at], (E->n - at) * sizeof(E->line[0]));
    str_copy(E->line[at], text, ED_WIDTH + 1);
    E->n++;
    return TRUE;
}

static void delete_line(int at)
{
    if (E->n <= 1) { EL(0)[0] = 0; return; }
    memmove(E->line[at], E->line[at + 1], (E->n - at - 1) * sizeof(E->line[0]));
    E->n--;
}

/* typed a character: insert, and wrap the line if it's now too long */
static void type_char(UBYTE c)
{
    char *l = EL(cy);
    int len = llen(cy);
    if (len > W) return;                    /* one over W is allowed: it triggers the wrap */
    memmove(l + cx + 1, l + cx, len - cx + 1);
    l[cx++] = (char)c;
    len++;
    if (len <= W) {
        /* redraw from the cursor only */
        tputraw((const UBYTE *)l + cx - 1, len - cx + 1, CS_CP437);
        return;
    }
    {
        int sp = W, cut, rest;
        while (sp > 0 && l[sp] != ' ') sp--;
        if (sp <= 0) { sp = W; cut = W; rest = W; }   /* one long word: hard break */
        else { cut = sp; rest = sp + 1; }
        if (!insert_line(cy + 1, l + rest)) {        /* message full: take the character back */
            cx--;
            memmove(l + cx, l + cx + 1, len - cx);
            draw_line(cy);
            return;
        }
        l[cut] = 0;
        if (cx > cut) { cx -= rest; cy++; }
        scroll();
        draw_text();
    }
}

static void enter(void)
{
    if (!insert_line(cy + 1, EL(cy) + cx)) return;
    EL(cy)[cx] = 0;
    cy++;
    cx = 0;
    scroll();
    draw_text();
}

static void backspace(void)
{
    if (cx > 0) {
        char *l = EL(cy);
        memmove(l + cx - 1, l + cx, llen(cy) - cx + 1);
        cx--;
        draw_line(cy);
    } else if (cy > 0 && llen(cy - 1) + llen(cy) <= W) {
        cx = llen(cy - 1);
        strcat(EL(cy - 1), EL(cy));
        delete_line(cy);
        cy--;
        scroll();
        draw_text();
    }
}

static void del(void)
{
    if (cx < llen(cy)) {
        char *l = EL(cy);
        memmove(l + cx, l + cx + 1, llen(cy) - cx);
        draw_line(cy);
    } else if (cy + 1 < E->n && llen(cy) + llen(cy + 1) <= W) {
        strcat(EL(cy), EL(cy + 1));
        delete_line(cy + 1);
        draw_text();
    }
}

static BOOL ask(const char *prompt)
{
    LONG k;
    status(prompt);
    k = tgethot("YN\r");
    return k == 'Y';
}

static int finish(int result)
{
    /* trailing empty lines don't belong to the message */
    while (E->n > 0 && !EL(E->n - 1)[0]) E->n--;
    tgotoxy(1, N.rows);
    tcleol();
    tputs("\n");
    return result;
}

int fse_edit(struct Editor *ed, const char *quote, const char *qfrom, const char *to, const char *subj)
{
    E = ed;
    q_text = quote; q_from = qfrom; h_to = to; h_subj = subj;
    W = (N.cols > ED_WIDTH + 1) ? ED_WIDTH : N.cols - 1;
    H = N.rows - TOP;                   /* the last row is the status line */
    if (E->n == 0) { E->n = 1; EL(0)[0] = 0; }
    cx = cy = top = 0;
    draw_all();
    for (;;) {
        LONG k;
        place();
        k = tgetkey(0);
        if (k == KEY_HANGUP) return -1;
        if (k == KEY_NONE || k == 18) { draw_all(); continue; }    /* a node message scribbled on it / ^R */
        if (cx > llen(cy)) cx = llen(cy);
        switch (k) {
        case '\r': enter(); break;
        case 8: backspace(); break;
        case KEY_DEL: del(); break;
        case KEY_LEFT:
            if (cx > 0) cx--;
            else if (cy > 0) { cy--; cx = llen(cy); if (scroll()) draw_text(); }
            break;
        case KEY_RIGHT:
            if (cx < llen(cy)) cx++;
            else if (cy + 1 < E->n) { cy++; cx = 0; if (scroll()) draw_text(); }
            break;
        case KEY_UP:   if (cy > 0) { cy--; if (scroll()) draw_text(); } break;
        case KEY_DOWN: if (cy + 1 < E->n) { cy++; if (scroll()) draw_text(); } break;
        case KEY_HOME: cx = 0; break;
        case KEY_END:  cx = llen(cy); break;
        case KEY_PGUP: cy = cy > H ? cy - H : 0; if (scroll()) draw_text(); break;
        case KEY_PGDN: cy = cy + H < E->n ? cy + H : E->n - 1; if (scroll()) draw_text(); break;
        case 25:                                            /* ^Y */
            delete_line(cy);
            if (cy >= E->n) cy = E->n - 1;
            cx = 0;
            draw_text();
            break;
        case 17:                                            /* ^Q */
            if (q_text) {
                int n = ed_quote(E, cy, q_text, q_from);
                cy += n;
                cx = 0;
                if (cy >= E->n) { insert_line(E->n, ""); cy = E->n - 1; }
                scroll();
                draw_text();
            } else status(L("fse.status.nothing_to_quote", "|08There's nothing to quote.|07"));
            break;
        case 26:                                            /* ^Z */
            return finish(1);
        case 1:                                             /* ^A */
            if (ask(L("fse.ask.abort", "|12Abort this message? |08(y/N)|07 "))) return finish(0);
            status("");
            break;
        case 27: {
            LONG m;
            status(L("fse.status.menu", "|15S|07ave  |15A|07bort  |15Q|07uote  |15C|07ontinue: "));
            m = tgethot("SAQC\r");
            if (m == KEY_HANGUP) return -1;
            if (m == 'S') return finish(1);
            if (m == 'A' && ask(L("fse.ask.abort", "|12Abort this message? |08(y/N)|07 "))) return finish(0);
            if (m == 'Q' && q_text) {
                int n = ed_quote(E, cy, q_text, q_from);
                cy += n;
                cx = 0;
                if (cy >= E->n) { insert_line(E->n, ""); cy = E->n - 1; }
                scroll();
                draw_text();
            }
            status("");
            break;
        }
        default:
            if (k >= 32 && k <= 255) type_char((UBYTE)k);
        }
    }
}
