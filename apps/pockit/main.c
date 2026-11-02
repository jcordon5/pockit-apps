/*
 * Pockit: el centro de lectura de tu PocketBook.
 *
 * Una sola app con módulos (pockit.h). Aquí están el manejador de eventos que reparte a
 * cada módulo, el Inicio y los Ajustes. El Inicio es un panel de un vistazo: el libro que
 * lees (con acceso a «Dónde voy»), tu semana, tus tareas pendientes y los módulos.
 * Los Ajustes gestionan lo mismo que la web: clave y modelo de la IA, gasto y versión.
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

static char new_version[24];   /* versión nueva publicada, si la hay (ver «actualizaciones») */
static void update_check(int force);

/* ---------- Inicio ---------- */
static int home_shown;
static int card_y0, card_y1, donde_y0, tasks_y0, tasks_y1, tiles_y0;
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

static void draw_home(void)
{
    fonts();
    ClearScreen();
    time_t now = time(NULL);
    struct tm tn;
    localtime_r(&now, &tn);
    char date[80];
    snprintf(date, sizeof date, "%s, %d de %s", DIAS[tn.tm_wday], tn.tm_mday, MESES[tn.tm_mon]);
    SetFont(f_title, BLACK);
    DrawString(M, 44, "Pockit");
    SetFont(f_small, SOFT);
    DrawTextRect(W / 2, 64, W / 2 - M, 40, date, ALIGN_RIGHT);
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
        DrawString(tx, y, "LEYENDO AHORA");
        SetFont(f_big, BLACK);
        DrawTextRect(tx, y + 40, tw, 120, r.title, ALIGN_LEFT | DOTS);
        SetFont(f_body, SOFT);
        DrawTextRect(tx, y + 160, tw, 44, r.author[0] ? r.author : " ", ALIGN_LEFT | DOTS);
        DrawRect(tx, y + 214, tw, 14, BLACK);
        FillArea(tx, y + 214, (int)(tw * r.progress), 14, BLACK);
        char info[120];
        snprintf(info, sizeof info, "%d %% · página %d de %d", (int)(r.progress * 100 + 0.5), r.page, r.pages);
        SetFont(f_small, SOFT);
        DrawString(tx, y + 240, info);
        y += 300;
        donde_y0 = y;
        FillArea(M, y, W - 2 * M, 90, BLACK);
        SetFont(f_bold, WHITE);
        DrawTextRect(M, y, W - 2 * M, 90, "¿Dónde me quedé?  ›", ALIGN_CENTER | VALIGN_MIDDLE);
        y += 90;
    } else {
        SetFont(f_body, SOFT);
        DrawTextRect(M, y, W - 2 * M, 120, "No tienes ningún libro a medias. Abre uno y aquí verás tu progreso.", ALIGN_LEFT);
        donde_y0 = -1000;
        y += 130;
    }
    card_y1 = y;
    y += 40;

    /* tu semana */
    int act[7], wd = lectura_week(act), n = 0;
    for (int d = 0; d < 7; d++) n += act[d];
    char wk[80];
    snprintf(wk, sizeof wk, "Esta semana · %d día%s leyendo", n, n == 1 ? "" : "s");
    SetFont(f_bold, BLACK);
    DrawString(M, y, wk);
    y += 60;
    static const char *dn[] = { "L", "M", "X", "J", "V", "S", "D" };
    int cw = (W - 2 * M) / 7;
    for (int d = 0; d < 7; d++) {
        int cx = M + d * cw + cw / 2;
        SetFont(d == wd ? f_smallb : f_small, d == wd ? BLACK : SOFT);
        DrawTextRect(M + d * cw, y, cw, 34, dn[d], ALIGN_CENTER);
        if (act[d]) FillArea(cx - 24, y + 42, 48, 48, BLACK);
        else DrawRect(cx - 24, y + 42, 48, 48, d > wd ? 0xE6E6E6 : LINE);
    }
    y += 120;

    /* tareas pendientes */
    static char tasks[3][200];
    int nt = cuaderno_tasks(tasks, 3);
    tasks_y0 = y;
    SetFont(f_bold, BLACK);
    DrawString(M, y, "Pendiente");
    y += 58;
    if (!nt) { SetFont(f_body, SOFT); DrawString(M, y, "Nada pendiente en el Cuaderno."); y += 52; }
    for (int i = 0; i < nt; i++) {
        DrawRect(M, y + 4, 32, 32, BLACK);
        DrawRect(M + 1, y + 5, 30, 30, BLACK);
        SetFont(f_body, BLACK);
        DrawTextRect(M + 52, y, W - 2 * M - 52, 44, tasks[i], ALIGN_LEFT | DOTS);
        y += 52;
    }
    tasks_y1 = y;

    /* módulos */
    tiles_y0 = H - 2 * TILE_H - 40;
    int tw = (W - 2 * M) / 3;
    for (int i = 0; i < NTILES; i++) {
        int x = M + (i % 3) * tw, ty = tiles_y0 + (i / 3) * TILE_H;
        DrawRect(x + 8, ty + 8, tw - 16, TILE_H - 16, LINE);
        ibitmap *b = icon(TILES[i].name, TILES[i].icon, TILES[i].len);
        if (b) { DrawBitmap(x + (tw - b->width) / 2, ty + 18, b); free(b); }
        SetFont(f_body, BLACK);
        DrawTextRect(x, ty + 128, tw, 40, TILES[i].mod == MOD_AJUSTES && new_version[0] ? "Ajustes ·" : TILES[i].name, ALIGN_CENTER);
    }
    POCKIT_REFRESH(home_shown);
}

static void home_update_tick(void)
{
    if (cur != MOD_HOME) return;
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
        break;
    case EVT_POINTERUP: {
        int x = par1, y = par2;
        if (y >= tiles_y0) {
            int i = ((y - tiles_y0) / TILE_H) * 3 + (x - M) / ((W - 2 * M) / 3);
            if (i >= 0 && i < NTILES && x >= M) pockit_open(TILES[i].mod);
        } else if (y >= donde_y0 && y < donde_y0 + 90) lectura_open_current(1);
        else if (y >= card_y0 && y < card_y1) lectura_open_current(0);
        else if (y >= tasks_y0 && y < tasks_y1) pockit_open(MOD_CUADERNO);
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
            FILE *p = popen("curl -s -m 8 '" APPS_CDN "/apps.json'", "r");
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

static void close_after_update(void) { CloseApp(); }
static void draw_ajustes(void);
static void do_update(int button)
{
    if (button != 1) { draw_ajustes(); return; }
    char self[600], tmp[620], cmd[900];
    int hf = self_path(self, sizeof self) ? elf_hardfloat(self) : -1;
    if (hf < 0) { Message(ICON_WARNING, "Pockit", "No se pudo saber dónde está instalado Pockit. Actualiza desde pockit.yous.dev.", 4000); return; }
    if (!(QueryNetwork() & NET_CONNECTED) && NetConnect2(NULL, true) != 0 && !(QueryNetwork() & NET_CONNECTED)) {
        Message(ICON_WARNING, "Pockit", "No hay conexión. Activa la wifi y vuelve a intentarlo.", 3000);
        return;
    }
    int by = H / 2 - 80;   /* aviso mientras baja: un recuadro, sin esperar a que se cierre un mensaje */
    FillArea(M, by, W - 2 * M, 160, WHITE);
    for (int i = 0; i < 3; i++) DrawRect(M + i, by + i, W - 2 * M - 2 * i, 160 - 2 * i, BLACK);
    SetFont(f_body, BLACK);
    DrawTextRect(M + 30, by, W - 2 * M - 60, 160, "Descargando la versión nueva…", ALIGN_CENTER | VALIGN_MIDDLE);
    PartialUpdate(M, by, W - 2 * M, 160);
    snprintf(tmp, sizeof tmp, "%s.nuevo", self);
    snprintf(cmd, sizeof cmd, "curl -sfL -m 180 -o '%s' '" APPS_CDN "/%s/pockit.app'", tmp, hf ? "RK3566" : "B288");
    int st = system(cmd);
    struct stat sb;
    /* solo se cambia si lo descargado es un programa de la misma plataforma */
    if (st != 0 || stat(tmp, &sb) || sb.st_size < 100000 || elf_hardfloat(tmp) != hf) {
        remove(tmp);
        Message(ICON_WARNING, "Pockit", "No se pudo descargar la actualización. Pockit sigue como estaba.", 4000);
        return;
    }
    chmod(tmp, 0755);
    if (rename(tmp, self)) { remove(tmp); Message(ICON_WARNING, "Pockit", "No se pudo instalar. Pockit sigue como estaba.", 4000); return; }
   
    static char msg[200];
    snprintf(msg, sizeof msg, "Pockit %s instalado. Se cierra ahora: ábrelo otra vez para usar la versión nueva.", new_version);
    Message(ICON_INFORMATION, "Pockit", msg, 3500);
    SetWeakTimer("pk_update", close_after_update, 3600);
}

/* ---------- Ajustes ---------- */
static int aj_shown;
static int ai_y, ver_y;

static void draw_ajustes(void)
{
    fonts();
    ClearScreen();
    int has = ai_load_cfg();
    SetFont(f_head, BLACK);
    DrawString(M - 20, 42, "‹ Pockit");
    SetFont(f_title, BLACK);
    DrawString(M, 110, "Ajustes");
    int y = 230;
    char line[300];

    SetFont(f_smallb, SOFT);
    DrawString(M, y, "POCKIT AI");
    y += 50;
    ai_y = y;
    double mon;
    ai_spent(&mon);
    char a[32];
    ai_money(mon, a, sizeof a);
    SetFont(f_body, BLACK);
    if (has) snprintf(line, sizeof line, "Activada · %s · %s este mes", AI_MODELS[ai_model].name, a);
    else snprintf(line, sizeof line, "Sin activar");
    DrawString(M, y, line);
    SetFont(f_small, SOFT);
    DrawTextRect(M, y + 50, W - 2 * M, 80, has ? "Modelo, clave, gasto y errores: en Pockit AI → Uso y ajustes  ›"
                                               : "Es opcional. Para activarla, pega una clave en pockit.yous.dev → Tu lector → Apps → IA.", ALIGN_LEFT);
    y += 160;
    DrawLine(M, y, W - M, y, LINE);
    y += 30;

    SetFont(f_smallb, SOFT);
    DrawString(M, y, "POCKIT");
    y += 50;
    ver_y = y;
    SetFont(f_body, BLACK);
    snprintf(line, sizeof line, "Versión %s", POCKIT_VERSION);
    DrawString(M, y, line);
    if (new_version[0]) {
        snprintf(line, sizeof line, "Hay una versión nueva: %s. Toca para instalarla  ›", new_version);
        SetFont(f_bold, BLACK);
    } else if (update_checked) {
        char d[40];
        struct tm tm;
        time_t t = update_checked;
        localtime_r(&t, &tm);
        snprintf(d, sizeof d, "%d/%d %02d:%02d", tm.tm_mday, tm.tm_mon + 1, tm.tm_hour, tm.tm_min);
        snprintf(line, sizeof line, "Al día (mirado el %s). Toca para mirar otra vez.", d);
        SetFont(f_small, SOFT);
    } else { snprintf(line, sizeof line, "Toca para buscar actualizaciones."); SetFont(f_small, SOFT); }
    DrawTextRect(M, y + 50, W - 2 * M, 80, line, ALIGN_LEFT);
    y += 150;
    SetFont(f_small, SOFT);
    DrawTextRect(M, y, W - 2 * M, 120, "Para instalar o quitar apps, y para cambiar fondos y música: pockit.yous.dev", ALIGN_LEFT);
    POCKIT_REFRESH(aj_shown);
}

static int ajustes_h(int type, int par1, int par2)
{
    switch (type) {
    case EVT_INIT: SetPanelType(PANEL_DISABLED); fonts(); break;
    case EVT_SHOW: update_check(0); draw_ajustes(); break;
    case EVT_POINTERUP: {
        int x = par1, y = par2;
        if (y < 90 && x < W / 3) { pockit_home(); return 1; }
        if (y >= ai_y - 10 && y < ai_y + 140 && ai_bearer()[0]) { pockit_open(MOD_IA); return 1; }
        if (y >= ver_y - 10 && y < ver_y + 140) {
            if (new_version[0]) {
                static char q[300];
                snprintf(q, sizeof q, "¿Instalar Pockit %s? Se descarga (menos de 1 MB) y Pockit se cierra para abrir la versión nueva. Tus notas, chats y datos no se tocan.", new_version);
                Dialog(ICON_QUESTION, "Actualizar Pockit", q, "Instalar", "Ahora no", do_update);
            } else { update_check(1); draw_ajustes(); if (!new_version[0]) Message(ICON_INFORMATION, "Pockit", update_checked ? "Tienes la última versión." : "No se pudo mirar: comprueba la wifi.", 2000); }
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
