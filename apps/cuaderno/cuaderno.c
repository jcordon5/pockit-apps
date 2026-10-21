/*
 * Cuaderno: notas en Markdown para PocketBook, al estilo de Notas del iPhone.
 *
 * Cada nota es un archivo .md en Notas/ del lector, así que se pueden leer y editar
 * desde el ordenador (y desde Cliply), y lo que copie la IA se pega tal cual.
 * Se entiende: # títulos, **negrita**, *cursiva*, listas (- y 1.), tareas (- [ ] / - [x])
 * y citas (>). La nota se pinta sobre papel pautado; tocar una línea la edita con el
 * teclado del sistema y tocar la casilla de una tarea la marca.
 */
#include "inkview.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include "../../common/md.h"
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>

/* Rastro para encontrar fallos en el lector: cada paso se apunta y, si la app se cae,
 * queda escrito en qué paso fue. */
#define TRACE_PATH FLASHDIR "/applications/.pruebas/cuaderno.txt"
static const char *last_step = "inicio";
static void trace(const char *step)
{
    last_step = step;
    return;
    FILE *f = fopen(TRACE_PATH, "a");
    if (f) { fprintf(f, "%ld %s\n", (long)time(NULL), step); fclose(f); }
}
static void on_crash(int sig)
{
    int fd = open(TRACE_PATH, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd >= 0) {
        char b[200];
        int n = snprintf(b, sizeof b, "CAIDA señal %d tras: %s\n", sig, last_step);
        write(fd, b, n);
        close(fd);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

#define NOTES_DIR FLASHDIR "/Notas"
#define MAX_NOTES 300
#define MAX_LINES 3000
#define LINE_MAX 2048

/* ---------- aspecto ---------- */
#define PAPER_LINE 0xC8C8C8
#define SOFT 0x777777
#define M 64            /* margen lateral */
#define LH 62           /* altura de renglón */
#define TOP_BAR 130
#define BASE 14         /* de la línea del papel a la base del texto */
#define TOOLBAR 110

static ifont *f_title, *f_head, *f_body, *f_bold, *f_ital, *f_boldital, *f_small, *f_smallb, *f_h1, *f_h2, *f_mono, *f_key, *f_hint;
static int W, H;

/* ---------- datos ---------- */
typedef struct { char path[512]; char title[256]; char preview[256]; time_t mtime; } NoteInfo;
static NoteInfo notes[MAX_NOTES];
static int nnotes;

static char *lines[MAX_LINES];
static int nlines;
static char cur_path[512];
static int line_y0[MAX_LINES], line_y1[MAX_LINES];   /* dónde quedó cada línea en pantalla */
static int scroll_row, total_rows;

enum { LIST, NOTE } screen = LIST;
static int shown_once;

/* Sin destello salvo la primera vez: el usuario no quiere un parpadeo por toque. */
static void refresh(void) { if (!shown_once) { FullUpdate(); shown_once = 1; } else SoftUpdate(); }
static int list_scroll;
static char kbd[LINE_MAX];
static int editing = -1;    /* línea que se está editando; nlines = nueva al final */
static int new_kind;        /* 0 texto, 1 tarea */

static const char *MESES[] = { "ene", "feb", "mar", "abr", "may", "jun", "jul", "ago", "sep", "oct", "nov", "dic" };

/* ---------- utilidades ---------- */
static void trim_nl(char *s) { size_t n = strlen(s); while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0; }

static const char *skip_md_prefix(const char *s)
{
    while (*s == ' ') s++;
    if (*s == '#') { while (*s == '#') s++; while (*s == ' ') s++; }
    else if (!strncmp(s, "- [ ] ", 6) || !strncmp(s, "- [x] ", 6) || !strncmp(s, "- [X] ", 6)) s += 6;
    else if ((*s == '-' || *s == '*' || *s == '>') && s[1] == ' ') s += 2;
    return s;
}

/* Quita ** y * para títulos y vistas previas. */
static void plain(const char *in, char *out, size_t n)
{
    size_t j = 0;
    for (const char *p = skip_md_prefix(in); *p && j + 1 < n; p++)
        if (*p != '*' && *p != '_' && *p != '`') out[j++] = *p;
    out[j] = 0;
}

static void date_str(time_t t, char *out, size_t n)
{
    struct tm tm, now;
    time_t nw = time(NULL);
    localtime_r(&t, &tm);
    localtime_r(&nw, &now);
    if (tm.tm_year == now.tm_year && tm.tm_yday == now.tm_yday)
        snprintf(out, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
    else if (tm.tm_year == now.tm_year)
        snprintf(out, n, "%d %s", tm.tm_mday, MESES[tm.tm_mon]);
    else
        snprintf(out, n, "%d %s %d", tm.tm_mday, MESES[tm.tm_mon], tm.tm_year + 1900);
}

/* ---------- archivos ---------- */
static int by_date(const void *a, const void *b)
{
    time_t x = ((const NoteInfo *)a)->mtime, y = ((const NoteInfo *)b)->mtime;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void scan_notes(void)
{
    mkdir(NOTES_DIR, 0755);
    nnotes = 0;
    DIR *d = opendir(NOTES_DIR);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && nnotes < MAX_NOTES) {
        size_t l = strlen(e->d_name);
        if (e->d_name[0] == '.' || l < 4 || strcmp(e->d_name + l - 3, ".md")) continue;
        NoteInfo *n = &notes[nnotes];
        snprintf(n->path, sizeof n->path, NOTES_DIR "/%s", e->d_name);
        struct stat st;
        if (stat(n->path, &st)) continue;
        n->mtime = st.st_mtime;
        n->title[0] = n->preview[0] = 0;
        FILE *f = fopen(n->path, "r");
        char buf[LINE_MAX];
        while (f && fgets(buf, sizeof buf, f)) {
            trim_nl(buf);
            if (!*skip_md_prefix(buf)) continue;
            if (!n->title[0]) plain(buf, n->title, sizeof n->title);
            else { plain(buf, n->preview, sizeof n->preview); break; }
        }
        if (f) fclose(f);
        if (!n->title[0]) snprintf(n->title, sizeof n->title, "Nota nueva");
        nnotes++;
    }
    closedir(d);
    qsort(notes, nnotes, sizeof *notes, by_date);
}

static void free_lines(void) { for (int i = 0; i < nlines; i++) free(lines[i]); nlines = 0; }

static void load_note(const char *path)
{
    free_lines();
    snprintf(cur_path, sizeof cur_path, "%s", path);
    FILE *f = fopen(path, "r");
    char buf[LINE_MAX];
    while (f && nlines < MAX_LINES && fgets(buf, sizeof buf, f)) {
        trim_nl(buf);
        lines[nlines++] = strdup(buf);
    }
    if (f) fclose(f);
    while (nlines > 0 && !lines[nlines - 1][0]) free(lines[--nlines]);   /* sin líneas vacías al final */
    scroll_row = 0;
}

static void save_note(void)
{
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", cur_path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < nlines; i++) fprintf(f, "%s\n", lines[i]);
    fclose(f);
    rename(tmp, cur_path);
}

/* ---------- pantalla de la nota ---------- */
static MdCtx md;

/* Modo edición: teclado propio abajo y una barra de formato encima, y la nota se
 * escribe en su sitio. La línea con el cursor se enseña tal cual (con sus ** y _) para
 * que se vea lo que se teclea; el resto, ya con formato. */
static int editing_mode, cur_line, cur_off;    /* línea y byte del cursor */
#define KB_ROWS 4
#define KBD_KEY_H 100
#define FMT_H 96
static int kb_top(void) { return H - KB_ROWS * KBD_KEY_H; }
static int fmt_top(void) { return kb_top() - FMT_H; }

static int body_top(void) { return TOP_BAR + 10; }
static int doc_bottom(void) { return editing_mode ? fmt_top() : H - TOOLBAR; }
static int rows_on_page(void) { return (doc_bottom() - body_top()) / LH; }

static void setup_md(void)
{
    md = (MdCtx){ f_body, f_bold, f_ital, f_boldital, f_h1, f_h2, f_mono, M, W - M, body_top(), doc_bottom(), LH, BASE, scroll_row, SOFT };
}

/* Largo del marcador de bloque al principio de la línea («- [ ] », «- », «1. », «# », «> »). */
static int prefix_len(const char *s)
{
    int i = 0;
    while (s[i] == ' ') i++;
    const char *t = s + i;
    if (md_is_task(t)) return i + 6;
    if ((t[0] == '-' || t[0] == '*' || t[0] == '+') && t[1] == ' ') return i + 2;
    if (t[0] == '>' && t[1] == ' ') return i + 2;
    if (t[0] == '#') { int k = 0; while (t[k] == '#') k++; if (t[k] == ' ') return i + k + 1; }
    int d = 0;
    while (t[d] >= '0' && t[d] <= '9') d++;
    if (d && (t[d] == '.' || t[d] == ')') && t[d + 1] == ' ') return i + d + 2;
    return 0;
}

/* Palabras de la línea que se edita, ya colocadas, para pintar el cursor y saber dónde se toca. */
typedef struct { int off, len, row, x; } Word;
static Word ew[1024];
static int new_count, ed_rows, ed_x0;
static ifont *ed_font;

static void edit_layout(const char *body, int boff, int x0, ifont *fo)
{
    new_count = 0;
    ed_rows = 1;
    ed_x0 = x0;
    ed_font = fo;
    SetFont(fo, BLACK);
    int x = x0, row = 0, right = W - M;
    const char *p = body;
    while (*p && new_count < 1024) {
        const char *q = p;
        while (*q && *q != ' ') q++;
        while (*q == ' ') q++;
        char word[512];
        int len = (int)(q - p) < 511 ? (int)(q - p) : 511;
        memcpy(word, p, len);
        word[len] = 0;
        int w = StringWidth(word);
        if (x + w > right && x > x0) { row++; x = x0; }
        ew[new_count++] = (Word){ boff + (int)(p - body), len, row, x };
        x += w;
        p = q;
    }
    ed_rows = row + 1;
}

/* x y fila (dentro de la línea) del cursor. */
static void cursor_pos(const char *line, int *cx, int *crow)
{
    *cx = ed_x0; *crow = 0;
    for (int i = 0; i < new_count; i++) {
        Word *w = &ew[i];
        if (cur_off < w->off) break;
        if (cur_off <= w->off + w->len) {
            char part[512];
            int n = cur_off - w->off;
            memcpy(part, line + w->off, n);
            part[n] = 0;
            SetFont(ed_font, BLACK);
            *cx = w->x + StringWidth(part);
            *crow = w->row;
            if (n == w->len && i + 1 < new_count && ew[i + 1].off == cur_off && ew[i + 1].row != w->row) { *cx = ew[i + 1].x; *crow = ew[i + 1].row; }
            return;
        }
    }
    if (new_count) { Word *w = &ew[new_count - 1]; SetFont(ed_font, BLACK); char part[512]; memcpy(part, line + w->off, w->len); part[w->len] = 0; *cx = w->x + StringWidth(part); *crow = w->row; }
}

/* Byte más cercano a un toque en la fila r (dentro de la línea) y la x dada. */
static int hit_offset(const char *line, int r, int x)
{
    int best = -1, bestd = 1 << 30;
    SetFont(ed_font, BLACK);
    for (int i = 0; i < new_count; i++) {
        Word *w = &ew[i];
        if (w->row != r) continue;
        for (int k = 0; k <= w->len; k++) {
            if (k < w->len && ((unsigned char)line[w->off + k] & 0xC0) == 0x80) continue;   /* mitad de una letra */
            char part[512];
            memcpy(part, line + w->off, k);
            part[k] = 0;
            int d = abs(w->x + StringWidth(part) - x);
            if (d < bestd) { bestd = d; best = w->off + k; }
        }
    }
    return best;
}

/* Pinta la línea que se edita en la fila row; devuelve las filas que ocupa. */
static int draw_edit_line(int i, int row, MdState *st, int draw)
{
    const char *s = lines[i];
    int pl = prefix_len(s), ind = 0;
    while (s[ind] == ' ') ind++;
    const char *t = s + ind;
    int ix = M + (ind / 2) * 40, x0 = ix;
    ifont *fo = f_body;
    int y = md_row_y(&md, row), baseline = y + LH - BASE, vis = draw && md_row_visible(&md, row);
    if (pl && md_is_task(t)) { if (vis) md_checkbox(ix, baseline - 31, t[3] != ' ', SOFT); x0 = ix + 56; st->num = 0; }
    else if (pl && (t[0] == '-' || t[0] == '*' || t[0] == '+')) { if (vis) FillArea(ix + 12, baseline - 18, 10, 10, BLACK); x0 = ix + 44; st->num = 0; }
    else if (pl && t[0] == '>') { fo = f_ital; x0 = ix + 34; st->num = 0; }
    else if (pl && t[0] == '#') { fo = t[1] == '#' ? f_h2 : f_h1; x0 = M; st->num = 0; }
    else if (pl && t[0] >= '0' && t[0] <= '9') {
        char lbl[12];
        snprintf(lbl, sizeof lbl, "%d.", ++st->num);
        if (vis) { SetFont(f_bold, BLACK); DrawString(ix, baseline - f_bold->baseline, lbl); }
        x0 = ix + 50;
    } else st->num = 0;
    edit_layout(s + pl, pl, x0, fo);
    if (draw) {
        SetFont(fo, BLACK);
        for (int k = 0; k < new_count; k++) {
            Word *w = &ew[k];
            if (!md_row_visible(&md, row + w->row)) continue;
            char word[512];
            memcpy(word, s + w->off, w->len);
            word[w->len] = 0;
            DrawString(w->x, md_row_y(&md, row + w->row) + LH - BASE - fo->baseline, word);
        }
        int cx, cr;
        cursor_pos(s, &cx, &cr);
        if (md_row_visible(&md, row + cr)) {
            int cy = md_row_y(&md, row + cr) + LH - BASE - fo->baseline - 4;
            FillArea(cx, cy, 4, fo->baseline + 12, BLACK);
        }
    }
    return ed_rows;
}

static int cursor_abs_row;

/* Pinta (o mide, con draw=0) todas las líneas; devuelve el total de renglones. */
static int layout_note(int draw)
{
    setup_md();
    MdState st = { 0, 0 };
    int row = 0;
    for (int i = 0; i < nlines; i++) {
        line_y0[i] = md_row_y(&md, row);
        if (editing_mode && i == cur_line) {
            int r0 = row;
            row += draw_edit_line(i, row, &st, draw);
            int cx, cr;
            cursor_pos(lines[i], &cx, &cr);
            cursor_abs_row = r0 + cr;
        } else row += md_line(&md, &st, lines[i], row, draw);
        line_y1[i] = md_row_y(&md, row);
    }
    return row;
}

static void draw_top_bar(const char *left, const char *center, const char *right)
{
    SetFont(f_head, BLACK);
    if (left) DrawString(M - 20, 42, left);
    if (center) { SetFont(f_small, SOFT); DrawTextRect(0, 52, W, 40, center, ALIGN_CENTER); }
    if (right) { SetFont(f_head, BLACK); DrawString(W - M + 20 - StringWidth(right), 42, right); }
    DrawLine(0, TOP_BAR, W, TOP_BAR, PAPER_LINE);
}

static void draw_toolbar(const char **labels, int n)
{
    int y = H - TOOLBAR;
    FillArea(0, y, W, TOOLBAR, WHITE);
    DrawLine(0, y, W, y, BLACK);
    SetFont(f_head, BLACK);
    for (int i = 0; i < n; i++)
        DrawTextRect(i * W / n, y, W / n, TOOLBAR, labels[i], ALIGN_CENTER | VALIGN_MIDDLE);
}

/* ---------- teclado propio ---------- */
enum { K_CHAR, K_SHIFT, K_BACK, K_ENTER, K_SPACE, K_SYM, K_ABC, K_ACUTE };
typedef struct { const char *label; int kind; float w; } Key;
static int kb_layer, kb_shift, kb_acute, kb_pressed_r = -1, kb_pressed_c = -1, long_used;

static const char *ROWS_ABC[3] = { "qwertyuiop", "asdfghjklñ", "zxcvbnm" };
static const char *ROWS_SYM[3][10] = {
    { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" },
    { "-", "/", ":", ";", "(", ")", "€", "&", "@", "\"" },
    { "?", "!", "'", "¿", "¡", "*", "#", NULL },
};

/* Teclas de una fila (n devuelto) con sus anchos en unidades de tecla. */
static int kb_row(int r, Key *out)
{
    static char letters[3][12][8];
    int n = 0;
    if (r == 3) {
        out[n++] = (Key){ kb_layer ? "ABC" : "123", kb_layer ? K_ABC : K_SYM, 1.5f };
        out[n++] = (Key){ ",", K_CHAR, 1 };
        out[n++] = (Key){ "espacio", K_SPACE, 5 };
        out[n++] = (Key){ ".", K_CHAR, 1 };
        out[n++] = (Key){ "intro", K_ENTER, 1.5f };
        return n;
    }
    if (r == 2) out[n++] = kb_layer ? (Key){ "´", K_ACUTE, 1.5f } : (Key){ "mayús", K_SHIFT, 1.5f };
    if (kb_layer) {
        for (int c = 0; c < 10 && ROWS_SYM[r][c]; c++) out[n++] = (Key){ ROWS_SYM[r][c], K_CHAR, 1 };
    } else {
        const char *p = ROWS_ABC[r];
        int c = 0;
        while (*p) {
            int l = ((unsigned char)*p >= 0xC0) ? 2 : 1;
            char *d = letters[r][c];
            memcpy(d, p, l);
            d[l] = 0;
            if (kb_shift) { if (l == 1) d[0] = d[0] - 32; else if (!strcmp(d, "ñ")) strcpy(d, "Ñ"); }
            out[n++] = (Key){ d, K_CHAR, 1 };
            p += l;
            c++;
        }
    }
    if (r == 2) out[n++] = (Key){ "borrar", K_BACK, 1.5f };
    return n;
}

static void key_rect(int r, int c, int *x, int *y, int *w, int *h)
{
    Key keys[12];
    int n = kb_row(r, keys);
    float units = 0;
    for (int i = 0; i < n; i++) units += keys[i].w;
    float uw = (float)(W - 20) / (units < 10 ? 10 : units);
    float startx = 10 + ((W - 20) - uw * units) / 2, xx = startx;
    for (int i = 0; i < c; i++) xx += keys[i].w * uw;
    *x = (int)xx + 4; *w = (int)(keys[c].w * uw) - 8;
    *y = kb_top() + r * KBD_KEY_H + 6; *h = KBD_KEY_H - 12;
}

static void draw_key(int r, int c, int pressed)
{
    Key keys[12];
    kb_row(r, keys);
    int x, y, w, h;
    key_rect(r, c, &x, &y, &w, &h);
    Key *k = &keys[c];
    int special = k->kind != K_CHAR;
    int on = pressed || (k->kind == K_SHIFT && kb_shift) || (k->kind == K_ACUTE && kb_acute);
    FillArea(x, y, w, h, on ? BLACK : special ? 0xE4E4E4 : WHITE);
    DrawRect(x, y, w, h, on ? BLACK : 0x999999);
    SetFont(special ? f_small : f_key, on ? WHITE : BLACK);
    DrawTextRect(x, y, w, h, k->label, ALIGN_CENTER | VALIGN_MIDDLE);
    /* pista de tilde en las vocales: mantener pulsado */
    if (!kb_layer && k->kind == K_CHAR && strchr("aeiouAEIOU", k->label[0]) && k->label[1] == 0) {
        SetFont(f_hint, on ? WHITE : SOFT);
        DrawString(x + w - 22, y + 4, "´");
    }
}

static void draw_keyboard(void)
{
    FillArea(0, kb_top(), W, KB_ROWS * KBD_KEY_H, 0xF2F2F2);
    DrawLine(0, kb_top(), W, kb_top(), 0x999999);
    Key keys[12];
    for (int r = 0; r < KB_ROWS; r++) {
        int n = kb_row(r, keys);
        for (int c = 0; c < n; c++) draw_key(r, c, r == kb_pressed_r && c == kb_pressed_c);
    }
}

static const char *FMT[] = { "☐", "•", "1.", "Título", "Negrita", "Quitar", "Listo" };
#define NFMT 7

static void draw_format_bar(void)
{
    int y = fmt_top();
    FillArea(0, y, W, FMT_H, WHITE);
    DrawLine(0, y, W, y, 0x999999);
    int bw = W / NFMT;
    for (int i = 0; i < NFMT; i++) {
        SetFont(i == NFMT - 1 ? f_smallb : (i < 3 ? f_head : f_small), BLACK);
        DrawTextRect(i * bw, y, bw, FMT_H, FMT[i], ALIGN_CENTER | VALIGN_MIDDLE);
    }
}

static const char *NOTE_TOOLS[] = { "✎ Escribir", "☐ Tarea", "• Lista", "Borrar" };

static void draw_doc(void)
{
    FillArea(0, body_top(), W, doc_bottom() - body_top(), WHITE);
    for (int y = body_top() + LH; y < doc_bottom(); y += LH) DrawLine(M - 20, y, W - M + 20, y, PAPER_LINE);
    total_rows = layout_note(1);
}

static void draw_note(void)
{
    trace("pinto nota");
    ClearScreen();
    struct stat st;
    char ds[40] = "";
    if (!stat(cur_path, &st)) date_str(st.st_mtime, ds, sizeof ds);
    total_rows = layout_note(0);
    int pages = (total_rows + rows_on_page() - 1) / rows_on_page();
    if (pages > 1 && !editing_mode) {
        char pg[64];
        snprintf(pg, sizeof pg, "%s · pág. %d de %d", ds, scroll_row / rows_on_page() + 1, pages);
        snprintf(ds, sizeof ds, "%s", pg);
    }
    draw_top_bar("‹ Notas", ds, editing_mode ? "Listo" : NULL);
    draw_doc();
    if (editing_mode) { draw_format_bar(); draw_keyboard(); }
    else draw_toolbar(NOTE_TOOLS, 4);
    refresh();
    trace("nota pintada");
}

/* Tras cada tecla: repintar el documento (en memoria) y refrescar solo lo que cambió. */
static int last_line_rows = -1, last_total = -1, last_line = -1;
static void edit_refresh(void)
{
    int rp = rows_on_page();
    total_rows = layout_note(0);
    int scrolled = 0;
    if (cursor_abs_row < scroll_row) { scroll_row = cursor_abs_row; scrolled = 1; }
    else if (cursor_abs_row >= scroll_row + rp) { scroll_row = cursor_abs_row - rp + 2; if (scroll_row < 0) scroll_row = 0; scrolled = 1; }
    draw_doc();
    int lr = (line_y1[cur_line] - line_y0[cur_line]) / LH;
    int y0, y1 = doc_bottom();
    if (!scrolled && lr == last_line_rows && total_rows == last_total && cur_line == last_line) {
        y0 = line_y0[cur_line]; y1 = line_y1[cur_line];   /* solo la línea */
    } else y0 = last_line >= 0 && last_line < cur_line && !scrolled ? line_y0[last_line] : body_top();
    if (y0 < body_top()) y0 = body_top();
    if (y1 > doc_bottom()) y1 = doc_bottom();
    if (y1 > y0) DynamicUpdate(0, y0, W, y1 - y0);
    last_line_rows = lr; last_total = total_rows; last_line = cur_line;
}

static int edits_since_save;
static void touched(void) { if (++edits_since_save >= 25) { save_note(); edits_since_save = 0; } }

static void set_line(int i, const char *s) { free(lines[i]); lines[i] = strdup(s); }

static void insert_line(int at, const char *s)
{
    if (nlines >= MAX_LINES) return;
    memmove(&lines[at + 1], &lines[at], (nlines - at) * sizeof *lines);
    lines[at] = strdup(s);
    nlines++;
}

static void delete_line(int at)
{
    free(lines[at]);
    memmove(&lines[at], &lines[at + 1], (nlines - at - 1) * sizeof *lines);
    nlines--;
}

/* Mayúscula sola al empezar línea o frase. */
static void auto_shift(void)
{
    const char *s = lines[cur_line];
    int pl = prefix_len(s), k = cur_off;
    while (k > pl && s[k - 1] == ' ') k--;
    kb_shift = k == pl || s[k - 1] == '.' || s[k - 1] == '?' || s[k - 1] == '!';
}

static void insert_text(const char *t)
{
    const char *s = lines[cur_line];
    size_t l = strlen(s), tl = strlen(t);
    char *n = malloc(l + tl + 1);
    memcpy(n, s, cur_off);
    memcpy(n + cur_off, t, tl);
    memcpy(n + cur_off + tl, s + cur_off, l - cur_off + 1);
    free(lines[cur_line]);
    lines[cur_line] = n;
    cur_off += tl;
    touched();
}

static void key_backspace(void)
{
    char *s = lines[cur_line];
    int pl = prefix_len(s);
    if (cur_off > pl) {
        int k = cur_off - 1;
        while (k > pl && ((unsigned char)s[k] & 0xC0) == 0x80) k--;
        memmove(s + k, s + cur_off, strlen(s + cur_off) + 1);
        cur_off = k;
    } else if (pl) {   /* al principio de una tarea o lista: deja de serlo */
        memmove(s, s + pl, strlen(s + pl) + 1);
        cur_off = 0;
    } else if (cur_line > 0) {   /* une con la línea de arriba */
        const char *prev = lines[cur_line - 1];
        size_t a = strlen(prev), b = strlen(s);
        char *n = malloc(a + b + 1);
        memcpy(n, prev, a);
        memcpy(n + a, s, b + 1);
        delete_line(cur_line);
        cur_line--;
        free(lines[cur_line]);
        lines[cur_line] = n;
        cur_off = a;
    }
    touched();
}

static void key_enter(void)
{
    char *s = lines[cur_line];
    int pl = prefix_len(s);
    if (pl && !s[pl] && cur_off == pl) {   /* intro en un punto vacío: se acaba la lista */
        s[0] = 0;
        cur_off = 0;
        return;
    }
    char pre[32] = "";
    int ind = 0;
    while (s[ind] == ' ') ind++;
    const char *t = s + ind;
    if (pl) {
        if (md_is_task(t)) snprintf(pre, sizeof pre, "%.*s- [ ] ", ind, s);
        else if (t[0] >= '0' && t[0] <= '9') snprintf(pre, sizeof pre, "%.*s%d. ", ind, s, atoi(t) + 1);
        else if (t[0] != '#') snprintf(pre, sizeof pre, "%.*s", pl, s);
    }
    char *rest = malloc(strlen(pre) + strlen(s + cur_off) + 1);
    strcpy(rest, pre);
    strcat(rest, s + cur_off);
    s[cur_off] = 0;
    insert_line(cur_line + 1, rest);
    free(rest);
    cur_line++;
    cur_off = strlen(pre);
    touched();
}

/* Cambia el marcador de bloque de la línea actual (o lo quita si ya era ese). */
static void set_prefix(const char *want)
{
    char *s = lines[cur_line];
    int pl = prefix_len(s);
    int same = pl == (int)strlen(want) && !strncmp(s, want, pl);
    if (!strcmp(want, "- [ ] ") && pl == 6 && md_is_task(s)) same = 1;
    const char *nw = same ? "" : want;
    char *n = malloc(strlen(nw) + strlen(s + pl) + 1);
    strcpy(n, nw);
    strcat(n, s + pl);
    cur_off = strlen(nw) + (cur_off - pl);
    free(lines[cur_line]);
    lines[cur_line] = n;
    touched();
}

static void start_editing(int line, int off)
{
    if (nlines == 0) { lines[nlines++] = strdup(""); line = 0; off = 0; }
    editing_mode = 1;
    cur_line = line;
    cur_off = off;
    kb_layer = 0;
    auto_shift();
    last_line = -1;
    total_rows = layout_note(0);
    int rp = rows_on_page();
    if (cursor_abs_row < scroll_row || cursor_abs_row >= scroll_row + rp) scroll_row = cursor_abs_row > rp / 2 ? cursor_abs_row - rp / 2 : 0;
    draw_note();
}

static void stop_editing(void)
{
    editing_mode = 0;
    while (nlines > 1 && !lines[nlines - 1][0]) delete_line(nlines - 1);
    save_note();
    edits_since_save = 0;
    total_rows = layout_note(0);
    int rp = rows_on_page();
    scroll_row = scroll_row / rp * rp;
    if (scroll_row >= total_rows) scroll_row = 0;
    draw_note();
}

static void format_action(int b)
{
    switch (b) {
    case 0: set_prefix("- [ ] "); break;
    case 1: set_prefix("- "); break;
    case 2: set_prefix("1. "); break;
    case 3: {
        const char *s = lines[cur_line];
        set_prefix(!strncmp(s, "# ", 2) ? "## " : !strncmp(s, "## ", 3) ? "## " : "# ");
        break;
    }
    case 4: insert_text("****"); cur_off -= 2; break;
    case 5:
        if (nlines > 1) { delete_line(cur_line); if (cur_line >= nlines) cur_line = nlines - 1; cur_off = strlen(lines[cur_line]); }
        else { set_line(0, ""); cur_off = 0; }
        last_line = -1;
        break;
    case 6: stop_editing(); return;
    }
    auto_shift();
    draw_keyboard();
    edit_refresh();
    DynamicUpdate(0, kb_top(), W, KB_ROWS * KBD_KEY_H);
}

static int key_at(int x, int y, int *r, int *c)
{
    if (y < kb_top()) return 0;
    *r = (y - kb_top()) / KBD_KEY_H;
    if (*r >= KB_ROWS) *r = KB_ROWS - 1;
    Key keys[12];
    int n = kb_row(*r, keys);
    for (int i = 0; i < n; i++) {
        int kx, ky, kw, kh;
        key_rect(*r, i, &kx, &ky, &kw, &kh);
        if (x >= kx - 4 && x < kx + kw + 4) { *c = i; return 1; }
    }
    return 0;
}

static const char *accented(const char *k)
{
    static const char *from[] = { "a", "e", "i", "o", "u", "A", "E", "I", "O", "U", "n", "N", NULL };
    static const char *to[] = { "á", "é", "í", "ó", "ú", "Á", "É", "Í", "Ó", "Ú", "ñ", "Ñ" };
    for (int i = 0; from[i]; i++) if (!strcmp(k, from[i])) return to[i];
    return NULL;
}

static void press_key(int r, int c, int long_press)
{
    Key keys[12];
    kb_row(r, keys);
    Key *k = &keys[c];
    int redraw_kb = 0;
    switch (k->kind) {
    case K_CHAR: {
        const char *t = k->label;
        if ((long_press || kb_acute) && accented(t)) t = accented(t);
        insert_text(t);
        redraw_kb = kb_shift || kb_acute;
        kb_acute = 0;
        if (!kb_layer) { int was = kb_shift; auto_shift(); redraw_kb |= was != kb_shift; }
        break;
    }
    case K_SPACE: insert_text(" "); { int was = kb_shift; auto_shift(); redraw_kb = was != kb_shift; } break;
    case K_BACK: key_backspace(); { int was = kb_shift; auto_shift(); redraw_kb = was != kb_shift; } break;
    case K_ENTER: key_enter(); auto_shift(); redraw_kb = 1; break;
    case K_SHIFT: kb_shift = !kb_shift; redraw_kb = 1; break;
    case K_SYM: kb_layer = 1; redraw_kb = 1; break;
    case K_ABC: kb_layer = 0; auto_shift(); redraw_kb = 1; break;
    case K_ACUTE: kb_acute = !kb_acute; kb_layer = 0; redraw_kb = 1; break;
    }
    if (k->kind != K_SHIFT && k->kind != K_SYM && k->kind != K_ABC && k->kind != K_ACUTE) edit_refresh();
    if (redraw_kb) { draw_keyboard(); DynamicUpdate(0, kb_top(), W, KB_ROWS * KBD_KEY_H); }
    else { draw_key(r, c, 0); int x, y, w, h; key_rect(r, c, &x, &y, &w, &h); DynamicUpdateBW(x - 2, y - 2, w + 4, h + 4); }
}

/* Toque en el documento mientras se edita: mueve el cursor (o marca una tarea). */
static void edit_doc_tap(int x, int y)
{
    for (int i = 0; i < nlines; i++) {
        if (y < line_y0[i] || y >= line_y1[i]) continue;
        if (md_is_task(lines[i] + prefix_len(lines[i]) - (prefix_len(lines[i]) >= 6 ? 6 : 0)) && x < M + 70 && md_is_task(lines[i])) {
            lines[i][3] = lines[i][3] == ' ' ? 'x' : ' ';
            touched();
            edit_refresh();
            return;
        }
        cur_line = i;
        setup_md();
        MdState st = { 0, 0 };
        int row = (line_y0[i] - body_top()) / LH + scroll_row;
        cur_off = strlen(lines[i]);
        draw_edit_line(i, row, &st, 0);
        int off = hit_offset(lines[i], (y - line_y0[i]) / LH, x);
        if (off >= 0) cur_off = off;
        if (cur_off < prefix_len(lines[i])) cur_off = prefix_len(lines[i]);
        last_line = -1;
        auto_shift();
        edit_refresh();
        draw_keyboard();
        DynamicUpdate(0, kb_top(), W, KB_ROWS * KBD_KEY_H);
        return;
    }
    /* por debajo del texto: al final de la última línea */
    cur_line = nlines - 1;
    cur_off = strlen(lines[cur_line]);
    last_line = -1;
    edit_refresh();
}

/* ---------- lista de notas ---------- */
#define ROW_H 150
#define LIST_TOP 230

static int list_rows(void) { return (H - LIST_TOP - 40) / ROW_H; }

static void draw_list(void)
{
    trace("pinto lista");
    ClearScreen();
    SetFont(f_title, BLACK);
    DrawString(M, 60, "Notas");
    SetFont(f_title, BLACK);
    DrawTextRect(W - M - 120, 40, 120, 100, "+", ALIGN_RIGHT | VALIGN_MIDDLE);
    char cnt[40];
    snprintf(cnt, sizeof cnt, nnotes == 1 ? "1 nota" : "%d notas", nnotes);
    SetFont(f_small, SOFT);
    DrawString(M, 150, cnt);
    DrawLine(M, LIST_TOP - 20, W - M, LIST_TOP - 20, PAPER_LINE);
    if (!nnotes) {
        SetFont(f_body, SOFT);
        DrawTextRect(M, LIST_TOP + 200, W - 2 * M, 200, "Toca + para crear tu primera nota", ALIGN_CENTER);
    }
    for (int k = 0; k < list_rows() && list_scroll + k < nnotes; k++) {
        NoteInfo *n = &notes[list_scroll + k];
        int y = LIST_TOP + k * ROW_H;
        char ds[40];
        date_str(n->mtime, ds, sizeof ds);
        SetFont(f_bold, BLACK);
        DrawTextRect(M, y + 10, W - 2 * M, 50, n->title, ALIGN_LEFT | DOTS);
        SetFont(f_small, SOFT);
        int dw = StringWidth(ds);
        DrawString(M, y + 72, ds);
        DrawTextRect(M + dw + 24, y + 72, W - 2 * M - dw - 24, 44, n->preview[0] ? n->preview : "Sin más texto", ALIGN_LEFT | DOTS);
        DrawLine(M, y + ROW_H - 10, W - M, y + ROW_H - 10, PAPER_LINE);
    }
    if (nnotes > list_rows()) {
        char pg[32];
        snprintf(pg, sizeof pg, "%d / %d", list_scroll / list_rows() + 1, (nnotes + list_rows() - 1) / list_rows());
        SetFont(f_small, SOFT);
        DrawTextRect(0, H - 60, W, 40, pg, ALIGN_CENTER);
    }
    refresh();
}

/* ---------- acciones ---------- */
static void open_note(const char *path)
{
    load_note(path);
    screen = NOTE;
    editing_mode = 0;
    draw_note();
}

static void back_to_list(void)
{
    if (editing_mode) { editing_mode = 0; while (nlines > 1 && !lines[nlines - 1][0]) delete_line(nlines - 1); save_note(); }
    screen = LIST;
    scan_notes();
    draw_list();
}

static void new_note(void)
{
    char path[512];
    snprintf(path, sizeof path, NOTES_DIR "/%ld.md", (long)time(NULL));
    FILE *f = fopen(path, "w");
    if (f) { fputs("# \n", f); fclose(f); }
    load_note(path);
    if (!nlines) lines[nlines++] = strdup("# ");
    screen = NOTE;
    start_editing(0, 2);
}

static void confirm_delete(int button)
{
    if (button == 1) { remove(cur_path); editing_mode = 0; back_to_list(); }
    else draw_note();
}

static void note_tap(int x, int y)
{
    if (y < TOP_BAR) {
        if (x < W / 3) back_to_list();
        else if (editing_mode && x > W - M - 200) stop_editing();
        return;
    }
    if (editing_mode) {
        int r, c;
        if (y >= fmt_top() && y < kb_top()) { format_action(x * NFMT / W); return; }
        if (key_at(x, y, &r, &c)) {
            if (long_used) { long_used = 0; return; }
            kb_pressed_r = kb_pressed_c = -1;
            press_key(r, c, 0);
            return;
        }
        edit_doc_tap(x, y);
        return;
    }
    if (y > H - TOOLBAR) {
        int b = x * 4 / W;
        if (b == 3) { Dialog(ICON_QUESTION, "Borrar nota", "¿Borrar esta nota? No se puede deshacer.", "Borrar", "Cancelar", confirm_delete); return; }
        /* escribir al final; tarea o punto nuevos al final */
        int last = nlines - 1;
        if (b == 0) { if (nlines == 0 || lines[last][0]) insert_line(nlines, ""); start_editing(nlines - 1, strlen(lines[nlines - 1])); return; }
        const char *pre = b == 1 ? "- [ ] " : "- ";
        if (nlines && !lines[last][0]) set_line(last, pre); else insert_line(nlines, pre);
        start_editing(nlines - 1, strlen(pre));
        return;
    }
    for (int i = 0; i < nlines; i++) {
        if (y < line_y0[i] || y >= line_y1[i] || line_y1[i] <= body_top()) continue;
        if (md_is_task(lines[i]) && x < M + 70) {
            lines[i][3] = lines[i][3] == ' ' ? 'x' : ' ';
            save_note();
            int y0 = line_y0[i] < body_top() ? body_top() : line_y0[i], y1 = line_y1[i] > doc_bottom() ? doc_bottom() : line_y1[i];
            FillArea(0, y0, W, y1 - y0, WHITE);
            for (int yy = body_top() + LH; yy < doc_bottom(); yy += LH) if (yy > y0 && yy <= y1) DrawLine(M - 20, yy, W - M + 20, yy, PAPER_LINE);
            setup_md();
            MdState st = { 0, 0 };
            md_line(&md, &st, lines[i], (line_y0[i] - body_top()) / LH + scroll_row, 1);
            PartialUpdate(0, y0, W, y1 - y0);
            return;
        }
        /* tocar una línea: a escribir ahí mismo, con el cursor donde se tocó */
        editing_mode = 1;
        cur_line = i;
        cur_off = strlen(lines[i]);
        setup_md();
        MdState st = { 0, 0 };
        draw_edit_line(i, (line_y0[i] - body_top()) / LH + scroll_row, &st, 0);
        int off = hit_offset(lines[i], (y - line_y0[i]) / LH, x);
        start_editing(i, off >= prefix_len(lines[i]) ? off : (int)strlen(lines[i]));
        return;
    }
    if (nlines == 0 || lines[nlines - 1][0]) insert_line(nlines, "");
    start_editing(nlines - 1, 0);
}

static void list_tap(int x, int y)
{
    if (y < LIST_TOP - 20) {
        if (x > W - M - 160) new_note();
        return;
    }
    int k = (y - LIST_TOP) / ROW_H;
    if (k >= 0 && list_scroll + k < nnotes) open_note(notes[list_scroll + k].path);
}

static int handler(int type, int par1, int par2)
{
    switch (type) {
    case EVT_INIT:
        trace("EVT_INIT");
        SetPanelType(PANEL_DISABLED);   /* sin barra de estado: si no, el primer pintado sale desplazado */
        W = ScreenWidth(); H = ScreenHeight();
        f_title = OpenFont(DEFAULTFONTB, 64, 1);
        f_head = OpenFont(DEFAULTFONT, 38, 1);
        f_h1 = OpenFont(DEFAULTFONTB, 50, 1);
        f_h2 = OpenFont(DEFAULTFONTB, 40, 1);
        f_body = OpenFont(DEFAULTFONT, 36, 1);
        f_bold = OpenFont(DEFAULTFONTB, 36, 1);
        f_ital = OpenFont(DEFAULTFONTI, 36, 1);
        f_boldital = OpenFont(DEFAULTFONTBI, 36, 1);
        f_small = OpenFont(DEFAULTFONT, 30, 1);
        f_smallb = OpenFont(DEFAULTFONTB, 30, 1);
        f_mono = OpenFont(DEFAULTFONTM, 32, 1);
        f_key = OpenFont(DEFAULTFONT, 40, 1);
        f_hint = OpenFont(DEFAULTFONT, 24, 1);
        scan_notes();
        break;
    case EVT_SHOW:
        if (screen == NOTE) draw_note(); else draw_list();
        break;
    case EVT_POINTERDOWN:
        if (screen == NOTE && editing_mode) {
            int r, c;
            if (key_at(par1, par2, &r, &c)) {
                kb_pressed_r = r; kb_pressed_c = c;
                draw_key(r, c, 1);
                int x, y, w, h;
                key_rect(r, c, &x, &y, &w, &h);
                DynamicUpdateBW(x - 2, y - 2, w + 4, h + 4);
            }
        }
        return 1;
    case EVT_POINTERLONG:
        if (screen == NOTE && editing_mode && kb_pressed_r >= 0) {
            int r = kb_pressed_r, c = kb_pressed_c;
            kb_pressed_r = kb_pressed_c = -1;
            Key keys[12];
            kb_row(r, keys);
            if (keys[c].kind == K_CHAR && accented(keys[c].label)) { long_used = 1; press_key(r, c, 1); }
            else if (keys[c].kind == K_BACK) { long_used = 1; for (int i = 0; i < 6; i++) key_backspace(); edit_refresh(); }
        }
        return 1;
    case EVT_POINTERUP:
        if (screen == NOTE) note_tap(par1, par2); else list_tap(par1, par2);
        return 1;
    case EVT_KEYPRESS:
        if (par1 == IV_KEY_BACK || par1 == IV_KEY_HOME || par1 == IV_KEY_MENU) {
            if (screen == NOTE && editing_mode) stop_editing();
            else if (screen == NOTE) back_to_list(); else CloseApp();
            return 1;
        }
        if (par1 == IV_KEY_NEXT || par1 == IV_KEY_NEXT2 || par1 == IV_KEY_DOWN || par1 == IV_KEY_RIGHT) {
            if (screen == NOTE && !editing_mode && scroll_row + rows_on_page() < total_rows) { scroll_row += rows_on_page(); draw_note(); }
            else if (screen == LIST && list_scroll + list_rows() < nnotes) { list_scroll += list_rows(); draw_list(); }
            return 1;
        }
        if (par1 == IV_KEY_PREV || par1 == IV_KEY_PREV2 || par1 == IV_KEY_UP || par1 == IV_KEY_LEFT) {
            if (screen == NOTE && !editing_mode && scroll_row > 0) { scroll_row -= rows_on_page(); if (scroll_row < 0) scroll_row = 0; draw_note(); }
            else if (screen == LIST && list_scroll > 0) { list_scroll -= list_rows(); if (list_scroll < 0) list_scroll = 0; draw_list(); }
            return 1;
        }
        break;
    case EVT_EXIT:
        if (screen == NOTE && editing_mode) save_note();
        free_lines();
        break;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    mkdir(FLASHDIR "/applications/.pruebas", 0755);
    signal(SIGSEGV, on_crash); signal(SIGBUS, on_crash); signal(SIGABRT, on_crash); signal(SIGILL, on_crash); signal(SIGFPE, on_crash);
    /* Para las capturas del emulador: «cuaderno ruta.md [línea byte]» abre la nota (y la edita). */
    if (argc > 1) { snprintf(cur_path, sizeof cur_path, "%s", argv[1]); screen = NOTE; load_note(argv[1]); }
    if (argc > 3) { editing_mode = 1; cur_line = atoi(argv[2]); cur_off = atoi(argv[3]); }
    /* «… teclear» simula pulsaciones: | es intro y < borrar */
    if (argc > 4) for (const char *p = argv[4]; *p; p++) {
        if (*p == '|') key_enter(); else if (*p == '<') key_backspace();
        else { char c[2] = { *p, 0 }; insert_text(c); }
    }
    InkViewMain(handler);
    return 0;
}
