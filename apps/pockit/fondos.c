/*
 * Fondos: cambiar el fondo de reposo desde el propio lector.
 *
 * Enseña tus fondos (la carpeta «Fondos de reposo», con transparencia, la misma que llena
 * pockit.yous.dev) y tus fotos (Photo). Al elegir uno, lo decodifica aquí mismo con
 * stb_image, lo recorta a la pantalla, convierte la transparencia en el color clave
 * #808040 (el que el lector deja ver la página que estabas leyendo) y lo guarda como el
 * BMP de 24 bits de reposo del tema, con una copia del anterior.
 * Las miniaturas se generan una vez y se guardan en system/config/pockit/miniaturas.
 */
#include "inkview.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "pockit.h"
#include "../../common/ai.h"
#define CloseApp pockit_home

#define STBI_NO_HDR
#include "../../common/stb_image.h"
unsigned long long fondos_hash(const char *s);

#define FONDOS_DIR FLASHDIR "/Fondos de reposo"
#define PHOTO_DIR FLASHDIR "/Photo"
#define THUMBS POCKIT_DIR "/miniaturas"
#define M 64
#define SOFT 0x777777
#define LINE 0xC8C8C8
#define COLS 3
#define MAX_IMG 400

static ifont *f_title, *f_head, *f_body, *f_small, *f_bold;
static int W, H, shown_once, page, preview = -1;
static char *imgs[MAX_IMG];
static int nimgs, nfondos;   /* los primeros nfondos son de «Fondos de reposo» */
static char theme[64] = "Line";

static void refresh(void) { POCKIT_REFRESH(shown_once); }

static int is_img(const char *n)
{
    const char *d = strrchr(n, '.');
    return d && (!strcasecmp(d, ".png") || !strcasecmp(d, ".jpg") || !strcasecmp(d, ".jpeg") || !strcasecmp(d, ".bmp"));
}

static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void scan_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    int from = nimgs;
    struct dirent *e;
    while ((e = readdir(d)) && nimgs < MAX_IMG) {
        if (e->d_name[0] == '.' || !is_img(e->d_name)) continue;
        char p[600];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        imgs[nimgs++] = strdup(p);
    }
    closedir(d);
    qsort(imgs + from, nimgs - from, sizeof *imgs, by_name);
}

static void scan(void)
{
    for (int i = 0; i < nimgs; i++) free(imgs[i]);
    nimgs = 0;
    scan_dir(FONDOS_DIR);
    nfondos = nimgs;
    scan_dir(PHOTO_DIR);
    /* tema activo, para saber dónde va el fondo */
    FILE *f = fopen(FLASHDIR "/system/config/global.cfg", "r");
    char buf[300];
    while (f && fgets(buf, sizeof buf, f)) if (!strncmp(buf, "theme=", 6)) { buf[strcspn(buf, "\r\n")] = 0; snprintf(theme, sizeof theme, "%s", buf + 6); }
    if (f) fclose(f);
}

/* ---------- BMP de 24 bits ---------- */
static int write_bmp(const char *path, const unsigned char *rgb, int w, int h)
{
    int row = (w * 3 + 3) & ~3;
    unsigned size = 54 + row * h;
    unsigned char hd[54] = { 'B', 'M' };
    #define LE32(o, v) do { hd[o] = (v) & 255; hd[o + 1] = ((v) >> 8) & 255; hd[o + 2] = ((v) >> 16) & 255; hd[o + 3] = ((v) >> 24) & 255; } while (0)
    LE32(2, size); LE32(10, 54); LE32(14, 40); LE32(18, w); LE32(22, h);
    hd[26] = 1; hd[28] = 24; LE32(34, row * h); LE32(38, 2835); LE32(42, 2835);
    char tmp[700];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    fwrite(hd, 1, 54, f);
    unsigned char *line = calloc(row, 1);
    for (int y = h - 1; y >= 0; y--) {   /* de abajo arriba, en BGR */
        const unsigned char *s = rgb + (long)y * w * 3;
        for (int x = 0; x < w; x++) { line[x * 3] = s[x * 3 + 2]; line[x * 3 + 1] = s[x * 3 + 1]; line[x * 3 + 2] = s[x * 3]; }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    return rename(tmp, path);
}

/* Recorta y escala a w×h «cubriendo» (como un fondo de pantalla). La transparencia pasa a
 * ser el color clave #808040; lo opaco se mezcla con blanco y nunca es justo ese color. */
static unsigned char *render(const char *src, int w, int h)
{
    int iw, ih, n;
    unsigned char *px = stbi_load(src, &iw, &ih, &n, 4);
    if (!px) return NULL;
    double s = (double)w / iw > (double)h / ih ? (double)w / iw : (double)h / ih;
    double ox = (iw * s - w) / 2, oy = (ih * s - h) / 2;
    unsigned char *out = malloc((long)w * h * 3);
    for (int y = 0; y < h; y++) {
        int sy0 = (int)((y + oy) / s), sy1 = (int)((y + 1 + oy) / s);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > ih) sy1 = ih;
        for (int x = 0; x < w; x++) {
            int sx0 = (int)((x + ox) / s), sx1 = (int)((x + 1 + ox) / s);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > iw) sx1 = iw;
            long r = 0, g = 0, b = 0, a = 0, k = 0;
            for (int yy = sy0; yy < sy1; yy++)   /* media del área: escalar hacia abajo sin dientes */
                for (int xx = sx0; xx < sx1; xx++) {
                    const unsigned char *p = px + ((long)yy * iw + xx) * 4;
                    r += p[0]; g += p[1]; b += p[2]; a += p[3]; k++;
                }
            unsigned char *o = out + ((long)y * w + x) * 3;
            if (!k || a / k < 128) { o[0] = 128; o[1] = 128; o[2] = 64; continue; }
            double al = a / (255.0 * k);
            int R = (int)(r / k * al + 255 * (1 - al)), G = (int)(g / k * al + 255 * (1 - al)), B = (int)(b / k * al + 255 * (1 - al));
            if (R == 128 && G == 128 && B == 64) B = 65;
            o[0] = R; o[1] = G; o[2] = B;
        }
    }
    stbi_image_free(px);
    return out;
}

static void thumb_path(const char *src, char *out, size_t n)
{
    struct stat st;
    long sz = stat(src, &st) ? 0 : (long)st.st_size;
    snprintf(out, n, THUMBS "/%016llx-%ld.bmp", fondos_hash(src), sz);
}

#define TW 300
#define TH 405
static ibitmap *thumb(const char *src)
{
    char p[300];
    thumb_path(src, p, sizeof p);
    ibitmap *b = LoadBitmap(p);
    if (b) return b;
    mkdir(POCKIT_DIR, 0755);
    mkdir(THUMBS, 0755);
    unsigned char *px = render(src, TW, TH);
    if (!px) return NULL;
    write_bmp(p, px, TW, TH);
    free(px);
    return LoadBitmap(p);
}

/* ---------- pantallas ---------- */
static int per_page(void) { return COLS * ((H - 330) / (TH + 70)); }

static void header(const char *title, const char *sub)
{
    SetFont(f_head, BLACK);
    DrawString(M - 20, 42, "‹ Pockit");
    SetFont(f_title, BLACK);
    DrawString(M, 110, title);
    if (sub) { SetFont(f_small, SOFT); DrawString(M, 190, sub); }
}

static void draw_grid(void)
{
    ClearScreen();
    header("Fondos", nimgs ? "Toca uno para ponerlo de fondo de reposo" : NULL);
    if (!nimgs) {
        SetFont(f_body, SOFT);
        DrawTextRect(M, 400, W - 2 * M, 400,
                     "Aún no tienes fondos.\n\nAñádelos desde el ordenador en pockit.yous.dev → Tu lector → Fondos, o pon fotos en la carpeta Photo del lector.", ALIGN_CENTER);
        refresh();
        return;
    }
    int per = per_page(), from = page * per, gap = (W - 2 * M - COLS * TW) / (COLS - 1);
    int y0 = 250;
    for (int k = 0; k < per && from + k < nimgs; k++) {
        int i = from + k, x = M + (k % COLS) * (TW + gap), y = y0 + (k / COLS) * (TH + 70);
        if (i == nfondos && k) { /* las fotos empiezan en su propia fila: se marca con la etiqueta */ }
        ibitmap *b = thumb(imgs[i]);
        if (b) {
            /* lo transparente se enseña como una página de libro (rayas grises) */
            FillArea(x, y, TW, TH, WHITE);
            for (int ly = y + 40; ly < y + TH - 20; ly += 22) FillArea(x + 24, ly, TW - 48, 6, 0xDDDDDD);
            DrawBitmap(x, y, b);
            free(b);
        } else FillArea(x, y, TW, TH, 0xE6E6E6);
        DrawRect(x, y, TW, TH, LINE);
        const char *nm = strrchr(imgs[i], '/') + 1;
        SetFont(f_small, SOFT);
        DrawTextRect(x, y + TH + 6, TW, 40, i < nfondos ? nm : "Foto", ALIGN_CENTER | DOTS);
    }
    int pages = (nimgs + per - 1) / per;
    if (pages > 1) {
        char pg[40];
        snprintf(pg, sizeof pg, "‹   %d de %d   ›", page + 1, pages);
        SetFont(f_body, BLACK);
        DrawTextRect(0, H - 90, W, 60, pg, ALIGN_CENTER | VALIGN_MIDDLE);
    }
    refresh();
}

static void draw_preview(void)
{
    ClearScreen();
    header("Fondo de reposo", NULL);
    int ph = H - 520, pw = ph * W / H, x = (W - pw) / 2, y = 220;
    ibitmap *b = thumb(imgs[preview]);
    FillArea(x, y, pw, ph, WHITE);
    for (int ly = y + 60; ly < y + ph - 40; ly += 34) FillArea(x + 40, ly, pw - 80, 8, 0xDDDDDD);
    if (b) { StretchBitmap(x, y, pw, ph, b, 0); free(b); }
    DrawRect(x, y, pw, ph, BLACK);
    SetFont(f_small, SOFT);
    DrawTextRect(M, y + ph + 16, W - 2 * M, 40, preview < nfondos ? "Lo transparente deja ver la página que estabas leyendo" : "Las fotos se ponen enteras, sin transparencia", ALIGN_CENTER);
    int by = H - 200;
    FillArea(M, by, W - 2 * M, 100, BLACK);
    SetFont(f_bold, WHITE);
    DrawTextRect(M, by, W - 2 * M, 100, "Poner de fondo de reposo", ALIGN_CENTER | VALIGN_MIDDLE);
    SetFont(f_body, BLACK);
    DrawTextRect(M, by + 110, W - 2 * M, 60, "Cancelar", ALIGN_CENTER | VALIGN_MIDDLE);
    refresh();
}

static void apply(void)
{
    const char *src = imgs[preview];
    FillArea(M, H - 200, W - 2 * M, 100, WHITE);
    SetFont(f_body, BLACK);
    DrawTextRect(M, H - 200, W - 2 * M, 100, "Preparando el fondo…", ALIGN_CENTER | VALIGN_MIDDLE);
    PartialUpdate(M, H - 200, W - 2 * M, 100);
    unsigned char *px = render(src, W, H);
    char dst[400], bak[420];
    snprintf(dst, sizeof dst, FLASHDIR "/system/resources/%s/taskmgr_lock_background.bmp", theme);
    snprintf(bak, sizeof bak, FLASHDIR "/system/resources/%s/taskmgr_lock_background.anterior.bmp", theme);
    char dir[400];
    snprintf(dir, sizeof dir, FLASHDIR "/system/resources/%s", theme);
    mkdir(FLASHDIR "/system/resources", 0755);
    mkdir(dir, 0755);
    struct stat st;
    if (!stat(dst, &st) && stat(bak, &st)) rename(dst, bak);   /* copia del de antes, la primera vez */
    int ok = px && write_bmp(dst, px, W, H) == 0;
    free(px);
    preview = -1;
    Message(ok ? ICON_INFORMATION : ICON_WARNING, "Fondos",
            ok ? "Fondo de reposo cambiado. Bloquea el lector para verlo; si sigue el anterior, reinícialo una vez." : "No se pudo preparar la imagen.", 4000);
    draw_grid();
}

static int handler(int type, int par1, int par2)
{
    switch (type) {
    case EVT_INIT:
        SetPanelType(PANEL_DISABLED);
        W = ScreenWidth(); H = ScreenHeight();
        f_title = OpenFont(DEFAULTFONTB, 60, 1);
        f_head = OpenFont(DEFAULTFONT, 38, 1);
        f_body = OpenFont(DEFAULTFONT, 34, 1);
        f_bold = OpenFont(DEFAULTFONTB, 36, 1);
        f_small = OpenFont(DEFAULTFONT, 28, 1);
        break;
    case EVT_SHOW:
        scan();
        if (preview >= 0) draw_preview(); else draw_grid();
        break;
    case EVT_POINTERUP: {
        int x = par1, y = par2;
        if (y < 90 && x < W / 3) { if (preview >= 0) { preview = -1; draw_grid(); } else CloseApp(); return 1; }
        if (preview >= 0) {
            if (y >= H - 200 && y < H - 100) apply();
            else if (y >= H - 100) { preview = -1; draw_grid(); }
            return 1;
        }
        int per = per_page(), pages = (nimgs + per - 1) / per;
        if (y > H - 110) { if (x > W / 2 && page + 1 < pages) page++; else if (x <= W / 2 && page > 0) page--; else return 1; draw_grid(); return 1; }
        int gap = (W - 2 * M - COLS * TW) / (COLS - 1), c = (x - M) / (TW + gap), r = (y - 250) / (TH + 70);
        if (y < 250 || c < 0 || c >= COLS) return 1;
        int i = page * per + r * COLS + c;
        if (i < nimgs && r * COLS + c < per) { preview = i; draw_preview(); }
        return 1;
    }
    case EVT_KEYPRESS:
        if (par1 == IV_KEY_BACK || par1 == IV_KEY_HOME || par1 == IV_KEY_MENU) { if (preview >= 0) { preview = -1; draw_grid(); } else CloseApp(); return 1; }
        if (preview < 0 && (par1 == IV_KEY_NEXT || par1 == IV_KEY_NEXT2) && (page + 1) * per_page() < nimgs) { page++; draw_grid(); return 1; }
        if (preview < 0 && (par1 == IV_KEY_PREV || par1 == IV_KEY_PREV2) && page > 0) { page--; draw_grid(); return 1; }
        break;
    }
    return 0;
}

int fondos_handler(int type, int par1, int par2) { return handler(type, par1, par2); }
