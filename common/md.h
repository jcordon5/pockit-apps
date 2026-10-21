/*
 * md.h: pinta Markdown sencillo en renglones de altura fija con InkView.
 *
 * Lo comparten el Cuaderno y la IA. Entiende # y ## títulos, **negrita**, *cursiva*,
 * listas (- y 1.), tareas (- [ ] / - [x]), citas (>), bloques de código (```) y
 * separadores (---). El texto se coloca en «renglones»: la fila 0 es la primera del
 * documento y ctx->scroll_row la primera que se ve; ctx->top es la y de esa fila.
 */
#ifndef MD_H
#define MD_H

#include "inkview.h"
#include <string.h>
#include <ctype.h>

typedef struct {
    ifont *body, *bold, *ital, *boldital, *h1, *h2, *mono;
    int left, right;      /* zona de texto en x */
    int top, bottom;      /* zona visible en y */
    int lh, base;         /* altura de renglón y de la línea a la base del texto */
    int scroll_row;
    int soft;             /* color secundario */
} MdCtx;

typedef struct { int num; int in_code; } MdState;

static int md_is_task(const char *s)
{
    return !strncmp(s, "- [ ] ", 6) || !strncmp(s, "- [x] ", 6) || !strncmp(s, "- [X] ", 6);
}

static int md_row_y(const MdCtx *c, int row) { return c->top + (row - c->scroll_row) * c->lh; }
static int md_row_visible(const MdCtx *c, int row) { int y = md_row_y(c, row); return row >= c->scroll_row && y + c->lh <= c->bottom; }

typedef struct { const char *s; int n; int bold, ital; } MdRun;

/* Parte una línea en trozos con su estilo. */
static int md_parse_inline(const char *s, MdRun *runs, int max)
{
    int n = 0, bold = 0, ital = 0;
    const char *start = s;
    while (*s && n < max - 1) {
        int tog = 0;
        if (s[0] == '*' && s[1] == '*') tog = 2;
        else if ((s[0] == '*' || s[0] == '_') && (ital || (s[1] && s[1] != ' ')) &&
                 (s == start || ital || s[-1] == ' ' || s[-1] == '(' )) tog = 1;
        if (tog) {
            if (s > start) runs[n++] = (MdRun){ start, (int)(s - start), bold, ital };
            if (tog == 2) bold = !bold; else ital = !ital;
            s += tog;
            start = s;
        } else s++;
    }
    if (s > start) runs[n++] = (MdRun){ start, (int)(s - start), bold, ital };
    return n;
}

/* Coloca texto con formato desde x0 en la fila row. Con draw=0 solo mide.
 * Devuelve cuántas filas ocupa. */
static int md_flow(const MdCtx *c, const char *text, int x0, int row, int color, int strike, int draw, ifont *forced)
{
    MdRun runs[256];
    int mono = forced && forced == c->mono;
    int nr = mono ? 1 : md_parse_inline(text, runs, 256);
    if (mono) runs[0] = (MdRun){ text, (int)strlen(text), 0, 0 };
    int x = x0, used = 0;
    for (int r = 0; r < nr; r++) {
        ifont *fo = forced ? forced : runs[r].bold && runs[r].ital ? c->boldital : runs[r].bold ? c->bold : runs[r].ital ? c->ital : c->body;
        SetFont(fo, color);
        const char *p = runs[r].s, *end = p + runs[r].n;
        while (p < end) {
            const char *q = p;
            while (q < end && *q != ' ') q++;
            while (q < end && *q == ' ') q++;
            char word[512];
            int len = (int)(q - p) < 511 ? (int)(q - p) : 511;
            memcpy(word, p, len);
            word[len] = 0;
            int w = StringWidth(word);
            if (x + w > c->right && x > x0) {
                used++;
                x = x0;
                char *t = word;
                while (*t == ' ') t++;
                memmove(word, t, strlen(t) + 1);
                w = StringWidth(word);
                if (w > c->right - x0) w = c->right - x0;   /* palabra más larga que el renglón */
            }
            if (draw && md_row_visible(c, row + used)) {
                int ty = md_row_y(c, row + used) + c->lh - c->base - fo->baseline;
                DrawString(x, ty, word);
                if (strike) DrawLine(x, ty + fo->baseline * 6 / 10, x + w, ty + fo->baseline * 6 / 10, color);
            }
            x += w;
            p = q;
        }
    }
    return used + 1;
}

static void md_checkbox(int x, int y, int checked, int soft)
{
    int s = 34;
    for (int i = 0; i < 2; i++) DrawRect(x + i, y + i, s - 2 * i, s - 2 * i, checked ? soft : BLACK);
    if (checked) {
        FillArea(x, y, s, s, soft);
        for (int i = -2; i <= 1; i++) {
            DrawLine(x + 7, y + 17 + i, x + 14, y + 25 + i, WHITE);
            DrawLine(x + 14, y + 25 + i, x + 27, y + 9 + i, WHITE);
        }
    }
}

/* Pinta (o mide) una línea de Markdown en la fila row. Devuelve las filas que ocupa. */
static int md_line(const MdCtx *c, MdState *st, const char *s, int row, int draw)
{
    int vis = draw && md_row_visible(c, row);
    int y = md_row_y(c, row), baseline = y + c->lh - c->base;
    int used;

    if (!strncmp(s, "```", 3)) { st->in_code = !st->in_code; return 0; }
    if (st->in_code) {
        if (vis) FillArea(c->left - 12, y + 4, c->right - c->left + 24, c->lh - 4, 0xEEEEEE);
        return md_flow(c, s, c->left + 8, row, BLACK, 0, draw, c->mono);
    }
    if (!strncmp(s, "# ", 2)) { st->num = 0; return md_flow(c, s + 2, c->left, row, BLACK, 0, draw, c->h1); }
    if (!strncmp(s, "## ", 3) || !strncmp(s, "### ", 4) || !strncmp(s, "#### ", 5)) {
        st->num = 0;
        const char *t = s; while (*t == '#') t++; while (*t == ' ') t++;
        return md_flow(c, t, c->left, row, BLACK, 0, draw, c->h2);
    }
    if (!strcmp(s, "---") || !strcmp(s, "***") || !strcmp(s, "___")) {
        if (vis) DrawLine(c->left + 80, y + c->lh / 2, c->right - 80, y + c->lh / 2, c->soft);
        return 1;
    }
    /* sangría de listas anidadas */
    int indent = 0;
    while (s[indent] == ' ') indent++;
    const char *t = s + indent;
    int ix = c->left + (indent / 2) * 40;

    if (md_is_task(t)) {
        int done = t[3] != ' ';
        if (vis) md_checkbox(ix, baseline - 31, done, c->soft);
        st->num = 0;
        return md_flow(c, t + 6, ix + 56, row, done ? c->soft : BLACK, done, draw, NULL);
    }
    if ((t[0] == '-' || t[0] == '*' || t[0] == '+') && t[1] == ' ') {
        if (vis) FillArea(ix + 12, baseline - 18, 10, 10, BLACK);
        st->num = 0;
        return md_flow(c, t + 2, ix + 44, row, BLACK, 0, draw, NULL);
    }
    if (isdigit((unsigned char)t[0])) {
        const char *d = t;
        while (isdigit((unsigned char)*d)) d++;
        if ((*d == '.' || *d == ')') && d[1] == ' ') {
            char lbl[12];
            snprintf(lbl, sizeof lbl, "%d.", ++st->num);
            if (vis) { SetFont(c->bold, BLACK); DrawString(ix, baseline - c->bold->baseline, lbl); }
            return md_flow(c, d + 2, ix + 50, row, BLACK, 0, draw, NULL);
        }
    }
    if (t[0] == '>') {
        const char *q = t[1] == ' ' ? t + 2 : t + 1;
        used = md_flow(c, q, ix + 34, row, c->soft, 0, draw, c->ital);
        if (draw) {
            int a = y + 10, b = y + used * c->lh - 6;
            if (a < c->top) a = c->top;
            if (b > c->bottom) b = c->bottom;
            if (b > a && row + used > c->scroll_row) FillArea(ix + 4, a, 6, b - a, c->soft);
        }
        st->num = 0;
        return used;
    }
    if (!*t) { st->num = 0; return 1; }
    /* tablas: se enseñan como texto, sin las barras de los bordes */
    st->num = 0;
    return md_flow(c, t, ix, row, BLACK, 0, draw, NULL);
}

#endif
