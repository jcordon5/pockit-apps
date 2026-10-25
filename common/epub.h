/*
 * epub.h: todo lo que se puede hacer con un ePub SIN IA, en el propio lector.
 *
 *  - Abre el zip (inflate de stb_image), lee container.xml y el OPF y saca el texto de
 *    cada capítulo en orden, sin HTML. Se guarda en caché (system/config/pockit/libros/<id>)
 *    y solo se rehace si el ePub cambia.
 *  - Detecta DRM (LCP/Adobe): esos libros no se pueden leer y la IA queda desactivada.
 *  - Convierte la posición del lector (epubcfi + progreso) en un carácter del texto, un
 *    poco antes de donde vas para no destripar nada.
 *  - Encuentra los personajes: nombres propios que se repiten en mitad de frase, cuántas
 *    veces salen, dónde aparecen por primera vez y con quién coinciden. Con eso la IA
 *    solo tiene que describirlos a partir de dos frases, no leer el libro.
 */
#ifndef EPUB_H
#define EPUB_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include <sys/stat.h>

char *stbi_zlib_decode_noheader_malloc(const char *buffer, int len, int *outlen);

#ifndef POCKIT_DIR
#define POCKIT_DIR FLASHDIR "/system/config/pockit"
#endif

/* ---------- zip ---------- */
typedef struct { char *name; int method; long comp, size, local; } EpEntry;
typedef struct { FILE *f; EpEntry *e; int n; } EpZip;

static unsigned ep_u16(const unsigned char *p) { return p[0] | p[1] << 8; }
static unsigned long ep_u32(const unsigned char *p) { return p[0] | p[1] << 8 | (unsigned long)p[2] << 16 | (unsigned long)p[3] << 24; }

static int ep_zip_open(EpZip *z, const char *path)
{
    memset(z, 0, sizeof *z);
    if (!(z->f = fopen(path, "rb"))) return -1;
    fseek(z->f, 0, SEEK_END);
    long size = ftell(z->f), tail = size < 66000 ? size : 66000;
    unsigned char *buf = malloc(tail);
    fseek(z->f, size - tail, SEEK_SET);
    if (fread(buf, 1, tail, z->f) != (size_t)tail) { free(buf); return -1; }
    long eocd = -1;
    for (long i = tail - 22; i >= 0; i--) if (ep_u32(buf + i) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) { free(buf); return -1; }
    int n = ep_u16(buf + eocd + 10);
    long cdsize = ep_u32(buf + eocd + 12), cdoff = ep_u32(buf + eocd + 16);
    free(buf);
    unsigned char *cd = malloc(cdsize);
    fseek(z->f, cdoff, SEEK_SET);
    if (fread(cd, 1, cdsize, z->f) != (size_t)cdsize) { free(cd); return -1; }
    z->e = calloc(n, sizeof *z->e);
    long p = 0;
    for (int i = 0; i < n && p + 46 <= cdsize && ep_u32(cd + p) == 0x02014b50; i++) {
        EpEntry *e = &z->e[z->n++];
        e->method = ep_u16(cd + p + 10);
        e->comp = ep_u32(cd + p + 20);
        e->size = ep_u32(cd + p + 24);
        int nl = ep_u16(cd + p + 28), xl = ep_u16(cd + p + 30), cl = ep_u16(cd + p + 32);
        e->local = ep_u32(cd + p + 42);
        e->name = malloc(nl + 1);
        memcpy(e->name, cd + p + 46, nl);
        e->name[nl] = 0;
        p += 46 + nl + xl + cl;
    }
    free(cd);
    return 0;
}

static void ep_zip_close(EpZip *z)
{
    for (int i = 0; i < z->n; i++) free(z->e[i].name);
    free(z->e);
    if (z->f) fclose(z->f);
    memset(z, 0, sizeof *z);
}

/* Contenido de un archivo del zip (malloc, terminado en 0) o NULL. */
static char *ep_zip_read(EpZip *z, const char *name, long *outlen)
{
    for (int i = 0; i < z->n; i++) {
        EpEntry *e = &z->e[i];
        if (strcmp(e->name, name)) continue;
        unsigned char h[30];
        fseek(z->f, e->local, SEEK_SET);
        if (fread(h, 1, 30, z->f) != 30 || ep_u32(h) != 0x04034b50) return NULL;
        fseek(z->f, e->local + 30 + ep_u16(h + 26) + ep_u16(h + 28), SEEK_SET);
        char *comp = malloc(e->comp + 1);
        if (fread(comp, 1, e->comp, z->f) != (size_t)e->comp) { free(comp); return NULL; }
        if (e->method == 0) { comp[e->comp] = 0; if (outlen) *outlen = e->comp; return comp; }
        int ol = 0;
        char *raw = stbi_zlib_decode_noheader_malloc(comp, (int)e->comp, &ol);
        free(comp);
        if (!raw) return NULL;
        raw = realloc(raw, ol + 1);
        raw[ol] = 0;
        if (outlen) *outlen = ol;
        return raw;
    }
    return NULL;
}

/* ---------- XML sencillo ---------- */
static int ep_attr(const char *tag, const char *end, const char *name, char *out, size_t n)
{
    char pat[48];
    snprintf(pat, sizeof pat, " %s=", name);
    const char *p = tag;
    while ((p = strstr(p, pat)) && p < end) {
        p += strlen(pat);
        char q = *p;
        if (q != '"' && q != '\'') continue;
        const char *s = ++p, *e = strchr(s, q);
        if (!e || e > end) return 0;
        size_t l = e - s < (long)n - 1 ? (size_t)(e - s) : n - 1;
        memcpy(out, s, l);
        out[l] = 0;
        return 1;
    }
    return 0;
}

static void ep_urldecode(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) { char h[3] = { p[1], p[2], 0 }; *o++ = (char)strtol(h, NULL, 16); p += 2; }
        else *o++ = *p;
    }
    *o = 0;
}

/* Une la carpeta del OPF con un href relativo, resolviendo «..». */
static void ep_join(const char *dir, const char *href, char *out, size_t n)
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s%s", dir, href);
    char *h = strchr(tmp, '#');
    if (h) *h = 0;
    ep_urldecode(tmp);
    char *parts[64];
    int np = 0;
    for (char *t = strtok(tmp, "/"); t && np < 64; t = strtok(NULL, "/")) {
        if (!strcmp(t, "..")) { if (np) np--; }
        else if (strcmp(t, ".")) parts[np++] = t;
    }
    out[0] = 0;
    for (int i = 0; i < np; i++) { if (i) strncat(out, "/", n - strlen(out) - 1); strncat(out, parts[i], n - strlen(out) - 1); }
}

/* ---------- XHTML → texto ---------- */
typedef struct { char *s; long len, cap; } EpBuf;
static void ep_put(EpBuf *b, const char *s, long n)
{
    if (b->len + n + 1 > b->cap) { b->cap = (b->len + n + 1) * 2; b->s = realloc(b->s, b->cap); }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}
static void ep_putc(EpBuf *b, char c) { ep_put(b, &c, 1); }
static void ep_utf8(EpBuf *b, unsigned cp)
{
    char tmp[5];
    int n = 0;
    if (cp < 0x80) tmp[n++] = cp;
    else if (cp < 0x800) { tmp[n++] = 0xC0 | (cp >> 6); tmp[n++] = 0x80 | (cp & 63); }
    else if (cp < 0x10000) { tmp[n++] = 0xE0 | (cp >> 12); tmp[n++] = 0x80 | ((cp >> 6) & 63); tmp[n++] = 0x80 | (cp & 63); }
    else { tmp[n++] = 0xF0 | (cp >> 18); tmp[n++] = 0x80 | ((cp >> 12) & 63); tmp[n++] = 0x80 | ((cp >> 6) & 63); tmp[n++] = 0x80 | (cp & 63); }
    ep_put(b, tmp, n);
}

static const char *ep_block_tags[] = { "p", "div", "br", "h1", "h2", "h3", "h4", "h5", "h6", "li", "tr", "blockquote", "section", "article", "dd", "dt", "hr", "pre", "table", "figure", NULL };

/* Añade el texto de un XHTML a b: párrafos separados por un salto, espacios colapsados. */
static void ep_xhtml_text(const char *x, EpBuf *b)
{
    int skip = 0, space = 0;
    const char *p = x;
    const char *body = strstr(x, "<body");
    if (body) p = body;
    while (*p) {
        if (*p == '<') {
            const char *e = strchr(p, '>');
            if (!e) break;
            int close = p[1] == '/';
            const char *t = p + 1 + close;
            char name[16];
            int k = 0;
            while (k < 15 && (isalnum((unsigned char)t[k]) || t[k] == ':')) { name[k] = tolower((unsigned char)t[k]); k++; }
            name[k] = 0;
            const char *nm = strchr(name, ':') ? strchr(name, ':') + 1 : name;
            if (!strcmp(nm, "script") || !strcmp(nm, "style") || !strcmp(nm, "head") || !strcmp(nm, "title")) skip = !close && e[-1] != '/';
            for (int i = 0; ep_block_tags[i]; i++)
                if (!strcmp(nm, ep_block_tags[i])) {
                    while (b->len && b->s[b->len - 1] == ' ') b->s[--b->len] = 0;
                    if (b->len && b->s[b->len - 1] != '\n') ep_putc(b, '\n');
                    space = 0;
                    break;
                }
            p = e + 1;
            continue;
        }
        if (skip) { p++; continue; }
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            if (b->len && b->s[b->len - 1] != '\n' && b->s[b->len - 1] != ' ') space = 1;
            p++;
            continue;
        }
        if (space) { ep_putc(b, ' '); space = 0; }
        if (*p == '&') {
            const char *sc = strchr(p, ';');
            if (sc && sc - p < 10) {
                char ent[12];
                memcpy(ent, p + 1, sc - p - 1);
                ent[sc - p - 1] = 0;
                unsigned cp = 0;
                if (ent[0] == '#') cp = ent[1] == 'x' || ent[1] == 'X' ? strtoul(ent + 2, NULL, 16) : strtoul(ent + 1, NULL, 10);
                else {
                    static const struct { const char *n; unsigned c; } E[] = {
                        { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", ' ' },
                        { "mdash", 0x2014 }, { "ndash", 0x2013 }, { "laquo", 0xAB }, { "raquo", 0xBB }, { "hellip", 0x2026 },
                        { "iexcl", 0xA1 }, { "iquest", 0xBF }, { "rsquo", 0x2019 }, { "lsquo", 0x2018 }, { "rdquo", 0x201D }, { "ldquo", 0x201C },
                        { "aacute", 0xE1 }, { "eacute", 0xE9 }, { "iacute", 0xED }, { "oacute", 0xF3 }, { "uacute", 0xFA }, { "ntilde", 0xF1 },
                        { "Aacute", 0xC1 }, { "Eacute", 0xC9 }, { "Iacute", 0xCD }, { "Oacute", 0xD3 }, { "Uacute", 0xDA }, { "Ntilde", 0xD1 },
                        { "uuml", 0xFC }, { "Uuml", 0xDC }, { "ccedil", 0xE7 }, { NULL, 0 } };
                    for (int i = 0; E[i].n; i++) if (!strcmp(ent, E[i].n)) cp = E[i].c;
                }
                if (cp) { ep_utf8(b, cp == 0xA0 ? ' ' : cp); p = sc + 1; continue; }
            }
        }
        ep_putc(b, *p++);
    }
    while (b->len && (b->s[b->len - 1] == ' ' || b->s[b->len - 1] == '\n')) b->s[--b->len] = 0;
}

/* ---------- libro ---------- */
typedef struct { long start, len; char title[160]; } EpChapter;
typedef struct {
    char id[24], dir[300], title[300];
    char *text;
    long len;
    EpChapter *ch;
    int nch, drm;
    long *skip;   /* tramos [inicio, fin) que no son la historia: índices, listas, avisos legales */
    int nskip;
} EpBook;

static unsigned long long ep_hash(const char *s)
{
    unsigned long long h = 1469598103934665603ULL;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}

static void ep_free(EpBook *b) { free(b->text); free(b->ch); free(b->skip); memset(b, 0, sizeof *b); }

static int ep_contains(const char *h, long hl, const char *n)
{
    long nl = strlen(n);
    for (long i = 0; i + nl <= hl; i++) if (h[i] == n[0] && !memcmp(h + i, n, nl)) return 1;
    return 0;
}

/* Marca lo que no es la historia: 5 o más líneas cortas seguidas (un índice, aunque no se
 * llame así) y los párrafos con el aviso de Project Gutenberg. No se mandan a la IA (ahorra
 * tokens y evita que un índice destripe capítulos) ni cuentan como apariciones. */
static void ep_mark_skips(EpBook *b)
{
    int cap = 64;
    b->skip = malloc(cap * 2 * sizeof(long));
    b->nskip = 0;
    long run_s = -1;
    int run_n = 0;
    long i = 0;
    while (i <= b->len) {
        long e = i;
        while (e < b->len && b->text[e] != '\n') e++;
        int len = (int)(e - i), legal = 0;
        if (len && ep_contains(b->text + i, len, "Project Gutenberg")) legal = 1;
        int short_line = len > 0 && len < 60;
        if (short_line) { if (run_s < 0) run_s = i; run_n++; }
        if (!short_line || e >= b->len) {
            long run_e = short_line ? e : i;
            if (run_n >= 5) {
                if (b->nskip == cap) { cap *= 2; b->skip = realloc(b->skip, cap * 2 * sizeof(long)); }
                b->skip[2 * b->nskip] = run_s; b->skip[2 * b->nskip + 1] = run_e; b->nskip++;
            }
            run_s = -1; run_n = 0;
        }
        if (legal) {
            if (b->nskip == cap) { cap *= 2; b->skip = realloc(b->skip, cap * 2 * sizeof(long)); }
            b->skip[2 * b->nskip] = i; b->skip[2 * b->nskip + 1] = e; b->nskip++;
        }
        i = e + 1;
    }
}

/* Si off cae en un tramo que no cuenta, devuelve dónde acaba; si no, el propio off. */
static long ep_skip_to(const EpBook *b, long off)
{
    for (int k = 0; k < b->nskip; k++) if (off >= b->skip[2 * k] && off < b->skip[2 * k + 1]) return b->skip[2 * k + 1];
    return off;
}

static int ep_load_cache(EpBook *b, long size, long mtime)
{
    char path[400], buf[600];
    snprintf(path, sizeof path, "%s/meta.txt", b->dir);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    long s = 0, m = 0;
    int drm = 0, ok = fgets(buf, sizeof buf, f) && sscanf(buf, "%ld\t%ld\t%d", &s, &m, &drm) == 3 && s == size && m == mtime;
    if (ok && fgets(buf, sizeof buf, f)) { buf[strcspn(buf, "\n")] = 0; snprintf(b->title, sizeof b->title, "%s", buf); }
    fclose(f);
    if (!ok) return 0;
    b->drm = drm;
    if (drm) return 1;
    snprintf(path, sizeof path, "%s/texto.txt", b->dir);
    if (!(f = fopen(path, "rb"))) return 0;
    fseek(f, 0, SEEK_END);
    b->len = ftell(f);
    fseek(f, 0, SEEK_SET);
    b->text = malloc(b->len + 1);
    b->len = fread(b->text, 1, b->len, f);
    b->text[b->len] = 0;
    fclose(f);
    snprintf(path, sizeof path, "%s/capitulos.tsv", b->dir);
    if (!(f = fopen(path, "r"))) return 0;
    int cap = 64;
    b->ch = malloc(cap * sizeof *b->ch);
    while (fgets(buf, sizeof buf, f)) {
        if (b->nch == cap) { cap *= 2; b->ch = realloc(b->ch, cap * sizeof *b->ch); }
        EpChapter *c = &b->ch[b->nch];
        char *t1 = strchr(buf, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (!t2) continue;
        c->start = atol(buf); c->len = atol(t1 + 1);
        t2[strcspn(t2, "\n")] = 0;
        snprintf(c->title, sizeof c->title, "%s", t2 + 1);
        b->nch++;
    }
    fclose(f);
    return 1;
}

/* Abre el ePub (o su caché). 0 bien, 1 con DRM (sin texto), -1 error. */
static int ep_load(EpBook *b, const char *epub)
{
    memset(b, 0, sizeof *b);
    struct stat st;
    if (stat(epub, &st)) return -1;
    snprintf(b->id, sizeof b->id, "%016llx", ep_hash(epub) ^ (unsigned long long)st.st_size);
    mkdir(POCKIT_DIR, 0755);
    char d[300];
    snprintf(d, sizeof d, POCKIT_DIR "/libros");
    mkdir(d, 0755);
    snprintf(b->dir, sizeof b->dir, POCKIT_DIR "/libros/%s", b->id);
    mkdir(b->dir, 0755);
    if (ep_load_cache(b, st.st_size, st.st_mtime)) { if (!b->drm) ep_mark_skips(b); return b->drm ? 1 : 0; }

    EpZip z;
    if (ep_zip_open(&z, epub)) return -1;
    /* DRM: licencia LCP, o encryption.xml que cifra algo que no sea una fuente */
    long el = 0;
    char *enc = ep_zip_read(&z, "META-INF/encryption.xml", &el);
    for (int i = 0; i < z.n; i++) if (!strcmp(z.e[i].name, "META-INF/license.lcpl") || !strcmp(z.e[i].name, "META-INF/rights.xml")) b->drm = 1;
    if (enc) {
        for (const char *p = enc; (p = strstr(p, "URI=")); p += 4) {
            char uri[300];
            if (!ep_attr(p - 1, p + 400, "URI", uri, sizeof uri)) continue;
            size_t l = strlen(uri);
            int font = l > 4 && (!strcasecmp(uri + l - 4, ".ttf") || !strcasecmp(uri + l - 4, ".otf") || (l > 5 && !strcasecmp(uri + l - 5, ".woff")));
            if (!font) b->drm = 1;
        }
        free(enc);
    }
    char path[400];
    snprintf(path, sizeof path, "%s/meta.txt", b->dir);
    if (b->drm) {
        FILE *f = fopen(path, "w");
        if (f) { fprintf(f, "%ld\t%ld\t1\n", (long)st.st_size, (long)st.st_mtime); fclose(f); }
        ep_zip_close(&z);
        return 1;
    }
    char *cont = ep_zip_read(&z, "META-INF/container.xml", NULL), opf[400] = "";
    if (cont) { const char *r = strstr(cont, "<rootfile "); if (r) ep_attr(r, strchr(r, '>'), "full-path", opf, sizeof opf); free(cont); }
    char *o = opf[0] ? ep_zip_read(&z, opf, NULL) : NULL;
    if (!o) { ep_zip_close(&z); return -1; }
    char dir[400] = "";
    if (strrchr(opf, '/')) { memcpy(dir, opf, strrchr(opf, '/') - opf + 1); dir[strrchr(opf, '/') - opf + 1] = 0; }
    const char *ts = strstr(o, "<dc:title");
    if (ts && (ts = strchr(ts, '>')) && strstr(ts, "</dc:title>")) {
        long l = strstr(ts, "</dc:title>") - ts - 1;
        snprintf(b->title, sizeof b->title, "%.*s", (int)(l < 290 ? l : 290), ts + 1);
    }
    /* manifest (id → href) y spine (orden) */
    int ni = 0, cap = 256;
    char (*ids)[80] = malloc(cap * 80), (*hrefs)[300] = malloc(cap * 300);
    for (const char *p = o; (p = strstr(p, "<item ")); ) {
        const char *e = strchr(p, '>');
        if (!e) break;
        if (ni == cap) { cap *= 2; ids = realloc(ids, cap * 80); hrefs = realloc(hrefs, cap * 300); }
        if (ep_attr(p, e, "id", ids[ni], 80) && ep_attr(p, e, "href", hrefs[ni], 300)) ni++;
        p = e;
    }
    EpBuf text = { 0 };
    int chcap = 64;
    b->ch = malloc(chcap * sizeof *b->ch);
    for (const char *p = o; (p = strstr(p, "<itemref")); ) {
        const char *e = strchr(p, '>');
        if (!e) break;
        char idref[80];
        if (ep_attr(p, e, "idref", idref, sizeof idref))
            for (int i = 0; i < ni; i++) {
                if (strcmp(ids[i], idref)) continue;
                char full[400];
                ep_join(dir, hrefs[i], full, sizeof full);
                char *x = ep_zip_read(&z, full, NULL);
                if (b->nch == chcap) { chcap *= 2; b->ch = realloc(b->ch, chcap * sizeof *b->ch); }
                EpChapter *c = &b->ch[b->nch++];
                if (text.len) ep_putc(&text, '\n');
                c->start = text.len;
                if (x) { ep_xhtml_text(x, &text); free(x); }
                c->len = text.len - c->start;
                /* título: la primera línea si es corta (suele ser el encabezado) */
                const char *ls = text.s ? text.s + c->start : "";
                long ll = 0;
                while (ll < c->len && ls[ll] != '\n') ll++;
                if (c->len && ll <= 80) snprintf(c->title, sizeof c->title, "%.*s", (int)ll, ls);
                else c->title[0] = 0;
                break;
            }
        p = e;
    }
    free(ids); free(hrefs); free(o);
    ep_zip_close(&z);
    b->text = text.s ? text.s : calloc(1, 1);
    b->len = text.len;
    /* caché */
    FILE *f = fopen(path, "w");
    if (f) { fprintf(f, "%ld\t%ld\t0\n%s\n", (long)st.st_size, (long)st.st_mtime, b->title); fclose(f); }
    snprintf(path, sizeof path, "%s/texto.txt", b->dir);
    if ((f = fopen(path, "wb"))) { fwrite(b->text, 1, b->len, f); fclose(f); }
    snprintf(path, sizeof path, "%s/capitulos.tsv", b->dir);
    if ((f = fopen(path, "w"))) { for (int i = 0; i < b->nch; i++) fprintf(f, "%ld\t%ld\t%s\n", b->ch[i].start, b->ch[i].len, b->ch[i].title); fclose(f); }
    ep_mark_skips(b);
    return 0;
}

static int ep_chapter_at(const EpBook *b, long off)
{
    int k = 0;
    for (int i = 0; i < b->nch; i++) if (b->ch[i].len && b->ch[i].start <= off) k = i;
    return k;
}

/* Carácter hasta el que has leído. position es la de books_settings ("…epubcfi(/6/60!/4/…:N)"),
 * progress va de 0 a 1. Se queda un poco antes y en un final de párrafo: mejor quedarse corto
 * que destripar. */
static long ep_position(const EpBook *b, const char *position, double progress)
{
    if (!b->len) return 0;
    long target = (long)(progress * b->len);
    const char *c = position ? strstr(position, "epubcfi(/6/") : NULL;
    if (c) {
        int idx = atoi(c + 11) / 2 - 1;
        if (idx >= 0 && idx < b->nch) {
            long s = b->ch[idx].start, e = s + b->ch[idx].len;
            if (target < s) target = s + 200 < e ? s + 200 : e;   /* al menos el principio del capítulo */
            if (target > e) target = e;
        }
    }
    target -= 600;
    if (target <= 0) return 0;
    if (target > b->len) target = b->len;
    while (target > 0 && b->text[target - 1] != '\n') target--;
    return target;
}

/* ---------- personajes (sin IA) ---------- */
typedef struct {
    char name[64];
    int count, first_ch, with[3], nwith;
    long first;
} EpPerson;

typedef struct { char key[48]; char shown[64]; int cap, start, lower; long first; } EpWord;

static unsigned ep_cp(const unsigned char *s, int *n)
{
    if (s[0] < 0x80) { *n = 1; return s[0]; }
    if ((s[0] & 0xE0) == 0xC0 && s[1]) { *n = 2; return (s[0] & 31) << 6 | (s[1] & 63); }
    if ((s[0] & 0xF0) == 0xE0 && s[1] && s[2]) { *n = 3; return (s[0] & 15) << 12 | (s[1] & 63) << 6 | (s[2] & 63); }
    *n = 1;
    return 0xFFFD;
}
static int ep_is_letter(unsigned c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7) || (c >= 0x370 && c < 0x2000); }
static int ep_is_upper(unsigned c) { return (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7); }

static const char *ep_stop[] = {
    "el", "la", "los", "las", "lo", "un", "una", "unos", "unas", "y", "e", "o", "u", "ni", "pero", "mas", "más", "que", "qué",
    "es", "no", "sí", "si", "se", "de", "del", "en", "por", "para", "con", "sin", "al", "a", "mi", "tu", "su", "sus", "mis",
    "yo", "él", "ella", "ellos", "ellas", "usted", "ustedes", "nosotros", "vosotros", "señor", "señora", "señorita", "don", "doña",
    "dios", "capítulo", "parte", "libro", "lunes", "martes", "miércoles", "jueves", "viernes", "sábado", "domingo", "enero",
    "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto", "septiembre", "octubre", "noviembre", "diciembre", "pues",
    "cuando", "como", "cómo", "ya", "ah", "oh", "eh", "bueno", "entonces", "así", "este", "esta", "esto", "ese", "esa", "eso",
    "aquel", "aquella", "aquí", "allí", "ahora", "nunca", "siempre", "también", "tampoco", "vamos", "mire", "oye", "hola",
    "adiós", "gracias", "sr", "sra", "ii", "iii", "iv", "vi", "vii", "viii", "ix", "x", "xi", "xii", "the", "and", "of",
    "dónde", "quién", "cuál", "cuánto", "porque", "aunque", "mientras", "luego", "tal", "todo", "toda", "nada", "nadie",
    "le", "les", "me", "te", "nos", "os", "otro", "otra", "muy", "bien", "mal", "fin", "dios", "virgen", "santo", "san", NULL };

static int ep_is_stop(const char *k) { for (int i = 0; ep_stop[i]; i++) if (!strcmp(k, ep_stop[i])) return 1; return 0; }

/* minúsculas para la clave (ASCII y latín-1) */
static void ep_fold(const char *w, int n, char *out, size_t on)
{
    size_t j = 0;
    for (int i = 0; i < n && j + 3 < on; ) {
        int l;
        unsigned c = ep_cp((const unsigned char *)w + i, &l);
        if (c >= 'A' && c <= 'Z') c += 32;
        else if (c >= 0xC0 && c <= 0xDE && c != 0xD7) c += 32;
        if (c < 0x80) out[j++] = c;
        else if (c < 0x800) { out[j++] = 0xC0 | (c >> 6); out[j++] = 0x80 | (c & 63); }
        else { out[j++] = 0xE0 | (c >> 12); out[j++] = 0x80 | ((c >> 6) & 63); out[j++] = 0x80 | (c & 63); }
        i += l;
    }
    out[j] = 0;
}

#define EP_HASH 65536
#define EP_BIG 8192
static int ep_cmp_people(const void *a, const void *b) { return ((const EpPerson *)b)->count - ((const EpPerson *)a)->count; }

/* ¿Es un capítulo de índice? Sus nombres no cuentan como apariciones. */
static int ep_is_toc(const EpChapter *c)
{
    static const char *t[] = { "Índice", "índice", "Indice", "Contenido", "Sumario", "Contents", "Table of Contents", "Índice general", NULL };
    for (int i = 0; t[i]; i++) if (!strcmp(c->title, t[i])) return 1;
    return 0;
}

typedef struct { char key[96]; int n; } EpBigram;

/* Recorre las palabras de [0, upto) saltando índices. Para cada palabra llama a fn. */
typedef void (*ep_word_fn)(void *ctx, long off, int len, int upper, int sentence, int new_par, int joined);
static void ep_words(const EpBook *b, long upto, ep_word_fn fn, void *ctx)
{
    const unsigned char *s = (const unsigned char *)b->text;
    int sentence = 1, new_par = 1, ch = 0;
    long i = 0, prev_end = -10;
    while (i < upto) {
        while (ch + 1 < b->nch && b->ch[ch + 1].start <= i) ch++;
        if (ep_is_toc(&b->ch[ch])) { i = b->ch[ch].start + b->ch[ch].len + 1; sentence = new_par = 1; continue; }
        long sk = ep_skip_to(b, i);
        if (sk != i) { i = sk; sentence = new_par = 1; continue; }
        int l;
        unsigned c = ep_cp(s + i, &l);
        if (!ep_is_letter(c)) {
            if (c == '.' || c == '!' || c == '?' || c == '\n' || c == 0x2026 || c == ':') sentence = 1;
            if (c == '\n') new_par = 1;
            i += l;
            continue;
        }
        long w0 = i;
        int up = ep_is_upper(c);
        while (i < upto) { unsigned d = ep_cp(s + i, &l); if (!ep_is_letter(d) && d != '\'') break; i += l; }
        fn(ctx, w0, (int)(i - w0), up, sentence, new_par, w0 == prev_end + 1 && s[prev_end] == ' ');
        prev_end = i;
        sentence = new_par = 0;
    }
}

typedef struct { EpWord *tab; EpBigram *big; const char *text; char prev[48]; int prev_up; } EpCount;

static EpWord *ep_slot(EpWord *tab, const char *key)
{
    unsigned long long h = ep_hash(key) & (EP_HASH - 1);
    while (tab[h].key[0] && strcmp(tab[h].key, key)) h = (h + 1) & (EP_HASH - 1);
    return &tab[h];
}

static void ep_count_word(void *vc, long off, int len, int up, int sentence, int new_par, int joined)
{
    EpCount *c = vc;
    if (len < 2 || len > 40) { c->prev_up = 0; return; }
    char key[48];
    ep_fold(c->text + off, len, key, sizeof key);
    EpWord *w = ep_slot(c->tab, key);
    if (!w->key[0]) { snprintf(w->key, sizeof w->key, "%s", key); w->first = -1; }
    if (!up) w->lower++;
    else if (sentence) w->start++;
    else { w->cap++; if (w->first < 0) { w->first = off; snprintf(w->shown, sizeof w->shown, "%.*s", len < 60 ? len : 60, c->text + off); } }
    /* dos nombres seguidos («Sancho Panza»): se cuentan para unirlos luego */
    if (up && joined && c->prev_up) {
        char bk[96];
        snprintf(bk, sizeof bk, "%s %s", c->prev, key);
        unsigned long long h = ep_hash(bk) & (EP_BIG - 1);
        while (c->big[h].key[0] && strcmp(c->big[h].key, bk)) h = (h + 1) & (EP_BIG - 1);
        if (!c->big[h].key[0]) snprintf(c->big[h].key, sizeof c->big[h].key, "%s", bk);
        c->big[h].n++;
    }
    snprintf(c->prev, sizeof c->prev, "%s", key);
    c->prev_up = up;
}

typedef struct { const char *text; char keys[40][48]; int n; int in[40]; int pair[40][40]; } EpCo;

static void ep_flush_par(EpCo *c)
{
    for (int a = 0; a < c->n; a++) if (c->in[a]) for (int b = 0; b < c->n; b++) if (b != a && c->in[b]) c->pair[a][b]++;
    memset(c->in, 0, sizeof c->in);
}

static void ep_co_word(void *vc, long off, int len, int up, int sentence, int new_par, int joined)
{
    EpCo *c = vc;
    if (new_par) ep_flush_par(c);
    if (!up || len < 2 || len > 40) return;
    char key[48];
    ep_fold(c->text + off, len, key, sizeof key);
    for (int a = 0; a < c->n; a++) if (!strcmp(c->keys[a], key)) { c->in[a] = 1; break; }
}

/* Personajes (y lugares: eso lo distingue la IA) hasta el carácter `upto`. */
static int ep_people(const EpBook *b, long upto, EpPerson *out, int max)
{
    EpCount cnt = { calloc(EP_HASH, sizeof(EpWord)), calloc(EP_BIG, sizeof(EpBigram)), b->text, "", 0 };
    ep_words(b, upto, ep_count_word, &cnt);
    int n = 0;
    EpPerson *all = malloc(sizeof *all * 400);
    char (*keys)[48] = malloc(400 * 48);
    for (int k = 0; k < EP_HASH && n < 400; k++) {
        EpWord *w = &cnt.tab[k];
        if (!w->key[0] || w->cap < 3 || w->lower * 3 > w->cap || ep_is_stop(w->key) || strlen(w->key) < 3) continue;
        EpPerson *p = &all[n];
        memset(p, 0, sizeof *p);
        snprintf(p->name, sizeof p->name, "%s", w->shown);
        snprintf(keys[n], 48, "%s", w->key);
        p->count = w->cap + w->start;
        p->first = w->first;
        p->first_ch = ep_chapter_at(b, w->first);
        n++;
    }
    /* unir nombres compuestos: si «A B» es la forma habitual, queda «A B» y B desaparece */
    for (int k = 0; k < EP_BIG; k++) {
        EpBigram *g = &cnt.big[k];
        if (!g->key[0] || g->n < 2) continue;
        char *sp = strchr(g->key, ' ');
        *sp = 0;
        int a = -1, c = -1;
        for (int i = 0; i < n; i++) { if (!strcmp(keys[i], g->key)) a = i; if (!strcmp(keys[i], sp + 1)) c = i; }
        if (a >= 0 && c >= 0 && all[c].count > 0 && g->n * 10 >= 6 * (all[a].count < all[c].count ? all[a].count : all[c].count)) {
            char joined[64];
            snprintf(joined, sizeof joined, "%s %s", all[a].name, all[c].name);
            snprintf(all[a].name, sizeof all[a].name, "%s", joined);
            if (all[c].count > all[a].count) all[a].count = all[c].count;
            all[c].count = 0;
        }
        *sp = ' ';
    }
    free(cnt.tab);
    free(cnt.big);
    int m = 0;
    for (int i = 0; i < n; i++) if (all[i].count > 0) { all[m] = all[i]; memcpy(keys[m], keys[i], 48); m++; }
    n = m;
    /* ordenar con sus claves */
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && all[j].count > all[j - 1].count; j--) {
            EpPerson t = all[j]; all[j] = all[j - 1]; all[j - 1] = t;
            char tk[48]; memcpy(tk, keys[j], 48); memcpy(keys[j], keys[j - 1], 48); memcpy(keys[j - 1], tk, 48);
        }
    if (n > max) n = max;
    /* con quién coincide en el mismo párrafo (un segundo recorrido, sin buscar texto) */
    EpCo *co = calloc(1, sizeof *co);
    co->text = b->text;
    co->n = n < 40 ? n : 40;
    for (int i = 0; i < co->n; i++) memcpy(co->keys[i], keys[i], 48);
    ep_words(b, upto, ep_co_word, co);
    ep_flush_par(co);
    for (int a = 0; a < co->n; a++)
        for (int k = 0; k < 3; k++) {
            int best = -1, bv = 1;
            for (int c = 0; c < co->n; c++) {
                int used = 0;
                for (int j = 0; j < all[a].nwith; j++) if (all[a].with[j] == c) used = 1;
                if (!used && co->pair[a][c] > bv) { bv = co->pair[a][c]; best = c; }
            }
            if (best >= 0) all[a].with[all[a].nwith++] = best;
        }
    free(co);
    memcpy(out, all, n * sizeof *out);
    free(all);
    free(keys);
    return n;
}

/* Hasta dos frases (cortas) donde sale el nombre, para que la IA lo describa sin leer el libro. */
static void ep_context(const EpBook *b, const char *name, long upto, char *out, size_t n)
{
    out[0] = 0;
    const char *p = b->text;
    int got = 0;
    while (got < 2 && (p = strstr(p, name)) && p < b->text + upto) {
        const char *s = p, *e = p;
        while (s > b->text && s[-1] != '.' && s[-1] != '\n' && s[-1] != '!' && s[-1] != '?' && p - s < 220) s--;
        while (*e && *e != '.' && *e != '\n' && *e != '!' && *e != '?' && e - p < 220) e++;
        if (*e && *e != '\n') e++;
        while (((unsigned char)*e & 0xC0) == 0x80) e++;   /* sin partir letras (UTF-8) */
        while (((unsigned char)*s & 0xC0) == 0x80) s++;
        for (;;) {   /* sin guiones de diálogo ni espacios al principio */
            if (*s == ' ' || *s == '-') s++;
            else if ((unsigned char)s[0] == 0xE2 && (unsigned char)s[1] == 0x80 && ((unsigned char)s[2] == 0x94 || (unsigned char)s[2] == 0x93)) s += 3;
            else break;
        }
        if (e - s < 40 || ep_is_toc(&b->ch[ep_chapter_at(b, s - b->text)]) || ep_skip_to(b, s - b->text) != s - b->text) { p = e > p ? e : p + 1; continue; }
        size_t l = strlen(out);
        snprintf(out + l, n - l, "%s%.*s", got ? " / " : "", (int)(e - s), s);
        got++;
        p = e > p ? e : p + 1;
    }
}

#endif
