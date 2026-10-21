/*
 * Lectura: tus estadísticas de lectura en el propio PocketBook, sin conectarlo a nada.
 *
 * Lee lo mismo que Cliply, directamente de las bases de datos del lector (solo lectura):
 *   system/explorer-3/explorer-3.db   biblioteca, página actual y total, última apertura, terminado
 *   system/config/books.db            subrayados y palabras guardadas desde el diccionario
 * Las portadas salen de la caché del propio lector (system/cover_chache/1/<ruta>.png).
 * SQLite se carga con dlopen: el lector trae libsqlite3.so.0 y el SDK no trae su cabecera.
 */
#include "inkview.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include "../../common/md.h"

#define EXPLORER FLASHDIR "/system/explorer-3/explorer-3.db"
#define BOOKSDB FLASHDIR "/system/config/books.db"
#define COVERS FLASHDIR "/system/cover_chache/1"

#define SOFT 0x777777
#define LINE 0xC8C8C8
#define M 64
#define TABS_H 110
#define MAX_BOOKS 2000
#define MAX_MARKS 5000

/* ---------- SQLite por dlopen ---------- */
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
static int (*db_open)(const char *, sqlite3 **, int, const char *);
static int (*db_prepare)(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
static int (*db_step)(sqlite3_stmt *);
static const unsigned char *(*db_text)(sqlite3_stmt *, int);
static long long (*db_int)(sqlite3_stmt *, int);
static int (*db_bind_text)(sqlite3_stmt *, int, const char *, int, void (*)(void *));
static int (*db_finalize)(sqlite3_stmt *);
static int (*db_close)(sqlite3 *);
#define SQLITE_ROW 100
#define SQLITE_OPEN_READONLY 1

static int load_sqlite(void)
{
    void *h = dlopen("libsqlite3.so.0", RTLD_NOW);
    if (!h) h = dlopen("libsqlite3.so", RTLD_NOW);
    if (!h) return 0;
    db_open = dlsym(h, "sqlite3_open_v2");
    db_prepare = dlsym(h, "sqlite3_prepare_v2");
    db_step = dlsym(h, "sqlite3_step");
    db_text = dlsym(h, "sqlite3_column_text");
    db_int = dlsym(h, "sqlite3_column_int64");
    db_bind_text = dlsym(h, "sqlite3_bind_text");
    db_finalize = dlsym(h, "sqlite3_finalize");
    db_close = dlsym(h, "sqlite3_close");
    return db_open && db_prepare && db_step && db_text && db_int && db_finalize && db_close;
}

static const char *col(sqlite3_stmt *s, int i) { const unsigned char *t = db_text(s, i); return t ? (const char *)t : ""; }

/* ---------- datos ---------- */
typedef struct {
    char *title, *author, *cover;
    long added, opened, completed_ts;
    int page, pages, done, marks, words;
    double progress;   /* -1 si no se sabe */
} Book;
typedef struct { char *text, *note, *book, *extra; long ts; int word; } Mark;

static Book books[MAX_BOOKS];
static int nbooks;
static Mark marks[MAX_MARKS];
static int nmarks, nwords;
static char error_msg[200];

static const char *BOOK_EXT[] = { "epub", "pdf", "fb2", "mobi", "azw", "azw3", "djvu", "txt", "rtf", "doc", "docx", "cbz", "cbr", "chm", "html", "htm", "prc", "odt", NULL };

static int is_book(const char *fn)
{
    const char *dot = strrchr(fn, '.');
    if (!dot) return 0;
    char e[16];
    snprintf(e, sizeof e, "%s", dot + 1);
    for (char *p = e; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
    for (int i = 0; BOOK_EXT[i]; i++) if (!strcmp(e, BOOK_EXT[i])) return 1;
    return 0;
}

/* Valor de "key":"..." en un JSON pequeño (sin escapes raros más allá de \" y \n). */
static char *json_text(const char *s, const char *key)
{
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = s ? strstr(s, pat) : NULL;
    if (!p) return NULL;
    p += strlen(pat);
    char *out = malloc(strlen(p) + 1), *o = out;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) { p++; *o++ = *p == 'n' ? ' ' : *p; p++; }
        else *o++ = *p++;
    }
    *o = 0;
    return out;
}
static long json_long(const char *s, const char *key)
{
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = s ? strstr(s, pat) : NULL;
    return p ? atol(p + strlen(pat)) : 0;
}

static Book *book_by_title(const char *t)
{
    for (int i = 0; i < nbooks; i++) if (!strcmp(books[i].title, t)) return &books[i];
    return NULL;
}

static int by_opened(const void *a, const void *b)
{
    const Book *x = a, *y = b;
    if (x->opened != y->opened) return x->opened < y->opened ? 1 : -1;
    return x->added < y->added ? 1 : x->added > y->added ? -1 : 0;
}
static int by_ts(const void *a, const void *b) { long x = ((const Mark *)a)->ts, y = ((const Mark *)b)->ts; return x < y ? 1 : x > y ? -1 : 0; }

static void load_data(void)
{
    if (!load_sqlite()) { snprintf(error_msg, sizeof error_msg, "No se encuentra SQLite en el lector."); return; }
    sqlite3 *lib = NULL, *meta = NULL;
    if (db_open(EXPLORER, &lib, SQLITE_OPEN_READONLY, NULL)) { snprintf(error_msg, sizeof error_msg, "No se puede abrir la biblioteca."); return; }
    sqlite3_stmt *st;
    const char *q =
        "select b.title, b.author, f.filename, fo.name, b.ts_added, s.cpage, s.npage, s.opentime, s.completed, s.completed_ts "
        "from books_impl b join files f on f.book_id = b.id join folders fo on fo.id = f.folder_id "
        "left join books_settings s on s.bookid = b.id where b.hidden = 0";
    if (!db_prepare(lib, q, -1, &st, NULL)) {
        while (db_step(st) == SQLITE_ROW && nbooks < MAX_BOOKS) {
            const char *fn = col(st, 2), *folder = col(st, 3), *author = col(st, 1);
            if (!is_book(fn) || !strcmp(author, "PocketBook International SA")) continue;
            char path[1024];
            snprintf(path, sizeof path, "%s/%s", folder, fn);
#ifndef EMULATOR
            struct stat sb;
            if (stat(path, &sb)) continue;   /* fichas de libros que ya no están */
#endif
            const char *title = col(st, 0);
            if (book_by_title(title[0] ? title : fn)) continue;   /* el mismo libro en dos carpetas */
            Book *b = &books[nbooks++];
            b->title = strdup(title[0] ? title : fn);
            b->author = strdup(author);
            b->added = db_int(st, 4);
            b->page = db_int(st, 5);
            b->pages = db_int(st, 6);
            b->opened = db_int(st, 7);
            b->completed_ts = db_int(st, 9);
            b->progress = b->pages > 0 ? (double)b->page / b->pages : -1;
            if (b->progress > 1) b->progress = 1;
            b->done = db_int(st, 8) || b->progress >= 0.99;
            const char *rel = strncmp(path, "/mnt/ext1/", 10) ? path : path + 10;
            char cover[1200];
            snprintf(cover, sizeof cover, COVERS "/%s.png", rel);
            b->cover = strdup(cover);
        }
        db_finalize(st);
    }
    db_close(lib);
    qsort(books, nbooks, sizeof *books, by_opened);

    if (db_open(BOOKSDB, &meta, SQLITE_OPEN_READONLY, NULL)) return;
    q = "select b.Title, "
        "(select t.Val from Tags t join TagNames n on n.OID = t.TagID where t.ItemID = i.OID and n.TagName = 'bm.type'), "
        "(select t.Val from Tags t join TagNames n on n.OID = t.TagID where t.ItemID = i.OID and n.TagName = 'bm.quotation'), "
        "(select t.Val from Tags t join TagNames n on n.OID = t.TagID where t.ItemID = i.OID and n.TagName = 'bm.note'), "
        "(select t.Val from Tags t join TagNames n on n.OID = t.TagID where t.ItemID = i.OID and n.TagName = 'bm.book_mark') "
        "from Items i join Books b on b.OID = i.ParentID where i.TypeID = 4";
    if (!db_prepare(meta, q, -1, &st, NULL)) {
        while (db_step(st) == SQLITE_ROW && nmarks < MAX_MARKS) {
            const char *type = col(st, 1), *quote = col(st, 2), *note = col(st, 3), *anchor = col(st, 4);
            if (strcmp(type, "highlight") && strcmp(type, "note")) continue;
            char *text = json_text(quote, "text");
            if (!text || !text[0]) { free(text); continue; }
            Mark *m = &marks[nmarks++];
            m->text = text;
            m->book = strdup(col(st, 0));
            m->ts = json_long(anchor, "created");
            /* «Guardar en notas» desde el diccionario: la nota es {"context":…,"text": definición} */
            m->word = strstr(anchor, "\"translation\"") || strstr(note, "\"context\"");
            if (m->word) {
                m->note = json_text(note, "text");
                if (m->note) {
                    char *r = m->note, *w = m->note;
                    while (*r == ' ') r++;
                    size_t tl = strlen(m->text);
                    if (!strncmp(r, m->text, tl) && r[tl] == ' ') r += tl;
                    for (; *r; r++) {
                        if ((unsigned char)r[0] == 0xE2 && (unsigned char)r[1] == 0x80 && (unsigned char)r[2] == 0x83) { r += 2; continue; }   /* espacio largo del diccionario */
                        if (!(*r == ' ' && (w == m->note || w[-1] == ' '))) *w++ = *r;
                    }
                    *w = 0;
                    if (strlen(m->note) > 420) strcpy(m->note + 417, "…");
                }
                m->extra = json_text(note, "context");
                nwords++;
            } else {
                m->note = note[0] == '{' ? json_text(note, "text") : strdup(note);
            }
            Book *b = book_by_title(m->book);
            if (b) { if (m->word) b->words++; else b->marks++; }
        }
        db_finalize(st);
    }
    db_close(meta);
    qsort(marks, nmarks, sizeof *marks, by_ts);
}

/* ---------- vocabulario: aprendidas y quitadas (archivo propio, no se toca la base del lector) ---------- */
#define VOCAB FLASHDIR "/system/config/lectura-vocabulario.txt"
static char vstate[MAX_MARKS];   /* por marca: 0 por aprender, 'a' aprendida, 'q' quitada */

static void load_vocab(void)
{
    FILE *f = fopen(VOCAB, "r");
    char buf[600];
    while (f && fgets(buf, sizeof buf, f)) {
        size_t l = strlen(buf);
        while (l && (buf[l - 1] == '\n' || buf[l - 1] == '\r')) buf[--l] = 0;
        if (l < 3 || buf[1] != '\t') continue;
        for (int i = 0; i < nmarks; i++) if (marks[i].word && !strcmp(marks[i].text, buf + 2)) vstate[i] = buf[0];
    }
    if (f) fclose(f);
}

static void save_vocab(void)
{
    FILE *f = fopen(VOCAB, "w");
    if (!f) return;
    for (int i = 0; i < nmarks; i++) if (marks[i].word && vstate[i]) fprintf(f, "%c\t%s\n", vstate[i], marks[i].text);
    fclose(f);
}

/* ---------- actividad: cada cosa que el lector deja con fecha ---------- */
typedef struct { long ts; int kind; const char *book; } Event;   /* kind: 0 abierto, 1 terminado, 2 subrayado, 3 palabra */
static Event events[MAX_BOOKS * 2 + MAX_MARKS];
static int nevents;

static void build_events(void)
{
    nevents = 0;
    for (int i = 0; i < nbooks; i++) {
        if (books[i].opened) events[nevents++] = (Event){ books[i].opened, 0, books[i].title };
        if (books[i].done && books[i].completed_ts) events[nevents++] = (Event){ books[i].completed_ts, 1, books[i].title };
    }
    for (int i = 0; i < nmarks; i++) if (marks[i].ts) events[nevents++] = (Event){ marks[i].ts, marks[i].word ? 3 : 2, marks[i].book };
}

static long day_start(long t) { struct tm tm; time_t x = t; localtime_r(&x, &tm); tm.tm_hour = tm.tm_min = tm.tm_sec = 0; tm.tm_isdst = -1; return mktime(&tm); }

/* ---------- aspecto ---------- */
static ifont *f_title, *f_big, *f_huge, *f_head, *f_body, *f_bold, *f_ital, *f_small, *f_smallb, *f_tab, *f_tabb;
static int W, H, shown_once;
enum { RESUMEN, ACTIVIDAD, LIBROS, SUBRAYADOS, PALABRAS, NTABS } tab = RESUMEN;
static int page_of[NTABS];
static int last_page_end;      /* ¿la página actual llega al final? */
static int detail = -1;        /* libro abierto en ficha, o -1 */
static int detail_page;
static int cal_year, cal_month, cal_day;   /* mes que se ve en Actividad y día elegido (0 ninguno) */
static int card, card_back, vfilter;       /* tarjeta actual, si está dada la vuelta, filtro (0 por aprender, 1 aprendidas) */
static int vcards;                         /* 0 lista de palabras, 1 tarjetas */

static void refresh(void) { if (!shown_once) { FullUpdate(); shown_once = 1; } else SoftUpdate(); }

static const char *MESES[] = { "enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto", "septiembre", "octubre", "noviembre", "diciembre" };

static void ago(long t, char *out, size_t n)
{
    if (!t) { snprintf(out, n, "sin abrir"); return; }
    long d = (day_start(time(NULL)) - day_start(t)) / 86400;
    if (d <= 0) snprintf(out, n, "hoy");
    else if (d == 1) snprintf(out, n, "ayer");
    else if (d < 30) snprintf(out, n, "hace %ld días", d);
    else if (d < 60) snprintf(out, n, "hace un mes");
    else if (d < 365) snprintf(out, n, "hace %ld meses", d / 30);
    else snprintf(out, n, "hace más de un año");
}

static void fecha(long t, char *out, size_t n)
{
    struct tm tm;
    time_t x = t;
    localtime_r(&x, &tm);
    snprintf(out, n, "%d de %s de %d", tm.tm_mday, MESES[tm.tm_mon], tm.tm_year + 1900);
}

static void bar(int x, int y, int w, int h, double p)
{
    DrawRect(x, y, w, h, BLACK);
    if (p > 0) FillArea(x, y, (int)(w * p), h, BLACK);
}

/* Portada escalada dentro de w×h; si no hay, un rectángulo con la inicial. */
static void cover(const Book *b, int x, int y, int w, int h)
{
    ibitmap *bm = LoadPNG(b->cover, 0);
    if (bm) {
        double s = (double)w / bm->width < (double)h / bm->height ? (double)w / bm->width : (double)h / bm->height;
        int cw = bm->width * s, ch = bm->height * s;
        StretchBitmap(x + (w - cw) / 2, y + (h - ch), cw, ch, bm, 0);
        DrawRect(x + (w - cw) / 2, y + (h - ch), cw, ch, LINE);
        free(bm);
    } else {
        FillArea(x, y, w, h, 0xE6E6E6);
        char ini[8] = { 0 };
        int l = (unsigned char)b->title[0] >= 0xC0 ? 2 : 1;
        memcpy(ini, b->title, l);
        SetFont(f_big, SOFT);
        DrawTextRect(x, y, w, h, ini, ALIGN_CENTER | VALIGN_MIDDLE);
    }
}

static void draw_tabs(void)
{
    static const char *names[] = { "Resumen", "Actividad", "Libros", "Subrayados", "Palabras" };
    int y = H - TABS_H, tw = W / NTABS;
    FillArea(0, y, W, TABS_H, WHITE);
    DrawLine(0, y, W, y, BLACK);
    for (int i = 0; i < NTABS; i++) {
        int on = i == (int)tab;
        SetFont(on ? f_tabb : f_tab, on ? BLACK : SOFT);
        DrawTextRect(i * tw, y, tw, TABS_H, names[i], ALIGN_CENTER | VALIGN_MIDDLE);
        if (on) FillArea(i * tw + 30, y + TABS_H - 14, tw - 60, 6, BLACK);
    }
}

static void header(const char *title, const char *sub)
{
    SetFont(f_title, BLACK);
    DrawString(M, 50, title);
    if (sub) { SetFont(f_small, SOFT); DrawString(M, 136, sub); }
}

/* Botones de página encima de las pestañas: «‹ Anterior    2 de 5    Siguiente ›». */
#define PAGER_H 90
static void pager(int page, int pages)
{
    if (pages <= 1) return;
    int y = H - TABS_H - PAGER_H;
    DrawLine(M, y, W - M, y, LINE);
    char mid[40];
    snprintf(mid, sizeof mid, "%d de %d", page + 1, pages);
    SetFont(f_small, SOFT);
    DrawTextRect(0, y, W, PAGER_H, mid, ALIGN_CENTER | VALIGN_MIDDLE);
    SetFont(f_head, page > 0 ? BLACK : LINE);
    DrawTextRect(M, y, W / 3, PAGER_H, "‹ Anterior", ALIGN_LEFT | VALIGN_MIDDLE);
    SetFont(f_head, page + 1 < pages ? BLACK : LINE);
    DrawTextRect(W - M - W / 3, y, W / 3, PAGER_H, "Siguiente ›", ALIGN_RIGHT | VALIGN_MIDDLE);
}

/* ---------- Resumen ---------- */
static void stat_box(int x, int w, int y, int value, const char *label)
{
    char v[32];
    snprintf(v, sizeof v, "%d", value);
    SetFont(f_big, BLACK);
    DrawTextRect(x, y, w, 80, v, ALIGN_CENTER | VALIGN_MIDDLE);
    SetFont(f_small, SOFT);
    DrawTextRect(x, y + 82, w, 40, label, ALIGN_CENTER);
}

static int active_day(long d0)
{
    for (int i = 0; i < nevents; i++) if (events[i].ts >= d0 && events[i].ts < d0 + 86400) return 1;
    return 0;
}

static void reading_item(const Book *b, int y)
{
    cover(b, M, y, 120, 180);
    int tx = M + 150, tw = W - M - tx;
    SetFont(f_bold, BLACK);
    DrawTextRect(tx, y, tw, 50, b->title, ALIGN_LEFT | DOTS);
    SetFont(f_body, SOFT);
    DrawTextRect(tx, y + 50, tw, 44, b->author[0] ? b->author : " ", ALIGN_LEFT | DOTS);
    bar(tx, y + 112, tw, 14, b->progress);
    char info[120], when[40];
    ago(b->opened, when, sizeof when);
    snprintf(info, sizeof info, "%d %%  ·  página %d de %d  ·  %s", (int)(b->progress * 100 + 0.5), b->page, b->pages, when);
    SetFont(f_small, SOFT);
    DrawString(tx, y + 140, info);
}

static int reading_idx[3], nreading;

static void draw_resumen(void)
{
    int done = 0, reading = 0, pages = 0, year_done = 0;
    long last = 0;
    time_t now = time(NULL);
    struct tm tn;
    localtime_r(&now, &tn);
    for (int i = 0; i < nbooks; i++) {
        Book *b = &books[i];
        if (b->done) done++; else if (b->progress > 0) reading++;
        pages += b->done ? b->pages : b->page;
        if (b->opened > last) last = b->opened;
        if (b->done && b->completed_ts) { struct tm tc; time_t c = b->completed_ts; localtime_r(&c, &tc); if (tc.tm_year == tn.tm_year) year_done++; }
    }
    char sub[120], a[40];
    ago(last, a, sizeof a);
    snprintf(sub, sizeof sub, "Última lectura: %s", a);
    header("Tu lectura", sub);

    int y = 200, bw = (W - 2 * M) / 4;
    stat_box(M, bw, y, nbooks, "libros");
    stat_box(M + bw, bw, y, reading, "a medias");
    stat_box(M + 2 * bw, bw, y, done, "terminados");
    stat_box(M + 3 * bw, bw, y, pages, "páginas leídas");
    DrawLine(M, y + 150, W - M, y + 150, LINE);

    y += 180;
    SetFont(f_bold, BLACK);
    DrawString(M, y, "Leyendo ahora");
    y += 64;
    nreading = 0;
    for (int i = 0; i < nbooks && nreading < 3; i++) {
        Book *b = &books[i];
        if (b->done || b->progress <= 0 || (now - b->opened > 60 * 86400 && nreading)) continue;
        reading_item(b, y);
        reading_idx[nreading++] = i;
        y += 210;
    }
    if (!nreading) { SetFont(f_body, SOFT); DrawString(M, y, "Ningún libro a medias."); y += 70; }
    DrawLine(M, y, W - M, y, LINE);

    /* Esta semana, de lunes a domingo */
    y += 30;
    SetFont(f_bold, BLACK);
    DrawString(M, y, "Esta semana");
    y += 70;
    static const char *dn[] = { "L", "M", "X", "J", "V", "S", "D" };
    long today = day_start(now);
    int wd = (tn.tm_wday + 6) % 7, cw = (W - 2 * M) / 7, n_active = 0;
    for (int d = 0; d < 7; d++) {
        long d0 = today + (d - wd) * 86400L;
        int cx = M + d * cw + cw / 2, on = d <= wd && active_day(d0);
        n_active += on;
        SetFont(d == wd ? f_smallb : f_small, d == wd ? BLACK : SOFT);
        DrawTextRect(M + d * cw, y, cw, 36, dn[d], ALIGN_CENTER);
        if (on) FillArea(cx - 26, y + 46, 52, 52, BLACK);
        else DrawRect(cx - 26, y + 46, 52, 52, d > wd ? 0xE6E6E6 : LINE);
    }
    y += 120;
    char line[200];
    snprintf(line, sizeof line, "%d terminado%s este año  ·  %d subrayado%s  ·  %d palabra%s",
             year_done, year_done == 1 ? "" : "s", nmarks - nwords, nmarks - nwords == 1 ? "" : "s", nwords, nwords == 1 ? "" : "s");
    SetFont(f_small, SOFT);
    DrawTextRect(M, y, W - 2 * M, 44, line, ALIGN_CENTER);
}

/* ---------- Actividad: calendario por meses ---------- */
static int cal_top(void) { return 290; }
static int cal_cell_h(void) { return 96; }

static void draw_actividad(void)
{
    char t[60];
    snprintf(t, sizeof t, "%s %d", MESES[cal_month], cal_year);
    t[0] -= t[0] >= 'a' && t[0] <= 'z' ? 32 : 0;
    header("Actividad", "Días con lectura registrada en el lector");
    SetFont(f_head, BLACK);
    DrawTextRect(M, 180, W - 2 * M, 70, t, ALIGN_CENTER | VALIGN_MIDDLE);
    DrawTextRect(M, 180, 120, 70, "‹", ALIGN_LEFT | VALIGN_MIDDLE);
    DrawTextRect(W - M - 120, 180, 120, 70, "›", ALIGN_RIGHT | VALIGN_MIDDLE);

    static const char *dn[] = { "L", "M", "X", "J", "V", "S", "D" };
    int cw = (W - 2 * M) / 7, ch = cal_cell_h(), y0 = cal_top();
    SetFont(f_small, SOFT);
    for (int d = 0; d < 7; d++) DrawTextRect(M + d * cw, y0 - 44, cw, 40, dn[d], ALIGN_CENTER);
    struct tm first = { 0 };
    first.tm_year = cal_year - 1900; first.tm_mon = cal_month; first.tm_mday = 1; first.tm_hour = 12; first.tm_isdst = -1;
    time_t f = mktime(&first);
    localtime_r(&f, &first);
    int off = (first.tm_wday + 6) % 7, days = 31, read_days = 0;
    for (int d = 28; d <= 31; d++) { struct tm x = first; x.tm_mday = d; time_t tt = mktime(&x); struct tm y; localtime_r(&tt, &y); if (y.tm_mon == cal_month) days = d; }
    long today = day_start(time(NULL));
    for (int d = 1; d <= days; d++) {
        int k = off + d - 1, r = k / 7, c = k % 7;
        int x = M + c * cw, y = y0 + r * ch;
        struct tm x2 = first; x2.tm_mday = d; x2.tm_hour = 0; time_t d0 = mktime(&x2);
        int on = active_day(d0), sel = d == cal_day;
        if (on) read_days++;
        char num[4];
        snprintf(num, sizeof num, "%d", d);
        if (on) { FillArea(x + 8, y + 6, cw - 16, ch - 12, BLACK); SetFont(f_bold, WHITE); }
        else { SetFont(f_body, d0 > today ? LINE : BLACK); }
        DrawTextRect(x, y, cw, ch, num, ALIGN_CENTER | VALIGN_MIDDLE);
        if (sel) for (int i = 0; i < 3; i++) DrawRect(x + 2 + i, y + i, cw - 4 - 2 * i, ch - 2 * i, on ? SOFT : BLACK);
        if (d0 == today && !on) DrawLine(x + cw / 2 - 16, y + ch - 18, x + cw / 2 + 16, y + ch - 18, BLACK);
    }
    int y = y0 + 6 * ch + 10;
    DrawLine(M, y, W - M, y, LINE);
    y += 24;
    char line[200];
    if (!cal_day) {
        snprintf(line, sizeof line, "%d día%s con lectura en %s. Toca un día para ver qué pasó.", read_days, read_days == 1 ? "" : "s", MESES[cal_month]);
        SetFont(f_body, SOFT);
        DrawTextRect(M, y, W - 2 * M, 120, line, ALIGN_LEFT);
        return;
    }
    struct tm x2 = first; x2.tm_mday = cal_day; x2.tm_hour = 0; time_t d0 = mktime(&x2);
    snprintf(line, sizeof line, "%d de %s", cal_day, MESES[cal_month]);
    SetFont(f_bold, BLACK);
    DrawString(M, y, line);
    y += 60;
    int shown = 0;
    static const char *what[] = { "Abriste", "Terminaste", "Subrayaste en", "Guardaste una palabra de" };
    for (int k = 0; k < 4; k++)
        for (int i = 0; i < nevents && y < H - TABS_H - 50; i++) {
            if (events[i].kind != k || events[i].ts < d0 || events[i].ts >= d0 + 86400) continue;
            int dup = 0;
            for (int j = 0; j < i; j++) if (events[j].kind == k && events[j].ts >= d0 && events[j].ts < d0 + 86400 && !strcmp(events[j].book, events[i].book)) dup = 1;
            if (dup) continue;
            int count = 0;
            for (int j = 0; j < nevents; j++) if (events[j].kind == k && events[j].ts >= d0 && events[j].ts < d0 + 86400 && !strcmp(events[j].book, events[i].book)) count++;
            if (k >= 2 && count > 1) snprintf(line, sizeof line, "%s «%s» (%d veces)", what[k], events[i].book, count);
            else snprintf(line, sizeof line, "%s «%s»", what[k], events[i].book);
            SetFont(f_body, BLACK);
            DrawTextRect(M, y, W - 2 * M, 44, line, ALIGN_LEFT | DOTS);
            y += 52;
            shown++;
        }
    if (!shown) { SetFont(f_body, SOFT); DrawString(M, y, "Sin actividad registrada."); }
}

/* ---------- Libros ---------- */
#define BOOK_ROW 210
static int libros_rows(void) { return (H - TABS_H - PAGER_H - 190) / BOOK_ROW; }

static void draw_libros(void)
{
    char sub[80];
    snprintf(sub, sizeof sub, "%d libros, del último abierto al primero. Toca uno para verlo.", nbooks);
    header("Libros", sub);
    int per = libros_rows(), from = page_of[LIBROS] * per;
    int y = 190;
    for (int i = from; i < nbooks && i < from + per; i++, y += BOOK_ROW) {
        Book *b = &books[i];
        cover(b, M, y, 110, 165);
        int tx = M + 140, tw = W - M - tx;
        SetFont(f_bold, BLACK);
        DrawTextRect(tx, y, tw, 50, b->title, ALIGN_LEFT | DOTS);
        SetFont(f_small, SOFT);
        DrawTextRect(tx, y + 50, tw, 40, b->author[0] ? b->author : " ", ALIGN_LEFT | DOTS);
        char info[160], when[40];
        ago(b->opened, when, sizeof when);
        if (b->done) snprintf(info, sizeof info, "Terminado  ·  %s", when);
        else if (b->progress > 0) snprintf(info, sizeof info, "%d %%  ·  %s", (int)(b->progress * 100 + 0.5), when);
        else snprintf(info, sizeof info, "Sin empezar");
        if (b->marks) { char m[40]; snprintf(m, sizeof m, "  ·  %d subrayado%s", b->marks, b->marks == 1 ? "" : "s"); strncat(info, m, sizeof info - strlen(info) - 1); }
        if (b->words) { char m[40]; snprintf(m, sizeof m, "  ·  %d palabra%s", b->words, b->words == 1 ? "" : "s"); strncat(info, m, sizeof info - strlen(info) - 1); }
        if (b->progress > 0) bar(tx, y + 104, tw, 10, b->done ? 1 : b->progress);
        DrawString(tx, y + 126, info);
        DrawLine(M, y + BOOK_ROW - 18, W - M, y + BOOK_ROW - 18, LINE);
    }
    int pages = (nbooks + per - 1) / per;
    last_page_end = page_of[LIBROS] + 1 >= pages;
    pager(page_of[LIBROS], pages);
}

/* Lista de subrayados o palabras con alto variable. book: solo los de ese libro (o NULL).
 * Devuelve si la página llega al final. */
static int draw_mark_list(int words, const char *book, int target, int y0, int bottom)
{
    int pg = 0, y = y0, w = W - 2 * M, i = 0;
    for (; i < nmarks; i++) {
        Mark *m = &marks[i];
        if (m->word != words || (book && strcmp(m->book, book)) || (words && vstate[i] == 'q')) continue;
        char head[600];
        int h1, h2 = 44, h3 = 0;
        if (words) {
            snprintf(head, sizeof head, "%s", m->text);
            SetFont(f_bold, BLACK); h1 = TextRectHeight(w, head, 0);
            SetFont(f_small, SOFT); h2 = m->note && m->note[0] ? TextRectHeight(w, m->note, 0) : 0;
            if (h2 > 140) h2 = 140;
        } else {
            snprintf(head, sizeof head, "«%s»", m->text);
            SetFont(f_ital, BLACK); h1 = TextRectHeight(w, head, 0);
            if (book) h2 = 0;
            if (m->note && m->note[0]) { SetFont(f_body, BLACK); h3 = TextRectHeight(w - 30, m->note, 0) + 10; }
        }
        int need = h1 + h2 + h3 + 50;
        if (need > bottom - y0) need = bottom - y0;
        if (y + need > bottom) { if (pg == target) break; pg++; y = y0; }
        if (pg == target) {
            if (words) {
                SetFont(f_bold, BLACK); DrawTextRect(M, y, w, h1, head, ALIGN_LEFT);
                if (h2) { SetFont(f_small, SOFT); DrawTextRect(M, y + h1 + 4, w, h2, m->note, ALIGN_LEFT | DOTS); }
            } else {
                SetFont(f_ital, BLACK); DrawTextRect(M, y, w, h1 > bottom - y ? bottom - y : h1, head, ALIGN_LEFT | DOTS);
                if (h2) { SetFont(f_small, SOFT); DrawTextRect(M, y + h1 + 6, w, 40, m->book, ALIGN_LEFT | DOTS); }
                if (h3) { FillArea(M, y + h1 + h2 + 8, 5, h3 - 10, SOFT); SetFont(f_body, BLACK); DrawTextRect(M + 30, y + h1 + h2 + 8, w - 30, h3 - 10, m->note, ALIGN_LEFT); }
            }
            DrawLine(M, y + need - 24, W - M, y + need - 24, LINE);
        }
        y += need;
    }
    return i >= nmarks;
}

static void draw_subrayados(void)
{
    char sub[80];
    snprintf(sub, sizeof sub, "%d subrayados, del más reciente", nmarks - nwords);
    header("Subrayados", sub);
    if (nmarks - nwords == 0) {
        SetFont(f_body, SOFT);
        DrawTextRect(M, 400, W - 2 * M, 300, "Aún no has subrayado nada.", ALIGN_CENTER);
        last_page_end = 1;
        return;
    }
    last_page_end = draw_mark_list(0, NULL, page_of[SUBRAYADOS], 190, H - TABS_H - PAGER_H - 10);
    pager(page_of[SUBRAYADOS], last_page_end ? page_of[SUBRAYADOS] + 1 : page_of[SUBRAYADOS] + 2);
}

/* ---------- Palabras: tarjetas de vocabulario ---------- */
static int vocab_list[MAX_MARKS], nvocab, nlearned, npending;

static void build_vocab(void)
{
    nvocab = nlearned = npending = 0;
    for (int i = 0; i < nmarks; i++) {
        if (!marks[i].word || vstate[i] == 'q') continue;
        if (vstate[i] == 'a') nlearned++; else npending++;
        if ((vfilter == 1) == (vstate[i] == 'a')) vocab_list[nvocab++] = i;
    }
    if (card >= nvocab) card = nvocab ? nvocab - 1 : 0;
}

/* La definición del diccionario llega en un solo párrafo: cada acepción («2. …») y cada
 * subapartado («a) …») va a su línea. */
static void format_definition(const char *in, char *out, size_t n)
{
    size_t j = 0;
    for (const char *p = in; *p && j + 4 < n; p++) {
        int acep = p > in && p[-1] == ' ' && p[0] >= '1' && p[0] <= '9' && (p[1] == '.' || (p[1] >= '0' && p[1] <= '9' && p[2] == '.'));
        int sub = p > in && p[-1] == ' ' && p[0] >= 'a' && p[0] <= 'h' && p[1] == ')';
        int etim = p > in && !strncmp(p, "Etimología", 11);
        if ((acep || etim) && j) { while (j && out[j - 1] == ' ') j--; out[j++] = '\n'; }
        if (sub && j) { while (j && out[j - 1] == ' ') j--; out[j++] = '\n'; out[j++] = ' '; out[j++] = ' '; out[j++] = ' '; }
        out[j++] = *p;
    }
    out[j] = 0;
}

#define CARD_TOP 250
static int card_bottom(void) { return H - TABS_H - 220; }

#define VROW 128
#define VLIST_TOP 190
static int vlist_rows(void) { return (H - TABS_H - PAGER_H - VLIST_TOP) / VROW; }

static void draw_palabras(void)
{
    build_vocab();
    char sub[120];
    snprintf(sub, sizeof sub, "%d por aprender  ·  %d aprendida%s", npending, nlearned, nlearned == 1 ? "" : "s");
    if (vcards) { SetFont(f_head, BLACK); DrawString(M - 20, 52, "‹ Lista"); SetFont(f_small, SOFT); DrawString(M, 136, sub); }
    else header("Palabras", sub);
    /* filtro */
    static const char *fl[] = { "Por aprender", "Aprendidas" };
    for (int i = 0; i < 2; i++) {
        int x = W - M - 2 * 230 + i * 230, on = i == vfilter;
        if (on) FillArea(x, 40, 220, 64, BLACK); else DrawRect(x, 40, 220, 64, LINE);
        SetFont(f_small, on ? WHITE : BLACK);
        DrawTextRect(x, 40, 220, 64, fl[i], ALIGN_CENTER | VALIGN_MIDDLE);
    }
    if (!nvocab) {
        SetFont(f_body, SOFT);
        DrawTextRect(M, 450, W - 2 * M, 400, vfilter ? "Aún no has marcado ninguna como aprendida." :
                     nwords ? "¡Las has aprendido todas!" : "Busca una palabra en el diccionario mientras lees y pulsa «Guardar en notas».", ALIGN_CENTER);
        last_page_end = 1;
        return;
    }
    if (!vcards) {   /* todas de un vistazo; al tocar una se abre su tarjeta */
        int per = vlist_rows(), from = page_of[PALABRAS] * per, y = VLIST_TOP;
        for (int k = from; k < nvocab && k < from + per; k++, y += VROW) {
            Mark *m = &marks[vocab_list[k]];
            SetFont(f_bold, BLACK);
            DrawTextRect(M, y + 4, W - 2 * M, 48, m->text, ALIGN_LEFT | DOTS);
            static char def[1600];
            format_definition(m->note ? m->note : "", def, sizeof def);
            for (char *q = def; *q; q++) if (*q == '\n') *q = ' ';
            SetFont(f_small, SOFT);
            DrawTextRect(M, y + 56, W - 2 * M, 40, def[0] ? def : m->book, ALIGN_LEFT | DOTS);
            DrawLine(M, y + VROW - 14, W - M, y + VROW - 14, LINE);
        }
        int pages = (nvocab + per - 1) / per;
        last_page_end = page_of[PALABRAS] + 1 >= pages;
        pager(page_of[PALABRAS], pages);
        return;
    }
    Mark *m = &marks[vocab_list[card]];
    int x0 = M, x1 = W - M, y0 = CARD_TOP, y1 = card_bottom(), iw = x1 - x0 - 100;
    for (int i = 0; i < 3; i++) DrawRect(x0 + i, y0 + i, x1 - x0 - 2 * i, y1 - y0 - 2 * i, BLACK);
    char cnt[40];
    snprintf(cnt, sizeof cnt, "%d / %d", card + 1, nvocab);
    SetFont(f_small, SOFT);
    DrawTextRect(x0, y0 + 20, x1 - x0 - 30, 40, cnt, ALIGN_RIGHT);
    if (!card_back) {
        SetFont(f_huge, BLACK);
        DrawTextRect(x0 + 50, y0 + 120, iw, 120, m->text, ALIGN_CENTER | VALIGN_MIDDLE);
        if (m->extra && m->extra[0]) {
            char ctx[900];
            snprintf(ctx, sizeof ctx, "«%s»", m->extra);
            SetFont(f_ital, BLACK);
            DrawTextRect(x0 + 50, y0 + 290, iw, y1 - y0 - 450, ctx, ALIGN_CENTER | DOTS);
        }
        SetFont(f_small, SOFT);
        DrawTextRect(x0 + 50, y1 - 150, iw, 40, m->book, ALIGN_CENTER | DOTS);
        DrawTextRect(x0 + 50, y1 - 90, iw, 40, "Toca la tarjeta para ver el significado", ALIGN_CENTER);
    } else {
        SetFont(f_title, BLACK);
        DrawTextRect(x0 + 50, y0 + 60, iw, 80, m->text, ALIGN_LEFT | VALIGN_MIDDLE);
        static char def[1600];
        format_definition(m->note ? m->note : "", def, sizeof def);
        SetFont(f_body, BLACK);
        DrawTextRect(x0 + 50, y0 + 170, iw, y1 - y0 - 230, def[0] ? def : "Sin definición guardada.", ALIGN_LEFT | DOTS);
    }
    /* acciones */
    int by = y1 + 40, bw = (W - 2 * M - 3 * 20) / 4;
    const char *labels[] = { "‹", vfilter ? "Por aprender" : "Aprendida ✓", "Quitar", "›" };
    for (int i = 0; i < 4; i++) {
        int x = M + i * (bw + 20);
        int dis = (i == 0 && card == 0) || (i == 3 && card + 1 >= nvocab);
        DrawRect(x, by, bw, 100, dis ? LINE : BLACK);
        DrawRect(x + 1, by + 1, bw - 2, 98, dis ? LINE : BLACK);
        SetFont(i == 0 || i == 3 ? f_title : f_body, dis ? LINE : BLACK);
        DrawTextRect(x, by, bw, 100, labels[i], ALIGN_CENTER | VALIGN_MIDDLE);
    }
    last_page_end = 1;
}

static void palabras_tap(int x, int y)
{
    if (y >= 40 && y <= 104 && x > W - M - 2 * 230) { int f = (x - (W - M - 2 * 230)) / 230; if (f != vfilter) { vfilter = f; card = 0; card_back = 0; page_of[PALABRAS] = 0; } return; }
    if (vcards && y < 110 && x < W / 3) { vcards = 0; page_of[PALABRAS] = card / vlist_rows(); return; }
    if (!nvocab) return;
    if (!vcards) {
        if (y > H - TABS_H - PAGER_H) { if (x > W / 2 && !last_page_end) page_of[PALABRAS]++; else if (x <= W / 2 && page_of[PALABRAS] > 0) page_of[PALABRAS]--; return; }
        int k = page_of[PALABRAS] * vlist_rows() + (y - VLIST_TOP) / VROW;
        if (y >= VLIST_TOP && k < nvocab) { card = k; card_back = 0; vcards = 1; }
        return;
    }
    if (y >= CARD_TOP && y <= card_bottom()) { card_back = !card_back; return; }
    int by = card_bottom() + 40, bw = (W - 2 * M - 3 * 20) / 4;
    if (y < by || y > by + 100) return;
    int b = (x - M) / (bw + 20);
    int idx = vocab_list[card];
    if (b == 0 && card > 0) { card--; card_back = 0; }
    else if (b == 3 && card + 1 < nvocab) { card++; card_back = 0; }
    else if (b == 1) { vstate[idx] = vfilter ? 0 : 'a'; save_vocab(); card_back = 0; }
    else if (b == 2) { vstate[idx] = 'q'; save_vocab(); card_back = 0; }
}

/* ---------- ficha de un libro ---------- */
static void draw_detail(void)
{
    Book *b = &books[detail];
    SetFont(f_head, BLACK);
    DrawString(M - 20, 42, "‹ Libros");
    int y = 130;
    if (detail_page == 0) {
        cover(b, M, y, 240, 360);
        int tx = M + 280, tw = W - M - tx;
        SetFont(f_title, BLACK);
        int th = TextRectHeight(tw, b->title, 0);
        if (th > 230) th = 230;
        DrawTextRect(tx, y, tw, th, b->title, ALIGN_LEFT | DOTS);
        SetFont(f_body, SOFT);
        DrawTextRect(tx, y + th + 10, tw, 44, b->author[0] ? b->author : " ", ALIGN_LEFT | DOTS);
        int yy = y + th + 80;
        if (b->progress > 0) {
            bar(tx, yy, tw, 16, b->done ? 1 : b->progress);
            char p[80];
            snprintf(p, sizeof p, b->done ? "Terminado" : "%d %%  ·  página %d de %d", (int)(b->progress * 100 + 0.5), b->page, b->pages);
            SetFont(f_small, SOFT);
            DrawString(tx, yy + 30, p);
        }
        y += 400;
        char l[160], d[80];
        SetFont(f_body, BLACK);
        if (b->opened) { ago(b->opened, d, sizeof d); snprintf(l, sizeof l, "Última lectura: %s", d); DrawString(M, y, l); y += 56; }
        if (b->added) { fecha(b->added, d, sizeof d); snprintf(l, sizeof l, "En el lector desde el %s", d); DrawString(M, y, l); y += 56; }
        if (b->done && b->completed_ts) { fecha(b->completed_ts, d, sizeof d); snprintf(l, sizeof l, "Terminado el %s", d); DrawString(M, y, l); y += 56; }
        snprintf(l, sizeof l, "%d subrayado%s  ·  %d palabra%s guardada%s", b->marks, b->marks == 1 ? "" : "s", b->words, b->words == 1 ? "" : "s", b->words == 1 ? "" : "s");
        SetFont(f_small, SOFT);
        DrawString(M, y, l);
        y += 50;
        if (b->words) {   /* sus palabras, en una línea */
            static char wl[1200];
            snprintf(wl, sizeof wl, "Palabras: ");
            int first = 1;
            for (int i = 0; i < nmarks; i++)
                if (marks[i].word && vstate[i] != 'q' && !strcmp(marks[i].book, b->title)) {
                    if (!first) strncat(wl, ", ", sizeof wl - strlen(wl) - 1);
                    strncat(wl, marks[i].text, sizeof wl - strlen(wl) - 1);
                    first = 0;
                }
            SetFont(f_body, BLACK);
            int hh = TextRectHeight(W - 2 * M, wl, 0);
            if (hh > 130) hh = 130;
            DrawTextRect(M, y, W - 2 * M, hh, wl, ALIGN_LEFT | DOTS);
            y += hh + 20;
        }
        DrawLine(M, y, W - M, y, LINE);
        y += 30;
    }
    /* subrayados y palabras del libro, en las páginas siguientes si no caben */
    if (!b->marks) { if (!b->words) { SetFont(f_body, SOFT); DrawString(M, y, "Sin subrayados ni palabras guardadas."); } last_page_end = 1; return; }
    int bottom = H - TABS_H - PAGER_H - 10;
    if (b->marks && detail_page == 0) { SetFont(f_bold, BLACK); DrawString(M, y, "Subrayados"); y += 60; }
    int end = b->marks ? draw_mark_list(0, b->title, detail_page, detail_page ? 130 : y, bottom) : 1;
    last_page_end = end;
    pager(detail_page, end ? detail_page + 1 : detail_page + 2);
}

static void draw(void)
{
    ClearScreen();
    if (error_msg[0]) {
        header("Tu lectura", NULL);
        SetFont(f_body, BLACK);
        DrawTextRect(M, 300, W - 2 * M, 300, error_msg, ALIGN_LEFT);
    } else if (detail >= 0) draw_detail();
    else switch (tab) {
        case RESUMEN: draw_resumen(); last_page_end = 1; break;
        case ACTIVIDAD: draw_actividad(); last_page_end = 1; break;
        case LIBROS: draw_libros(); break;
        case SUBRAYADOS: draw_subrayados(); break;
        case PALABRAS: draw_palabras(); break;
        default: break;
    }
    draw_tabs();
    refresh();
}

static void page(int dir)
{
    int *pg = detail >= 0 ? &detail_page : &page_of[tab];
    if (detail < 0 && tab == ACTIVIDAD) {
        cal_month += dir; cal_day = 0;
        if (cal_month < 0) { cal_month = 11; cal_year--; }
        if (cal_month > 11) { cal_month = 0; cal_year++; }
    } else if (detail < 0 && tab == PALABRAS && vcards) {
        if (dir > 0 && card + 1 < nvocab) card++; else if (dir < 0 && card > 0) card--; else return;
        card_back = 0;
    } else if (dir > 0 && !last_page_end) (*pg)++;
    else if (dir < 0 && *pg > 0) (*pg)--;
    else if (detail < 0 && tab == RESUMEN) return;
    else return;
    draw();
}

static void tap(int x, int y)
{
    if (y > H - TABS_H) {
        int t = x * NTABS / W;
        if (t != (int)tab || detail >= 0) { tab = t; detail = -1; draw(); }
        return;
    }
    if (detail >= 0 && y < 110 && x < W / 3) { detail = -1; draw(); return; }
    int py = H - TABS_H - PAGER_H;
    if ((detail >= 0 || tab == LIBROS || tab == SUBRAYADOS) && y > py) { page(x > W / 2 ? 1 : -1); return; }
    if (detail >= 0) return;
    switch (tab) {
    case RESUMEN:
        for (int i = 0; i < nreading; i++) {
            int yy = 444 + i * 210;
            if (y >= yy && y < yy + 200) { detail = reading_idx[i]; detail_page = 0; draw(); return; }
        }
        break;
    case ACTIVIDAD: {
        if (y >= 180 && y < 250) { page(x > W / 2 ? 1 : -1); return; }
        int cw = (W - 2 * M) / 7, ch = cal_cell_h(), y0 = cal_top();
        if (y < y0 || y >= y0 + 6 * ch || x < M) return;
        struct tm first = { 0 };
        first.tm_year = cal_year - 1900; first.tm_mon = cal_month; first.tm_mday = 1; first.tm_hour = 12; first.tm_isdst = -1;
        time_t f = mktime(&first);
        localtime_r(&f, &first);
        int k = ((y - y0) / ch) * 7 + (x - M) / cw, d = k - (first.tm_wday + 6) % 7 + 1;
        if (d >= 1 && d <= 31) { cal_day = d == cal_day ? 0 : d; draw(); }
        break;
    }
    case LIBROS: {
        int k = (y - 190) / BOOK_ROW, i = page_of[LIBROS] * libros_rows() + k;
        if (y >= 190 && k < libros_rows() && i < nbooks) { detail = i; detail_page = 0; draw(); }
        break;
    }
    case PALABRAS:
        palabras_tap(x, y);
        draw();
        break;
    default: break;
    }
}

static int handler(int type, int par1, int par2)
{
    switch (type) {
    case EVT_INIT: {
        SetPanelType(PANEL_DISABLED);   /* sin barra de estado: si no, el primer pintado sale desplazado */
        W = ScreenWidth(); H = ScreenHeight();
        f_title = OpenFont(DEFAULTFONTB, 60, 1);
        f_big = OpenFont(DEFAULTFONTB, 58, 1);
        f_huge = OpenFont(DEFAULTFONTB, 84, 1);
        f_head = OpenFont(DEFAULTFONT, 38, 1);
        f_body = OpenFont(DEFAULTFONT, 34, 1);
        f_bold = OpenFont(DEFAULTFONTB, 36, 1);
        f_ital = OpenFont(DEFAULTFONTI, 34, 1);
        f_small = OpenFont(DEFAULTFONT, 28, 1);
        f_smallb = OpenFont(DEFAULTFONTB, 28, 1);
        f_tab = OpenFont(DEFAULTFONT, 30, 1);
        f_tabb = OpenFont(DEFAULTFONTB, 30, 1);
        load_data();
        load_vocab();
        build_events();
        time_t now = time(NULL);
        struct tm tn;
        localtime_r(&now, &tn);
        cal_year = tn.tm_year + 1900; cal_month = tn.tm_mon;
        break;
    }
    case EVT_SHOW:
        draw();
        break;
    case EVT_POINTERUP:
        tap(par1, par2);
        return 1;
    case EVT_KEYPRESS:
        if (par1 == IV_KEY_BACK || par1 == IV_KEY_HOME || par1 == IV_KEY_MENU) {
            if (detail >= 0) { detail = -1; draw(); } else CloseApp();
            return 1;
        }
        if (par1 == IV_KEY_NEXT || par1 == IV_KEY_NEXT2 || par1 == IV_KEY_DOWN || par1 == IV_KEY_RIGHT) { page(1); return 1; }
        if (par1 == IV_KEY_PREV || par1 == IV_KEY_PREV2 || par1 == IV_KEY_UP || par1 == IV_KEY_LEFT) { page(-1); return 1; }
        break;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    /* para las capturas del emulador: «lectura <pestaña> [libro|vuelta]» */
    if (argc > 1) tab = atoi(argv[1]);
    if (argc > 2 && tab == LIBROS) detail = atoi(argv[2]);
    if (argc > 2 && tab == PALABRAS) { vcards = 1; card_back = atoi(argv[2]); }
    if (argc > 2 && tab == ACTIVIDAD) cal_day = atoi(argv[2]);
    InkViewMain(handler);
    return 0;
}
