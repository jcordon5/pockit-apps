/*
 * Modo bolsillo para PocketBook: escuchar música o audiolibros con el lector en el bolsillo.
 *
 * Al bloquear con el botón de encendido el sistema corta el puerto USB unos 20 s después
 * y la música se para (registros de las pruebas en el Verse Pro, firmware 6.10). Esta app
 * no bloquea: pone el fondo de reposo con una tarjeta de lo que suena, apaga la luz,
 * ignora el táctil y no deja que el lector se duerma. Un botón de página vuelve a donde
 * estabas; el de encendido bloquea como siempre.
 *
 * Lo que pasa se apunta en /mnt/ext1/applications/.pruebas/bolsillo.txt.
 */
#include "inkview.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>

#define LOG_PATH "/mnt/ext1/applications/.pruebas/bolsillo.txt"
#define RESOURCES "/mnt/ext1/system/resources"
#define SILENT_LOCK_S (30 * 60)   /* sin música media hora: bloqueo normal para no gastar */

/* En el «buzón» del reproductor 1 es «sonando» y 2 «en pausa». En pausa el título
 * llega con basura, así que se guarda el último que llegó bien. */
#define SONANDO(s) ((s) == 1)
#define EN_PAUSA(s) ((s) == 2)

static int light_before = INT_MIN, light_enabled_before = 1;
static int last_keylock = -1, last_state = -1, last_output = -1;
static double silent_since = -1, suspended_before;
static ifont *big, *small;
static ibitmap *wallpaper;
static char title[512], author[512];
static int shown_state = -2;
static char shown_title[512];

static double clock_s(clockid_t id)
{
    struct timespec ts;
    clock_gettime(id, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void logf_(const char *fmt, ...)
{
    mkdir("/mnt/ext1/applications/.pruebas", 0755);
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) return;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    fprintf(f, "%02d:%02d:%02d dormido=%.0fs | ", tm.tm_hour, tm.tm_min, tm.tm_sec,
            clock_s(CLOCK_BOOTTIME) - clock_s(CLOCK_MONOTONIC));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* UTF-8 bien formado y sin caracteres de control: si no, es basura del buzón. */
static int clean_text(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        int n = *p < 0x80 ? 0 : (*p & 0xe0) == 0xc0 ? 1 : (*p & 0xf0) == 0xe0 ? 2 : (*p & 0xf8) == 0xf0 ? 3 : -1;
        if (n < 0 || (*p < 0x20)) return 0;
        if (*p == 0xef && p[1] == 0xbf && p[2] == 0xbd) return 0;   /* U+FFFD */
        p++;
        while (n--) { if ((*p & 0xc0) != 0x80) return 0; p++; }
    }
    return 1;
}

/* Estado de la música o, si no suena, del audiolibro. Actualiza title/author si llegan bien. */
static int audio_state(void)
{
    AudioPlayerType types[] = { kAudioPlayer, kAudioBooks };
    int best = -1;
    for (int i = 0; i < 2; i++) {
        AudioPlayingInfo ai;
        memset(&ai, 0, sizeof ai);
        ai.type = types[i];
        if (GetAudioPlayingInfo(&ai) != 0) continue;
        if (SONANDO(ai.play_state) && ai.title[0] && clean_text(ai.title) && clean_text(ai.author)) {
            snprintf(title, sizeof title, "%s", ai.title);
            snprintf(author, sizeof author, "%s", ai.author);
        }
        if (SONANDO(ai.play_state)) return ai.play_state;
        if (best < 0 || EN_PAUSA(ai.play_state)) best = ai.play_state;
    }
    return best;
}

/* El fondo de reposo del tema en uso: system/resources/<tema>/taskmgr_lock_background.bmp.
 * LoadBitmap ya trata el color clave #808040 como transparente. */
static ibitmap *load_wallpaper(void)
{
    DIR *d = opendir(RESOURCES);
    if (!d) return NULL;
    ibitmap *b = NULL;
    struct dirent *e;
    while (!b && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof path, RESOURCES "/%s/taskmgr_lock_background.bmp", e->d_name);
        b = LoadBitmap(path);
        if (b) logf_("fondo: %s (%dx%d)", path, b->width, b->height);
    }
    closedir(d);
    return b;
}

#define CARD_H 300
static void card_rect(int *x, int *y, int *w)
{
    *x = 80; *w = ScreenWidth() - 160; *y = ScreenHeight() - CARD_H - 110;
}

static void draw_card(int st)
{
    int cx, cy, cw;
    card_rect(&cx, &cy, &cw);
    FillArea(cx, cy, cw, CARD_H, WHITE);
    for (int i = 0; i < 3; i++) DrawRect(cx + i, cy + i, cw - 2 * i, CARD_H - 2 * i, BLACK);

    SetFont(big, BLACK);
    DrawTextRect(cx + 40, cy + 30, cw - 80, 70, "Modo bolsillo", ALIGN_LEFT | VALIGN_MIDDLE);
    char line[1100];
    if (title[0] && (SONANDO(st) || EN_PAUSA(st)))
        snprintf(line, sizeof line, "%s%s%s%s", EN_PAUSA(st) ? "En pausa · " : "", title, author[0] ? " · " : "", author);
    else
        snprintf(line, sizeof line, "No suena nada");
    SetFont(small, BLACK);
    DrawTextRect(cx + 40, cy + 110, cw - 80, 90, line, ALIGN_LEFT | VALIGN_TOP | DOTS);
    SetFont(small, DGRAY);
    DrawTextRect(cx + 40, cy + CARD_H - 80, cw - 80, 50, "Botón de página para volver", ALIGN_LEFT | VALIGN_MIDDLE);
    shown_state = st;
    snprintf(shown_title, sizeof shown_title, "%s", title);
}

static void draw_all(void)
{
    int st = audio_state();
    ClearScreen();
    if (wallpaper) StretchBitmap(0, 0, ScreenWidth(), ScreenHeight(), wallpaper, 0);
    draw_card(st);
    FullUpdate();
}

static void leave(void)
{
    if (light_before != INT_MIN && light_enabled_before) SetFrontlightStateEx(light_before, 1);
    logf_("salgo, luz=%d", light_before);
    CloseApp();
}

static void tick(void)
{
    int st = audio_state();
    int keylock = get_keylock(), out = (int)GetAudioOutput();
    int on = SONANDO(st);
    double now = clock_s(CLOCK_BOOTTIME);

    /* Abierto y sin bloquear: que no se duerma ni se apague solo. Bloqueado con el
     * botón de encendido: el sistema manda, como siempre. */
    if (!keylock) {
        if (on) silent_since = -1;
        else if (silent_since < 0) silent_since = now;
        if (on || now - silent_since < SILENT_LOCK_S) {
            BanSleep(60);
            PostponeTimedPoweroff();
        } else {
            logf_("media hora sin música: bloqueo");
            silent_since = -1;
            LockDevice();
        }
    }

    double gap = now - clock_s(CLOCK_MONOTONIC);
    if (keylock != last_keylock || st != last_state || out != last_output || gap - suspended_before > 2)
        logf_("cambio: bloqueado %d->%d, audio %d->%d, salida %d->%d «%s»%s",
              last_keylock, keylock, last_state, st, last_output, out, title,
              gap - suspended_before > 2 ? "  <-- SE HA DORMIDO" : "");

    /* Canción nueva, pausa o reanudación: redibujar solo la tarjeta. */
    if (!keylock && (st != shown_state || strcmp(title, shown_title))) {
        int cx, cy, cw;
        card_rect(&cx, &cy, &cw);
        if (wallpaper) DrawBitmapArea(cx, cy, wallpaper, cx, cy, cw, CARD_H);
        draw_card(st);
        PartialUpdate(cx, cy, cw, CARD_H);
    }
    last_keylock = keylock; last_state = st; last_output = out;
    suspended_before = gap;
    SetWeakTimer("bolsillo_tick", tick, keylock ? 5000 : 1000);
}

static int handler(int type, int par1, int par2)
{
    switch (type) {
    case EVT_INIT:
        SetPanelType(PANEL_DISABLED);   /* sin barra de estado: si no, el primer pintado sale desplazado */
        big = OpenFont(DEFAULTFONTB, 56, 1);
        small = OpenFont(DEFAULTFONT, 34, 1);
        iv_sleepmode(0);
        SetAutoKeylock(false);
        SetAutoPowerOff(false);
        suspended_before = clock_s(CLOCK_BOOTTIME) - clock_s(CLOCK_MONOTONIC);
        wallpaper = load_wallpaper();
        light_before = GetFrontlightState();
        light_enabled_before = GetFrontlightEnabled();
        logf_("inicio: luz=%d salida=%d", light_before, (int)GetAudioOutput());
        SetFrontlightStateEx(-1, 1);
        tick();
        break;
    case EVT_SHOW:
        draw_all();
        break;
    case EVT_FOREGROUND:
        SetFrontlightStateEx(-1, 1);
        draw_all();
        break;
    case EVT_KEYPRESS:
        return par1 == IV_KEY_POWER ? 0 : 1;
    case EVT_KEYRELEASE:
        if (par1 == IV_KEY_POWER) return 0;
        leave();
        return 1;
    case EVT_POINTERDOWN: case EVT_POINTERUP: case EVT_POINTERMOVE: case EVT_POINTERLONG:
    case EVT_POINTERHOLD: case EVT_MTSYNC:
        return 1;   /* en el bolsillo el táctil no hace nada */
    case EVT_EXIT:
        if (light_before != INT_MIN && light_enabled_before) SetFrontlightStateEx(light_before, 1);
        logf_("fin");
        break;
    }
    return 0;
}

int main(void)
{
    InkViewMain(handler);
    return 0;
}
