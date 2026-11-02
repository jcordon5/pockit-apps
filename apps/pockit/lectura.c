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
#include <dirent.h>
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
    int gone;          /* ya no está en el lector: sale de su ficha en Libros/<etiqueta>/ */
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

/* Ficha mínima de cada libro empezado, en Libros/<etiqueta>/ficha.ini: si lo borras del
 * lector, sigue saliendo en Lectura (con sus subrayados, resúmenes y notas). No es .txt:
 * el lector cree que un .txt es un libro y lo pone en la biblioteca (la 2.2.0 lo hacía). */
static void save_ficha(const Book *b)
{
    char dir[800], p[900], now[2600], old[2600] = "";
    pockit_book_dir(b->title, dir, sizeof dir);
    snprintf(p, sizeof p, "%s/ficha.txt", dir);
    remove(p);
    snprintf(p, sizeof p, "%s/ficha.ini", dir);
    snprintf(now, sizeof now, "title=%s\nauthor=%s\ncover=%s\nadded=%ld\nopened=%ld\ncompleted=%ld\npage=%d\npages=%d\ndone=%d\n",
             b->title, b->author, b->cover, b->added, b->opened, b->completed_ts, b->page, b->pages, b->done);
    FILE *f = fopen(p, "r");
    if (f) { size_t r = fread(old, 1, sizeof old - 1, f); old[r] = 0; fclose(f); }
    if (strcmp(old, now) && (f = fopen(p, "w"))) { fputs(now, f); fclose(f); }   /* solo si cambió: con Dropbox, cada escritura se sube */
}

static void load_gone(void)
{
    char libros[700];
    pockit_path(libros, sizeof libros, "Libros");
    DIR *d = opendir(libros);
    struct dirent *e;
    while (d && (e = readdir(d)) && nbooks < MAX_BOOKS) {
        if (e->d_name[0] == '.') continue;
        char p[1100], old[1100], line[1300];
        snprintf(p, sizeof p, "%s/%s/ficha.ini", libros, e->d_name);
        snprintf(old, sizeof old, "%s/%s/ficha.txt", libros, e->d_name);
        rename(old, p);   /* las de la 2.2.0 */
        FILE *f = fopen(p, "r");
        if (!f) continue;
        Book b = { 0 };
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\n")] = 0;
            char *v = strchr(line, '=');
            if (!v) continue;
            *v++ = 0;
            if (!strcmp(line, "title")) b.title = strdup(v);
            else if (!strcmp(line, "author")) b.author = strdup(v);
            else if (!strcmp(line, "cover")) b.cover = strdup(v);
            else if (!strcmp(line, "added")) b.added = atol(v);
            else if (!strcmp(line, "opened")) b.opened = atol(v);
            else if (!strcmp(line, "completed")) b.completed_ts = atol(v);
            else if (!strcmp(line, "page")) b.page = atoi(v);
            else if (!strcmp(line, "pages")) b.pages = atoi(v);
            else if (!strcmp(line, "done")) b.done = atoi(v);
        }
        fclose(f);
        if (!b.title || book_by_title(b.title)) { free(b.title); free(b.author); free(b.cover); continue; }
        b.author = b.author ? b.author : strdup("");
        b.cover = b.cover ? b.cover : strdup("");
        b.path = strdup("");
        b.position = strdup("");
        b.progress = b.pages > 0 ? (double)b.page / b.pages : -1;
        b.gone = 1;
        books[nbooks++] = b;
    }
    if (d) closedir(d);
}

/* Algunos diccionarios guardan la definición con formato HTML («<div><b>1.</b> …</div>»):
 * se queda el texto, cada bloque en su línea. */
static void strip_html(char *s)
{
    char *w = s;
    for (char *r = s; *r; ) {
        if (*r == '<') {
            int block = !strncasecmp(r, "<div", 4) || !strncasecmp(r, "</div", 5) || !strncasecmp(r, "<br", 3) || !strncasecmp(r, "<p", 2) || !strncasecmp(r, "</p", 3);
            char *e = strchr(r, '>');
            if (!e) break;
            if (block && w > s && w[-1] != '\n') *w++ = '\n';
            r = e + 1;
            continue;
        }
        if (*r == '&') {
            static const char *ENT[][2] = { { "&amp;", "&" }, { "&lt;", "<" }, { "&gt;", ">" }, { "&quot;", "\"" }, { "&#39;", "'" }, { "&nbsp;", " " } };
            int done = 0;
            for (int i = 0; i < 6 && !done; i++) { size_t l = strlen(ENT[i][0]); if (!strncmp(r, ENT[i][0], l)) { *w++ = ENT[i][1][0]; r += l; done = 1; } }
            if (done) continue;
        }
        *w++ = *r++;
    }
    *w = 0;
    while (w > s && (w[-1] == '\n' || w[-1] == ' ')) *--w = 0;
}

/* Los subrayados y palabras de cada libro, también en Libros/<etiqueta>/subrayados.md: así se
 * ven en la web (Dropbox) sin conectar el lector. Solo se escribe si cambió. */
static char vstate[MAX_MARKS];   /* por marca: 0 por aprender, 'a' aprendida, 'q' quitada (ver «vocabulario») */
static void export_marks(void)
{
    static const char *MES[] = { "ene", "feb", "mar", "abr", "may", "jun", "jul", "ago", "sep", "oct", "nov", "dic" };
    for (int b = 0; b < nbooks; b++) {
        Book *bk = &books[b];
        if (!bk->marks && !bk->words) continue;
        JBuf md = { 0 };
        char line[600];
        snprintf(line, sizeof line, "# Subrayados de «%s»\n", bk->title);
        jb_cat(&md, line);
        for (int pass = 0; pass < 2; pass++) {
            int first = 1;
            for (int i = nmarks - 1; i >= 0; i--) {   /* del más viejo al más nuevo: el orden de lectura */
                Mark *m = &marks[i];
                if (m->word != pass || strcmp(m->book, bk->title) || (pass && vstate[i] == 'q')) continue;
                if (first && pass) jb_cat(&md, "\n## Palabras\n");
                first = 0;
                struct tm tm;
                time_t t = m->ts;
                localtime_r(&t, &tm);
                if (!pass) {
                    jb_cat(&md, "\n> ");
                    for (const char *q = m->text; *q; q++) { if (*q == '\n') jb_cat(&md, "\n> "); else jb_put(&md, q, 1); }
                    if (m->note && m->note[0]) { jb_cat(&md, "\n\n"); jb_cat(&md, m->note); }
                    snprintf(line, sizeof line, "\n\n*%d %s %d*\n", tm.tm_mday, MES[tm.tm_mon], tm.tm_year + 1900);
                    jb_cat(&md, line);
                } else {
                    snprintf(line, sizeof line, "\n- **%s**%s: ", m->text, vstate[i] == 'a' ? " (aprendida)" : "");
                    jb_cat(&md, line);
                    char def[300];
                    snprintf(def, sizeof def, "%.250s", m->note ? m->note : "");
                    for (char *q = def; *q; q++) if (*q == '\n') *q = ' ';
                    jb_cat(&md, def);
                }
            }
        }
        jb_cat(&md, "\n");
        char dir[800], p[900];
        pockit_book_dir(bk->title, dir, sizeof dir);
        snprintf(p, sizeof p, "%s/subrayados.md", dir);
        FILE *f = fopen(p, "r");
        int same = 0;
        if (f) {
            char *old = malloc(md.len + 2);
            size_t r = fread(old, 1, md.len + 1, f);
            same = r == (size_t)md.len && !memcmp(old, md.s, md.len);
            free(old);
            fclose(f);
        }
        if (!same && (f = fopen(p, "w"))) { fwrite(md.s, 1, md.len, f); fclose(f); }
        free(md.s);
    }
}

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
            if (strstr(folder, "/Pockit/")) continue;   /* los archivos de Pockit no son libros */
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
    for (int i = 0; i < nbooks; i++) if (books[i].done || books[i].progress > 0) save_ficha(&books[i]);
    load_gone();
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
                if (m->note && strchr(m->note, '<')) strip_html(m->note);
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
    if (f) fclose(f);    export_marks();   /* con el estado de las palabras ya leído */
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
static ifont *f_title, *f_big, *f_huge, *f_head, *f_htitle, *f_body, *f_bold, *f_ital, *f_small, *f_smallb, *f_tab, *f_tabb;
static int W, H, shown_once;
static enum { RESUMEN, ACTIVIDAD, LIBROS, SUBRAYADOS, PALABRAS, NTABS } tab = RESUMEN;
static int page_of[NTABS];
static int last_page_end;      /* ¿la página actual llega al final? */
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

/* «‹ Pockit», el título y una línea debajo; el contenido empieza en TOP */
#define TOP 240
static void header(const char *title, const char *sub)
{
    pockit_back();
    SetFont(f_title, BLACK);
    DrawString(M, 110, title);
    if (sub) { SetFont(f_small, SOFT); DrawString(M, 186, sub); }
}

/* Botones de página encima de las pestañas: «‹ Anterior    2 de 5    Siguiente ›». */
#define PAGER_H 90
static int detail = -1;        /* libro abierto en ficha, o -1 */
static int pager_y(void) { return (detail >= 0 ? H : H - TABS_H) - PAGER_H; }   /* la ficha no lleva pestañas abajo */
static void pager(int page, int pages)
{
    if (pages <= 1) return;
    int y = pager_y();
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

static int reading_idx[3], nreading, reading_y0;

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

    int y = TOP + 10, bw = (W - 2 * M) / 4;
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
    reading_y0 = y;
    for (int i = 0; i < nbooks && nreading < 3 && y + 210 + 330 < H - TABS_H; i++) {   /* los que quepan sobre «Esta semana» */
        Book *b = &books[i];
        if (b->done || b->gone || b->progress <= 0 || (now - b->opened > 60 * 86400 && nreading)) continue;
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
static int cal_top(void) { return TOP + 100; }
static int cal_cell_h(void) { return 96; }

static void draw_actividad(void)
{
    char t[60];
    snprintf(t, sizeof t, "%s %d", MESES[cal_month], cal_year);
    t[0] -= t[0] >= 'a' && t[0] <= 'z' ? 32 : 0;
    header("Actividad", "Días con lectura registrada en el lector");
    SetFont(f_head, BLACK);
    DrawTextRect(M, TOP - 10, W - 2 * M, 70, t, ALIGN_CENTER | VALIGN_MIDDLE);
    DrawTextRect(M, TOP - 10, 120, 70, "‹", ALIGN_LEFT | VALIGN_MIDDLE);
    DrawTextRect(W - M - 120, TOP - 10, 120, 70, "›", ALIGN_RIGHT | VALIGN_MIDDLE);

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
static int libros_rows(void) { return (H - TABS_H - PAGER_H - TOP) / BOOK_ROW; }

static void draw_libros(void)
{
    char sub[80];
    snprintf(sub, sizeof sub, "%d libros, del último abierto al primero. Toca uno para verlo.", nbooks);
    header("Libros", sub);
    int per = libros_rows(), from = page_of[LIBROS] * per;
    int y = TOP;
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
        if (b->gone) snprintf(info, sizeof info, "Ya no está en el lector");
        else if (b->done) snprintf(info, sizeof info, "Terminado  ·  %s", when);
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

static int draw_mark_list(int words, const char *book, int target, int first_y0, int y0, int bottom)
{
    ml_n = 0;
    int pg = 0, y = first_y0, w = W - 2 * M, i = 0;
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
        if (y + need > bottom && y > (pg == 0 ? first_y0 : y0)) { if (pg == target) break; pg++; y = y0; }
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
    last_page_end = draw_mark_list(0, NULL, page_of[SUBRAYADOS], TOP, TOP, H - TABS_H - PAGER_H - 10);
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

#define CARD_TOP (TOP + 60)
static int card_bottom(void) { return H - TABS_H - 220; }

#define VROW 128
#define VLIST_TOP TOP
static int vlist_rows(void) { return (H - TABS_H - PAGER_H - VLIST_TOP) / VROW; }

static void draw_palabras(void)
{
    build_vocab();
    char sub[120];
    snprintf(sub, sizeof sub, "%d por aprender  ·  %d aprendida%s", npending, nlearned, nlearned == 1 ? "" : "s");
    if (vcards) { SetFont(f_head, BLACK); DrawString(M - 20, 42, "‹ Lista"); SetFont(f_small, SOFT); DrawString(M, 186, sub); }
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
    const char *labels[] = { "‹", vfilter ? "Por aprender" : "Aprendida", "Quitar", "›" };
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

/* ---------- ficha de un libro ----------
 *
 * Tres pestañas y ningún botón de IA:
 *  - Dónde voy: lo último que leíste y, si Pockit AI lo resumió, la historia hasta aquí.
 *  - Personajes: los nombres que más salen hasta donde vas (epub.h), con quién y dónde
 *    aparecen; los lugares y las cosas van aparte. Toca uno para ver dónde sale.
 *  - Notas: las notas del Cuaderno con la etiqueta del libro (#niebla) y tus subrayados.
 * Pockit AI lee todo esto y lo completa (resúmenes y quién es quién, en Libros/<etiqueta>/);
 * Lectura solo lo enseña. Nada pasa de tu posición: sin destripes.
 */
static void draw(void);
enum { D_DONDE, D_PERSONAJES, D_NOTAS, D_COUNT };
static const char *DTABS[] = { "Dónde voy", "Personajes", "Notas" };
static int dtab;
static EpBook eb;
static int eb_for = -1, eb_state;   /* libro cargado; 0 bien, 1 DRM, -1 no se puede leer */
static long readpos;
static int ai_on;                   /* hay clave: se puede sugerir preguntárselo a Pockit AI */
#define D_TABS_Y 365
#define D_TOP 465
static int d_bottom(void) { return H - PAGER_H - 10; }

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
    if (b->gone || !is_epub(b->path)) { eb_state = -1; return; }
    busy("Preparando el libro…");
    eb_state = ep_load(&eb, b->path);
    if (eb_state == 0) {
        pockit_book_adopt(b->title, eb.dir);
        readpos = ep_position(&eb, b->position, b->done ? 1.0 : b->progress > 0 ? b->progress : 0);
    }
}

/* Libros/<etiqueta>/<name>: lo que Pockit sabe del libro */
static void mem_path(char *out, size_t n, const char *name)
{
    char dir[800];
    pockit_book_dir(books[detail].title, dir, sizeof dir);
    snprintf(out, n, "%s/%s", dir, name);
}

/* «hasta aquí», gris y en un recuadro: lo que no se puede hacer y por qué */
static int hint_box(int y, const char *text)
{
    SetFont(f_body, SOFT);
    int h = TextRectHeight(W - 2 * M - 60, text, 0) + 40;
    DrawRect(M, y, W - 2 * M, h, LINE);
    DrawTextRect(M + 30, y + 20, W - 2 * M - 60, h - 40, text, ALIGN_LEFT);
    return y + h + 24;
}

/* una pista en gris, sin recuadro */
static int hint(int y, const char *text)
{
    SetFont(f_small, SOFT);
    int h = TextRectHeight(W - 2 * M, text, 0);
    DrawTextRect(M, y, W - 2 * M, h, text, ALIGN_LEFT);
    return y + h + 30;
}

/* ---- Dónde voy ---- */
typedef struct { long start, end; char *text; } Tramo;
static Tramo tramos[400];
static int ntramos;

static void free_tramos(void) { for (int i = 0; i < ntramos; i++) free(tramos[i].text); ntramos = 0; }

static void load_tramos(void)
{
    free_tramos();
    char p[900], *buf = malloc(8192);
    mem_path(p, sizeof p, "tramos.tsv");
    FILE *f = fopen(p, "r");
    while (f && ntramos < 400 && fgets(buf, 8192, f)) {
        char *t1 = strchr(buf, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (!t2) continue;
        t2[strcspn(t2, "\n")] = 0;
        if (!strcmp(t2 + 1, "—")) continue;   /* tramos sin historia (créditos, portadillas) */
        Tramo *t = &tramos[ntramos++];
        t->start = atol(buf); t->end = atol(t1 + 1);
        t->text = strdup(t2 + 1);
        for (char *q = t->text, *o = t->text; ; q++) {   /* \n escapado */
            if (q[0] == '\\' && q[1] == 'n') { *o++ = '\n'; q++; } else { *o++ = *q; if (!*q) break; }
        }
    }
    if (f) fclose(f);
    free(buf);
    for (int i = 1; i < ntramos; i++)   /* por dónde empiezan: «lo último» puede resumirse antes que lo de antes */
        for (int j = i; j > 0 && tramos[j].start < tramos[j - 1].start; j--) { Tramo t = tramos[j]; tramos[j] = tramos[j - 1]; tramos[j - 1] = t; }
}

static void draw_donde(int y)
{
    Book *b = &books[detail];
    load_tramos();
    int bottom = d_bottom();
    if (detail_page == 0) {
        if (eb_state == 0) {
            int ci = ep_chapter_at(&eb, readpos);
            char line[400];
            if (eb.ch[ci].title[0] && strlen(eb.ch[ci].title) < 70) snprintf(line, sizeof line, "Vas por «%s» · %d %% del libro", eb.ch[ci].title, (int)(100.0 * readpos / (eb.len ? eb.len : 1) + 0.5));
            else snprintf(line, sizeof line, "Llevas el %d %% del libro", (int)(100.0 * readpos / (eb.len ? eb.len : 1) + 0.5));
            SetFont(f_small, SOFT);
            DrawTextRect(M, y, W - 2 * M, 40, line, ALIGN_LEFT | DOTS);
            y += 50;
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
        } else {
            y = hint_box(y, b->gone ? "Este libro ya no está en el lector. Pockit conserva lo que guardó de él, tus subrayados y tus notas."
                          : eb_state == 1 ? "Este libro tiene DRM (por ejemplo, un préstamo de la biblioteca): su texto está cifrado y no se puede leer."
                                          : "«Dónde voy» y «Personajes» leen el texto de los libros en ePub, y este está en otro formato.");
        }
        if (!ntramos) {
            if (ai_on && eb_state == 0) hint(y, "¿Se te ha olvidado algo? Pídele a Pockit AI «Recuérdame qué ha pasado» y el resumen se quedará aquí.");
            last_page_end = 1;
            return;
        }
        SetFont(f_bold, BLACK);
        DrawString(M, y, "La historia hasta aquí");
        y += 60;
    }
    /* los resúmenes guardados, en orden; cada uno con el capítulo donde empieza */
    MdCtx c = { f_body, f_bold, f_ital, f_ital, f_bold, f_bold, f_body, M, W - M, 0, bottom, 46, 12, 0, SOFT };
    int pg = 0, top0 = detail_page == 0 ? y : D_TOP, yy = top0, i = 0;
    for (; i < ntramos; i++) {
        char head[300];
        snprintf(head, sizeof head, "Parte %d", i + 1);
        if (eb_state == 0 && tramos[i].start < eb.len) {
            int chi = ep_chapter_at(&eb, tramos[i].start);
            for (int k = chi; k < eb.nch && eb.ch[k].start < tramos[i].end; k++)   /* el primer capítulo de verdad del tramo */
                if (!ep_is_toc(&eb.ch[k]) && eb.ch[k].len >= 400) { chi = k; break; }
            if (eb.ch[chi].title[0]) snprintf(head, sizeof head, "%s", eb.ch[chi].title);
        }
        c.top = 0; c.scroll_row = 0;
        int need = (md_flow(&c, tramos[i].text, M, 0, BLACK, 0, 0, NULL) + 1) * 46 + 60;   /* medir */
        if (yy + need > bottom && yy > (pg == 0 ? top0 : D_TOP)) { if (pg == detail_page) break; pg++; yy = D_TOP; }
        if (pg == detail_page) {
            SetFont(f_smallb, SOFT);
            DrawTextRect(M, yy, W - 2 * M, 36, head, ALIGN_LEFT | DOTS);
            c.top = yy + 30; c.scroll_row = 0; c.bottom = bottom;
            md_flow(&c, tramos[i].text, M, 0, BLACK, 0, 1, NULL);
        }
        yy += need;
    }
    last_page_end = i >= ntramos;
}

/* ---- Personajes ---- */
typedef struct { char name[64], kind[24], desc[220]; } Saved;   /* lo que guardó Pockit AI */
static EpPerson people[40];
static int npeople = -1, people_for = -1;
static long people_pos = -1;
static Saved saved[200];
static int nsaved;
typedef struct { int det, sav; } PRow;   /* una fila: un nombre encontrado (det) o uno guardado (sav) */
static PRow prow[240];
static int nprow, p_open = -1;
static long p_offs[300];                 /* dónde sale el nombre abierto */
static int p_nfrag, p_frag, frag_y = -1;  /* cuántas veces, cuál se ve y dónde están sus flechas */
static char others[1600];               /* «París, Pekín, Historia» */
static int p_n, p_idx[40], p_y0[40], p_y1[40];

/* ¿Es el mismo nombre? «Sancho» vale por «Sancho Panza» */
static int same_name(const char *a, const char *b)
{
    char x[200], y[200];
    pockit_fold(a, x, sizeof x);
    pockit_fold(b, y, sizeof y);
    size_t lx = strlen(x), ly = strlen(y);
    if (!strcmp(x, y)) return 1;
    if (lx < ly) return !strncmp(x, y, lx) && y[lx] == ' ';
    return !strncmp(x, y, ly) && x[ly] == ' ';
}

/* ¿Ha salido ya hasta donde vas? (sin el texto del libro no se puede saber: se enseña) */
static int reached(const char *name)
{
    long o;
    if (eb_state != 0 || ep_mentions(&eb, name, readpos, &o, 1)) return 1;
    char first[64];
    snprintf(first, sizeof first, "%.*s", (int)strcspn(name, " "), name);
    return strlen(first) >= 4 && strcmp(first, name) && ep_mentions(&eb, first, readpos, &o, 1);
}

static Saved *saved_for(const char *name) { for (int i = 0; i < nsaved; i++) if (same_name(saved[i].name, name)) return &saved[i]; return NULL; }

static void add_other(const char *name)
{
    size_t l = strlen(others);
    if (strstr(others, name)) return;
    snprintf(others + l, sizeof others - l, "%s%s", l ? ", " : "", name);
}

static void load_people(void)
{
    if (people_for != detail) { npeople = -1; people_for = detail; p_open = -1; }
    if (eb_state != 0) npeople = 0;
    else if (people_pos != readpos || npeople < 0) {
        busy("Buscando personajes…");
        EpPerson tmp[60];
        int n = ep_people(&eb, readpos, tmp, 60);
        npeople = 0;
        const char *author = books[detail].author;
        for (int i = 0; i < n && npeople < 40; i++) if (!strstr(author, tmp[i].name)) people[npeople++] = tmp[i];
        people_pos = readpos;
    }
    nsaved = 0;
    char p[900], buf[600];
    mem_path(p, sizeof p, "personajes.tsv");
    FILE *f = fopen(p, "r");
    while (f && nsaved < 200 && fgets(buf, sizeof buf, f)) {
        char *t1 = strchr(buf, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (!t2) continue;
        *t1 = *t2 = 0;
        t2[1 + strcspn(t2 + 1, "\n")] = 0;
        Saved *s = &saved[nsaved++];
        snprintf(s->name, sizeof s->name, "%s", buf); snprintf(s->kind, sizeof s->kind, "%s", t1 + 1); snprintf(s->desc, sizeof s->desc, "%.*s", (int)strcspn(t2 + 1, "\t"), t2 + 1);
    }
    if (f) fclose(f);
    /* filas: primero lo que encuentra el lector; lo que Pockit AI dice que es un lugar o una cosa, aparte */
    nprow = 0;
    others[0] = 0;
    for (int i = 0; i < npeople; i++) {
        Saved *s = saved_for(people[i].name);
        if (s ? strcmp(s->kind, "personaje") != 0 : people[i].kind != 0) { add_other(people[i].name); continue; }
        prow[nprow++] = (PRow){ i, -1 };
    }
    for (int k = 0; k < nsaved && nprow < 240; k++) {
        int found = 0;
        for (int i = 0; i < npeople; i++) if (same_name(saved[k].name, people[i].name)) found = 1;
        if (found || !reached(saved[k].name)) continue;   /* lo revisado de más adelante, cuando llegues */
        if (strcmp(saved[k].kind, "personaje")) add_other(saved[k].name);
        else prow[nprow++] = (PRow){ -1, k };
    }
}

/* Una fila: el nombre y, debajo, quién es (si se sabe) o cuánto sale. Abierta: con quién y dónde. */
static int person_row(int r, int y, int draw)
{
    PRow *pr = &prow[r];
    EpPerson *p = pr->det >= 0 ? &people[pr->det] : NULL;
    Saved *s = p ? saved_for(p->name) : &saved[pr->sav];
    const char *name = p ? p->name : s->name;
    int open = r == p_open, w = W - 2 * M, h = 50;
    char info[400] = "";
    if (p) {
        snprintf(info, sizeof info, "%d menciones · desde «%s»", open && p_nfrag ? p_nfrag : p->count, eb.ch[p->first_ch].title[0] ? eb.ch[p->first_ch].title : "el principio");
        if (open && p->nwith) {
            strncat(info, " · con ", sizeof info - strlen(info) - 1);
            for (int k = 0; k < p->nwith; k++) { if (k) strncat(info, ", ", sizeof info - strlen(info) - 1); strncat(info, people[p->with[k]].name, sizeof info - strlen(info) - 1); }
        }
    }
    if (draw) { SetFont(f_bold, BLACK); DrawTextRect(M, y, w, 46, name, ALIGN_LEFT | DOTS); }
    if (s && s->desc[0]) {
        SetFont(f_ital, BLACK);
        int dh = open ? TextRectHeight(w, s->desc, 0) : 44;
        if (draw) DrawTextRect(M, y + h, w, dh, s->desc, ALIGN_LEFT | DOTS);
        h += dh + 4;
    }
    if (info[0] && (open || !(s && s->desc[0]))) {
        SetFont(f_small, SOFT);
        int ih = open ? TextRectHeight(w, info, 0) : 38;
        if (draw) DrawTextRect(M, y + h, w, ih, info, ALIGN_LEFT | DOTS);
        h += ih + 4;
    }
    if (open && p && p_nfrag) {   /* una de las frases donde sale, del propio libro; con flechas para pasar a otra */
        char frag[700], q[720], nav[60];
        ep_sentence(&eb, p_offs[p_frag], frag, sizeof frag);
        snprintf(q, sizeof q, "«%s»", frag);
        SetFont(f_ital, SOFT);
        int ch = TextRectHeight(w - 30, q, 0);
        if (ch > 330) ch = 330;
        if (draw) { FillArea(M, y + h + 10, 5, ch, LINE); DrawTextRect(M + 30, y + h + 10, w - 30, ch, q, ALIGN_LEFT | DOTS); }
        h += ch + 24;
        if (p_nfrag > 1) {
            snprintf(nav, sizeof nav, "%d de %d", p_frag + 1, p_nfrag);
            if (draw) {
                SetFont(f_small, SOFT);
                DrawTextRect(M, y + h, w, 56, nav, ALIGN_CENTER | VALIGN_MIDDLE);
                SetFont(f_head, p_frag > 0 ? BLACK : LINE);
                DrawTextRect(M, y + h, w / 3, 56, "‹ Antes", ALIGN_LEFT | VALIGN_MIDDLE);
                SetFont(f_head, p_frag + 1 < p_nfrag ? BLACK : LINE);
                DrawTextRect(M + w - w / 3, y + h, w / 3, 56, "Después ›", ALIGN_RIGHT | VALIGN_MIDDLE);
                frag_y = y + h;
            }
            h += 60;
        }
    }
    h += 30;
    if (draw) DrawLine(M, y + h - 16, W - M, y + h - 16, LINE);
    return h;
}

static void draw_personajes(int y)
{
    int bottom = d_bottom();
    if (detail_page == 0) {
        if (eb_state != 0 && !nprow) { y = hint_box(y, books[detail].gone ? "Este libro ya no está en el lector." : eb_state == 1 ? "Este libro tiene DRM: su texto no se puede leer." : "Esto funciona con libros en ePub, y este está en otro formato."); last_page_end = 1; return; }
        if (!nprow && !others[0]) { hint_box(y, "Aún no aparecen nombres que se repitan."); last_page_end = 1; return; }
        int raw = 0;   /* nombres que nadie ha revisado aún */
        for (int r = 0; r < nprow; r++) if (prow[r].det >= 0 && !saved_for(people[prow[r].det].name)) raw++;
        y = hint(y, ai_on && raw ? "Los nombres que más salen hasta donde vas. Toca uno para ver dónde aparece. Pídele a Pockit AI «Revisa los personajes» y separará los lugares y dirá quién es cada uno."
                                 : "Los nombres que más salen hasta donde vas, sin destripes. Toca uno para ver dónde aparece.");
    }
    p_n = 0;
    frag_y = -1;
    int pg = 0, top0 = detail_page == 0 ? y : D_TOP, yy = top0, r = 0;
    for (; r <= nprow; r++) {
        int need;
        if (r == nprow) {   /* al final, los lugares y las cosas */
            if (!others[0]) break;
            char line[1700];
            snprintf(line, sizeof line, "Lugares y otros nombres: %s", others);
            SetFont(f_small, SOFT);
            need = TextRectHeight(W - 2 * M, line, 0) + 20;
            if (yy + need > bottom && yy > (pg == 0 ? top0 : D_TOP)) { if (pg == detail_page) break; pg++; yy = D_TOP; }
            if (pg == detail_page) DrawTextRect(M, yy, W - 2 * M, need, line, ALIGN_LEFT);
            r++;
            break;
        }
        need = person_row(r, yy, 0);
        if (need > bottom - D_TOP) need = bottom - D_TOP;
        if (yy + need > bottom && yy > (pg == 0 ? top0 : D_TOP)) { if (pg == detail_page) break; pg++; yy = D_TOP; }
        if (pg == detail_page) {
            person_row(r, yy, 1);
            if (p_n < 40) { p_idx[p_n] = r; p_y0[p_n] = yy; p_y1[p_n] = yy + need; p_n++; }
        }
        yy += need;
    }
    last_page_end = r > nprow || (r == nprow && !others[0]);
}

/* ---- Notas: las del Cuaderno con la etiqueta del libro, y los subrayados ---- */
typedef struct { char path[600], title[200]; long mtime; } BookNote;
static BookNote bnotes[60];
static int nbnotes, bn_y0[5], bn_y1[5], bn_n, bn_all_y = -1;
static char btag[64];

static int by_note_date(const void *a, const void *b) { long x = ((const BookNote *)a)->mtime, y = ((const BookNote *)b)->mtime; return x < y ? 1 : x > y ? -1 : 0; }

static void load_book_notes(void)
{
    pockit_tag(books[detail].title, btag, sizeof btag);
    nbnotes = 0;
    char dir[700];
    pockit_path(dir, sizeof dir, "Notas");
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)) && nbnotes < 60) {
        size_t l = strlen(e->d_name);
        if (e->d_name[0] == '.' || l < 4 || strcmp(e->d_name + l - 3, ".md")) continue;
        BookNote *n = &bnotes[nbnotes];
        snprintf(n->path, sizeof n->path, "%s/%s", dir, e->d_name);
        FILE *f = fopen(n->path, "r");
        char buf[2048], tags[16][48];
        int match = 0;
        n->title[0] = 0;
        while (f && fgets(buf, sizeof buf, f)) {
            buf[strcspn(buf, "\r\n")] = 0;
            int h = 0;
            while (buf[h] == '#') h++;
            if (!n->title[0] && ((h && buf[h] == ' ') || (!h && buf[0]))) snprintf(n->title, sizeof n->title, "%s", buf + (h ? h + 1 : 0));
            int k = pockit_tags(buf, tags, 16);
            for (int i = 0; i < k; i++) if (pockit_tag_matches(tags[i], btag)) match = 1;
        }
        if (f) fclose(f);
        if (!match) continue;
        struct stat st;
        n->mtime = stat(n->path, &st) ? 0 : st.st_mtime;
        if (!n->title[0]) snprintf(n->title, sizeof n->title, "Nota");
        nbnotes++;
    }
    if (d) closedir(d);
    qsort(bnotes, nbnotes, sizeof *bnotes, by_note_date);
}

/* ---- explicaciones de subrayados guardadas por la 2.1 (se siguen enseñando) ---- */
static char expl_q[80][120], *expl_t[80];
static int nexpl = -1;

static void load_expl(void)
{
    for (int i = 0; i < nexpl; i++) free(expl_t[i]);
    nexpl = 0;
    char p[900], *buf = malloc(4096);
    mem_path(p, sizeof p, "explicaciones.tsv");
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

static const char *explanation(const char *quote)
{
    char k[24];
    snprintf(k, sizeof k, "%016llx", ep_hash(quote));
    for (int i = 0; i < nexpl; i++) if (!strcmp(expl_q[i], k)) return expl_t[i];
    return NULL;
}

static void draw_notas(int y)
{
    Book *b = &books[detail];
    int bottom = d_bottom(), first = y;
    bn_n = 0;
    bn_all_y = -1;
    if (detail_page == 0) {
        char line[200];
        SetFont(f_bold, BLACK);
        DrawString(M, y, "En tu Cuaderno");
        y += 56;
        if (!nbnotes) {
            snprintf(line, sizeof line, "Etiqueta una nota con #%s y aparecerá aquí.", btag);
            y = hint(y, line);
        }
        for (int i = 0; i < nbnotes && i < 4; i++) {
            char ds[60];
            ago(bnotes[i].mtime, ds, sizeof ds);
            SetFont(f_body, BLACK);
            DrawTextRect(M, y, W - 2 * M - 160, 46, bnotes[i].title, ALIGN_LEFT | DOTS);
            SetFont(f_small, SOFT);
            DrawTextRect(W - M - 160, y + 4, 160, 40, ds, ALIGN_RIGHT);
            bn_y0[bn_n] = y; bn_y1[bn_n] = y + 64; bn_n++;
            y += 64;
        }
        if (nbnotes > 4) {
            snprintf(line, sizeof line, "Ver las %d notas con #%s  ›", nbnotes, btag);
            SetFont(f_small, BLACK);
            DrawString(M, y + 4, line);
            bn_all_y = y;
            y += 60;
        }
        y += 24;
        SetFont(f_bold, BLACK);
        DrawString(M, y, "Subrayados");
        y += 60;
        if (!b->marks) { hint(y, "Lo que subrayes en este libro aparecerá aquí."); last_page_end = 1; return; }
        first = y;
    }
    last_page_end = draw_mark_list(0, b->title, detail_page, first, D_TOP, bottom);
}

static void draw_detail(void)
{
    Book *b = &books[detail];
    ai_on = ai_load_cfg();
    if (!ml_extra) ml_extra = explanation;
    SetFont(f_head, BLACK);
    DrawString(M - 20, 42, "‹ Libros");
    /* el libro: portada, título, autor y cómo vas */
    cover(b, M, 110, 150, 225);
    int tx = M + 180, tw = W - M - tx;
    SetFont(f_htitle, BLACK);
    int th = TextRectHeight(tw, b->title, 0);
    if (th > 112) th = 112;
    DrawTextRect(tx, 108, tw, th, b->title, ALIGN_LEFT | DOTS);
    SetFont(f_body, SOFT);
    DrawTextRect(tx, 112 + th, tw, 44, b->author[0] ? b->author : " ", ALIGN_LEFT | DOTS);
    char l1[160], l2[160], d[80];
    if (b->done) {
        if (b->completed_ts) { fecha(b->completed_ts, d, sizeof d); snprintf(l1, sizeof l1, "Terminado el %s", d); } else snprintf(l1, sizeof l1, "Terminado");
    } else if (b->progress > 0) snprintf(l1, sizeof l1, "%d %%  ·  página %d de %d", (int)(b->progress * 100 + 0.5), b->page, b->pages);
    else snprintf(l1, sizeof l1, "Sin empezar");
    if (b->gone) snprintf(l2, sizeof l2, "Ya no está en el lector");
    else if (b->opened) { ago(b->opened, d, sizeof d); snprintf(l2, sizeof l2, "Leído por última vez %s", d); }
    else l2[0] = 0;
    if (b->progress > 0) bar(tx, 262, tw, 12, b->done ? 1 : b->progress);
    SetFont(f_small, SOFT);
    DrawTextRect(tx, 284, tw, 36, l1, ALIGN_LEFT | DOTS);
    if (l2[0]) DrawTextRect(tx, 320, tw, 36, l2, ALIGN_LEFT | DOTS);
    /* pestañas */
    int segw = (W - 2 * M) / D_COUNT;
    for (int i = 0; i < D_COUNT; i++) {
        int on = i == dtab, x = M + i * segw;
        if (on) FillArea(x, D_TABS_Y, segw - 8, 70, BLACK); else { DrawRect(x, D_TABS_Y, segw - 8, 70, SOFT); DrawRect(x + 1, D_TABS_Y + 1, segw - 10, 68, LINE); }
        SetFont(on ? f_tabb : f_tab, on ? WHITE : BLACK);
        DrawTextRect(x, D_TABS_Y, segw - 8, 70, DTABS[i], ALIGN_CENTER | VALIGN_MIDDLE);
    }
    switch (dtab) {
    case D_DONDE: draw_donde(D_TOP); break;
    case D_PERSONAJES: draw_personajes(D_TOP); break;
    case D_NOTAS: draw_notas(D_TOP); break;
    }
    pager(detail_page, last_page_end ? detail_page + 1 : detail_page + 2);
}

static void detail_tap(int x, int y)
{
    if (y >= D_TABS_Y && y < D_TABS_Y + 70) {
        int t = (x - M) / ((W - 2 * M) / D_COUNT);
        if (t >= 0 && t < D_COUNT && t != dtab) { dtab = t; detail_page = 0; draw(); }
        return;
    }
    if (dtab == D_PERSONAJES) {
        if (frag_y >= 0 && y >= frag_y && y < frag_y + 60 && p_open >= 0) {   /* pasar de fragmento */
            if (x < W / 2 && p_frag > 0) p_frag--; else if (x >= W / 2 && p_frag + 1 < p_nfrag) p_frag++; else return;
            draw();
            return;
        }
        for (int i = 0; i < p_n; i++)
            if (y >= p_y0[i] && y < p_y1[i]) {
                p_open = p_open == p_idx[i] ? -1 : p_idx[i];
                p_frag = p_nfrag = 0;   /* prepare_detail busca dónde sale */
                draw();
                return;
            }
    }
    if (dtab == D_NOTAS) {
        for (int i = 0; i < bn_n; i++) if (y >= bn_y0[i] && y < bn_y1[i]) { cuaderno_open_note(bnotes[i].path); return; }
        if (bn_all_y >= 0 && y >= bn_all_y && y < bn_all_y + 60) { cuaderno_open_tag(btag); return; }
    }
}

static void prepare_detail(void)
{
    if (detail < 0) return;
    ensure_book();   /* antes de pintar: los avisos de «Preparando…» no se quedan en la pantalla */
    if (dtab == D_PERSONAJES) {
        load_people();
        if (p_open >= 0 && p_open < nprow && !p_nfrag && prow[p_open].det >= 0 && eb_state == 0) p_nfrag = ep_mentions(&eb, people[prow[p_open].det].name, readpos, p_offs, 300);
    }
    if (dtab == D_NOTAS) { load_book_notes(); load_expl(); }
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
    if (detail < 0) draw_tabs();
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
    else return;
    draw();
}

static void open_detail(int i) { detail = i; detail_page = 0; dtab = D_DONDE; draw(); }

static void tap(int x, int y)
{
    if (detail >= 0) {   /* la ficha: «‹ Libros», pestañas, páginas */
        if (y < 100 && x < W / 3) { detail = -1; draw(); return; }
        if (y > pager_y()) { page(x > W / 2 ? 1 : -1); return; }
        detail_tap(x, y);
        return;
    }
    if (y > H - TABS_H) {
        int t = x * NTABS / W;
        if (t != (int)tab) { tab = t; draw(); }
        return;
    }
    if (!(tab == PALABRAS && vcards) && pockit_back_hit(x, y)) { CloseApp(); return; }
    if ((tab == LIBROS || tab == SUBRAYADOS) && y > pager_y()) { page(x > W / 2 ? 1 : -1); return; }
    switch (tab) {
    case RESUMEN:
        for (int i = 0; i < nreading; i++) {
            int yy = reading_y0 + i * 210;
            if (y >= yy && y < yy + 200) { open_detail(reading_idx[i]); return; }
        }
        break;
    case ACTIVIDAD: {
        if (y >= TOP - 10 && y < TOP + 60) { page(x > W / 2 ? 1 : -1); return; }
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
        int k = (y - TOP) / BOOK_ROW, i = page_of[LIBROS] * libros_rows() + k;
        if (y >= TOP && k < libros_rows() && i < nbooks) open_detail(i);
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
        f_htitle = OpenFont(DEFAULTFONTB, 44, 1);
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
    /* para las capturas del emulador: «lectura <pestaña> [libro [pestaña de la ficha [nombre abierto]]]» */
    if (argc > 1) tab = atoi(argv[1]);
    if (argc > 2 && tab == LIBROS) detail = atoi(argv[2]);
    if (argc > 3 && tab == LIBROS) dtab = atoi(argv[3]);
    if (argc > 4 && tab == LIBROS) p_open = atoi(argv[4]), people_for = detail;
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
        if (b->done || b->gone || b->progress <= 0) continue;
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

void lectura_open_current(void)
{
    ensure_data();
    for (int i = 0; i < nbooks; i++)
        if (!books[i].done && !books[i].gone && books[i].progress > 0) { detail = i; detail_page = 0; dtab = D_DONDE; break; }
    pockit_open(MOD_LECTURA);
}

/* ---------- lo que Lectura le da a Pockit AI (solo lectura; apunta a la memoria de este módulo) ---------- */
int lectura_book(int i, PockitBook *out)
{
    ensure_data();
    if (i < 0 || i >= nbooks) return 0;
    Book *b = &books[i];
    *out = (PockitBook){ b->title, b->author, b->path, b->position, b->progress, b->done, b->page, b->pages,
                         b->marks, b->words, b->added, b->opened, b->completed_ts, b->gone };
    return 1;
}

int lectura_mark(int i, PockitMark *out)
{
    ensure_data();
    if (i < 0 || i >= nmarks) return 0;
    Mark *m = &marks[i];
    *out = (PockitMark){ m->text, m->note ? m->note : "", m->book, m->extra ? m->extra : "", m->ts, m->word, vstate[i] };
    return 1;
}
