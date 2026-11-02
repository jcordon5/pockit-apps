/*
 * datos.c: dónde viven tus cosas y cómo se enlazan entre módulos.
 *
 *  - Tus datos son texto: Notas/ (el Cuaderno), Chats/ (Pockit AI) y Libros/ (lo que Pockit
 *    sabe de cada libro: resúmenes, personajes y una ficha). Viven en Pockit/, en el lector. Si
 *    lo enlazas con Dropbox desde pockit.yous.dev, Pockit los sincroniza con la carpeta /Pockit
 *    de tu Dropbox (nube.c). Sin conexión no pasa nada: se sincroniza cuando vuelve la wifi.
 *  - Hasta la 2.2.4 se podían guardar en Dropbox PocketBook/Pockit para que los subiera la
 *    sincronización del propio lector, pero esa solo sabe subir archivos nuevos (si uno cambia,
 *    sube una copia «(1)» y da error). Al enlazar Pockit, vuelven a Pockit/.
 *  - Cada libro se reconoce por su título, no por su archivo: si lo borras, lo mueves o
 *    bajas otra edición, lo que Pockit sabía de él sigue ahí (Libros/<etiqueta>/).
 *  - Etiquetas: #palabra en cualquier nota. La de un libro sale de su título en minúsculas,
 *    sin tildes y con guiones: «Niebla (Nivola)» → #niebla, «La muralla china» → #la-muralla-china.
 *
 * Lo que es caché (el texto extraído de los ePub, el gasto, las copias para deshacer) se
 * queda en system/config/pockit y no se sincroniza.
 */
#include "inkview.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include "pockit.h"

#define CACHE_DIR FLASHDIR "/system/config/pockit"
#define WHERE_CFG CACHE_DIR "/datos.txt"            /* «dropbox» (hasta la 2.2.4) o nada */
#define DROPBOX_CFG FLASHDIR "/system/config/dropbox/dropbox.cfg"
#define LOCAL_DIR FLASHDIR "/Pockit"

static char data_dir[600];
static int in_dropbox = -1;

/* ---------- letras ---------- */
int pockit_u8(const unsigned char *s, unsigned *cp)
{
    if (s[0] < 0x80) { *cp = s[0]; return 1; }
    if ((s[0] & 0xE0) == 0xC0 && s[1]) { *cp = (s[0] & 31) << 6 | (s[1] & 63); return 2; }
    if ((s[0] & 0xF0) == 0xE0 && s[1] && s[2]) { *cp = (s[0] & 15) << 12 | (s[1] & 63) << 6 | (s[2] & 63); return 3; }
    if ((s[0] & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) { *cp = (s[0] & 7) << 18 | (s[1] & 63) << 12 | (s[2] & 63) << 6 | (s[3] & 63); return 4; }
    *cp = s[0];
    return 1;
}

/* minúscula y sin tilde (latín-1); la ñ cuenta como n: en el teclado del lector cuesta escribirla */
unsigned pockit_fold_cp(unsigned c)
{
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c < 0xC0 || c > 0xFF || c == 0xD7 || c == 0xF7 || c == 0xDF) return c;
    if (c == 0xFF) return 'y';
    unsigned u = c >= 0xE0 ? c - 0x20 : c;   /* a mayúscula latín-1: una sola tabla */
    if (u <= 0xC5) return 'a';
    if (u == 0xC7) return 'c';
    if (u >= 0xC8 && u <= 0xCB) return 'e';
    if (u >= 0xCC && u <= 0xCF) return 'i';
    if (u == 0xD1) return 'n';
    if ((u >= 0xD2 && u <= 0xD6) || u == 0xD8) return 'o';
    if (u >= 0xD9 && u <= 0xDC) return 'u';
    if (u == 0xDD) return 'y';
    return u + 0x20;
}

static void put_utf8(char **o, unsigned cp)
{
    unsigned char *p = (unsigned char *)*o;
    if (cp < 0x80) *p++ = cp;
    else if (cp < 0x800) { *p++ = 0xC0 | cp >> 6; *p++ = 0x80 | (cp & 63); }
    else if (cp < 0x10000) { *p++ = 0xE0 | cp >> 12; *p++ = 0x80 | (cp >> 6 & 63); *p++ = 0x80 | (cp & 63); }
    else { *p++ = 0xF0 | cp >> 18; *p++ = 0x80 | (cp >> 12 & 63); *p++ = 0x80 | (cp >> 6 & 63); *p++ = 0x80 | (cp & 63); }
    *o = (char *)p;
}

void pockit_fold(const char *s, char *out, size_t n)
{
    size_t j = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && j + 4 < n; ) {
        unsigned cp;
        p += pockit_u8(p, &cp);
        char *o = out + j;
        put_utf8(&o, pockit_fold_cp(cp));
        j = o - out;
    }
    out[j] = 0;
}

static int tag_char(unsigned cp) { return (cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9') || cp == '-' || cp == '_' || cp >= 0x80; }

/* ---------- etiquetas ---------- */
void pockit_tag(const char *title, char *out, size_t n)
{
    char f[600], *o = out;
    pockit_fold(title, f, sizeof f);
    /* sin subtítulo: «Niebla (Nivola)», «Sapiens: de animales a dioses», «Dune - edición…» */
    char *cut = f + strcspn(f, ":([");
    char *dash = strstr(f, " - ");
    if (dash && dash < cut) cut = dash;
    *cut = 0;
    int pending_dash = 0;
    for (const unsigned char *p = (const unsigned char *)f; *p && (size_t)(o - out) + 5 < n && o - out < 40; ) {
        unsigned cp;
        p += pockit_u8(p, &cp);
        if (tag_char(cp) && cp != '-' && cp != '_') {
            if (pending_dash && o > out) *o++ = '-';
            pending_dash = 0;
            put_utf8(&o, cp);
        } else pending_dash = 1;
    }
    *o = 0;
    if (o - out >= 40) { char *d = strrchr(out, '-'); if (d && d > out + 10) *d = 0; }   /* cortar en un guion */
    if (!out[0]) snprintf(out, n, "libro");
}

int pockit_tags(const char *text, char tags[][48], int max)
{
    int n = 0;
    for (const char *p = text; *p && n < max; p++) {
        if (*p != '#' || (p > text && p[-1] != ' ' && p[-1] != '\n' && p[-1] != '(' && p[-1] != '\t')) continue;
        char raw[96], *o = raw;
        const unsigned char *q = (const unsigned char *)p + 1;
        while (*q && o - raw < 80) {
            unsigned cp;
            int l = pockit_u8(q, &cp);
            cp = pockit_fold_cp(cp);
            if (!tag_char(cp)) break;
            put_utf8(&o, cp);
            q += l;
        }
        *o = 0;
        while (o > raw && (o[-1] == '-' || o[-1] == '_')) *--o = 0;
        if (!raw[0] || (raw[0] >= '0' && raw[0] <= '9' && strspn(raw, "0123456789") == strlen(raw))) continue;   /* «#3» no es una etiqueta */
        int dup = 0;
        for (int k = 0; k < n; k++) if (!strcmp(tags[k], raw)) dup = 1;
        if (!dup) snprintf(tags[n++], 48, "%s", raw);
        p = (const char *)q - 1;
    }
    return n;
}

/* sin el artículo del principio: «la-muralla-china» → «muralla-china» */
static const char *no_article(const char *t)
{
    static const char *A[] = { "el-", "la-", "los-", "las-", "un-", "una-", "lo-", "the-", "a-", "an-", NULL };
    for (int i = 0; A[i]; i++) { size_t l = strlen(A[i]); if (!strncmp(t, A[i], l) && t[l]) return t + l; }
    return t;
}

int pockit_tag_matches(const char *tag, const char *book_tag)
{
    if (!strcmp(tag, book_tag)) return 1;
    const char *a = no_article(tag), *b = no_article(book_tag);
    if (!strcmp(a, b)) return 1;
    size_t l = strlen(a);
    return l >= 5 && !strncmp(b, a, l) && b[l] == '-';   /* #muralla vale para «La muralla china» */
}

/* ---------- carpetas ---------- */
static int is_dir(const char *p) { struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }
static int exists(const char *p) { struct stat st; return !stat(p, &st); }

/* La carpeta de Dropbox del lector, si la sincronización está conectada. */
int pockit_dropbox_ready(char *folder, size_t n)
{
    FILE *f = fopen(DROPBOX_CFG, "r");
    if (!f) return 0;
    char line[1024], dir[600] = "";
    int token = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "dropbox_folder=", 15)) snprintf(dir, sizeof dir, "%s", line + 15);
        else if ((!strncmp(line, "refresh_token_v2=", 17) && line[17]) || (!strncmp(line, "access_token_v2=", 16) && line[16])) token = 1;
    }
    fclose(f);
    if (!token || !dir[0]) return 0;
    if (!strncmp(dir, "/mnt/ext1/", 10)) snprintf(folder, n, FLASHDIR "/%s", dir + 10);   /* en el emulador FLASHDIR es otra */
    else snprintf(folder, n, "%s", dir);
    return 1;
}

int pockit_dropbox(void) { pockit_data(); return in_dropbox; }

static void move_tree(const char *src, const char *dst, int copy);

/* Notas/ y Chats/ estaban en la raíz del lector hasta la versión 2.1: se mudan una vez. */
static void adopt_legacy(void)
{
    static const char *OLD[] = { "Notas", "Chats", NULL };
    for (int i = 0; OLD[i]; i++) {
        char from[600], to[700];
        snprintf(from, sizeof from, FLASHDIR "/%s", OLD[i]);
        if (!is_dir(from)) continue;
        snprintf(to, sizeof to, "%s/%s", data_dir, OLD[i]);
        move_tree(from, to, 0);
        rmdir(from);
    }
}

/* Los datos que estaban en la carpeta de Dropbox del lector vuelven a Pockit/ (con una copia antes).
 * Lo normal es que ya lo haya hecho la web al enlazar; esto es por si no pudo. */
static void adopt_native(void)
{
    char dbx[600], old[700];
    if (!pockit_dropbox_ready(dbx, sizeof dbx)) snprintf(dbx, sizeof dbx, FLASHDIR "/Dropbox PocketBook");
    snprintf(old, sizeof old, "%s/Pockit", dbx);
    if (!is_dir(old)) return;
    char stamp[40], backup[700];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    mkdir(CACHE_DIR, 0755);
    mkdir(CACHE_DIR "/copias", 0755);
    snprintf(backup, sizeof backup, CACHE_DIR "/copias/%s", stamp);
    move_tree(old, backup, 1);
    move_tree(old, data_dir, 0);
    rmdir(old);
    remove(WHERE_CFG);
}

const char *pockit_data(void)
{
    if (in_dropbox >= 0) return data_dir;
    in_dropbox = 0;
    char want[32] = "", dbx[600];
    FILE *f = fopen(WHERE_CFG, "r");
    if (f) { if (fgets(want, sizeof want, f)) want[strcspn(want, "\r\n")] = 0; fclose(f); }
    if (!nube_linked() && !strcmp(want, "dropbox") && pockit_dropbox_ready(dbx, sizeof dbx)) { snprintf(data_dir, sizeof data_dir, "%s/Pockit", dbx); in_dropbox = 1; }
    else snprintf(data_dir, sizeof data_dir, LOCAL_DIR);
    mkdir(data_dir, 0755);
    if (nube_linked()) adopt_native();
    adopt_legacy();
    return data_dir;
}

void pockit_path(char *out, size_t n, const char *sub)
{
    snprintf(out, n, "%s/%s", pockit_data(), sub);
    mkdir(out, 0755);
}

/* ---------- mudanzas ---------- */
static int same_file(const char *a, const char *b)
{
    FILE *x = fopen(a, "rb"), *y = fopen(b, "rb");
    int same = x && y;
    while (same) {
        int c = fgetc(x), d = fgetc(y);
        if (c != d) same = 0;
        if (c == EOF || d == EOF) break;
    }
    if (x) fclose(x);
    if (y) fclose(y);
    return same;
}

static int pk_copy_file(const char *a, const char *b)
{
    FILE *x = fopen(a, "rb"), *y = x ? fopen(b, "wb") : NULL;
    if (!x || !y) { if (x) fclose(x); return 0; }
    char buf[8192];
    size_t r;
    while ((r = fread(buf, 1, sizeof buf, x)) > 0) fwrite(buf, 1, r, y);
    fclose(x);
    fclose(y);
    return 1;
}

/* Lleva src a dst sin perder nada: si en dst ya hay un archivo distinto con el mismo nombre,
 * el que llega se guarda como «nombre (2).md». copy=1 copia en vez de mover. */
static void move_tree(const char *src, const char *dst, int copy)
{
    mkdir(dst, 0755);
    DIR *d = opendir(src);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;   /* ni . ni .. ni ocultos (._ de macOS, .dropbox) */
        char a[1100], b[1100];
        snprintf(a, sizeof a, "%s/%s", src, e->d_name);
        snprintf(b, sizeof b, "%s/%s", dst, e->d_name);
        if (is_dir(a)) { move_tree(a, b, copy); if (!copy) rmdir(a); continue; }
        if (exists(b)) {
            if (same_file(a, b)) { if (!copy) remove(a); continue; }
            const char *dot = strrchr(e->d_name, '.');
            int stem = dot ? (int)(dot - e->d_name) : (int)strlen(e->d_name);
            for (int k = 2; exists(b) && k < 100; k++) snprintf(b, sizeof b, "%s/%.*s (%d)%s", dst, stem, e->d_name, k, dot ? dot : "");
        }
        if (copy || rename(a, b)) { if (pk_copy_file(a, b) && !copy) remove(a); }
    }
    closedir(d);
}

/* ---------- libros ---------- */
void pockit_book_dir(const char *title, char *out, size_t n)
{
    char tag[64], libros[700];
    pockit_tag(title, tag, sizeof tag);
    pockit_path(libros, sizeof libros, "Libros");
    snprintf(out, n, "%s/%s", libros, tag);
    mkdir(out, 0755);
}

/* Hasta la 2.1, lo que Pockit sabía de un libro estaba en la caché de su archivo
 * (system/config/pockit/libros/<id>/); se muda a Libros/<etiqueta>/ la primera vez. */
void pockit_book_adopt(const char *title, const char *cache_dir)
{
    static const char *F[] = { "tramos.tsv", "personajes.tsv", "explicaciones.tsv", NULL };
    char dir[800];
    pockit_book_dir(title, dir, sizeof dir);
    for (int i = 0; F[i]; i++) {
        char a[1100], b[1100];
        snprintf(a, sizeof a, "%s/%s", cache_dir, F[i]);
        snprintf(b, sizeof b, "%s/%s", dir, F[i]);
        if (exists(a) && !exists(b) && rename(a, b)) { if (pk_copy_file(a, b)) remove(a); }
    }
}
