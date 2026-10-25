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
#include "../../common/ai.h"
#include "../../common/epub.h"
#include "pockit.h"
/* en Pockit, «cerrar» un módulo es volver al Inicio */
#define CloseApp pockit_home

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
    char *title, *author, *cover, *path, *position;
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
    if (!p && s) { snprintf(pat, sizeof pat, "\"%s\": \"", key); p = strstr(s, pat); }
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

static int data_loaded;
static void load_data(void)
{
    if (data_loaded) return;
    data_loaded = 1;
    if (!load_sqlite()) { snprintf(error_msg, sizeof error_msg, "No se encuentra SQLite en el lector."); return; }
    sqlite3 *lib = NULL, *meta = NULL;
    if (db_open(EXPLORER, &lib, SQLITE_OPEN_READONLY, NULL)) { snprintf(error_msg, sizeof error_msg, "No se puede abrir la biblioteca."); return; }
    sqlite3_stmt *st;
    const char *q =
        "select b.title, b.author, f.filename, fo.name, b.ts_added, s.cpage, s.npage, s.opentime, s.completed, s.completed_ts, s.position "
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
#ifdef EMULATOR
            if (!strncmp(path, "/mnt/ext1/", 10)) { char q[1100]; snprintf(q, sizeof q, FLASHDIR "/%s", path + 10); b->path = strdup(q); } else
#endif
            b->path = strdup(path);
            b->position = strdup(col(st, 10));
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
static enum { RESUMEN, ACTIVIDAD, LIBROS, SUBRAYADOS, PALABRAS, NTABS } tab = RESUMEN;
static int page_of[NTABS];
static int last_page_end;      /* ¿la página actual llega al final? */
static int detail = -1;        /* libro abierto en ficha, o -1 */
static int detail_page;
static int cal_year, cal_month, cal_day;   /* mes que se ve en Actividad y día elegido (0 ninguno) */
static int card, card_back, vfilter;       /* tarjeta actual, si está dada la vuelta, filtro (0 por aprender, 1 aprendidas) */
static int vcards;                         /* 0 lista de palabras, 1 tarjetas */

static void refresh(void) { POCKIT_REFRESH(shown_once); }

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
    SetFont(f_small, SOFT);
    DrawString(M - 20, 6, "‹ Pockit");
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
static int ml_n, ml_idx[64], ml_y0[64], ml_y1[64];
static const char *(*ml_extra)(const char *quote);   /* explicación guardada de un subrayado, si la hay */

static int draw_mark_list(int words, const char *book, int target, int y0, int bottom)
{
    ml_n = 0;
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
        const char *ex = !words && ml_extra ? ml_extra(m->text) : NULL;
        int h4 = 0;
        if (ex) { SetFont(f_ital, SOFT); h4 = TextRectHeight(w - 30, ex, 0) + 14; }
        int need = h1 + h2 + h3 + h4 + 50;
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
                if (h4) { SetFont(f_ital, SOFT); DrawTextRect(M + 30, y + h1 + h2 + h3 + 10, w - 30, h4 - 14, ex, ALIGN_LEFT); }
            }
            if (ml_n < 64) { ml_idx[ml_n] = i; ml_y0[ml_n] = y; ml_y1[ml_n] = y + need; ml_n++; }
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

/* ---------- ficha de un libro, con IA de lectura ----------
 *
 * La IA está pensada para gastar lo mínimo:
 *  - Todo lo que se puede hacer sin IA se hace en el lector (epub.h): sacar el texto, saber
 *    hasta dónde has leído, encontrar los personajes y con quién salen.
 *  - «Dónde voy» resume por tramos: cada trozo del libro se manda UNA vez en la vida y se
 *    guarda su resumen. Si no has avanzado lo bastante, no se llama a la IA.
 *  - Personajes: de cada nombre se mandan dos frases, no el libro; solo los nuevos.
 *  - Explicar un subrayado: el pasaje y su párrafo. Apuntes: solo tus subrayados.
 *  - Nada pasa de tu posición, y se le prohíbe usar lo que sepa del libro: sin spoilers.
 * Todo se guarda en system/config/pockit/libros/<id>/.
 */
static void draw(void);
enum { D_FICHA, D_DONDE, D_PERSONAJES, D_SUBRAYADOS, D_APUNTES, D_COUNT };
static const char *DTABS[] = { "Ficha", "Dónde voy", "Personajes", "Subrayados", "Apuntes" };
static int dtab;
static EpBook eb;
static int eb_for = -1, eb_state;   /* libro cargado; 0 bien, 1 DRM, -1 no se puede leer */
static long readpos;
static int ai_on;
#define D_TOP 210
#define MIN_TRAMO 12000             /* ~7 páginas: por debajo no compensa llamar a la IA */
#define CHUNK 36000                 /* ~9.000 tokens por petición */

static const char *SYS_RESUMEN =
    "Resumes fragmentos de un libro para que su lector recuerde lo que ya ha leído. Usa SOLO el texto que se te da: "
    "no uses lo que sepas del libro ni anticipes nada de lo que viene después. Escribe en español, en un párrafo de "
    "120 palabras como máximo, en presente, con los nombres de los personajes en **negrita**.";
static const char *SYS_PERSONAJES =
    "Te paso nombres propios de un libro, cada uno con una o dos frases donde aparece. Usa SOLO esas frases: no uses "
    "lo que sepas del libro ni adelantes nada. Responde una línea por nombre, con este formato exacto y nada más: "
    "Nombre | personaje, lugar u otro | descripción de 12 palabras como máximo";
static const char *SYS_EXPLICA =
    "Explicas a un lector, en español y en 80 palabras como máximo, qué significa un pasaje que ha subrayado y por qué "
    "importa en ese momento del libro. Apóyate solo en el contexto que se te da y no adelantes nada de la trama.";
static const char *SYS_APUNTES =
    "Conviertes los subrayados y notas de un lector en unos apuntes breves en Markdown, en español. Usa SOLO lo que se "
    "te da: no añadas información del libro. Formato: una línea '# Apuntes de «Título»', después de 3 a 6 ideas clave "
    "como lista con viñetas, cada una con su cita más relevante entre «», y al final una línea '> En una frase: …'. "
    "Máximo 220 palabras.";

static int is_epub(const char *path) { size_t l = strlen(path); return l > 5 && !strcasecmp(path + l - 5, ".epub"); }

static void busy(const char *msg)
{
    int y = H / 2 - 80;
    FillArea(M, y, W - 2 * M, 160, WHITE);
    for (int i = 0; i < 3; i++) DrawRect(M + i, y + i, W - 2 * M - 2 * i, 160 - 2 * i, BLACK);
    SetFont(f_body, BLACK);
    DrawTextRect(M + 30, y, W - 2 * M - 60, 160, msg, ALIGN_CENTER | VALIGN_MIDDLE);
    PartialUpdate(M, y, W - 2 * M, 160);
}

/* Carga el texto del libro de la ficha (de la caché, si está). */
static void ensure_book(void)
{
    if (eb_for == detail) return;
    ep_free(&eb);
    eb_for = detail;
    readpos = 0;
    Book *b = &books[detail];
    if (!is_epub(b->path)) { eb_state = -1; return; }
    busy("Preparando el libro…");
    eb_state = ep_load(&eb, b->path);
    if (eb_state == 0) readpos = ep_position(&eb, b->position, b->done ? 1.0 : b->progress > 0 ? b->progress : 0);
}

static void cache_path(char *out, size_t n, const char *name) { snprintf(out, n, "%s/%s", eb.dir, name); }

/* «hasta aquí», gris y en un recuadro: lo que no se puede hacer y por qué */
static int hint_box(int y, const char *text)
{
    SetFont(f_body, SOFT);
    int h = TextRectHeight(W - 2 * M - 60, text, 0) + 40;
    DrawRect(M, y, W - 2 * M, h, LINE);
    DrawTextRect(M + 30, y + 20, W - 2 * M - 60, h - 40, text, ALIGN_LEFT);
    return y + h + 24;
}

/* botón grande; devuelve su fondo para comprobar toques */
static int btn_y0, btn_y1, btn_kind;   /* btn_kind: 0 ninguno, 1 resumir, 2 personajes, 3 apuntes, 4 abrir apuntes, 5 rehacer apuntes */
static int big_button(int y, const char *label, const char *sub, int kind)
{
    SetFont(f_bold, WHITE);
    int h = sub ? 120 : 90;
    FillArea(M, y, W - 2 * M, h, BLACK);
    DrawTextRect(M, y + (sub ? 14 : 0), W - 2 * M, sub ? 60 : h, label, ALIGN_CENTER | VALIGN_MIDDLE);
    if (sub) { SetFont(f_small, WHITE); DrawTextRect(M, y + 66, W - 2 * M, 40, sub, ALIGN_CENTER); }
    btn_y0 = y; btn_y1 = y + h; btn_kind = kind;
    return y + h + 24;
}

static const char *NO_AI = "Esto usa la IA. Actívala desde el ordenador: pockit.yous.dev → Tu lector → Apps → IA, pega tu clave de DeepSeek y guarda.";

/* ---- tramos (Dónde voy) ---- */
typedef struct { long start, end; char *text; } Tramo;
static Tramo tramos[400];
static int ntramos;

static void free_tramos(void) { for (int i = 0; i < ntramos; i++) free(tramos[i].text); ntramos = 0; }

static void load_tramos(void)
{
    free_tramos();
    char p[400], *buf = malloc(8192);
    cache_path(p, sizeof p, "tramos.tsv");
    FILE *f = fopen(p, "r");
    while (f && ntramos < 400 && fgets(buf, 8192, f)) {
        char *t1 = strchr(buf, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (!t2) continue;
        t2[strcspn(t2, "\n")] = 0;
        Tramo *t = &tramos[ntramos++];
        t->start = atol(buf); t->end = atol(t1 + 1);
        t->text = strdup(t2 + 1);
        for (char *q = t->text, *o = t->text; ; q++) {   /* \n escapado */
            if (q[0] == '\\' && q[1] == 'n') { *o++ = '\n'; q++; } else { *o++ = *q; if (!*q) break; }
        }
    }
    if (f) fclose(f);
    free(buf);
}

static void save_tramo(long start, long end, const char *text)
{
    char p[400];
    cache_path(p, sizeof p, "tramos.tsv");
    FILE *f = fopen(p, "a");
    if (!f) return;
    fprintf(f, "%ld\t%ld\t", start, end);
    for (const char *q = text; *q; q++) { if (*q == '\n') fputs("\\n", f); else if (*q != '\t' && *q != '\r') fputc(*q, f); }
    fputc('\n', f);
    fclose(f);
}

static long tramos_end(void) { return ntramos ? tramos[ntramos - 1].end : 0; }

static void do_resumir(int button)
{
    if (button != 1) { draw(); return; }
    Book *b = &books[detail];
    long from = tramos_end();
    char err[400] = "";
    int total = (int)((readpos - from + CHUNK - 1) / CHUNK), k = 0;
    while (from < readpos) {
        long to = from + CHUNK < readpos ? from + CHUNK : readpos;
        if (to < readpos) { long t = to; while (t > from + CHUNK / 2 && eb.text[t - 1] != '\n') t--; if (t > from + CHUNK / 2) to = t; }
        char msg[120];
        snprintf(msg, sizeof msg, "Resumiendo lo leído… %d de %d", ++k, total);
        busy(msg);
        size_t ul = (to - from) + 1200;
        char *user = malloc(ul);
        int ul0 = snprintf(user, ul, "Libro: «%s»%s%s.\nLo anterior, en breve: %.600s\n\nFragmento:\n", b->title, b->author[0] ? " de " : "", b->author,
                           ntramos ? tramos[ntramos - 1].text : "Es el principio del libro.");
        size_t ulen = ul0;
        for (int c = 0; c < eb.nch; c++) {
            EpChapter *chp = &eb.ch[c];
            long cs = chp->start > from ? chp->start : from, ce = chp->start + chp->len < to ? chp->start + chp->len : to;
            if (ce <= cs || ep_is_toc(chp) || chp->len < 400) continue;   /* índice, créditos, portadillas */
            for (long q = cs; q < ce; ) {   /* y dentro, sin listas ni avisos legales */
                long sk = ep_skip_to(&eb, q);
                if (sk != q) { q = sk; continue; }
                long nx = ce;
                for (int k = 0; k < eb.nskip; k++) if (eb.skip[2 * k] > q && eb.skip[2 * k] < nx) nx = eb.skip[2 * k];
                memcpy(user + ulen, eb.text + q, nx - q);
                ulen += nx - q;
                q = nx;
            }
            user[ulen++] = '\n';
        }
        user[ulen] = 0;
        if (ulen - ul0 < 200) { free(user); save_tramo(from, to, "—"); load_tramos(); from = to; continue; }
        char *r = ai_complete(SYS_RESUMEN, user, 260, "Lectura · resumen", err, sizeof err);
        free(user);
        if (!r) break;
        save_tramo(from, to, r);
        free(r);
        load_tramos();
        from = to;
    }
    if (err[0]) Message(ICON_WARNING, "IA", err, 4000);
    draw();
}

static void draw_donde(int y)
{
    Book *b = &books[detail];
    load_tramos();
    int ci = ep_chapter_at(&eb, readpos);
    char line[400];
    if (eb.ch[ci].title[0] && strlen(eb.ch[ci].title) < 70) snprintf(line, sizeof line, "Vas por «%s» · %d %%", eb.ch[ci].title, (int)(100.0 * readpos / (eb.len ? eb.len : 1) + 0.5));
    else snprintf(line, sizeof line, "Llevas el %d %% del libro", (int)(100.0 * readpos / (eb.len ? eb.len : 1) + 0.5));
    SetFont(f_small, SOFT);
    DrawTextRect(M, y, W - 2 * M, 40, line, ALIGN_LEFT | DOTS);
    y += 50;
    if (detail_page == 0) {
        /* sin IA: los últimos párrafos que leíste */
        SetFont(f_bold, BLACK);
        DrawString(M, y, "Lo último que leíste");
        y += 56;
        long s = readpos > 520 ? readpos - 520 : 0;
        while (s > 0 && eb.text[s - 1] != '\n' && readpos - s < 900) s--;
        static char last[1100];
        snprintf(last, sizeof last, "%.*s", (int)(readpos - s), eb.text + s);
        for (char *q = last; *q; q++) if (*q == '\n') *q = ' ';
        SetFont(f_ital, BLACK);
        int h = TextRectHeight(W - 2 * M - 30, last, 0);
        if (h > 300) h = 300;
        FillArea(M, y, 6, h, SOFT);
        DrawTextRect(M + 30, y, W - 2 * M - 30, h, last[0] ? last : "Aún no has empezado.", ALIGN_LEFT | DOTS);
        y += h + 40;
    }
    /* el botón: solo si hay bastante texto nuevo; si no, se enseña lo guardado sin gastar */
    long pending = readpos - tramos_end();
    if (detail_page == 0) {
        if (!ai_on) y = hint_box(y, NO_AI);
        else if (pending >= MIN_TRAMO) {
            char sub[160], mny[32];
            ai_money(ai_estimate(pending, 260 * (int)((pending + CHUNK - 1) / CHUNK)), mny, sizeof mny);
            snprintf(sub, sizeof sub, "%ld páginas nuevas · ≈ %s", pending / 1800 + 1, mny);
            y = big_button(y, ntramos ? "Resumir lo leído desde la última vez" : "Resumir lo que llevo leído", sub, 1);
        } else if (ntramos) {
            SetFont(f_small, SOFT);
            DrawTextRect(M, y, W - 2 * M, 40, "Resumen al día. Se podrá ampliar cuando avances unas páginas.", ALIGN_LEFT);
            y += 60;
        } else {
            y = hint_box(y, "Aún llevas poco leído para resumir. Vuelve cuando avances unas páginas.");
        }
    }
    /* la historia hasta aquí: los resúmenes guardados, en orden */
    if (!ntramos) { last_page_end = 1; return; }
    if (detail_page == 0) { SetFont(f_bold, BLACK); DrawString(M, y, "La historia hasta aquí"); y += 60; }
    MdCtx c = { f_body, f_bold, f_ital, f_ital, f_bold, f_bold, f_body, M, W - M, 0, H - TABS_H - PAGER_H - 10, 46, 12, 0, SOFT };
    int bottom = H - TABS_H - PAGER_H - 10, pg = 0, yy = detail_page == 0 ? y : D_TOP + 50, i = 0;
    for (; i < ntramos; i++) {
        int chi = ep_chapter_at(&eb, tramos[i].start);
        for (int c = chi; c < eb.nch && eb.ch[c].start < tramos[i].end; c++)   /* el primer capítulo de verdad del tramo */
            if (!ep_is_toc(&eb.ch[c]) && eb.ch[c].len >= 400) { chi = c; break; }
        char head[300];
        snprintf(head, sizeof head, "%s", eb.ch[chi].title[0] ? eb.ch[chi].title : "Capítulo");
        c.top = 0; c.scroll_row = 0;
        MdState st = { 0, 0 };
        int rows = md_flow(&c, tramos[i].text, M, 0, BLACK, 0, 0, NULL) + 1;   /* medir */
        (void)st;
        int need = rows * 46 + 60;
        if (yy + need > bottom && yy > (pg == 0 ? y : D_TOP + 50)) { if (pg == detail_page) break; pg++; yy = D_TOP + 50; }
        if (pg == detail_page) {
            SetFont(f_smallb, SOFT);
            DrawTextRect(M, yy, W - 2 * M, 36, head, ALIGN_LEFT | DOTS);
            c.top = yy + 30; c.scroll_row = 0; c.bottom = bottom;
            md_flow(&c, tramos[i].text, M, 0, BLACK, 0, 1, NULL);
        }
        yy += need;
    }
    last_page_end = i >= ntramos;
    (void)b;
}

/* ---- personajes ---- */
typedef struct { char name[64], kind[24], desc[200]; } PersonAI;
static EpPerson people[30];
static int npeople = -1;
static long people_pos = -1;
static PersonAI pai[200];
static int npai;

static int people_for = -1;
static void load_people(void)
{
    if (people_for != detail) { npeople = -1; people_for = detail; }
    if (people_pos != readpos || npeople < 0) {
        busy("Buscando personajes…");
        EpPerson tmp[40];
        int n = ep_people(&eb, readpos, tmp, 40);
        npeople = 0;
        const char *author = books[detail].author;
        for (int i = 0; i < n && npeople < 30; i++) if (!strstr(author, tmp[i].name)) people[npeople++] = tmp[i];
        people_pos = readpos;
    }
    npai = 0;
    char p[400], buf[600];
    cache_path(p, sizeof p, "personajes.tsv");
    FILE *f = fopen(p, "r");
    while (f && npai < 200 && fgets(buf, sizeof buf, f)) {
        char *t1 = strchr(buf, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (!t2) continue;
        *t1 = *t2 = 0;
        t2[1 + strcspn(t2 + 1, "\n")] = 0;
        snprintf(pai[npai].name, 64, "%s", buf); snprintf(pai[npai].kind, 24, "%s", t1 + 1); snprintf(pai[npai].desc, 200, "%s", t2 + 1);
        npai++;
    }
    if (f) fclose(f);
}

static PersonAI *person_ai(const char *name) { for (int i = 0; i < npai; i++) if (!strcmp(pai[i].name, name)) return &pai[i]; return NULL; }
static int people_new(void) { int n = 0; for (int i = 0; i < npeople && i < 20; i++) if (!person_ai(people[i].name)) n++; return n; }

static void do_personajes(int button)
{
    if (button != 1) { draw(); return; }
    busy("Describiendo personajes…");
    size_t ul = 20000;
    char *user = malloc(ul), ctx[800], err[400] = "";
    snprintf(user, ul, "Libro: «%s».\n", books[detail].title);
    int n = 0;
    for (int i = 0; i < npeople && i < 20; i++) {
        if (person_ai(people[i].name)) continue;
        ep_context(&eb, people[i].name, readpos, ctx, sizeof ctx);
        size_t l = strlen(user);
        snprintf(user + l, ul - l, "\n%s: %s\n", people[i].name, ctx);
        n++;
    }
    char *r = n ? ai_complete(SYS_PERSONAJES, user, 40 * n + 60, "Lectura · personajes", err, sizeof err) : NULL;
#ifdef EMULATOR
    { FILE *dbg = fopen(FLASHDIR "/pockit-debug.txt", "a"); if (dbg) { fprintf(dbg, "personajes n=%d err=[%s] r=[%.300s]\n", n, err, r ? r : ""); fclose(dbg); } }
#endif
    free(user);
    if (r) {
        char p[400];
        cache_path(p, sizeof p, "personajes.tsv");
        FILE *f = fopen(p, "a");
        for (char *line = strtok(r, "\n"); line && f; line = strtok(NULL, "\n")) {
            char *a = strchr(line, '|'), *bb = a ? strchr(a + 1, '|') : NULL;
            if (!bb) continue;
            *a = *bb = 0;
            char *nm = line, *kd = a + 1, *ds = bb + 1;
            while (*nm == ' ' || *nm == '-' || *nm == '*') nm++;
            while (*kd == ' ') kd++;
            while (*ds == ' ') ds++;
            for (char *e = nm + strlen(nm); e > nm && (e[-1] == ' ' || e[-1] == '*'); ) *--e = 0;
            for (char *e = kd + strlen(kd); e > kd && e[-1] == ' '; ) *--e = 0;
            fprintf(f, "%s\t%s\t%s\n", nm, kd, ds);
        }
        if (f) fclose(f);
        free(r);
    } else if (err[0]) Message(ICON_WARNING, "IA", err, 4000);
    draw();
}

#define P_ROW 150
static void draw_personajes(int y)
{
    load_people();
    if (detail_page == 0) {
        SetFont(f_small, SOFT);
        DrawTextRect(M, y, W - 2 * M, 40, "Los nombres que más aparecen hasta donde vas, sin destripes.", ALIGN_LEFT);
        y += 56;
        int nn = people_new();
        if (!npeople) y = hint_box(y, "Aún no aparecen nombres que se repitan.");
        else if (!ai_on) y = hint_box(y, NO_AI);
        else if (nn) {
            char sub[120], mny[32];
            ai_money(ai_estimate(nn * 450, 40 * nn), mny, sizeof mny);
            snprintf(sub, sizeof sub, "%d nombre%s nuevo%s · ≈ %s", nn, nn == 1 ? "" : "s", nn == 1 ? "" : "s", mny);
            y = big_button(y, "Describir con IA quién es quién", sub, 2);
        }
    } else y = D_TOP + 50;
    int per = (H - TABS_H - PAGER_H - y) / P_ROW, first = detail_page == 0 ? 0 : 0;
    /* páginas: la primera empieza tras el botón; el resto, arriba */
    int per0 = (H - TABS_H - PAGER_H - (detail_page == 0 ? y : 0)) / P_ROW;
    (void)per0;
    int start = 0;
    if (detail_page > 0) {
        int y0 = D_TOP + 50 + 56 + (ai_on && people_new() ? 144 : 0);
        int p0 = (H - TABS_H - PAGER_H - y0) / P_ROW, pn = (H - TABS_H - PAGER_H - (D_TOP + 50)) / P_ROW;
        start = p0 + (detail_page - 1) * pn;
    }
    (void)first;
    int i = start;
    for (; i < npeople && y + P_ROW <= H - TABS_H - PAGER_H; i++, y += P_ROW) {
        EpPerson *p = &people[i];
        PersonAI *a = person_ai(p->name);
        SetFont(f_bold, BLACK);
        char nm[120];
        snprintf(nm, sizeof nm, "%s%s%s", p->name, a && a->kind[0] && strncmp(a->kind, "personaje", 9) ? "  · " : "", a && strncmp(a->kind, "personaje", 9) ? a->kind : "");
        DrawTextRect(M, y, W - 2 * M, 46, nm, ALIGN_LEFT | DOTS);
        char info[300];
        snprintf(info, sizeof info, "%d menciones · desde «%s»", p->count, eb.ch[p->first_ch].title[0] ? eb.ch[p->first_ch].title : "el principio");
        if (p->nwith) {
            strncat(info, " · con ", sizeof info - strlen(info) - 1);
            for (int k = 0; k < p->nwith; k++) { if (k) strncat(info, ", ", sizeof info - strlen(info) - 1); strncat(info, people[p->with[k]].name, sizeof info - strlen(info) - 1); }
        }
        SetFont(f_small, SOFT);
        DrawTextRect(M, y + 48, W - 2 * M, 36, info, ALIGN_LEFT | DOTS);
        if (a && a->desc[0]) { SetFont(f_ital, BLACK); DrawTextRect(M, y + 88, W - 2 * M, 44, a->desc, ALIGN_LEFT | DOTS); }
        DrawLine(M, y + P_ROW - 10, W - M, y + P_ROW - 10, LINE);
    }
    last_page_end = i >= npeople;
    (void)per;
}

/* ---- explicar subrayados ---- */
static char expl_q[80][120], *expl_t[80];
static int nexpl = -1;

static void load_expl(void)
{
    for (int i = 0; i < nexpl; i++) free(expl_t[i]);
    nexpl = 0;
    char p[400], *buf = malloc(4096);
    cache_path(p, sizeof p, "explicaciones.tsv");
    FILE *f = fopen(p, "r");
    while (f && nexpl < 80 && fgets(buf, 4096, f)) {
        char *t = strchr(buf, '\t');
        if (!t) continue;
        *t = 0;
        t[1 + strcspn(t + 1, "\n")] = 0;
        snprintf(expl_q[nexpl], 120, "%s", buf);
        expl_t[nexpl++] = strdup(t + 1);
    }
    if (f) fclose(f);
    free(buf);
}

static void quote_key(const char *q, char *out, size_t n) { snprintf(out, n, "%016llx", ep_hash(q)); }

static const char *explanation(const char *quote)
{
    char k[24];
    quote_key(quote, k, sizeof k);
    for (int i = 0; i < nexpl; i++) if (!strcmp(expl_q[i], k)) return expl_t[i];
    return NULL;
}

static int pending_mark = -1;
static int auto_action;
static void do_explicar(int button)
{
    int mi = pending_mark;
    pending_mark = -1;
    if (button != 1 || mi < 0) { draw(); return; }
    Mark *m = &marks[mi];
    busy("Explicando el pasaje…");
    /* contexto: el párrafo del subrayado y el anterior, buscado en el texto (sin pasar de tu posición) */
    char probe[64];
    snprintf(probe, sizeof probe, "%.40s", m->text);
    const char *hit = eb_state == 0 ? strstr(eb.text, probe) : NULL;
    static char ctx[2400];
    ctx[0] = 0;
    if (hit) {
        const char *s = hit, *e = hit;
        int pars = 0;
        while (s > eb.text && pars < 2) { s--; if (*s == '\n') pars++; }
        while (*e && *e != '\n' && e - hit < 1200) e++;
        snprintf(ctx, sizeof ctx, "%.*s", (int)(e - s), s);
    }
    size_t ul = strlen(m->text) + strlen(ctx) + 600;
    char *user = malloc(ul), err[400] = "";
    snprintf(user, ul, "Libro: «%s».\nPasaje subrayado: «%s»\n%s%s", m->book, m->text, ctx[0] ? "\nContexto:\n" : "", ctx);
    char *r = ai_complete(SYS_EXPLICA, user, 180, "Lectura · explicar", err, sizeof err);
    free(user);
    if (r) {
        char p[400], k[24];
        cache_path(p, sizeof p, "explicaciones.tsv");
        quote_key(m->text, k, sizeof k);
        FILE *f = fopen(p, "a");
        if (f) { fprintf(f, "%s\t", k); for (char *q = r; *q; q++) fputc(*q == '\n' || *q == '\t' ? ' ' : *q, f); fputc('\n', f); fclose(f); }
        free(r);
        load_expl();
    } else Message(ICON_WARNING, "IA", err, 4000);
    draw();
}

/* ---- apuntes ---- */
static void apuntes_paths(char *note, size_t nn, char *hash, size_t hn)
{
    snprintf(note, nn, FLASHDIR "/Notas/apuntes-%s.md", eb_state == 0 ? eb.id : "libro");
    unsigned long long h = 1469598103934665603ULL;
    for (int i = 0; i < nmarks; i++)
        if (!marks[i].word && !strcmp(marks[i].book, books[detail].title)) { h ^= ep_hash(marks[i].text); h *= 1099511628211ULL; if (marks[i].note) h ^= ep_hash(marks[i].note); }
    snprintf(hash, hn, "%016llx", h);
}

static int apuntes_stale(void)
{
    char note[400], hash[24], p[400], old[40] = "";
    apuntes_paths(note, sizeof note, hash, sizeof hash);
    cache_path(p, sizeof p, "apuntes.hash");
    FILE *f = fopen(p, "r");
    if (f) { if (fgets(old, sizeof old, f)) old[strcspn(old, "\n")] = 0; fclose(f); }
    return strcmp(old, hash) != 0;
}

static void do_apuntes(int button)
{
    if (button != 1) { draw(); return; }
    Book *b = &books[detail];
    busy("Creando tus apuntes…");
    size_t ul = 30000;
    char *user = malloc(ul), err[400] = "";
    snprintf(user, ul, "Libro: «%s»%s%s.\nSubrayados y notas:\n", b->title, b->author[0] ? " de " : "", b->author);
    int n = 0;
    for (int i = nmarks - 1; i >= 0 && n < 40; i--) {   /* en orden de lectura (las marcas van de nuevas a viejas) */
        Mark *m = &marks[i];
        if (m->word || strcmp(m->book, b->title)) continue;
        size_t l = strlen(user);
        snprintf(user + l, ul - l, "- «%.300s»%s%.200s%s\n", m->text, m->note && m->note[0] ? " (nota: " : "", m->note ? m->note : "", m->note && m->note[0] ? ")" : "");
        n++;
    }
    char *r = ai_complete(SYS_APUNTES, user, 420, "Lectura · apuntes", err, sizeof err);
    free(user);
    if (r) {
        char note[400], hash[24], p[400];
        apuntes_paths(note, sizeof note, hash, sizeof hash);
        mkdir(FLASHDIR "/Notas", 0755);
        FILE *f = fopen(note, "w");
        if (f) { fputs(r, f); fputc('\n', f); fclose(f); }
        cache_path(p, sizeof p, "apuntes.hash");
        if ((f = fopen(p, "w"))) { fprintf(f, "%s\n", hash); fclose(f); }
        free(r);
    } else Message(ICON_WARNING, "IA", err, 4000);
    draw();
}

static void draw_apuntes(int y)
{
    Book *b = &books[detail];
    char note[400], hash[24];
    apuntes_paths(note, sizeof note, hash, sizeof hash);
    FILE *f = fopen(note, "r");
    if (!f) {
        if (b->marks < 2) y = hint_box(y, "Subraya algunos pasajes mientras lees y aquí podrás convertirlos en unos apuntes del libro.");
        else if (!ai_on) y = hint_box(y, NO_AI);
        else {
            char sub[120], mny[32];
            ai_money(ai_estimate(b->marks * 250, 420), mny, sizeof mny);
            snprintf(sub, sizeof sub, "con tus %d subrayados · ≈ %s", b->marks, mny);
            y = big_button(y, "Crear mis apuntes con IA", sub, 3);
            SetFont(f_small, SOFT);
            DrawTextRect(M, y, W - 2 * M, 80, "Se guardan como una nota del Cuaderno, para leerlos o editarlos cuando quieras.", ALIGN_LEFT);
        }
        last_page_end = 1;
        return;
    }
    /* los apuntes ya hechos, con el mismo Markdown del Cuaderno */
    if (detail_page == 0) {
        y = big_button(y, "Abrir en el Cuaderno", NULL, 4);
        if (ai_on && apuntes_stale()) {
            SetFont(f_small, BLACK);
            DrawTextRect(M, y - 10, W - 2 * M, 40, "Tienes subrayados nuevos: toca aquí para rehacerlos.", ALIGN_LEFT);
            btn_kind = 5;   /* la franja de debajo rehace */
            btn_y1 = y + 40;
            y += 50;
        }
    } else y = D_TOP + 50;
    MdCtx c = { f_body, f_bold, f_ital, f_ital, f_title, f_bold, f_body, M, W - M, y, H - TABS_H - PAGER_H - 10, 50, 12, 0, SOFT };
    MdState st = { 0, 0 };
    char line[2048];
    int row = 0, total = 0;
    /* medir y pintar la página que toca: cada página son las filas que caben */
    int rows_page = (c.bottom - c.top) / c.lh;
    c.scroll_row = detail_page * rows_page;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        row += md_line(&c, &st, line, row, 1);
    }
    fclose(f);
    total = row;
    last_page_end = c.scroll_row + rows_page >= total;
}

static void draw_detail(void)
{
    Book *b = &books[detail];
    ai_on = ai_load_cfg();
    btn_kind = 0;
    if (!ml_extra) ml_extra = explanation;
    SetFont(f_head, BLACK);
    DrawString(M - 20, 42, "‹ Libros");
    SetFont(f_smallb, SOFT);
    DrawTextRect(W / 3 - 40, 52, W * 2 / 3 - M + 40, 40, b->title, ALIGN_RIGHT | DOTS);
    /* pestañas de la ficha */
    int tw = (W - 2 * M) / D_COUNT;
    for (int i = 0; i < D_COUNT; i++) {
        int on = i == dtab, x = M + i * tw;
        if (on) FillArea(x, 120, tw - 6, 64, BLACK); else DrawRect(x, 120, tw - 6, 64, LINE);
        SetFont(f_small, on ? WHITE : BLACK);
        DrawTextRect(x, 120, tw - 6, 64, DTABS[i], ALIGN_CENTER | VALIGN_MIDDLE);
    }
    int y = D_TOP;
    if (dtab != D_FICHA && dtab != D_SUBRAYADOS && dtab != D_APUNTES) {
        ensure_book();
        if (eb_state != 0) {
            hint_box(y, eb_state == 1 ? "Este libro tiene DRM (por ejemplo, un préstamo de la biblioteca): su texto está cifrado y no se puede analizar."
                                      : "Esto funciona con libros en ePub. Este libro está en otro formato.");
            last_page_end = 1;
            pager(0, 1);
            return;
        }
    }
    switch (dtab) {
    case D_FICHA: {
        cover(b, M, y, 240, 360);
        int tx = M + 280, tw2 = W - M - tx;
        SetFont(f_title, BLACK);
        int th = TextRectHeight(tw2, b->title, 0);
        if (th > 230) th = 230;
        DrawTextRect(tx, y, tw2, th, b->title, ALIGN_LEFT | DOTS);
        SetFont(f_body, SOFT);
        DrawTextRect(tx, y + th + 10, tw2, 44, b->author[0] ? b->author : " ", ALIGN_LEFT | DOTS);
        int yy = y + th + 80;
        if (b->progress > 0) {
            bar(tx, yy, tw2, 16, b->done ? 1 : b->progress);
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
        if (b->words) {
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
        if (is_epub(b->path) && y + 200 < H - TABS_H) {
            DrawLine(M, y, W - M, y, LINE);
            SetFont(f_small, SOFT);
            DrawTextRect(M, y + 20, W - 2 * M, 120, ai_load_cfg() ? "Con la IA: «Dónde voy» te resume lo leído sin destripes, «Personajes» explica quién es quién y «Apuntes» convierte tus subrayados en notas."
                                                                  : "«Dónde voy» y «Personajes» funcionan sin IA. Con tu clave de DeepSeek (pockit.yous.dev) además resumen y explican.", ALIGN_LEFT);
        }
        last_page_end = 1;
        break;
    }
    case D_DONDE: draw_donde(y); break;
    case D_PERSONAJES: draw_personajes(y); break;
    case D_SUBRAYADOS:

        if (!b->marks) { hint_box(y, "Aún no has subrayado nada en este libro."); last_page_end = 1; break; }
        SetFont(f_small, SOFT);
        DrawTextRect(M, y, W - 2 * M, 40, ai_on ? "Toca un subrayado para que la IA te lo explique en su contexto." : "Con la IA activada podrás tocar un subrayado para que te lo explique.", ALIGN_LEFT);
        last_page_end = draw_mark_list(0, b->title, detail_page, y + 56, H - TABS_H - PAGER_H - 10);
        break;
    case D_APUNTES:
        if (eb_for != detail) ensure_book();
        draw_apuntes(y);
        break;
    }
    pager(detail_page, last_page_end ? detail_page + 1 : detail_page + 2);
}

static void detail_tap(int x, int y)
{
    if (y >= 120 && y < 184) {
        int t = (x - M) / ((W - 2 * M) / D_COUNT);
        if (t >= 0 && t < D_COUNT && t != dtab) { dtab = t; detail_page = 0; draw(); }
        return;
    }
    if (btn_kind && y >= btn_y0 && y < btn_y1) {
        static char q[400];
        char mny[32];
        switch (btn_kind) {
        case 1: {
            long pending = readpos - tramos_end();
            ai_money(ai_estimate(pending, 260 * (int)((pending + CHUNK - 1) / CHUNK)), mny, sizeof mny);
            snprintf(q, sizeof q, "Se enviarán a DeepSeek unas %ld páginas que aún no ha resumido (≈ %s). Lo ya resumido no se vuelve a mandar.", pending / 1800 + 1, mny);
            Dialog(ICON_QUESTION, "Resumir", q, "Resumir", "Cancelar", do_resumir);
            return;
        }
        case 2: do_personajes(1); return;
        case 3: do_apuntes(1); return;
        case 4: { char note[400], h[24]; apuntes_paths(note, sizeof note, h, sizeof h); cuaderno_open_note(note); return; }
        case 5: if (y >= btn_y1 - 40) { do_apuntes(1); return; } else { char note[400], h[24]; apuntes_paths(note, sizeof note, h, sizeof h); cuaderno_open_note(note); return; }
        }
    }
    if (dtab == D_SUBRAYADOS && ai_on) {
        for (int i = 0; i < ml_n; i++)
            if (y >= ml_y0[i] && y < ml_y1[i]) {
                if (explanation(marks[ml_idx[i]].text)) return;   /* ya explicado: no se vuelve a pagar */
                pending_mark = ml_idx[i];
                char mny[32];
                ai_money(ai_estimate(strlen(marks[pending_mark].text) + 1500, 180), mny, sizeof mny);
                static char q[200];
                snprintf(q, sizeof q, "¿Explicar este pasaje con IA? (≈ %s)", mny);
                Dialog(ICON_QUESTION, "Explicar", q, "Explicar", "Cancelar", do_explicar);
                return;
            }
    }
}

static void prepare_detail(void)
{
    if (detail < 0) return;
    if (dtab != D_FICHA) ensure_book();
    if (dtab == D_PERSONAJES && eb_state == 0 && (people_pos != readpos || npeople < 0)) load_people();
    if (dtab == D_SUBRAYADOS && eb_state == 0) load_expl();
}

static void draw(void)
{
    prepare_detail();
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
    if (detail < 0 && !(tab == PALABRAS && vcards) && y < 45 && x < W / 3) { CloseApp(); return; }
    int py = H - TABS_H - PAGER_H;
    if ((detail >= 0 || tab == LIBROS || tab == SUBRAYADOS) && y > py) { page(x > W / 2 ? 1 : -1); return; }
    if (detail >= 0) { detail_tap(x, y); return; }
    switch (tab) {
    case RESUMEN:
        for (int i = 0; i < nreading; i++) {
            int yy = 444 + i * 210;
            if (y >= yy && y < yy + 200) { detail = reading_idx[i]; detail_page = 0; dtab = D_FICHA; draw(); return; }
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
        if (y >= 190 && k < libros_rows() && i < nbooks) { detail = i; detail_page = 0; dtab = D_FICHA; draw(); }
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
        if (!nevents) { load_vocab(); build_events(); }
        time_t now = time(NULL);
        struct tm tn;
        localtime_r(&now, &tn);
        cal_year = tn.tm_year + 1900; cal_month = tn.tm_mon;
        break;
    }
    case EVT_SHOW:
        draw();
        if (auto_action) {   /* pruebas en el emulador: lanzar la acción sin tocar la pantalla */
            int a = auto_action;
            auto_action = 0;
            if (a == 1) do_resumir(1);
            else if (a == 2) do_personajes(1);
            else if (a == 3) do_apuntes(1);
            else if (a == 4 && ml_n) { pending_mark = ml_idx[0]; do_explicar(1); }
        }
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

void lectura_args(int argc, char *argv[])
{
    /* para las capturas del emulador: «lectura <pestaña> [libro|vuelta]» */
    if (argc > 1) tab = atoi(argv[1]);
    if (argc > 2 && tab == LIBROS) detail = atoi(argv[2]);
    if (argc > 3 && tab == LIBROS) dtab = atoi(argv[3]);
    if (argc > 4 && tab == LIBROS) auto_action = atoi(argv[4]);   /* 1 resumir, 2 personajes, 3 apuntes, 4 explicar el primero */
    if (argc > 2 && tab == PALABRAS) { vcards = 1; card_back = atoi(argv[2]); }
    if (argc > 2 && tab == ACTIVIDAD) cal_day = atoi(argv[2]);
}

int lectura_handler(int type, int par1, int par2) { return handler(type, par1, par2); }

/* ---------- lo que Lectura le da al Inicio ---------- */
static void ensure_data(void) { if (!data_loaded) { load_data(); load_vocab(); build_events(); } }

int lectura_now(PockitReading *out)
{
    ensure_data();
    memset(out, 0, sizeof *out);
    for (int i = 0; i < nbooks; i++) {
        Book *b = &books[i];
        if (b->done || b->progress <= 0) continue;
        snprintf(out->title, sizeof out->title, "%s", b->title);
        snprintf(out->author, sizeof out->author, "%s", b->author);
        snprintf(out->cover, sizeof out->cover, "%s", b->cover);
        out->progress = b->progress; out->page = b->page; out->pages = b->pages; out->opened = b->opened;
        out->ok = 1;
        return 1;
    }
    return 0;
}

int lectura_week(int active[7])
{
    ensure_data();
    time_t now = time(NULL);
    struct tm tn;
    localtime_r(&now, &tn);
    int wd = (tn.tm_wday + 6) % 7;
    long today = day_start(now);
    for (int d = 0; d < 7; d++) active[d] = d <= wd && active_day(today + (d - wd) * 86400L);
    return wd;
}

void lectura_open_current(int sub)
{
    ensure_data();
    for (int i = 0; i < nbooks; i++)
        if (!books[i].done && books[i].progress > 0) { detail = i; detail_page = 0; dtab = sub; break; }
    pockit_open(MOD_LECTURA);
}
