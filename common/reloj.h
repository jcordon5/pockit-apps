/*
 * reloj.h: reloj de palabras en castellano para la tinta («son las siete y media»).
 * Lo usan el Inicio de Pockit (con los cuatro puntos de los minutos) y el Modo bolsillo
 * (sin ellos: así solo cambia cada cinco minutos).
 *
 * Rejilla propia que se adapta al hueco: en un cuadrado, 11 × 10; en uno apaisado, más
 * columnas y menos filas. Las palabras se solapan donde pueden («DOSEISIETE»,
 * «CINCOCHONCE») y eso deja sitio para «EN PUNTO». La tipografía es monolínea tipo DIN,
 * de 18 de alto y trazo de 3, con todos los trazos rectos centrados en x,5: la tinta la
 * pinta limpia. Cada letra se dibuja una vez con antialias y luego se estampa.
 *
 *   rj_draw_box(x, y, w, h, hora, minuto, puntos, margen x, margen y, alto de letra)
 *   rj_phrase(h, m, out, n)                     «son las siete y media»
 *   rj_on / rj_off / rj_bg                      colores (por defecto, tinta sobre papel)
 *
 * Archivo de ajustes compartido: system/config/pockit/reloj.cfg («inicio=1», «bolsillo=1»).
 */
#ifndef RELOJ_H
#define RELOJ_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Las piezas de la rejilla, en orden de lectura. Algunas guardan varias palabras que comparten
 * letras («DOSEISIETE»: dos, seis y siete). La rejilla se monta con tantas columnas como pida
 * el hueco (rj_layout): con 11 sale la cuadrada de siempre. */
static const char *const RJ_TOK[] = {
    "SON", "ES", "LAS", "UNA", "DOSEISIETE", "TRES", "CUATRO", "CINCOCHONCE", "NUEVE", "DIEZ", "DOCE",
    "EN", "PUNTO", "MENOS", "Y", "DIEZ", "CUARTO", "MEDIA", "VEINTICINCO", "VEINTE",
};
#define RJ_NTOK ((int)(sizeof RJ_TOK / sizeof RJ_TOK[0]))

enum { RJ_ES, RJ_SON, RJ_LA, RJ_LAS, RJ_PUNTO, RJ_Y, RJ_MENOS, RJ_M5, RJ_M10, RJ_M15, RJ_M20, RJ_M25, RJ_M30, RJ_H1 };
/* cada palabra: hasta dos trozos [pieza, desde, cuántas letras] */
static const signed char RJ_PARTS[RJ_H1 + 12][2][3] = {
    [RJ_ES] = { { 1, 0, 2 }, { -1 } }, [RJ_SON] = { { 0, 0, 3 }, { -1 } },
    [RJ_LA] = { { 2, 0, 2 }, { -1 } }, [RJ_LAS] = { { 2, 0, 3 }, { -1 } },
    [RJ_PUNTO] = { { 11, 0, 2 }, { 12, 0, 5 } }, [RJ_Y] = { { 14, 0, 1 }, { -1 } }, [RJ_MENOS] = { { 13, 0, 5 }, { -1 } },
    [RJ_M5] = { { 18, 6, 5 }, { -1 } }, [RJ_M10] = { { 15, 0, 4 }, { -1 } }, [RJ_M15] = { { 16, 0, 6 }, { -1 } },
    [RJ_M20] = { { 19, 0, 6 }, { -1 } }, [RJ_M25] = { { 18, 0, 11 }, { -1 } }, [RJ_M30] = { { 17, 0, 5 }, { -1 } },
    [RJ_H1 + 0] = { { 3, 0, 3 }, { -1 } },   /* UNA */
    [RJ_H1 + 1] = { { 4, 0, 3 }, { -1 } },   /* DOS */
    [RJ_H1 + 2] = { { 5, 0, 4 }, { -1 } },   /* TRES */
    [RJ_H1 + 3] = { { 6, 0, 6 }, { -1 } },   /* CUATRO */
    [RJ_H1 + 4] = { { 7, 0, 5 }, { -1 } },   /* CINCO */
    [RJ_H1 + 5] = { { 4, 2, 4 }, { -1 } },   /* SEIS */
    [RJ_H1 + 6] = { { 4, 5, 5 }, { -1 } },   /* SIETE */
    [RJ_H1 + 7] = { { 7, 4, 4 }, { -1 } },   /* OCHO */
    [RJ_H1 + 8] = { { 8, 0, 5 }, { -1 } },   /* NUEVE */
    [RJ_H1 + 9] = { { 9, 0, 4 }, { -1 } },   /* DIEZ */
    [RJ_H1 + 10] = { { 7, 7, 4 }, { -1 } },  /* ONCE */
    [RJ_H1 + 11] = { { 10, 0, 4 }, { -1 } }, /* DOCE */
};

/* La rejilla montada: filas de «cols» letras. pos[pieza] = fila*cols + columna. */
#define RJ_MAXC 40
#define RJ_MAXR 12
typedef struct { int cols, rows, pos[RJ_NTOK]; char g[RJ_MAXR][RJ_MAXC + 1]; } RjLayout;

/* Las piezas van seguidas, y donde una fila no se llena se reparten letras de relleno entre
 * ellas. La última fila acaba con la firma si cabe. */
static int rj_layout(int cols, RjLayout *L)
{
    static const char FILL[] = "ALOREZMUTAPCOLAIRTEMOZA";
    int fi = 0, t = 0;
    if (cols < 11 || cols > RJ_MAXC) return 0;
    L->cols = cols; L->rows = 0;
    while (t < RJ_NTOK) {
        if (L->rows == RJ_MAXR) return 0;
        int a = t, used = 0;
        while (t < RJ_NTOK && used + (int)strlen(RJ_TOK[t]) <= cols) used += (int)strlen(RJ_TOK[t++]);
        int n = t - a, extra = cols - used, r = L->rows++;
        char *row = L->g[r];
        int c = 0;
        int tail = n > 1 ? 0 : extra;   /* una sola pieza: el relleno, detrás */
        int inner = extra - tail;
        for (int i = a; i < t; i++) {
            if (i > a) {   /* hueco entre piezas: el relleno, repartido */
                int k = i - a - 1, gaps = n - 1, put = inner / gaps + (k < inner % gaps);
                while (put--) row[c++] = FILL[fi++ % (sizeof FILL - 1)];
            }
            L->pos[i] = r * cols + c;
            for (const char *q = RJ_TOK[i]; *q; q++) row[c++] = *q;
        }
        int sign = t == RJ_NTOK && cols - c >= 4;   /* la firma, al final de la última fila */
        while (c < cols - (sign ? 4 : 0)) row[c++] = FILL[fi++ % (sizeof FILL - 1)];
        if (sign) { memcpy(row + c, "YOUS", 4); c += 4; }
        row[cols] = 0;
    }
    return 1;
}

static const char *const RJ_NAMES[RJ_H1 + 12] = {
    "es", "son", "la", "las", "en punto", "y", "menos", "cinco", "diez", "cuarto", "veinte", "veinticinco", "media",
    "una", "dos", "tres", "cuatro", "cinco", "seis", "siete", "ocho", "nueve", "diez", "once", "doce",
};

/* Las palabras de una hora, en orden de lectura. Devuelve cuántas. */
static int rj_keys(int h, int m, int k[5])
{
    int step = m - m % 5, hour = h % 12, n = 0;
    if (step > 30) hour = (hour + 1) % 12;
    if (hour == 0) hour = 12;
    if (hour == 1) { k[n++] = RJ_ES; k[n++] = RJ_LA; }
    else { k[n++] = RJ_SON; k[n++] = RJ_LAS; }
    k[n++] = RJ_H1 + hour - 1;
    static const int MIN[] = { 0, RJ_M5, RJ_M10, RJ_M15, RJ_M20, RJ_M25, RJ_M30 };
    if (step == 0) k[n++] = RJ_PUNTO;
    else if (step > 30) { k[n++] = RJ_MENOS; k[n++] = MIN[(60 - step) / 5]; }
    else { k[n++] = RJ_Y; k[n++] = MIN[step / 5]; }
    return n;
}

/* Letras encendidas en una rejilla montada: on[fila*cols+col] = 1. */
static void rj_lit(const RjLayout *L, int h, int m, unsigned char *on)
{
    int k[5], n = rj_keys(h, m, k);
    memset(on, 0, (size_t)L->rows * L->cols);
    for (int i = 0; i < n; i++)
        for (int p = 0; p < 2; p++) {
            const signed char *w = RJ_PARTS[k[i]][p];
            if (w[0] < 0) break;
            for (int j = 0; j < w[2]; j++) on[L->pos[(int)w[0]] + w[1] + j] = 1;
        }
}

/* La hora como frase: «son las siete y media», «es la una en punto». */
static void rj_phrase(int h, int m, char *out, size_t n)
{
    int k[5], c = rj_keys(h, m, k);
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < c; i++) o += snprintf(out + o, o < n ? n - o : 0, "%s%s", i ? " " : "", RJ_NAMES[k[i]]);
}

/* ---- tipografía: [letra, ancho, trazado SVG] (M L H V C A Z absolutos) ---- */
#define RJ_GH 18.0f
static const struct { char c; float w; const char *d; } RJ_GLYPHS[] = {
    { 'A', 13, "M1.19 19.5 5.51-1.5M11.81 19.5 7.49-1.5M2.5 12.5H10.5" },
    { 'C', 12, "M9.9 3.75A4.5 4.5 0 0 0 1.5 6V12A4.5 4.5 0 0 0 9.9 14.25" },
    { 'D', 12, "M1.5 0V18M1.5 1.5H5.5A5 5 0 0 1 10.5 6.5V11.5A5 5 0 0 1 5.5 16.5H1.5" },
    { 'E', 10, "M1.5 0V18M1.5 1.5H10M1.5 8.5H9M1.5 16.5H10" },
    { 'H', 12, "M1.5 0V18M10.5 0V18M1.5 8.5H10.5" },
    { 'I', 3, "M1.5 0V18" },
    { 'L', 10, "M1.5 0V18M1.5 16.5H10" },
    { 'M', 14, "M1.5 0V18M12.5 0V18M1.2-.6 7 12.4 12.8-.6" },
    { 'N', 12, "M1.5 0V18M10.5 0V18M.9-.6 11.1 18.6" },
    { 'O', 12, "M1.5 6V12A4.5 4.5 0 0 0 10.5 12V6A4.5 4.5 0 0 0 1.5 6Z" },
    { 'P', 12, "M1.5 0V18M1.5 1.5H6.5A4 4 0 0 1 6.5 9.5H1.5" },
    { 'R', 12, "M1.5 0V18M1.5 1.5H6.5A4 4 0 0 1 6.5 9.5H1.5M6 8.5 11.2 19.6" },
    { 'S', 12, "M10.3 4.2C9.6 2.4 8 1.5 6 1.5C3.4 1.5 1.5 2.9 1.5 5.1C1.5 7.6 3.6 8.3 6 9C8.4 9.7 10.5 10.4 10.5 12.9C10.5 15.1 8.6 16.5 6 16.5C4 16.5 2.4 15.6 1.7 13.8" },
    { 'T', 11, "M0 1.5H11M5.5 0V18" },
    { 'U', 12, "M1.5 0V12A4.5 4.5 0 0 0 10.5 12V0" },
    { 'V', 13, "M.76-1.5 6.94 19.5M12.24-1.5 6.06 19.5" },
    { 'Y', 13, "M.2-1.4 6.5 9.5 12.8-1.4M6.5 9.5V18" },
    { 'Z', 11, "M0 1.5H11M0 16.5H11M11 .23 0 17.77" },
};
#define RJ_NGLYPHS ((int)(sizeof RJ_GLYPHS / sizeof RJ_GLYPHS[0]))

/* Trazado aplanado en segmentos. Los extremos de un trazo abierto son rectos (como en SVG);
 * las uniones, redondas. */
typedef struct { float x0, y0, x1, y1; unsigned char butt0, butt1; } RjSeg;
typedef struct { RjSeg s[160]; int n; } RjPath;

static float rj_num(const char **p)
{
    while (**p == ' ' || **p == ',') (*p)++;
    char *e;
    float v = strtof(*p, &e);
    *p = e;
    return v;
}

static void rj_flat_begin(RjPath *P, int *first) { *first = P->n; }
static void rj_line(RjPath *P, float x0, float y0, float x1, float y1)
{
    if (P->n >= 160 || (x0 == x1 && y0 == y1)) return;
    RjSeg *s = &P->s[P->n++];
    s->x0 = x0; s->y0 = y0; s->x1 = x1; s->y1 = y1; s->butt0 = s->butt1 = 0;
}

/* Arco SVG (de extremo a extremo) → segmentos */
static void rj_arc(RjPath *P, float x1, float y1, float rx, float ry, float rot, int large, int sweep, float x2, float y2)
{
    float cphi = cosf(rot * (float)M_PI / 180), sphi = sinf(rot * (float)M_PI / 180);
    float dx = (x1 - x2) / 2, dy = (y1 - y2) / 2;
    float x1p = cphi * dx + sphi * dy, y1p = -sphi * dx + cphi * dy;
    rx = fabsf(rx); ry = fabsf(ry);
    float lam = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry);
    if (lam > 1) { rx *= sqrtf(lam); ry *= sqrtf(lam); }
    float num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
    float den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    float co = den > 0 && num > 0 ? sqrtf(num / den) : 0;
    if (large == sweep) co = -co;
    float cxp = co * rx * y1p / ry, cyp = -co * ry * x1p / rx;
    float cx = cphi * cxp - sphi * cyp + (x1 + x2) / 2, cy = sphi * cxp + cphi * cyp + (y1 + y2) / 2;
    float t1 = atan2f((y1p - cyp) / ry, (x1p - cxp) / rx);
    float t2 = atan2f((-y1p - cyp) / ry, (-x1p - cxp) / rx);
    float dt = t2 - t1;
    if (sweep && dt < 0) dt += 2 * (float)M_PI;
    if (!sweep && dt > 0) dt -= 2 * (float)M_PI;
    int n = 4 + (int)(fabsf(dt) * 8);
    float px = x1, py = y1;
    for (int i = 1; i <= n; i++) {
        float t = t1 + dt * i / n;
        float ex = cx + rx * cosf(t) * cphi - ry * sinf(t) * sphi, ey = cy + rx * cosf(t) * sphi + ry * sinf(t) * cphi;
        if (i == n) { ex = x2; ey = y2; }
        rj_line(P, px, py, ex, ey);
        px = ex; py = ey;
    }
}

static void rj_parse(const char *d, RjPath *P)
{
    P->n = 0;
    float x = 0, y = 0, sx = 0, sy = 0;
    char cmd = 0;
    int first = 0;
    const char *p = d;
    /* al cerrar un subtrazo abierto, sus extremos son rectos */
#define RJ_END_SUB() do { if (P->n > first) { P->s[first].butt0 = 1; P->s[P->n - 1].butt1 = 1; } } while (0)
    for (;;) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        if ((*p >= 'A' && *p <= 'Z')) {
            cmd = *p++;
            if (cmd == 'Z') { rj_line(P, x, y, sx, sy); x = sx; y = sy; rj_flat_begin(P, &first); continue; }
            if (cmd == 'M') RJ_END_SUB();
        }
        switch (cmd) {
        case 'M': x = sx = rj_num(&p); y = sy = rj_num(&p); rj_flat_begin(P, &first); cmd = 'L'; break;
        case 'L': { float nx = rj_num(&p), ny = rj_num(&p); rj_line(P, x, y, nx, ny); x = nx; y = ny; break; }
        case 'H': { float nx = rj_num(&p); rj_line(P, x, y, nx, y); x = nx; break; }
        case 'V': { float ny = rj_num(&p); rj_line(P, x, y, x, ny); y = ny; break; }
        case 'C': {
            float c1x = rj_num(&p), c1y = rj_num(&p), c2x = rj_num(&p), c2y = rj_num(&p), ex = rj_num(&p), ey = rj_num(&p);
            float px = x, py = y;
            for (int i = 1; i <= 12; i++) {
                float t = i / 12.0f, u = 1 - t;
                float qx = u * u * u * x + 3 * u * u * t * c1x + 3 * u * t * t * c2x + t * t * t * ex;
                float qy = u * u * u * y + 3 * u * u * t * c1y + 3 * u * t * t * c2y + t * t * t * ey;
                rj_line(P, px, py, qx, qy);
                px = qx; py = qy;
            }
            x = ex; y = ey;
            break;
        }
        case 'A': {
            float rx = rj_num(&p), ry = rj_num(&p), rot = rj_num(&p);
            int large = (int)rj_num(&p), sweep = (int)rj_num(&p);
            float ex = rj_num(&p), ey = rj_num(&p);
            rj_arc(P, x, y, rx, ry, rot, large, sweep, ex, ey);
            x = ex; y = ey;
            break;
        }
        default: p++; break;
        }
    }
    /* el último subtrazo, si es abierto (los cerrados con Z ya empezaron otro vacío) */
    if (P->n > first) RJ_END_SUB();
#undef RJ_END_SUB
}

/* Distancia con signo al trazo (en unidades de letra): < 0 dentro. */
static float rj_sdf(const RjPath *P, float px, float py, float hw)
{
    float best = 1e9f;
    for (int i = 0; i < P->n; i++) {
        const RjSeg *s = &P->s[i];
        float vx = s->x1 - s->x0, vy = s->y1 - s->y0, l2 = vx * vx + vy * vy, len = sqrtf(l2);
        float t = ((px - s->x0) * vx + (py - s->y0) * vy) / l2, d;
        if ((t < 0 && s->butt0) || (t > 1 && s->butt1)) {
            float perp = fabsf((px - s->x0) * vy - (py - s->y0) * vx) / len - hw;
            float out = t < 0 ? -t * len : (t - 1) * len;
            d = perp > out ? perp : out;
        } else {
            float tc = t < 0 ? 0 : t > 1 ? 1 : t;
            float qx = s->x0 + vx * tc - px, qy = s->y0 + vy * tc - py;
            d = sqrtf(qx * qx + qy * qy) - hw;
        }
        if (d < best) best = d;
    }
    return best;
}

/* Máscara de cobertura de una letra (0..255), de ancho w·k y alto 18·k píxeles. */
typedef struct { int w, h; unsigned char *a; } RjMask;
static RjMask rj_masks[RJ_NGLYPHS];
static float rj_mask_k;

static void rj_build_masks(float k, float stroke)
{
    if (rj_mask_k == k) return;
    rj_mask_k = k;
    static RjPath P;
    for (int g = 0; g < RJ_NGLYPHS; g++) {
        free(rj_masks[g].a);
        float gw = RJ_GLYPHS[g].w;
        int w = (int)ceilf(gw * k), h = (int)ceilf(RJ_GH * k);
        unsigned char *a = calloc((size_t)w * h, 1);
        rj_parse(RJ_GLYPHS[g].d, &P);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                float gx = (x + 0.5f) / k, gy = (y + 0.5f) / k;
                float sd = rj_sdf(&P, gx, gy, stroke / 2);
                /* recorte a la caja de la letra: las diagonales se salen y ahí se cortan */
                float box = gx; if (gw - gx < box) box = gw - gx; if (gy < box) box = gy; if (RJ_GH - gy < box) box = RJ_GH - gy;
                float c = 0.5f - sd * k, cb = 0.5f + box * k;
                if (cb < c) c = cb;
                a[y * w + x] = c <= 0 ? 0 : c >= 1 ? 255 : (unsigned char)(c * 255);
            }
        rj_masks[g].w = w; rj_masks[g].h = h; rj_masks[g].a = a;
    }
}

static int rj_on = 0x111111, rj_off = 0xCFCFCF, rj_bg = 0xFFFFFF;

static int rj_mix(int bg, int fg, int a)
{
    int r = (((bg >> 16) & 255) * (255 - a) + ((fg >> 16) & 255) * a) / 255;
    int g = (((bg >> 8) & 255) * (255 - a) + ((fg >> 8) & 255) * a) / 255;
    int b = ((bg & 255) * (255 - a) + (fg & 255) * a) / 255;
    return r << 16 | g << 8 | b;
}

static void rj_stamp(int g, int cx, int cy, int color)
{
    const RjMask *m = &rj_masks[g];
    int x0 = cx - m->w / 2, y0 = cy - m->h / 2;
    for (int y = 0; y < m->h; y++)
        for (int x = 0; x < m->w; x++) {
            int a = m->a[y * m->w + x];
            if (a) DrawPixel(x0 + x, y0 + y, a == 255 ? color : rj_mix(rj_bg, color, a));
        }
}

static void rj_disc(int cx, int cy, float r, int color)
{
    int R = (int)ceilf(r) + 1;
    for (int y = -R; y <= R; y++)
        for (int x = -R; x <= R; x++) {
            float c = r + 0.5f - sqrtf((x + 0.f) * x + (y + 0.f) * y);
            if (c > 0) DrawPixel(cx + x, cy + y, c >= 1 ? color : rj_mix(rj_bg, color, (int)(c * 255)));
        }
}

/* El reloj en el rectángulo (x, y, w, h). La rejilla toma las columnas con las que las letras
 * salen más grandes: en un cuadrado, 11; en un hueco apaisado, más. Celdas de 26 × 30 (se
 * estiran a lo ancho para llenar el hueco); «cap» es el alto de las letras en esas unidades
 * (14 en el diseño). La rejilla queda a «px» y «py» del borde; los puntos de los minutos, en
 * las esquinas. */
static RjLayout rj_L;
static void rj_draw_box(int x, int y, int w, int h, int hh, int mm, int dots, int px, int py, float cap)
{
    const float CW = 26, CH = 30;
    int gw = w - 2 * px, gh = h - 2 * py;
    float best = 0;
    RjLayout L;
    for (int c = 11; c <= RJ_MAXC; c++) {
        if (!rj_layout(c, &L)) continue;
        float sx = gw / (c * CW), sy = gh / (L.rows * CH), s = sx < sy ? sx : sy;
        if (s > best * 1.02f) { best = s; rj_L = L; }
    }
    if (best <= 0) return;
    float s = best, cw = (float)gw / rj_L.cols, ch = CH * s;
    float ox = x + px, oy = y + py + (gh - rj_L.rows * ch) / 2;
    rj_build_masks(cap / RJ_GH * s, 3);
    FillArea(x, y, w, h, rj_bg);
    unsigned char on[RJ_MAXR * RJ_MAXC];
    rj_lit(&rj_L, hh, mm, on);
    for (int r = 0; r < rj_L.rows; r++)
        for (int c = 0; c < rj_L.cols; c++) {
            char chr = rj_L.g[r][c];
            int g = 0;
            while (g < RJ_NGLYPHS && RJ_GLYPHS[g].c != chr) g++;
            if (g == RJ_NGLYPHS) continue;
            rj_stamp(g, (int)(ox + (c + 0.5f) * cw + 0.5f), (int)(oy + (r + 0.5f) * ch + 0.5f), on[r * rj_L.cols + c] ? rj_on : rj_off);
        }
    if (dots) {
        float rad = 3.4f * s, e = rad + 2;
        float dx[4] = { x + e, x + w - e, x + w - e, x + e }, dy[4] = { y + e, y + e, y + h - e, y + h - e };
        for (int i = 0; i < 4; i++) rj_disc((int)dx[i], (int)dy[i], rad, i < mm % 5 ? rj_on : rj_off);
    }
}

/* Ajustes compartidos entre Pockit y el Modo bolsillo. Por defecto, encendido si el lector
 * está en español (el reloj habla castellano). */
#define RJ_DIR FLASHDIR "/system/config/pockit"
#define RJ_CFG RJ_DIR "/reloj.cfg"
static int rj_default = 1;
static int rj_enabled(const char *where)
{
    FILE *f = fopen(RJ_CFG, "r");
    if (!f) return rj_default;
    char line[80];
    int on = rj_default;
    size_t n = strlen(where);
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, where, n) && line[n] == '=') on = line[n + 1] != '0';
    fclose(f);
    return on;
}
static void rj_set_enabled(const char *where, int on)
{
    int ini = rj_enabled("inicio"), bol = rj_enabled("bolsillo");
    if (!strcmp(where, "inicio")) ini = on; else bol = on;
    mkdir(RJ_DIR, 0755);
    FILE *f = fopen(RJ_CFG, "w");
    if (!f) return;
    fprintf(f, "inicio=%d\nbolsillo=%d\n", ini, bol);
    fclose(f);
}

#endif
