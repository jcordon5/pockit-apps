/*
 * Pockit: el centro de lectura de tu PocketBook.
 *
 * Una sola app con módulos (pockit.h). Aquí están el manejador de eventos que reparte a
 * cada módulo, el Inicio y los Ajustes. El Inicio es un panel de un vistazo: el libro que
 * lees (con acceso a «Dónde voy»), tu semana y los módulos. Nada más: Pockit no te reclama.
 * Los Ajustes: dónde se guardan tus datos (el lector o Dropbox), Pockit AI y la versión.
 */
#include "inkview.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include "pockit.h"
#include "../../common/ai.h"
#include "icons.h"
#include "../../common/reloj.h"

#define M 64
#define SOFT 0x777777
#define LINE 0xC8C8C8

int pockit_full_next;
static int cur = MOD_HOME, inited[MOD_COUNT];
static int home_handler(int, int, int);
static int (*HANDLERS[MOD_COUNT])(int, int, int) = { home_handler, lectura_handler, cuaderno_handler, ia_handler, fondos_handler, ajustes_handler };

void pockit_open(int m)
{
    cur = m;
    pockit_full_next = 1;
    if (!inited[m]) { inited[m] = 1; HANDLERS[m](EVT_INIT, 0, 0); }
    HANDLERS[m](EVT_SHOW, 0, 0);
}

void pockit_home(void) { pockit_open(MOD_HOME); }

/* ---------- utilidades comunes de esta pantalla ---------- */
static ifont *f_title, *f_head, *f_body, *f_bold, *f_small, *f_smallb, *f_big;
static int W, H;

static void fonts(void)
{
    if (f_title) return;
    W = ScreenWidth(); H = ScreenHeight();
    f_title = OpenFont(DEFAULTFONTB, 64, 1);
    f_big = OpenFont(DEFAULTFONTB, 48, 1);
    f_head = OpenFont(DEFAULTFONT, 38, 1);
    f_body = OpenFont(DEFAULTFONT, 34, 1);
    f_bold = OpenFont(DEFAULTFONTB, 36, 1);
    f_small = OpenFont(DEFAULTFONT, 28, 1);
    f_smallb = OpenFont(DEFAULTFONTB, 28, 1);
}

/* «‹ Pockit», igual de grande y en el mismo sitio en todos los módulos */
void pockit_back(void)
{
    fonts();
    SetFont(f_head, BLACK);
    DrawString(M - 20, 42, "‹ Pockit");
}
int pockit_back_hit(int x, int y) { fonts(); return y < 100 && x < W / 3; }

/* Los iconos van dentro del programa; LoadBitmap necesita un archivo, así que se escriben una vez en /tmp. */
static ibitmap *icon(const char *name, const unsigned char *data, int len)
{
    char p[200];
    snprintf(p, sizeof p, "/tmp/pockit-%s.bmp", name);
    struct stat st;
    if (stat(p, &st)) { FILE *f = fopen(p, "wb"); if (f) { fwrite(data, 1, len, f); fclose(f); } }
    return LoadBitmap(p);
}

static const char *DIAS[] = { "domingo", "lunes", "martes", "miércoles", "jueves", "viernes", "sábado" };
static const char *MESES[] = { "enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto", "septiembre", "octubre", "noviembre", "diciembre" };
static const char *DAYS[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *MONTHS[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };

static char new_version[24];   /* versión nueva publicada, si la hay (ver «actualizaciones») */
static void update_check(int force);

/* ---------- Inicio ---------- */
static int home_shown;
static char sync_note[80];
static int card_y0, card_y1, donde_y0, tiles_y0, tiles_cols = 3;
static int clock_x, clock_y, clock_w, clock_side;   /* el reloj de palabras del Inicio (alto 0: no hay) */
#define TILE_H 180

typedef struct { const char *name; int mod; const unsigned char *icon; int len; } Tile;
static const Tile TILES[] = {
    { "Lectura", MOD_LECTURA, icon_lectura, sizeof icon_lectura },
    { "Cuaderno", MOD_CUADERNO, icon_cuaderno, sizeof icon_cuaderno },
    { "Pockit AI", MOD_IA, icon_ia, sizeof icon_ia },
    { "Fondos", MOD_FONDOS, icon_fondos, sizeof icon_fondos },
    { "Ajustes", MOD_AJUSTES, icon_ajustes, sizeof icon_ajustes },
};
#define NTILES 5

ibitmap *pockit_icon(int mod)
{
    for (int i = 0; i < NTILES; i++) if (TILES[i].mod == mod) return icon(TILES[i].name, TILES[i].icon, TILES[i].len);
    return NULL;
}

/* el reloj del Inicio, con los cuatro puntos de los minutos */
static int clock_min = -1;
static void draw_home_clock(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    rj_draw_box(clock_x, clock_y, clock_w, clock_side, tm.tm_hour, tm.tm_min, 1, 30, 16, 16);
    clock_min = tm.tm_hour * 60 + tm.tm_min;
}

/* cada minuto, mientras estás en el Inicio: solo se repinta el reloj, sin destello */
static void home_clock_tick(void)
{
    if (cur != MOD_HOME || !clock_side) return;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_hour * 60 + tm.tm_min != clock_min) {
        draw_home_clock();
        PartialUpdate(clock_x, clock_y, clock_w, clock_side);
    }
    SetWeakTimer("pk_reloj", home_clock_tick, (60 - tm.tm_sec) * 1000 + 300);
}

static void draw_home(void)
{
    fonts();
    ClearScreen();
    time_t now = time(NULL);
    struct tm tn;
    localtime_r(&now, &tn);
    char date[80];
    snprintf(date, sizeof date, T("%s, %d de %s"), (pockit_en() ? DAYS : DIAS)[tn.tm_wday], tn.tm_mday, (pockit_en() ? MONTHS : MESES)[tn.tm_mon]);
    SetFont(f_title, BLACK);
    DrawString(M, 44, "Pockit");
    SetFont(f_small, SOFT);
    DrawTextRect(W / 2, 64, W / 2 - M, 40, date, ALIGN_RIGHT);
    if (sync_note[0]) DrawTextRect(W / 2, 104, W / 2 - M, 36, sync_note, ALIGN_RIGHT | DOTS);
    int y = 150;

    /* lo que estás leyendo */
    PockitReading r;
    card_y0 = y;
    if (lectura_now(&r)) {
        ibitmap *c = LoadPNG(r.cover, 0);
        if (c) {
            double s = 170.0 / c->width < 255.0 / c->height ? 170.0 / c->width : 255.0 / c->height;
            int cw = c->width * s, ch = c->height * s;
            StretchBitmap(M, y + 255 - ch, cw, ch, c, 0);
            DrawRect(M, y + 255 - ch, cw, ch, LINE);
            free(c);
        } else FillArea(M, y, 170, 255, 0xE6E6E6);
        int tx = M + 200, tw = W - M - tx;
        SetFont(f_smallb, SOFT);
        DrawString(tx, y, T("LEYENDO AHORA"));
        SetFont(f_big, BLACK);
        DrawTextRect(tx, y + 40, tw, 120, r.title, ALIGN_LEFT | DOTS);
        SetFont(f_body, SOFT);
        DrawTextRect(tx, y + 160, tw, 44, r.author[0] ? r.author : " ", ALIGN_LEFT | DOTS);
        DrawRect(tx, y + 214, tw, 14, BLACK);
        FillArea(tx, y + 214, (int)(tw * r.progress), 14, BLACK);
        char info[120];
        snprintf(info, sizeof info, T("%d %% · página %d de %d"), (int)(r.progress * 100 + 0.5), r.page, r.pages);
        SetFont(f_small, SOFT);
        DrawString(tx, y + 240, info);
        y += 300;
        donde_y0 = y;
        FillArea(M, y, W - 2 * M, 90, BLACK);
        SetFont(f_bold, WHITE);
        DrawTextRect(M, y, W - 2 * M, 90, T("¿Dónde me quedé?  ›"), ALIGN_CENTER | VALIGN_MIDDLE);
        y += 90;
    } else {
        SetFont(f_body, SOFT);
        DrawTextRect(M, y, W - 2 * M, 120, T("No tienes ningún libro a medias. Abre uno y aquí verás tu progreso."), ALIGN_LEFT);
        donde_y0 = -1000;
        y += 130;
    }
    card_y1 = y;
    y += 40;

    /* tu semana */
    int act[7], wd = lectura_week(act), n = 0;
    for (int d = 0; d < 7; d++) n += act[d];
    char wk[80];
    snprintf(wk, sizeof wk, T("Esta semana · %d día%s leyendo"), n, n == 1 ? "" : "s");
    SetFont(f_bold, BLACK);
    DrawString(M, y, wk);
    y += 60;
    static const char *dn[] = { "L", "M", "X", "J", "V", "S", "D" }, *dn_en[] = { "M", "T", "W", "T", "F", "S", "S" };
    int cw = (W - 2 * M) / 7;
    for (int d = 0; d < 7; d++) {
        int cx = M + d * cw + cw / 2;
        SetFont(d == wd ? f_smallb : f_small, d == wd ? BLACK : SOFT);
        DrawTextRect(M + d * cw, y, cw, 34, (pockit_en() ? dn_en : dn)[d], ALIGN_CENTER);
        if (act[d]) FillArea(cx - 24, y + 42, 48, 48, BLACK);
        else DrawRect(cx - 24, y + 42, 48, 48, d > wd ? 0xE6E6E6 : LINE);
    }
    y += 120;

    /* módulos: con el reloj, en una sola fila para dejarle sitio */
    rj_default = !pockit_en();
    int with_clock = rj_enabled("inicio");
    tiles_cols = with_clock ? NTILES : 3;
    tiles_y0 = H - (with_clock ? 1 : 2) * TILE_H - 40;
    int tw = (W - 2 * M) / tiles_cols;
    for (int i = 0; i < NTILES; i++) {
        int x = M + (i % tiles_cols) * tw, ty = tiles_y0 + (i / tiles_cols) * TILE_H;
        DrawRect(x + 8, ty + 8, tw - 16, TILE_H - 16, LINE);
        ibitmap *b = icon(TILES[i].name, TILES[i].icon, TILES[i].len);
        if (b) {   /* en una fila de cinco en pantallas estrechas (el Q), los iconos encogen */
            int is = tw - 56 < b->width ? tw - 56 : b->width;
            StretchBitmap(x + (tw - is) / 2, ty + 18 + (b->width - is) / 2, is, is * b->height / b->width, b, 0);
            free(b);
        }
        SetFont(tw < 200 ? f_small : f_body, BLACK);
        DrawTextRect(x, ty + 128, tw, 40, TILES[i].mod == MOD_AJUSTES && new_version[0] ? T("Ajustes ·") : T(TILES[i].name), ALIGN_CENTER | DOTS);
    }

    /* el reloj, en todo el hueco entre tu semana y los módulos: la rejilla toma su forma */
    clock_side = 0;
    if (with_clock && tiles_y0 - y - 30 >= 200) {
        clock_x = M - 8;   /* las esquinas, a ras de los recuadros de los módulos */
        clock_w = W - 2 * M + 16;
        clock_y = y - 10;
        clock_side = tiles_y0 - clock_y - 20;
        draw_home_clock();
    }
    POCKIT_REFRESH(home_shown);
}

/* Al abrir Pockit y al volver al Inicio, si la wifi ya está encendida: baja lo que cambió en la
 * web y sube lo que cambió aquí. No espera a nada: el Inicio ya está pintado. */
static long sync_tried;
/* lo que llegó: los libros, a la biblioteca del lector; los subrayados de otros lectores, a subrayados.md */
static void after_sync(void (*progress)(const char *msg))
{
    pockit_marks_merge();
    pockit_shelf_report();   /* sube en la próxima sincronización */
    if (pockit_books_index(progress)) {   /* el buscador del lector pinta encima: se repinta todo */
        if (cur == MOD_HOME) draw_home(); else HANDLERS[cur](EVT_SHOW, 0, 0);
        FullUpdate();
    }
}
static void sync_status(const char *msg)
{
    snprintf(sync_note, sizeof sync_note, "%s", msg);
    if (cur != MOD_HOME) return;
    FillArea(W / 2, 100, W / 2 - M, 44, WHITE);
    SetFont(f_small, SOFT);
    DrawTextRect(W / 2, 104, W / 2 - M, 36, sync_note, ALIGN_RIGHT | DOTS);
    PartialUpdate(W / 2, 100, W / 2 - M, 44);
}
static void home_sync_tick(void)
{
    if (cur != MOD_HOME || !nube_linked() || !(QueryNetwork() & NET_CONNECTED)) return;
    if (time(NULL) - sync_tried < 120 && !nube_pending()) return;   /* sin cambios aquí, como mucho cada 2 minutos */
    sync_tried = time(NULL);
    sync_status(T("Sincronizando…"));
    char out[400];
    pockit_shelf_report();
    int ok = nube_sync(0, sync_status, out, sizeof out);   /* un libro tarda: se ve cuál */
    after_sync(sync_status);
    sync_status(ok ? T("Dropbox al día") : T("Dropbox: no se pudo sincronizar"));
}

static void bolsillo_update(int force);
static void home_update_tick(void)
{
    if (cur != MOD_HOME) return;
    static int bolsillo_checked;
    if (!bolsillo_checked && (QueryNetwork() & NET_CONNECTED)) { bolsillo_checked = 1; bolsillo_update(0); }
    int had = new_version[0];
    update_check(0);
    if (!had && new_version[0] && cur == MOD_HOME) draw_home();
}

static int home_handler(int type, int par1, int par2)
{
    switch (type) {
    case EVT_INIT:
        SetPanelType(PANEL_DISABLED);
        fonts();
        break;
    case EVT_SHOW:
        draw_home();
        SetWeakTimer("pk_update_check", home_update_tick, 4000);   /* después de pintar: el Inicio no espera a la red */
        SetWeakTimer("pk_nube", home_sync_tick, 1200);
        if (clock_side) home_clock_tick();
        break;
    case EVT_POINTERUP: {
        int x = par1, y = par2;
        if (y >= tiles_y0) {
            int i = ((y - tiles_y0) / TILE_H) * tiles_cols + (x - M) / ((W - 2 * M) / tiles_cols);
            if (i >= 0 && i < NTILES && x >= M) pockit_open(TILES[i].mod);
        } else if (y >= card_y0 && y < card_y1) lectura_open_current();
        return 1;
    }
    case EVT_KEYPRESS:
        if (par1 == IV_KEY_BACK || par1 == IV_KEY_HOME || par1 == IV_KEY_MENU) { CloseApp(); return 1; }
        break;
    }
    return 0;
}

/* ---------- actualizaciones ----------
 * Pockit mira, como mucho una vez al día y solo si la wifi ya está encendida (nunca la
 * enciende por esto), si hay una versión nueva en el catálogo. Si la hay, los Ajustes lo
 * dicen y se instala con un toque: baja el programa de su plataforma y sustituye al actual. */
#define APPS_CDN "https://cdn.jsdelivr.net/gh/jcordon5/pockit-apps@dist"
#define APPS_RAW "https://raw.githubusercontent.com/jcordon5/pockit-apps/dist"
#define UPDATE_TXT POCKIT_DIR "/actualizacion.txt"   /* «<cuándo se miró>\t<versión publicada>» */
static long update_checked;

static int ver_cmp(const char *a, const char *b)
{
    int x[3] = { 0 }, y[3] = { 0 };
    sscanf(a, "%d.%d.%d", &x[0], &x[1], &x[2]);
    sscanf(b, "%d.%d.%d", &y[0], &y[1], &y[2]);
    for (int i = 0; i < 3; i++) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

static void update_check(int force)
{
    char buf[200], ver[24] = "";
    FILE *f = fopen(UPDATE_TXT, "r");
    if (f) { if (fgets(buf, sizeof buf, f)) { update_checked = atol(buf); char *t = strchr(buf, '\t'); if (t) { snprintf(ver, sizeof ver, "%s", t + 1); ver[strcspn(ver, "\r\n")] = 0; } } fclose(f); }
    long now = time(NULL);
    if (force || now - update_checked > 86400) {
        if (force && !(QueryNetwork() & NET_CONNECTED)) NetConnect2(NULL, true);   /* lo ha pedido la persona */
        if (QueryNetwork() & NET_CONNECTED) {
            FILE *p = popen("curl -sf -m 8 '" APPS_RAW "/apps.json' || curl -s -m 8 '" APPS_CDN "/apps.json'", "r");
            JBuf b = { 0 };
            size_t r;
            while (p && (r = fread(buf, 1, sizeof buf, p)) > 0) jb_put(&b, buf, r);
            if (p) pclose(p);
            static JTok t[256];
            int n = b.s ? json_parse(b.s, (int)b.len, t, 256) : -1, apps = n > 0 ? json_key(b.s, t, n, 0, "apps") : -1;
            for (int k = 0; apps >= 0 && k < t[apps].size; k++) {
                int e = json_at(t, n, apps, k), id = json_key(b.s, t, n, e, "id"), v = json_key(b.s, t, n, e, "version");
                if (id >= 0 && v >= 0 && json_eq(b.s, &t[id], "pockit")) json_str(b.s, &t[v], ver, sizeof ver);
            }
            free(b.s);
            if (n > 0) {
                update_checked = now;
                mkdir(POCKIT_DIR, 0755);
                if ((f = fopen(UPDATE_TXT, "w"))) { fprintf(f, "%ld\t%s\n", now, ver); fclose(f); }
            }
        }
    }
    new_version[0] = 0;
    if (ver[0] && ver_cmp(ver, POCKIT_VERSION) > 0) snprintf(new_version, sizeof new_version, "%s", ver);
}

/* El programa que corre y su plataforma: el del Q (RK3566) es de coma flotante por hardware. */
static int self_path(char *out, size_t n) { ssize_t l = readlink("/proc/self/exe", out, n - 1); if (l <= 0) return 0; out[l] = 0; return 1; }
static int elf_hardfloat(const char *path)
{
    unsigned char h[40];
    FILE *f = fopen(path, "rb");
    int ok = f && fread(h, 1, sizeof h, f) == sizeof h;
    if (f) fclose(f);
    if (!ok || memcmp(h, "\x7f" "ELF", 4)) return -1;
    return (h[36] | h[37] << 8 | h[38] << 16 | (unsigned)h[39] << 24) & 0x400 ? 1 : 0;
}

/* ¿lleva dentro el texto de esa versión («2.7.0» suelto, como lo deja POCKIT_VERSION)? */
static int has_version(const char *path, const char *ver)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t vl = strlen(ver), keep = 0, r;
    char buf[65536 + 32];
    int found = 0;
    while (!found && (r = fread(buf + keep, 1, sizeof buf - 32 - keep, f)) > 0) {
        size_t n = keep + r;
        for (size_t i = 1; i + vl < n && !found; i++)
            found = !memcmp(buf + i, ver, vl) && buf[i + vl] == 0 && !(buf[i - 1] == '.' || (buf[i - 1] >= '0' && buf[i - 1] <= '9'));
        keep = n < 24 ? n : 24;
        memmove(buf, buf + n - keep, keep);
    }
    fclose(f);
    return found;
}

/* El Modo bolsillo, si está instalado al lado de Pockit, se pone al día con él (comparten el reloj).
 * force 0: solo si el que hay aún no tiene el reloj (lo instaló una versión anterior). */
static int file_has(const char *path, const char *needle)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t nl = strlen(needle), keep = 0, r;
    char buf[65536 + 64];
    int found = 0;
    while (!found && (r = fread(buf + keep, 1, sizeof buf - 64 - keep, f)) > 0) {
        size_t n = keep + r;
        for (size_t i = 0; i + nl <= n && !found; i++) found = !memcmp(buf + i, needle, nl);
        keep = n < 48 ? n : 48;
        memmove(buf, buf + n - keep, keep);
    }
    fclose(f);
    return found;
}
static void bolsillo_update(int force)
{
    char self[600], bol[640], tmp[660], cmd[900];
    if (!self_path(self, sizeof self)) return;
    int hf = elf_hardfloat(self);
    char *sl = strrchr(self, '/');
    if (hf < 0 || !sl) return;
    *sl = 0;
    snprintf(bol, sizeof bol, "%s/Bolsillo.app", self);
    struct stat sb;
    if (stat(bol, &sb) || (!force && file_has(bol, "reloj.cfg"))) return;
    snprintf(tmp, sizeof tmp, "%s.nuevo", bol);
    static const char *FROM[] = { APPS_RAW, APPS_CDN };
    for (int i = 0; i < 2; i++) {
        snprintf(cmd, sizeof cmd, "curl -sfL -m 120 -o '%s' '%s/%s/bolsillo.app'", tmp, FROM[i], hf ? "RK3566" : "B288");
        if (system(cmd) == 0 && !stat(tmp, &sb) && sb.st_size > 10000 && elf_hardfloat(tmp) == hf && file_has(tmp, "reloj.cfg")) {
            chmod(tmp, 0755);
            if (!rename(tmp, bol)) return;
        }
        remove(tmp);
    }
}

static void close_after_update(void) { CloseApp(); }
static void draw_ajustes(void);
static void do_update(int button)
{
    if (button != 1) { draw_ajustes(); return; }
    char self[600], tmp[620], cmd[900];
    int hf = self_path(self, sizeof self) ? elf_hardfloat(self) : -1;
    if (hf < 0) { Message(ICON_WARNING, "Pockit", T("No se pudo saber dónde está instalado Pockit. Actualiza desde pockit.yous.dev."), 4000); return; }
    if (!(QueryNetwork() & NET_CONNECTED) && NetConnect2(NULL, true) != 0 && !(QueryNetwork() & NET_CONNECTED)) {
        Message(ICON_WARNING, "Pockit", T("No hay conexión. Activa la wifi y vuelve a intentarlo."), 3000);
        return;
    }
    int by = H / 2 - 80;   /* aviso mientras baja: un recuadro, sin esperar a que se cierre un mensaje */
    FillArea(M, by, W - 2 * M, 160, WHITE);
    for (int i = 0; i < 3; i++) DrawRect(M + i, by + i, W - 2 * M - 2 * i, 160 - 2 * i, BLACK);
    SetFont(f_body, BLACK);
    DrawTextRect(M + 30, by, W - 2 * M - 60, 160, T("Descargando la versión nueva…"), ALIGN_CENTER | VALIGN_MIDDLE);
    PartialUpdate(M, by, W - 2 * M, 160);
    snprintf(tmp, sizeof tmp, "%s.nuevo", self);
    /* primero de GitHub (su caché dura minutos); jsDelivr puede tardar horas en tener la versión nueva.
     * Solo se cambia si lo descargado es un programa de la misma plataforma y de la versión anunciada. */
    static const char *FROM[] = { APPS_RAW, APPS_CDN };
    int good = 0, stale = 0;
    for (int i = 0; i < 2 && !good; i++) {
        snprintf(cmd, sizeof cmd, "curl -sfL -m 180 -o '%s' '%s/%s/pockit.app'", tmp, FROM[i], hf ? "RK3566" : "B288");
        int st = system(cmd);
        struct stat sb;
        if (st != 0 || stat(tmp, &sb) || sb.st_size < 100000 || elf_hardfloat(tmp) != hf) { remove(tmp); continue; }
        if (!has_version(tmp, new_version)) { stale = 1; remove(tmp); continue; }
        good = 1;
    }
    if (!good) {
        Message(ICON_WARNING, "Pockit", stale ? T("La versión nueva aún no está lista para descargar. Prueba dentro de un rato; Pockit sigue como estaba.")
                                              : T("No se pudo descargar la actualización. Pockit sigue como estaba."), 4000);
        return;
    }
    chmod(tmp, 0755);
    if (rename(tmp, self)) { remove(tmp); Message(ICON_WARNING, "Pockit", T("No se pudo instalar. Pockit sigue como estaba."), 4000); return; }
    bolsillo_update(1);
   
    static char msg[200];
    snprintf(msg, sizeof msg, T("Pockit %s instalado. Se cierra ahora: ábrelo otra vez para usar la versión nueva."), new_version);
    Message(ICON_INFORMATION, "Pockit", msg, 3500);
    SetWeakTimer("pk_update", close_after_update, 3600);
}

/* ---------- Ajustes ---------- */
static int aj_shown;
static int ai_y, ver_y, data_y, ai_h = 140, ver_h = 140, data_h = 200, clk_y[2];
static const char *const CLK_WHERE[2] = { "inicio", "bolsillo" };
static void draw_ajustes(void);

/* un interruptor como los del lector: «on» en negro a la derecha */
static void switch_at(int x, int y, int on)
{
    for (int i = 0; i < 3; i++) DrawRect(x + i, y + i, 140 - 2 * i, 64 - 2 * i, on ? BLACK : 0x999999);
    if (on) { FillArea(x + 64, y + 6, 70, 52, BLACK); SetFont(f_smallb, WHITE); DrawTextRect(x + 64, y + 6, 70, 52, "on", ALIGN_CENTER | VALIGN_MIDDLE); }
    else { FillArea(x + 6, y + 6, 70, 52, 0xCCCCCC); SetFont(f_small, SOFT); DrawTextRect(x + 70, y + 6, 64, 52, "off", ALIGN_CENTER | VALIGN_MIDDLE); }
}

static void sync_progress(const char *msg)
{
    int by = H / 2 - 80;
    FillArea(M, by, W - 2 * M, 160, WHITE);
    for (int i = 0; i < 3; i++) DrawRect(M + i, by + i, W - 2 * M - 2 * i, 160 - 2 * i, BLACK);
    SetFont(f_body, BLACK);
    DrawTextRect(M + 30, by, W - 2 * M - 60, 160, msg, ALIGN_CENTER | VALIGN_MIDDLE);
    PartialUpdate(M, by, W - 2 * M, 160);
}

static void on_data(int button)
{
    if (button == 1) {
        sync_progress(T("Sincronizando con Dropbox…"));
        char out[400];
        pockit_shelf_report();
        nube_sync(1, sync_progress, out, sizeof out);
        after_sync(sync_progress);
        draw_ajustes();
        Message(ICON_INFORMATION, "Dropbox", out, 3500);
        return;
    }
    if (button == 2) {
        nube_unlink();
        Message(ICON_INFORMATION, "Dropbox", T("Pockit ya no se sincroniza. Tus datos siguen en este lector y lo que hay en Dropbox se queda allí."), 4000);
    }
    draw_ajustes();
}

static void draw_ajustes(void)
{
    fonts();
    ClearScreen();
    int has = ai_load_cfg();
    SetFont(f_head, BLACK);
    DrawString(M - 20, 42, "‹ Pockit");
    SetFont(f_title, BLACK);
    DrawString(M, 110, T("Ajustes"));
    int y = 230;
    char line[300], dbx[600];

    /* dónde viven tus notas, chats y lo que Pockit sabe de tus libros */
    int linked = nube_linked();
    SetFont(f_smallb, SOFT);
    DrawString(M, y, T("TUS DATOS"));
    y += 50;
    data_y = y;
    SetFont(f_body, BLACK);
    DrawString(M, y, linked ? T("Sincronizar con Dropbox  ›") : T("Solo en este lector"));
    char last[400];
    if (linked) nube_last(last, sizeof last);
    SetFont(f_small, SOFT);
    const char *dtxt = linked ? last
                 : pockit_dropbox() ? T("Tus datos están en la carpeta Dropbox PocketBook, pero la sincronización del lector no sabe sustituir archivos. Para sincronizarlos bien, conecta el lector por cable y, en pockit.yous.dev → Tu lector → Apps, pulsa «Enlazar con Dropbox».")
                 : T("Para tener tus notas, chats y libros también en el móvil, conecta el lector por cable y, en pockit.yous.dev → Tu lector → Apps, pulsa «Enlazar con Dropbox».");
    int th = TextRectHeight(W - 2 * M, dtxt, ALIGN_LEFT);   /* en pantallas estrechas ocupa más líneas */
    DrawTextRect(M, y + 54, W - 2 * M, th, dtxt, ALIGN_LEFT);
    data_h = 54 + th;
    y += 54 + th + 40;
    DrawLine(M, y, W - M, y, LINE);
    y += 30;

    SetFont(f_smallb, SOFT);
    DrawString(M, y, "POCKIT AI");
    y += 50;
    ai_y = y;
    double mon;
    ai_spent(&mon);
    char a[32];
    ai_money(mon, a, sizeof a);
    SetFont(f_body, BLACK);
    if (has) snprintf(line, sizeof line, T("Activada · %s · %s este mes"), AI_MODELS[ai_model].name, a);
    else snprintf(line, sizeof line, T("Sin activar"));
    DrawString(M, y, line);
    SetFont(f_small, SOFT);
    const char *atxt = has ? T("Modelo, clave, gasto y errores: en Pockit AI → Uso y ajustes  ›")
                           : T("Es opcional. Para activarla, pega una clave en pockit.yous.dev → Tu lector → Apps → IA.");
    th = TextRectHeight(W - 2 * M, atxt, ALIGN_LEFT);
    DrawTextRect(M, y + 50, W - 2 * M, th, atxt, ALIGN_LEFT);
    ai_h = 50 + th;
    y += 50 + th + 30;
    DrawLine(M, y, W - M, y, LINE);
    y += 30;

    /* el reloj de palabras: en el Inicio y en el Modo bolsillo */
    SetFont(f_smallb, SOFT);
    DrawString(M, y, T("RELOJ DE PALABRAS"));
    y += 50;
    rj_default = !pockit_en();
    static const char *const CLK_LABEL[2] = { "En el Inicio", "En el Modo bolsillo" };
    static const char *const CLK_SUB[2] = { "Con un punto por cada minuto", "Cambia cada cinco minutos" };
    for (int i = 0; i < 2; i++) {
        clk_y[i] = y;
        SetFont(f_body, BLACK);
        DrawString(M, y, T(CLK_LABEL[i]));
        SetFont(f_small, SOFT);
        DrawString(M, y + 46, T(CLK_SUB[i]));
        switch_at(W - M - 140, y + 4, rj_enabled(CLK_WHERE[i]));
        y += 110;
    }
    y += 10;
    DrawLine(M, y, W - M, y, LINE);
    y += 30;

    SetFont(f_smallb, SOFT);
    DrawString(M, y, "POCKIT");
    y += 50;
    ver_y = y;
    SetFont(f_body, BLACK);
    snprintf(line, sizeof line, T("Versión %s"), POCKIT_VERSION);
    DrawString(M, y, line);
    if (new_version[0]) {
        snprintf(line, sizeof line, T("Hay una versión nueva: %s. Toca para instalarla  ›"), new_version);
        SetFont(f_bold, BLACK);
    } else if (update_checked) {
        char d[40];
        struct tm tm;
        time_t t = update_checked;
        localtime_r(&t, &tm);
        snprintf(d, sizeof d, "%d/%d %02d:%02d", tm.tm_mday, tm.tm_mon + 1, tm.tm_hour, tm.tm_min);
        snprintf(line, sizeof line, T("Al día (mirado el %s). Toca para mirar otra vez."), d);
        SetFont(f_small, SOFT);
    } else { snprintf(line, sizeof line, T("Toca para buscar actualizaciones.")); SetFont(f_small, SOFT); }
    th = TextRectHeight(W - 2 * M, line, ALIGN_LEFT);
    DrawTextRect(M, y + 50, W - 2 * M, th, line, ALIGN_LEFT);
    ver_h = 50 + th;
    y += 50 + th + 30;
    SetFont(f_small, SOFT);
    const char *itxt = T("Para instalar o quitar apps, y para cambiar fondos y música: pockit.yous.dev");
    DrawTextRect(M, y, W - 2 * M, TextRectHeight(W - 2 * M, itxt, ALIGN_LEFT), itxt, ALIGN_LEFT);
    POCKIT_REFRESH(aj_shown);
}

static int ajustes_h(int type, int par1, int par2)
{
    switch (type) {
    case EVT_INIT: SetPanelType(PANEL_DISABLED); fonts(); break;
    case EVT_SHOW: update_check(0); draw_ajustes(); break;
    case EVT_POINTERUP: {
        int x = par1, y = par2;
        if (pockit_back_hit(x, y)) { pockit_home(); return 1; }
        if (y >= data_y - 10 && y < data_y + data_h && nube_linked()) {
            Dialog3(ICON_QUESTION, "Dropbox", T("Pockit sincroniza tus notas, chats y libros con la carpeta Pockit de tu Dropbox al abrirse y al cerrarse, si hay wifi."),
                    T("Sincronizar ahora"), T("Dejar de sincronizar"), T("Cancelar"), on_data);
            return 1;
        }
        if (y >= ai_y - 10 && y < ai_y + ai_h && ai_bearer()[0]) { pockit_open(MOD_IA); return 1; }
        for (int i = 0; i < 2; i++)
            if (y >= clk_y[i] - 10 && y < clk_y[i] + 100) {
                rj_default = !pockit_en();
                rj_set_enabled(CLK_WHERE[i], !rj_enabled(CLK_WHERE[i]));
                FillArea(W - M - 140, clk_y[i] + 4, 140, 64, WHITE);
                switch_at(W - M - 140, clk_y[i] + 4, rj_enabled(CLK_WHERE[i]));
                PartialUpdate(W - M - 140, clk_y[i] + 4, 140, 64);
                return 1;
            }
        if (y >= ver_y - 10 && y < ver_y + ver_h) {
            if (new_version[0]) {
                static char q[300];
                snprintf(q, sizeof q, T("¿Instalar Pockit %s? Se descarga (menos de 1 MB) y Pockit se cierra para abrir la versión nueva. Tus notas, chats y datos no se tocan."), new_version);
                Dialog(ICON_QUESTION, T("Actualizar Pockit"), q, T("Instalar"), T("Ahora no"), do_update);
            } else { update_check(1); draw_ajustes(); if (!new_version[0]) Message(ICON_INFORMATION, "Pockit", update_checked ? T("Tienes la última versión.") : T("No se pudo mirar: comprueba la wifi."), 2000); }
        }
        return 1;
    }
    case EVT_KEYPRESS:
        if (par1 == IV_KEY_BACK || par1 == IV_KEY_HOME || par1 == IV_KEY_MENU) { pockit_home(); return 1; }
        break;
    }
    return 0;
}
int ajustes_handler(int type, int par1, int par2) { return ajustes_h(type, par1, par2); }

/* ---------- reparto de eventos ---------- */
static int main_handler(int type, int par1, int par2)
{
    if (type == EVT_INIT) {
        SetPanelType(PANEL_DISABLED);
        inited[cur] = 1;
        return HANDLERS[cur](EVT_INIT, par1, par2);
    }
    if (type == EVT_EXIT) {
        for (int m = 0; m < MOD_COUNT; m++) if (inited[m]) HANDLERS[m](EVT_EXIT, par1, par2);
        char out[400];
        if (nube_linked() && (QueryNetwork() & NET_CONNECTED) && nube_pending()) nube_sync(0, NULL, out, sizeof out);   /* lo que quede por subir */
        pockit_books_scan();   /* si la biblioteca del lector aún no tiene algún libro de Pockit */
        return 0;
    }
    return HANDLERS[cur](type, par1, par2);
}

int main(int argc, char *argv[])
{
    /* Para el emulador: «pockit <módulo> [argumentos del módulo]» abre directamente un módulo. */
    if (argc > 1) {
        static const char *names[] = { "inicio", "lectura", "cuaderno", "ia", "fondos", "ajustes" };
        for (int m = 0; m < MOD_COUNT; m++)
            if (!strcmp(argv[1], names[m])) {
                cur = m;
                if (m == MOD_LECTURA) lectura_args(argc - 1, argv + 1);
                if (m == MOD_CUADERNO) cuaderno_args(argc - 1, argv + 1);
                if (m == MOD_IA) ia_args(argc - 1, argv + 1);
            }
    }
    InkViewMain(main_handler);
    return 0;
}
