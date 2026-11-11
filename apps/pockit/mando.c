/*
 * mando.c: mandos Bluetooth que el lector no reconoce (pasapáginas, clickers…).
 *
 * El lector ya sabe usar botones Bluetooth: en global.cfg guarda cada uno como
 *   btkeys.count=N
 *   btkeys.bt<i>.name=…        btkeys.bt<i>.scanCode=<código de tecla de Linux>
 *   tkey.bt<i>.<0|1|2>=@KA_…   (en ePub y demás; pkey.bt<i> en PDF; gkey.bt<i> fuera de los libros)
 * y luego se le asignan funciones en Ajustes → Botones Bluetooth. Pero solo deja añadirlos si
 * el mando está en su lista de «compatibles». Aquí se añaden igual que lo haría él: el mando
 * manda sus teclas a la app como EVT_KEYPRESS_EXT con el código de Linux, que es el scanCode.
 * Si un nombre es «__DELETED__», ese hueco está borrado (así los borra el propio lector).
 *
 * Pantalla: «pulsa el botón de avanzar» → «ahora el de retroceder», tantas veces como modos
 * tenga el mando. Tocar un botón de la lista le da la vuelta (avanzar ↔ retroceder). El resto
 * de funciones (capítulos, luz, pulsación larga…) se ponen en la pantalla del propio lector
 * (Ajustes → … → Botones Bluetooth); abrirla desde Pockit con OpenTask colgaba el lector.
 *
 * Reconectar: el lector no vuelve a conectar el mando al despertar. Pockit deja un proceso
 * pequeño («Pockit.app --mando») que, al despertar el lector, intenta durante dos minutos
 * reconectar los mandos y teclados Bluetooth emparejados que estén desconectados. Fuera de
 * esos dos minutos solo mira el reloj cada 10 s. Se acaba solo si ya no hay botones.
 */
#include "inkview.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include "pockit.h"

#define GLOBAL_CFG FLASHDIR "/system/config/global.cfg"
#define PIDFILE "/tmp/pockit-mando.pid"
#define NEXT "@KA_next"
#define PREV "@KA_prev"
#define MAX_BT 64
#define M 64
#define SOFT 0x777777
#define LINE 0xC8C8C8

typedef struct { int slot, code; char name[120], act[40]; } Btn;
static Btn btn[MAX_BT];
static int nbtn, count;   /* count: btkeys.count (huecos, también los borrados) */

static ifont *f_title, *f_head, *f_body, *f_bold, *f_small, *f_big, *f_arrow;
static int W, H, shown, want_back;   /* want_back: el siguiente botón es el de retroceder */
static int lit = -1, fwd_code = -1;  /* lit: botón resaltado (el último pulsado) */
static char status[200];
static int list_y, more_y, rows_shown;
static void (*on_close)(void);

static void fonts(void)
{
    if (f_title) return;
    W = ScreenWidth(); H = ScreenHeight();
    f_title = OpenFont(DEFAULTFONTB, 64, 1);
    f_big = OpenFont(DEFAULTFONTB, 46, 1);
    f_arrow = OpenFont(DEFAULTFONTB, 150, 1);
    f_head = OpenFont(DEFAULTFONT, 38, 1);
    f_body = OpenFont(DEFAULTFONT, 34, 1);
    f_bold = OpenFont(DEFAULTFONTB, 36, 1);
    f_small = OpenFont(DEFAULTFONT, 28, 1);
}

/* ---------- global.cfg ---------- */

/* Sin InkView (lo usa también el proceso que reconecta): cuántos botones hay de verdad. */
int mando_count(void)
{
    FILE *f = fopen(GLOBAL_CFG, "r");
    if (!f) return 0;
    char l[400];
    int n = 0, del = 0;
    while (fgets(l, sizeof l, f)) {
        if (!strncmp(l, "btkeys.count=", 13)) n = atoi(l + 13);
        else if (!strncmp(l, "btkeys.bt", 9) && strstr(l, ".name=__DELETED__")) del++;
    }
    fclose(f);
    return n > del ? n - del : 0;
}

static void load(void)
{
    nbtn = 0;
    iconfig *c = OpenConfig(GLOBAL_CFG, NULL);
    if (!c) return;
    count = ReadInt(c, "btkeys.count", 0);
    for (int i = 1; i <= count && nbtn < MAX_BT; i++) {
        char k[64];
        snprintf(k, sizeof k, "btkeys.bt%d.name", i);
        const char *name = ReadString(c, k, "");
        if (!name[0] || !strcmp(name, "__DELETED__")) continue;
        Btn *b = &btn[nbtn++];
        b->slot = i;
        snprintf(b->name, sizeof b->name, "%s", name);
        snprintf(k, sizeof k, "btkeys.bt%d.scanCode", i);
        b->code = ReadInt(c, k, -1);
        snprintf(k, sizeof k, "tkey.bt%d.0", i);
        snprintf(b->act, sizeof b->act, "%s", ReadString(c, k, "@KA_none"));
    }
    CloseConfigNoSave(c);
}

/* El nombre de una tecla, tal como se ve en el mando */
static const char *key_label(int code, char *buf, size_t n)
{
    switch (code) {
    case 103: return "↑";
    case 108: return "↓";
    case 105: return "←";
    case 106: return "→";
    case 104: return T("RePág");
    case 109: return T("AvPág");
    case 115: return "Vol +";
    case 114: return "Vol −";
    case 113: return T("Silencio");
    case 57: return T("Espacio");
    case 28: return T("Intro");
    case 1: return "Esc";
    case 14: return T("Borrar");
    case 163: return "⏭";
    case 165: return "⏮";
    case 164: return "⏯";
    case 212: return T("Cámara");
    }
    snprintf(buf, n, T("Tecla %d"), code);
    return buf;
}

static int is_next(const Btn *b) { return !strcmp(b->act, NEXT); }
static int is_prev(const Btn *b) { return !strcmp(b->act, PREV); }

/* Guarda un botón (nuevo si slot == 0) con la acción de pasar página o volver, en ePub y en PDF. */
static void save(Btn *b, const char *act)
{
    iconfig *c = OpenConfig(GLOBAL_CFG, NULL);
    if (!c) { snprintf(status, sizeof status, "%s", T("No se pudo guardar en los ajustes del lector.")); return; }
    char k[64], lb[32];
    int fresh = !b->slot;
    if (fresh) {
        count = ReadInt(c, "btkeys.count", 0) + 1;
        b->slot = count;
        WriteInt(c, "btkeys.count", count);
        snprintf(k, sizeof k, "btkeys.bt%d.scanCode", b->slot);
        WriteInt(c, k, b->code);
        snprintf(k, sizeof k, "gkey.bt%d.0", b->slot);
        WriteString(c, k, "@KA_none");
    }
    snprintf(b->act, sizeof b->act, "%s", act);
    snprintf(b->name, sizeof b->name, !strcmp(act, NEXT) ? T("Mando · avanzar (%s)") : T("Mando · retroceder (%s)"),
             key_label(b->code, lb, sizeof lb));
    snprintf(k, sizeof k, "btkeys.bt%d.name", b->slot);
    WriteString(c, k, b->name);
    snprintf(k, sizeof k, "tkey.bt%d.0", b->slot);
    WriteString(c, k, act);
    snprintf(k, sizeof k, "pkey.bt%d.0", b->slot);
    WriteString(c, k, act);
    SaveConfig(c);
    CloseConfig(c);
    SendEventTo(-1, EVT_CONFIGCHANGED, 0, 0);   /* como el propio lector: que lo lean ya */
}

/* ---------- pantalla ---------- */

static void draw(void)
{
    fonts();
    ClearScreen();
    SetFont(f_head, BLACK);
    DrawString(M - 20, 42, T("‹ Ajustes"));
    SetFont(f_title, BLACK);
    DrawString(M, 110, T("Tu mando"));

    /* lo que hay que pulsar ahora, en grande */
    int y = 230, ch = 250;
    for (int i = 0; i < 3; i++) DrawRect(M + i, y + i, W - 2 * M - 2 * i, ch - 2 * i, BLACK);
    SetFont(f_arrow, BLACK);
    DrawTextRect(M + 20, y, 170, ch, want_back ? "‹" : "›", ALIGN_CENTER | VALIGN_MIDDLE);
    SetFont(f_big, BLACK);
    DrawTextRect(M + 200, y + 40, W - 2 * M - 240, 120,
                 want_back ? T("Ahora, el botón para retroceder") : T("Pulsa en tu mando el botón para avanzar"), ALIGN_LEFT | VALIGN_MIDDLE);
    SetFont(f_small, SOFT);
    DrawTextRect(M + 200, y + 160, W - 2 * M - 240, 70,
                 status[0] ? status : T("Si tu mando tiene varios modos, repítelo en cada uno."), ALIGN_LEFT | DOTS);
    y += ch + 50;

    /* los botones que ya tiene el lector; tocar uno le da la vuelta */
    more_y = H - 190;
    SetFont(f_small, SOFT);
    DrawString(M, y, nbtn ? T("TUS BOTONES · TOCA UNO PARA DARLE LA VUELTA") : T("TUS BOTONES"));
    y += 50;
    list_y = y;
    int rh = 96;
    rows_shown = 0;
    if (!nbtn) { SetFont(f_body, SOFT); DrawString(M, y + 20, T("Aún ninguno.")); }
    for (int i = 0; i < nbtn && y + rh <= more_y - 30; i++, y += rh, rows_shown++) {
        Btn *b = &btn[i];
        char lb[32], act[80];
        if (i == lit) FillArea(M - 16, y, W - 2 * M + 32, rh - 6, BLACK);
        int fg = i == lit ? WHITE : BLACK;
        SetFont(f_bold, fg);
        DrawTextRect(M, y, W / 2 - M, rh - 6, key_label(b->code, lb, sizeof lb), ALIGN_LEFT | VALIGN_MIDDLE | DOTS);
        snprintf(act, sizeof act, "%s", is_next(b) ? T("Avanza  ›") : is_prev(b) ? T("‹  Retrocede") : GetLangText(b->act));
        SetFont(f_body, i == lit ? WHITE : (is_next(b) || is_prev(b) ? BLACK : SOFT));
        DrawTextRect(W / 2, y, W / 2 - M, rh - 6, act, ALIGN_RIGHT | VALIGN_MIDDLE | DOTS);
        if (i != lit) DrawLine(M, y + rh - 3, W - M, y + rh - 3, LINE);
    }
    if (rows_shown < nbtn) {
        char more[80];
        snprintf(more, sizeof more, T("y %d más"), nbtn - rows_shown);
        SetFont(f_small, SOFT);
        DrawString(M, y + 10, more);
    }

    /* el resto de funciones, en la pantalla del propio lector */
    DrawLine(M, more_y - 20, W - M, more_y - 20, LINE);
    SetFont(f_bold, nbtn ? BLACK : SOFT);
    DrawString(M, more_y + 10, T("Más funciones  ›"));
    SetFont(f_small, SOFT);
    DrawString(M, more_y + 60, T("Capítulos, luz, pulsación larga… y quitar botones"));
    POCKIT_REFRESH(shown);
}

static int find(int code)
{
    for (int i = 0; i < nbtn; i++) if (btn[i].code == code) return i;
    return -1;
}

static void pressed(int code)
{
    char lb[32];
    int i = find(code);
    if (want_back && code == fwd_code) {
        snprintf(status, sizeof status, T("«%s» es el de avanzar: pulsa el otro."), key_label(code, lb, sizeof lb));
        lit = i;
        draw();
        return;
    }
    if (i < 0) {
        if (nbtn == MAX_BT) return;
        i = nbtn++;
        memset(&btn[i], 0, sizeof btn[i]);
        btn[i].code = code;
    }
    save(&btn[i], want_back ? PREV : NEXT);
    snprintf(status, sizeof status, want_back ? T("Retroceder: %s. Listo.") : T("Avanzar: %s."), key_label(code, lb, sizeof lb));
    fwd_code = want_back ? -1 : code;
    want_back = !want_back;
    lit = i;
    draw();
}

/* Abrir la pantalla del lector desde aquí (OpenTask de bt_keymapping.app) lo dejaba colgado:
 * se dice dónde está. */
static void open_native(void)
{
    Message(ICON_INFORMATION, T("Más funciones"),
            T("En los Ajustes del lector, dentro de la lectura de EPUB (y de PDF), en «Botones Bluetooth»: tus botones salen ahí con su nombre y a cada uno le puedes dar la función que quieras, también al mantenerlo pulsado."), 8000);
}

void mando_open(void (*close_cb)(void))
{
    on_close = close_cb;
    want_back = 0; lit = -1; fwd_code = -1; status[0] = 0;
    shown = 0;
    load();
    draw();
}

int mando_handler(int type, int par1, int par2)
{
    switch (type) {
    case EVT_SHOW: load(); draw(); return 1;
    case EVT_KEYPRESS_EXT: pressed(par1); return 1;
    case EVT_KEYRELEASE_EXT: case EVT_KEYREPEAT_EXT: return 1;
    case EVT_KEYPRESS:
        if (par1 == IV_KEY_BACK || par1 == IV_KEY_HOME || par1 == IV_KEY_MENU) { mando_watch(); on_close(); }
        return 1;
    case EVT_POINTERUP: {
        int x = par1, y = par2;
        if (y < 100 && x < W / 3) { mando_watch(); on_close(); return 1; }
        if (y >= more_y - 20 && nbtn) { open_native(); return 1; }
        if (y >= list_y && y < list_y + rows_shown * 96) {
            Btn *b = &btn[(y - list_y) / 96];
            save(b, is_next(b) ? PREV : NEXT);
            lit = (int)(b - btn);
            status[0] = 0;
            draw();
        }
        return 1;
    }
    }
    return 0;
}

/* ---------- reconectar al despertar ---------- */

/* Los mandos y teclados Bluetooth emparejados y desconectados; devuelve cuántos había. */
static int reconnect(void)
{
    FILE *p = popen("dbus-send --system --print-reply --dest=org.bluez / org.freedesktop.DBus.ObjectManager.GetManagedObjects 2>/dev/null", "r");
    if (!p) return 0;
    char line[400], cur[120] = "", todo[8][120];
    int hid = 0, paired = 0, conn = 1, want = 0, ntodo = 0;
    for (;;) {
        char *got = fgets(line, sizeof line, p);
        char *op = got ? strstr(line, "object path \"") : NULL;
        if (!got || op) {   /* acaba un objeto: si era un mando desconectado, a la lista */
            if (cur[0] && hid && paired && !conn && ntodo < 8) snprintf(todo[ntodo++], sizeof todo[0], "%s", cur);
            if (!got) break;
            cur[0] = 0; hid = paired = 0; conn = 1;
            char *s = op + 13, *e = strchr(s, '"');
            if (e && e - s < (int)sizeof cur) {
                *e = 0;
                const char *dev = strstr(s, "/dev_");
                if (dev && !strchr(dev + 1, '/')) snprintf(cur, sizeof cur, "%s", s);   /* el dispositivo, no sus servicios */
            }
            continue;
        }
        if (strstr(line, "00001812-") || strstr(line, "00001124-")) hid = 1;   /* HID por BLE o clásico */
        if (strstr(line, "\"Paired\"")) want = 1;
        else if (strstr(line, "\"Connected\"")) want = 2;
        else if (want && strstr(line, "boolean")) {
            int v = strstr(line, "true") != NULL;
            if (want == 1) paired = v; else conn = v;
            want = 0;
        }
    }
    pclose(p);
    for (int i = 0; i < ntodo; i++) {
        char cmd[300];
        snprintf(cmd, sizeof cmd, "dbus-send --system --print-reply --dest=org.bluez '%s' org.bluez.Device1.Connect >/dev/null 2>&1", todo[i]);
        system(cmd);
    }
    return ntodo;
}

int mando_daemon(void)
{
    signal(SIGHUP, SIG_IGN);
    FILE *f = fopen(PIDFILE, "w");
    if (f) { fprintf(f, "%d\n", (int)getpid()); fclose(f); }
    int tries = 12;   /* dos minutos */
    time_t last = time(NULL);
    for (;;) {
        if (tries > 0) { tries--; if (!reconnect()) tries = 0; }
        sleep(10);
        time_t now = time(NULL);
        if (now - last > 40 || now < last) {   /* el lector ha dormido */
            if (!mando_count()) break;
            tries = 12;
        }
        last = now;
    }
    remove(PIDFILE);
    return 0;
}

/* Arranca el proceso que reconecta, si hay botones y no está ya en marcha. */
void mando_watch(void)
{
#ifndef EMULATOR
    if (!mando_count()) return;
    FILE *f = fopen(PIDFILE, "r");
    int pid = 0;
    if (f) { if (fscanf(f, "%d", &pid) != 1) pid = 0; fclose(f); }
    if (pid > 0 && !kill(pid, 0)) {
        char p[64], cmd[300] = "";
        snprintf(p, sizeof p, "/proc/%d/cmdline", pid);
        FILE *c = fopen(p, "r");
        size_t n = c ? fread(cmd, 1, sizeof cmd - 1, c) : 0;
        if (c) fclose(c);
        for (size_t i = 0; i < n; i++) if (!cmd[i]) cmd[i] = ' ';
        if (strstr(cmd, "--mando")) return;   /* ya está */
    }
    char self[600], cmd[900];
    ssize_t l = readlink("/proc/self/exe", self, sizeof self - 1);
    if (l <= 0) return;
    self[l] = 0;
    snprintf(cmd, sizeof cmd, "R=; command -v setsid >/dev/null 2>&1 && R=setsid; $R '%s' --mando </dev/null >/dev/null 2>&1 &", self);
    system(cmd);
#endif
}
