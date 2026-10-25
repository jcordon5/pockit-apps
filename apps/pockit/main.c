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

/* ---------- Inicio ---------- */
static int home_shown;
static int card_y0, card_y1, donde_y0, tasks_y0, tasks_y1, tiles_y0;
#define TILE_H 180

typedef struct { const char *name; int mod; const unsigned char *icon; int len; } Tile;
static const Tile TILES[] = {
    { "Lectura", MOD_LECTURA, icon_lectura, sizeof icon_lectura },
    { "Cuaderno", MOD_CUADERNO, icon_cuaderno, sizeof icon_cuaderno },
    { "IA", MOD_IA, icon_ia, sizeof icon_ia },
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
        DrawTextRect(x, ty + 128, tw, 40, TILES[i].name, ALIGN_CENTER);
    }
    POCKIT_REFRESH(home_shown);
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

/* ---------- Ajustes ---------- */
static int aj_shown;
static char kbd[300];
static int key_y, model_y, apps_y;

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

    SetFont(f_smallb, SOFT);
    DrawString(M, y, "INTELIGENCIA ARTIFICIAL");
    y += 50;
    key_y = y;
    SetFont(f_body, BLACK);
    char line[300];
    if (has) {
        size_t l = strlen(ai_key);
        snprintf(line, sizeof line, "Clave de DeepSeek: guardada (…%s)", l > 4 ? ai_key + l - 4 : ai_key);
    } else snprintf(line, sizeof line, "Clave de DeepSeek: sin configurar");
    DrawString(M, y, line);
    SetFont(f_small, SOFT);
    DrawTextRect(M, y + 50, W - 2 * M, 80, has ? "Toca para cambiarla." : "Pégala en pockit.yous.dev → Tu lector → Apps → IA, o toca aquí para escribirla.", ALIGN_LEFT);
    y += 140;
    DrawLine(M, y, W - M, y, LINE);
    y += 24;
    model_y = y;
    for (int i = 0; i < AI_NMODELS; i++) {
        int on = i == ai_model;
        double per = ai_factor() * (1500 * AI_MODELS[i].in + 500 * AI_MODELS[i].out) / 1e6;
        char mny[32];
        ai_money(per, mny, sizeof mny);
        DrawRect(M, y + 6, 40, 40, BLACK);
        if (on) FillArea(M + 9, y + 15, 22, 22, BLACK);
        SetFont(on ? f_bold : f_body, BLACK);
        snprintf(line, sizeof line, "%s · %s", AI_MODELS[i].name, AI_MODELS[i].desc);
        DrawString(M + 64, y + 4, line);
        SetFont(f_small, SOFT);
        snprintf(line, sizeof line, "≈ %s por respuesta", mny);
        DrawString(M + 64, y + 52, line);
        y += 110;
    }
    SetFont(f_small, SOFT);
    DrawTextRect(M, y, W - 2 * M, 80, ai_peak() ? "Ahora es hora punta en DeepSeek: fuera de ella cuesta la mitad." : "Ahora es hora valle en DeepSeek: estás pagando la mitad.", ALIGN_LEFT);
    y += 70;
    DrawLine(M, y, W - M, y, LINE);
    y += 24;
    double mon, tot = ai_spent(&mon);
    char a[32], b[32];
    ai_money(mon, a, sizeof a);
    ai_money(tot, b, sizeof b);
    snprintf(line, sizeof line, "Gastado este mes: %s  ·  en total: %s", a, b);
    SetFont(f_body, BLACK);
    DrawString(M, y, line);
    y += 90;

    SetFont(f_smallb, SOFT);
    DrawString(M, y, "POCKIT");
    y += 50;
    apps_y = y;
    SetFont(f_body, BLACK);
    snprintf(line, sizeof line, "Versión %s", POCKIT_VERSION);
    DrawString(M, y, line);
    SetFont(f_small, SOFT);
    DrawTextRect(M, y + 50, W - 2 * M, 80, "Para instalar, actualizar o quitar apps, y para cambiar fondos y música: pockit.yous.dev", ALIGN_LEFT);
    y += 150;

    SetFont(f_smallb, SOFT);
    DrawString(M, y, "BATERÍA");
    y += 50;
    SetFont(f_small, BLACK);
    DrawTextRect(M, y, W - 2 * M, 200,
                 "Si escuchas música, ciérrala del todo antes de bloquear (mantén pulsado Inicio y quita el reproductor): "
                 "en pausa, el lector no llega a dormirse y gasta batería. Para el bolsillo, usa el Modo bolsillo.", ALIGN_LEFT);
    POCKIT_REFRESH(aj_shown);
}

static void on_key(char *s)
{
    if (s && s[0]) { ai_load_cfg(); snprintf(ai_key, sizeof ai_key, "%s", s); ai_save_cfg(); }
    draw_ajustes();
}

static int ajustes_h(int type, int par1, int par2)
{
    switch (type) {
    case EVT_INIT: SetPanelType(PANEL_DISABLED); fonts(); break;
    case EVT_SHOW: draw_ajustes(); break;
    case EVT_POINTERUP: {
        int x = par1, y = par2;
        if (y < 90 && x < W / 3) { pockit_home(); return 1; }
        if (y >= key_y - 10 && y < key_y + 130) { kbd[0] = 0; OpenKeyboard("Clave de DeepSeek", kbd, sizeof kbd - 1, KBD_ENTEXT | KBD_NOEVTSHOW, on_key); return 1; }
        if (y >= model_y && y < model_y + AI_NMODELS * 110) {
            int m = (y - model_y) / 110;
            if (m != ai_model && ai_key[0]) { ai_model = m; ai_save_cfg(); draw_ajustes(); }
            else if (!ai_key[0]) Message(ICON_INFORMATION, "IA", "Primero guarda tu clave de DeepSeek.", 2500);
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
