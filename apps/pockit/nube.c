/*
 * nube.c: Pockit sincroniza tus datos (Pockit/ en el lector) con la carpeta /Pockit de tu Dropbox.
 *
 * La sincronización de Dropbox que trae el lector solo sabe bajar archivos y subir los nuevos: si
 * cambias uno que ya está en Dropbox, sube una copia «nombre (1)» y avisa de error. Por eso Pockit
 * sincroniza por su cuenta, con su propio permiso, y fuera de la carpeta «Dropbox PocketBook».
 *
 *  - El permiso lo deja pockit.yous.dev al enlazar por cable (system/config/pockit/dropbox.cfg).
 *  - Se sincroniza al abrir Pockit, al volver al Inicio y al cerrar, si la wifi ya está encendida
 *    (nunca la enciende por esto), y con «Sincronizar ahora» en Ajustes.
 *  - Cada archivo se compara con cómo estaba en la última sincronización (nube.tsv): lo que cambió
 *    en un solo sitio se copia al otro; si cambió en los dos, se quedan los dos (el de Dropbox como
 *    «nombre (Dropbox).md»). Lo borrado en un sitio se borra en el otro: en Dropbox va a su
 *    papelera y en el lector a system/config/pockit/papelera, así que nada se pierde.
 *  - Los libros que mandas desde la web llegan igual: van a /Pockit/Biblioteca y se bajan a
 *    Pockit/Biblioteca, donde la biblioteca del lector los encuentra. Pueden pesar decenas de MB:
 *    no pasan por la memoria (/tmp es RAM) y se van contando en pantalla.
 */
#include "inkview.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>
#include <utime.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "pockit.h"
#include "../../common/json.h"

#define CACHE_DIR FLASHDIR "/system/config/pockit"
#define NUBE_CFG CACHE_DIR "/dropbox.cfg"           /* refresh=<permiso de Dropbox>, lo escribe la web */
#define NUBE_STATE CACHE_DIR "/nube.tsv"            /* ruta, versión, tamaño y fecha en la última sincronización */
#define NUBE_LAST CACHE_DIR "/nube-ultima.txt"      /* cuándo y cómo fue la última */
#define TRASH CACHE_DIR "/papelera"
#define REMOTE "/Pockit"
#define APP_KEY "bqmwi35xfpobsoe"                   /* la app «Pockit» de Dropbox (pública, sin secreto) */
#define TMP_HDR "/tmp/pk-nube-cabecera.txt"
#define TMP_BODY "/tmp/pk-nube-peticion.json"
#define TMP_OUT "/tmp/pk-nube-respuesta"
#define TMP_FILE "/tmp/pk-nube-archivo"           /* enlace al archivo que se sube: así su nombre no pasa por la shell */
#define DL_FILE CACHE_DIR "/nube-bajando"           /* lo que se baja, en la memoria del lector y no en RAM */
#define BIG (512 * 1024)                            /* a partir de aquí se dice qué se está copiando */

#ifdef NUBE_TEST   /* pruebas en el ordenador, contra un Dropbox de mentira */
static const char *api(void) { return getenv("PK_NUBE_API"); }
static const char *content(void) { return getenv("PK_NUBE_API"); }
#else
static const char *api(void) { return "https://api.dropboxapi.com"; }
static const char *content(void) { return "https://content.dropboxapi.com"; }
#endif

typedef struct {
    char key[400], rel[400];   /* clave en minúsculas y ruta tal cual, relativas a Pockit/ */
    int l, r, s;               /* está en el lector, en Dropbox y en la última sincronización */
    long lsize, lmtime;
    long rsize; char rrev[64]; long rmod;
    long ssize, smtime; char srev[64];
} Item;

static Item *items;
static int nitems, cap;
static char token[4096];   /* los permisos de Dropbox pasan de 1.300 caracteres */
static long token_until;

/* ---------- utilidades ---------- */

static Item *item(const char *rel)
{
    char key[400];
    snprintf(key, sizeof key, "%s", rel);
    for (char *p = key; *p; p++) *p = tolower((unsigned char)*p);
    for (int i = 0; i < nitems; i++) if (!strcmp(items[i].key, key)) return &items[i];
    if (nitems == cap) { cap = cap ? cap * 2 : 128; items = realloc(items, cap * sizeof *items); }
    Item *it = &items[nitems++];
    memset(it, 0, sizeof *it);
    snprintf(it->key, sizeof it->key, "%s", key);
    snprintf(it->rel, sizeof it->rel, "%s", rel);
    return it;
}

static char *slurp(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc(n + 1);
    if (s) { n = (long)fread(s, 1, n, f); s[n] = 0; }
    fclose(f);
    if (len) *len = n;
    return s;
}

static void mkdirs(const char *path)   /* las carpetas de una ruta de archivo */
{
    char p[700];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
}

static int copy(const char *a, const char *b)
{
    FILE *x = fopen(a, "rb"), *y = x ? fopen(b, "wb") : NULL;
    if (!x || !y) { if (x) fclose(x); return 0; }
    char buf[8192];
    size_t r;
    int ok = 1;
    while ((r = fread(buf, 1, sizeof buf, x)) > 0) if (fwrite(buf, 1, r, y) != r) ok = 0;
    fclose(x);
    if (fclose(y)) ok = 0;
    return ok;
}

static int same_content(const char *a, const char *b)   /* a trozos: pueden ser libros */
{
    FILE *x = fopen(a, "rb"), *y = fopen(b, "rb");
    int same = x && y;
    char p[8192], q[8192];
    while (same) {
        size_t n = fread(p, 1, sizeof p, x), m = fread(q, 1, sizeof q, y);
        if (n != m || memcmp(p, q, n)) same = 0;
        if (!n) break;
    }
    if (x) fclose(x);
    if (y) fclose(y);
    return same;
}

/* Cadena JSON solo con ASCII: las cabeceras HTTP no admiten otra cosa (Dropbox pide \uXXXX) */
static void hdr_str(JBuf *b, const char *s)
{
    jb_cat(b, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p; ) {
        unsigned cp;
        p += pockit_u8(p, &cp);
        char e[16];
        if (cp == '"' || cp == '\\') { e[0] = '\\'; e[1] = cp; e[2] = 0; }
        else if (cp >= 0x20 && cp < 0x7f) { e[0] = cp; e[1] = 0; }
        else if (cp < 0x10000) snprintf(e, sizeof e, "\\u%04x", cp);
        else { cp -= 0x10000; snprintf(e, sizeof e, "\\u%04x\\u%04x", 0xD800 + (cp >> 10), 0xDC00 + (cp & 0x3ff)); }
        jb_cat(b, e);
    }
    jb_cat(b, "\"");
}

/* ---------- HTTP (curl, como la IA) ----------
 * Devuelve el código HTTP, o -<código de curl> si no llegó a hablar con Dropbox. */
static int dbx_http(const char *url, const char *headers, const char *data_file, int binary, const char *out)
{
    FILE *h = fopen(TMP_HDR, "w");
    if (!h) return -1;
    fputs(headers, h);
    fclose(h);
    /* un libro puede tardar minutos: se corta solo si se queda parado, no por tardar */
    const char *limit = strstr(url, "/files/upload") || strstr(url, "/files/download") ? "-m 1800 --speed-limit 256 --speed-time 60" : "-m 90";
    char cmd[900];
    snprintf(cmd, sizeof cmd, "curl -s %s -o '%s' -w '%%{http_code}' -H @" TMP_HDR " %s%s%s -X POST '%s'",
             limit, out, data_file ? (binary ? "--data-binary @'" : "-d @'") : "", data_file ? data_file : "", data_file ? "'" : "", url);
    FILE *p = popen(cmd, "r");
    if (!p) { remove(TMP_HDR); return -1; }
    char code[16] = "";
    if (!fgets(code, sizeof code, p)) code[0] = 0;
    int st = pclose(p);
    remove(TMP_HDR);   /* lleva el permiso: no se deja en /tmp */
    int ex = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
    if (ex) return -ex;
    return atoi(code);
}

static int refresh_token(char *err, size_t n)
{
    if (token[0] && time(NULL) < token_until) return 1;
    char refresh[400] = "", line[600];
    FILE *f = fopen(NUBE_CFG, "r");
    while (f && fgets(line, sizeof line, f)) if (!strncmp(line, "refresh=", 8)) { snprintf(refresh, sizeof refresh, "%s", line + 8); refresh[strcspn(refresh, "\r\n")] = 0; }
    if (f) fclose(f);
    if (!refresh[0]) { snprintf(err, n, T("Pockit no está enlazado con Dropbox.")); return 0; }
    FILE *b = fopen(TMP_BODY, "w");
    if (!b) return 0;
    fprintf(b, "grant_type=refresh_token&refresh_token=%s&client_id=" APP_KEY, refresh);
    fclose(b);
    char url[200];
    snprintf(url, sizeof url, "%s/oauth2/token", api());
    int code = dbx_http(url, "Content-Type: application/x-www-form-urlencoded\n", TMP_BODY, 0, TMP_OUT);
    remove(TMP_BODY);
    long len;
    char *s = slurp(TMP_OUT, &len);
    remove(TMP_OUT);
    if (code == 200 && s) {
        JTok t[64];
        int k = json_parse(s, (int)len, t, 64);
        int a = k > 0 ? json_key(s, t, k, 0, "access_token") : -1;
        if (a >= 0) {
            json_str(s, &t[a], token, sizeof token);
            token_until = time(NULL) + (long)json_num_key(s, t, k, 0, "expires_in") - 120;
            free(s);
            return 1;
        }
    }
    free(s);
    if (code < 0) snprintf(err, n, code == -6 || code == -7 ? T("No hay conexión con Dropbox.") : T("No se pudo hablar con Dropbox (error %d de curl)."), -code);
    else if (code == 400 || code == 401) snprintf(err, n, T("Dropbox ya no da permiso a Pockit. Vuelve a enlazarlo desde pockit.yous.dev, con el cable."));
    else snprintf(err, n, T("Dropbox respondió con un error (%d)."), code);
    return 0;
}

/* Llamada a la API con cuerpo JSON. La respuesta queda en TMP_OUT. */
static int rpc(const char *name, const char *json)
{
    FILE *b = fopen(TMP_BODY, "w");
    if (!b) return -1;
    fputs(json, b);
    fclose(b);
    char url[200];
    snprintf(url, sizeof url, "%s/2/%s", api(), name);
    JBuf h = { 0 };
    jb_cat(&h, "Authorization: Bearer ");
    jb_cat(&h, token);
    jb_cat(&h, "\nContent-Type: application/json\n");
    int code = dbx_http(url, h.s, TMP_BODY, 0, TMP_OUT);
    free(h.s);
    remove(TMP_BODY);
    return code;
}

/* ---------- los tres lados ---------- */

static void walk(const char *dir, const char *rel)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t nl = strlen(e->d_name);
        if (e->d_name[0] == '.' || (nl > 4 && !strcmp(e->d_name + nl - 4, ".tmp"))) continue;
        char path[900], r[500];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        snprintf(r, sizeof r, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        struct stat st;
        if (stat(path, &st)) continue;
        if (S_ISDIR(st.st_mode)) { walk(path, r); continue; }
        Item *it = item(r);
        it->l = 1;
        it->lsize = st.st_size;
        it->lmtime = st.st_mtime;
    }
    closedir(d);
}

static void load_state(void)
{
    FILE *f = fopen(NUBE_STATE, "r");
    char line[1200];
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *rev = strchr(line, '\t'), *size = rev ? strchr(rev + 1, '\t') : NULL, *mt = size ? strchr(size + 1, '\t') : NULL;
        if (!mt) continue;
        *rev++ = *size++ = *mt++ = 0;
        Item *it = item(line);
        it->s = 1;
        snprintf(it->srev, sizeof it->srev, "%s", rev);
        it->ssize = atol(size);
        it->smtime = atol(mt);
    }
    if (f) fclose(f);
}

static void save_state(void)
{
    FILE *f = fopen(NUBE_STATE ".tmp", "w");
    if (!f) return;
    for (int i = 0; i < nitems; i++) if (items[i].s) fprintf(f, "%s\t%s\t%ld\t%ld\n", items[i].rel, items[i].srev, items[i].ssize, items[i].smtime);
    fclose(f);
    rename(NUBE_STATE ".tmp", NUBE_STATE);
}

static long iso_time(const char *s)   /* «2026-11-02T19:42:14Z» */
{
    struct tm tm = { 0 };
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) return 0;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    return (long)timegm(&tm);
}

/* Lo que hay en /Pockit de Dropbox. 0 si la carpeta no existe; -1 si falló. */
static int list_remote(char *err, size_t n)
{
    int found = 0, more = 1, first = 1;
    char cursor[1200] = "";
    while (more) {
        JBuf b = { 0 };
        if (first) jb_cat(&b, "{\"path\":\"" REMOTE "\",\"recursive\":true,\"limit\":2000}");
        else { jb_cat(&b, "{\"cursor\":"); jb_str(&b, cursor); jb_cat(&b, "}"); }
        int code = rpc(first ? "files/list_folder" : "files/list_folder/continue", b.s);
        free(b.s);
        long len;
        char *s = slurp(TMP_OUT, &len);
        remove(TMP_OUT);
        if (code == 409 && s && strstr(s, "not_found")) { free(s); return 0; }
        if (code != 200 || !s) {
            snprintf(err, n, code < 0 ? T("No hay conexión con Dropbox.") : T("Dropbox no dejó ver tu carpeta Pockit (%d)."), code);
            free(s);
            return -1;
        }
        int max = (int)(len / 6) + 64;
        JTok *t = malloc(max * sizeof *t);
        int k = t ? json_parse(s, (int)len, t, max) : -1;
        int ents = k > 0 ? json_key(s, t, k, 0, "entries") : -1;
        for (int e = 0; ents >= 0 && e < t[ents].size; e++) {
            int o = json_at(t, k, ents, e);
            char tag[16] = "", path[600] = "";
            int a = json_key(s, t, k, o, ".tag");
            if (a >= 0) json_str(s, &t[a], tag, sizeof tag);
            if (strcmp(tag, "file")) continue;
            if ((a = json_key(s, t, k, o, "path_display")) < 0) continue;
            json_str(s, &t[a], path, sizeof path);
            if (strncasecmp(path, REMOTE "/", strlen(REMOTE) + 1)) continue;
            Item *it = item(path + strlen(REMOTE) + 1);
            it->r = 1;
            it->rsize = (long)json_num_key(s, t, k, o, "size");
            if ((a = json_key(s, t, k, o, "rev")) >= 0) json_str(s, &t[a], it->rrev, sizeof it->rrev);
            char cm[40] = "";
            if ((a = json_key(s, t, k, o, "client_modified")) >= 0) json_str(s, &t[a], cm, sizeof cm);
            it->rmod = iso_time(cm);
        }
        int hm = k > 0 ? json_key(s, t, k, 0, "has_more") : -1;
        more = hm >= 0 && json_true(s, &t[hm]);
        int c = k > 0 ? json_key(s, t, k, 0, "cursor") : -1;
        if (c >= 0) json_str(s, &t[c], cursor, sizeof cursor);
        found = 1;
        first = 0;
        free(t);
        free(s);
    }
    return found;
}

/* ---------- copiar en un sentido u otro ---------- */

static void local_path(char *out, size_t n, const char *rel) { snprintf(out, n, "%s/%s", pockit_data(), rel); }

/* Sube el archivo del lector. rev: la versión de Dropbox que se sustituye (o NULL si es nuevo). */
static int upload(Item *it, const char *rev)
{
    char path[900];
    local_path(path, sizeof path, it->rel);
    remove(TMP_FILE);
    if (symlink(path, TMP_FILE) && !copy(path, TMP_FILE)) return 0;
    struct stat st;
    stat(path, &st);
    char when[40];
    struct tm tm;
    time_t mt = st.st_mtime;
    gmtime_r(&mt, &tm);
    strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%SZ", &tm);
    JBuf a = { 0 };
    jb_cat(&a, "Authorization: Bearer ");
    jb_cat(&a, token);
    jb_cat(&a, "\nContent-Type: application/octet-stream\nDropbox-API-Arg: {\"path\":");
    char rp[500];
    snprintf(rp, sizeof rp, REMOTE "/%s", it->rel);
    hdr_str(&a, rp);
    if (rev) { jb_cat(&a, ",\"mode\":{\".tag\":\"update\",\"update\":"); hdr_str(&a, rev); jb_cat(&a, "}"); }
    else jb_cat(&a, ",\"mode\":\"add\"");
    jb_cat(&a, ",\"autorename\":false,\"mute\":true,\"client_modified\":\"");
    jb_cat(&a, when);
    jb_cat(&a, "\"}\n");
    char url[200];
    snprintf(url, sizeof url, "%s/2/files/upload", content());
    int code = dbx_http(url, a.s, TMP_FILE, 1, TMP_OUT);
    free(a.s);
    remove(TMP_FILE);
    long len;
    char *s = slurp(TMP_OUT, &len);
    remove(TMP_OUT);
    int ok = 0;
    if (code == 200 && s) {
        JTok t[128];
        int k = json_parse(s, (int)len, t, 128), r = k > 0 ? json_key(s, t, k, 0, "rev") : -1;
        if (r >= 0) {
            json_str(s, &t[r], it->srev, sizeof it->srev);
            it->s = 1;
            it->ssize = st.st_size;
            it->smtime = st.st_mtime;
            ok = 1;
        }
    }
    free(s);
    return ok;
}

/* Baja la versión que hay en Dropbox a «to» (relativa a Pockit/) */
static int download(Item *it, const char *to)
{
    JBuf a = { 0 };
    jb_cat(&a, "Authorization: Bearer ");
    jb_cat(&a, token);
    jb_cat(&a, "\nDropbox-API-Arg: {\"path\":");
    char rp[100];
    snprintf(rp, sizeof rp, "rev:%s", it->rrev);   /* justo la versión que se vio en la lista */
    hdr_str(&a, rp);
    jb_cat(&a, "}\n");
    char url[200];
    snprintf(url, sizeof url, "%s/2/files/download", content());
    mkdir(CACHE_DIR, 0755);
    int code = dbx_http(url, a.s, NULL, 0, DL_FILE);
    free(a.s);
    if (code != 200) { remove(DL_FILE); return 0; }
    char path[900];
    local_path(path, sizeof path, to);
    mkdirs(path);
    int ok = !rename(DL_FILE, path);   /* misma tarjeta: se cambia de sitio sin copiarlo */
    if (!ok) {
        char tmp[920];
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        ok = copy(DL_FILE, tmp) && !rename(tmp, path);
        if (!ok) remove(tmp);
    }
    remove(DL_FILE);
    if (!ok) return 0;
    if (it->rmod > 0) { struct utimbuf u = { it->rmod, it->rmod }; utime(path, &u); }   /* la hora en que se escribió, no la de bajarlo */
    return 1;
}

static void record_download(Item *it)
{
    char path[900];
    local_path(path, sizeof path, it->rel);
    struct stat st;
    if (stat(path, &st)) return;
    it->s = 1;
    snprintf(it->srev, sizeof it->srev, "%s", it->rrev);
    it->ssize = st.st_size;
    it->smtime = st.st_mtime;
    it->l = 1;
    it->lsize = st.st_size;
    it->lmtime = st.st_mtime;
}

static void to_trash(const char *rel)
{
    char from[900], to[900];
    local_path(from, sizeof from, rel);
    snprintf(to, sizeof to, TRASH "/%ld/%s", (long)time(NULL), rel);
    mkdirs(to);
    if (rename(from, to)) remove(from);
}

static int remote_delete(Item *it)
{
    JBuf b = { 0 };
    char rp[500];
    snprintf(rp, sizeof rp, REMOTE "/%s", it->rel);
    jb_cat(&b, "{\"path\":");
    jb_str(&b, rp);
    jb_cat(&b, "}");
    int code = rpc("files/delete_v2", b.s);
    free(b.s);
    long len;
    char *s = slurp(TMP_OUT, &len);
    remove(TMP_OUT);
    int ok = code == 200 || (code == 409 && s && strstr(s, "not_found"));
    free(s);
    return ok;
}

/* «nota.md» → «nota (Dropbox).md», o «nota (Dropbox 2).md» si ya existe */
static void conflict_name(const char *rel, char *out, size_t n)
{
    const char *slash = strrchr(rel, '/'), *dot = strrchr(rel, '.');
    if (dot && slash && dot < slash) dot = NULL;
    int stem = dot ? (int)(dot - rel) : (int)strlen(rel);
    for (int k = 1; k < 100; k++) {
        char suffix[24];
        snprintf(suffix, sizeof suffix, k == 1 ? " (Dropbox)" : " (Dropbox %d)", k);
        snprintf(out, n, "%.*s%s%s", stem, rel, suffix, dot ? dot : "");
        char p[900];
        local_path(p, sizeof p, out);
        if (access(p, F_OK)) return;
    }
}

/* ---------- la sincronización ---------- */

int nube_linked(void) { return access(NUBE_CFG, F_OK) == 0; }

static int online(int may_ask)
{
#ifdef NUBE_TEST
    return 1;
#else
    if (QueryNetwork() & NET_CONNECTED) return 1;
    return may_ask && (NetConnect2(NULL, true) == 0 || (QueryNetwork() & NET_CONNECTED));
#endif
}

static void reset(void) { nitems = 0; }

static int changed_here(const Item *it) { return it->l && (!it->s || it->lsize != it->ssize || it->lmtime != it->smtime); }

int nube_pending(void)
{
    if (!nube_linked()) return 0;
    reset();
    load_state();
    walk(pockit_data(), "");
    for (int i = 0; i < nitems; i++) if (changed_here(&items[i]) || (items[i].s && !items[i].l)) return 1;
    return 0;
}

static void note_last(int ok, int up, int down, int conflicts, const char *err)
{
    FILE *f = fopen(NUBE_LAST, "w");
    if (!f) return;
    fprintf(f, "%ld\t%d\t%d\t%d\t%d\t%s\n", (long)time(NULL), ok, up, down, conflicts, err ? err : "");
    fclose(f);
}

long nube_last(char *msg, size_t n)
{
    char line[600] = "";
    FILE *f = fopen(NUBE_LAST, "r");
    if (f) { if (!fgets(line, sizeof line, f)) line[0] = 0; fclose(f); }
    long when = 0;
    int ok = 0, up = 0, down = 0, conflicts = 0;
    char err[400] = "";
    sscanf(line, "%ld\t%d\t%d\t%d\t%d\t%399[^\n]", &when, &ok, &up, &down, &conflicts, err);
    if (!when) { snprintf(msg, n, T("Aún no se ha sincronizado.")); return 0; }
    struct tm tm;
    time_t t = when;
    localtime_r(&t, &tm);
    char d[40];
    snprintf(d, sizeof d, T("%d/%d a las %02d:%02d"), tm.tm_mday, tm.tm_mon + 1, tm.tm_hour, tm.tm_min);
    if (!ok) snprintf(msg, n, T("El %s no se pudo: %s"), d, err);
    else if (conflicts) snprintf(msg, n, T("Sincronizado el %s. %d archivo%s cambió en los dos sitios: están las dos versiones, la de Dropbox con «(Dropbox)» en el nombre."), d, conflicts, conflicts == 1 ? "" : "s");
    else snprintf(msg, n, T("Sincronizado el %s."), d);
    return when;
}

int nube_sync(int interactive, void (*progress)(const char *msg), char *out, size_t n)
{
    char err[300] = "";
    out[0] = 0;
    if (!nube_linked()) { snprintf(out, n, T("Pockit no está enlazado con Dropbox.")); return 0; }
    if (!online(interactive)) { snprintf(out, n, T("No hay conexión. Se sincronizará cuando haya wifi.")); return 0; }
    if (!refresh_token(err, sizeof err)) { note_last(0, 0, 0, 0, err); snprintf(out, n, "%s", err); return 0; }
    reset();
    load_state();
    walk(pockit_data(), "");
    int had_state = 0;
    for (int i = 0; i < nitems; i++) had_state |= items[i].s;
    int found = list_remote(err, sizeof err);
    if (found < 0) { note_last(0, 0, 0, 0, err); snprintf(out, n, "%s", err); return 0; }
    /* Si la carpeta de Dropbox ya no está (la borraste o es otra cuenta), no se borra nada en el
     * lector: se vuelve a subir todo, como la primera vez. */
    if (!found && had_state) for (int i = 0; i < nitems; i++) items[i].s = 0;

    int up = 0, down = 0, conflicts = 0, failed = 0, total = 0, done = 0;
    for (int i = 0; i < nitems; i++) {
        Item *it = &items[i];
        int lchg = changed_here(it), rchg = it->r && (!it->s || strcmp(it->rrev, it->srev));
        total += (it->l && lchg) || (it->r && rchg) || (it->s && (!it->l || !it->r));
    }
    for (int i = 0; i < nitems; i++) {
        Item *it = &items[i];
        int lchg = changed_here(it), rchg = it->r && (!it->s || strcmp(it->rrev, it->srev));
        if (!(lchg || rchg || (it->s && (!it->l || !it->r)))) continue;
        ++done;
        const char *name = strrchr(it->rel, '/') ? strrchr(it->rel, '/') + 1 : it->rel;
        int big_down = it->r && it->rsize > BIG && rchg && !(it->s && !it->l), big_up = it->l && it->lsize > BIG && lchg;
        if (progress && (big_down || big_up)) { char m[600]; snprintf(m, sizeof m, big_down ? T("Bajando «%s»…") : T("Subiendo «%s»…"), name); progress(m); }
        else if (progress && total > 2) { char m[80]; snprintf(m, sizeof m, T("Sincronizando con Dropbox… %d de %d"), done, total); progress(m); }
        if (it->l && it->r) {
            if (lchg && !rchg) { if (upload(it, it->srev)) up++; else failed++; }
            else if (!lchg && rchg) { if (download(it, it->rel)) { record_download(it); down++; } else failed++; }
            else {
                /* subrayados.md se hace juntando los archivos de cada lector (subrayados/): si cambió en los
                 * dos sitios, vale el de Dropbox y, al acabar, se vuelve a juntar con lo que haya llegado */
                size_t rl = strlen(it->rel);
                if (rl >= 14 && !strcmp(it->rel + rl - 14, "subrayados.md")) {
                    if (download(it, it->rel)) { record_download(it); down++; } else failed++;
                    continue;
                }
                /* cambió en los dos sitios (o es la primera vez y están en los dos): si son iguales, nada */
                char path[900];
                local_path(path, sizeof path, it->rel);
                char other[500];
                conflict_name(it->rel, other, sizeof other);
                if (!download(it, other)) { failed++; continue; }
                char opath[900];
                local_path(opath, sizeof opath, other);
                if (same_content(path, opath)) {
                    remove(opath);
                    struct stat st;
                    stat(path, &st);
                    it->s = 1;
                    snprintf(it->srev, sizeof it->srev, "%s", it->rrev);
                    it->ssize = st.st_size;
                    it->smtime = st.st_mtime;
                    continue;
                }
                conflicts++;   /* la de Dropbox queda al lado y se sube en la próxima vuelta */
                if (upload(it, it->rrev)) up++; else failed++;
            }
        } else if (it->l) {
            if (it->s && !lchg) { to_trash(it->rel); it->s = 0; down++; }   /* borrado en Dropbox */
            else if (upload(it, NULL)) up++; else failed++;
        } else if (it->r) {
            if (it->s && !rchg) { if (remote_delete(it)) { it->s = 0; up++; } else failed++; }   /* borrado en el lector */
            else if (download(it, it->rel)) { record_download(it); down++; } else failed++;
        } else it->s = 0;   /* ya no está en ningún sitio */
    }
    save_state();
    if (failed) snprintf(err, sizeof err, failed == 1 ? T("un archivo no se pudo copiar; se reintentará la próxima vez.") : T("%d archivos no se pudieron copiar; se reintentará la próxima vez."), failed);
    note_last(!failed, up, down, conflicts, failed ? err : NULL);
    nube_last(out, n);
    return !failed;
}

void nube_unlink(void)
{
    remove(NUBE_CFG);
    remove(NUBE_STATE);
    remove(NUBE_LAST);
    token[0] = 0;
}
