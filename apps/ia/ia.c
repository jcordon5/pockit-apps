/*
 * IA: chat con DeepSeek en el PocketBook, al estilo de Claude o ChatGPT.
 *
 * Cada chat es un archivo .md en Chats/ ("### Tú" / "### IA" separan los mensajes), así
 * que se pueden leer en el ordenador. La respuesta llega por streaming y se va pintando
 * con el mismo Markdown que el Cuaderno; cualquier respuesta se puede guardar como nota.
 *
 * La clave de DeepSeek se guarda en system/config/ia.cfg. Para ponerla sin teclear:
 * dejar un archivo Chats/clave.txt con la clave; la app la mueve allí y lo borra.
 */
#include "inkview.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include "../../common/md.h"

#define CHATS_DIR FLASHDIR "/Chats"
#define NOTES_DIR FLASHDIR "/Notas"
#define KEY_CFG FLASHDIR "/system/config/ia.cfg"
#define KEY_DROP CHATS_DIR "/clave.txt"
#define API_URL "https://api.deepseek.com/chat/completions"
/* Modelos y precios de HORA PUNTA en dólares por millón de tokens (entrada en caché,
 * entrada, salida), de la tabla oficial de DeepSeek (octubre de 2026). Fuera de la hora
 * punta cuestan la mitad. ia.cfg puede traer otros (price.<id>.hit/in/out) y Cliply los
 * actualiza. */
typedef struct { const char *id, *name, *desc; double hit, in, out; } Model;
static Model MODELS[] = {
    { "deepseek-flash", "Flash", "rápido y muy barato", 0.006, 0.3, 1.2 },
    { "deepseek-v4-pro", "Pro", "más listo, más caro", 0.044, 1.32, 3.96 },
};

/* Hora punta: 01:00-04:00 y 06:00-10:00 UTC, de lunes a viernes. */
static int peak_now(void)
{
    time_t t = time(NULL);
    struct tm u;
    gmtime_r(&t, &u);
    if (u.tm_wday == 0 || u.tm_wday == 6) return 0;
    return (u.tm_hour >= 1 && u.tm_hour < 4) || (u.tm_hour >= 6 && u.tm_hour < 10);
}
static double price_factor(void) { return peak_now() ? 1.0 : 0.5; }
#define NMODELS (int)(sizeof MODELS / sizeof *MODELS)
static int model = 0;
#define SYSTEM_PROMPT "Eres un asistente dentro de un lector de libros electrónicos PocketBook. " \
    "Responde en el idioma del usuario, de forma clara y concisa. Usa Markdown sencillo " \
    "(títulos, negritas, cursivas, listas); evita tablas y bloques de código salvo que te los pidan."

#define MAX_CHATS 300
#define MAX_MSGS 400
#define PAPER_LINE 0xC8C8C8
#define SOFT 0x777777
#define M 64
#define LH 58
#define BASE 14
#define TOP_BAR 130
#define TOOLBAR 120

static ifont *f_title, *f_head, *f_body, *f_bold, *f_ital, *f_boldital, *f_small, *f_smallb, *f_h1, *f_h2, *f_mono;
static int W, H;
static char api_key[256];
static double chat_cost;   /* dólares gastados en el chat abierto */

typedef struct { char path[512]; char title[256]; char preview[256]; time_t mtime; } ChatInfo;
static ChatInfo chats[MAX_CHATS];
static int nchats, list_scroll;

typedef struct { int ia; char *text; int row0, row1; } Msg;
static Msg msgs[MAX_MSGS];
static int nmsgs;
static char cur_path[512], cur_title[256];
static int scroll_row, total_rows;

enum { LIST, CHAT, NOKEY } screen = LIST;
static int shown_once;
static char kbd[4096];

static const char *MESES[] = { "ene", "feb", "mar", "abr", "may", "jun", "jul", "ago", "sep", "oct", "nov", "dic" };

static void refresh(void) { if (!shown_once) { FullUpdate(); shown_once = 1; } else SoftUpdate(); }
static void trim_nl(char *s) { size_t n = strlen(s); while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) s[--n] = 0; }

static void date_str(time_t t, char *out, size_t n)
{
    struct tm tm, now;
    time_t nw = time(NULL);
    localtime_r(&t, &tm);
    localtime_r(&nw, &now);
    if (tm.tm_year == now.tm_year && tm.tm_yday == now.tm_yday) snprintf(out, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
    else if (tm.tm_year == now.tm_year) snprintf(out, n, "%d %s", tm.tm_mday, MESES[tm.tm_mon]);
    else snprintf(out, n, "%d %s %d", tm.tm_mday, MESES[tm.tm_mon], tm.tm_year + 1900);
}

static double now_s(void) { struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec + tv.tv_usec / 1e6; }

/* ---------- configuración (clave, modelo, precios) ---------- */
static void save_cfg(void)
{
    FILE *o = fopen(KEY_CFG, "w");
    if (!o) return;
    fprintf(o, "deepseek_key=%s\nmodel=%s\n", api_key, MODELS[model].id);
    for (int i = 0; i < NMODELS; i++)
        fprintf(o, "price.%s.hit=%g\nprice.%s.in=%g\nprice.%s.out=%g\n",
                MODELS[i].id, MODELS[i].hit, MODELS[i].id, MODELS[i].in, MODELS[i].id, MODELS[i].out);
    fclose(o);
}

static void load_key(void)
{
    char buf[512];
    FILE *f = fopen(KEY_CFG, "r");
    if (f) {
        while (fgets(buf, sizeof buf, f)) {
            trim_nl(buf);
            char *eq = strchr(buf, '=');
            if (!eq) continue;
            *eq = 0;
            const char *k = buf, *v = eq + 1;
            if (!strcmp(k, "deepseek_key")) snprintf(api_key, sizeof api_key, "%s", v);
            else if (!strcmp(k, "model")) { for (int i = 0; i < NMODELS; i++) if (!strcmp(v, MODELS[i].id)) model = i; }
            else if (!strncmp(k, "price.", 6)) {
                for (int i = 0; i < NMODELS; i++) {
                    size_t l = strlen(MODELS[i].id);
                    if (strncmp(k + 6, MODELS[i].id, l) || k[6 + l] != '.') continue;
                    const char *what = k + 7 + l;
                    double x = atof(v);
                    if (!strcmp(what, "hit")) MODELS[i].hit = x;
                    else if (!strcmp(what, "in")) MODELS[i].in = x;
                    else if (!strcmp(what, "out")) MODELS[i].out = x;
                }
            }
        }
        fclose(f);
    }
    /* clave dejada a mano en Chats/clave.txt (antes de que Cliply lo haga solo) */
    if ((f = fopen(KEY_DROP, "r"))) {
        if (fgets(buf, sizeof buf, f)) { trim_nl(buf); char *p = buf; while (*p == ' ') p++; if (*p) snprintf(api_key, sizeof api_key, "%s", p); }
        fclose(f);
        if (api_key[0]) { save_cfg(); remove(KEY_DROP); }
    }
}

/* Coste en dólares con el formato corto que se enseña: «0,0012 $». */
static void money(double usd, char *out, size_t n)
{
    if (usd <= 0) { snprintf(out, n, "0 $"); return; }
    int dec = usd >= 1 ? 2 : usd >= 0.01 ? 3 : 4;
    snprintf(out, n, "%.*f $", dec, usd);
    for (char *p = out; *p; p++) if (*p == '.') *p = ',';
    if (usd < 0.0001) snprintf(out, n, "< 0,0001 $");
}

/* Lo que cuesta una respuesta típica (1.500 tokens de entrada, 500 de salida). */
static double typical(const Model *m) { return price_factor() * (1500 * m->in + 500 * m->out) / 1e6; }

/* ---------- archivos de chat ---------- */
static int by_date(const void *a, const void *b)
{
    time_t x = ((const ChatInfo *)a)->mtime, y = ((const ChatInfo *)b)->mtime;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void scan_chats(void)
{
    mkdir(CHATS_DIR, 0755);
    nchats = 0;
    DIR *d = opendir(CHATS_DIR);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && nchats < MAX_CHATS) {
        size_t l = strlen(e->d_name);
        if (e->d_name[0] == '.' || l < 4 || strcmp(e->d_name + l - 3, ".md")) continue;
        ChatInfo *c = &chats[nchats];
        snprintf(c->path, sizeof c->path, CHATS_DIR "/%s", e->d_name);
        struct stat st;
        if (stat(c->path, &st)) continue;
        c->mtime = st.st_mtime;
        c->title[0] = c->preview[0] = 0;
        FILE *f = fopen(c->path, "r");
        char buf[2048];
        int in_ia = 0;
        while (f && fgets(buf, sizeof buf, f)) {
            trim_nl(buf);
            if (!strncmp(buf, "# ", 2) && !c->title[0]) snprintf(c->title, sizeof c->title, "%s", buf + 2);
            else if (!strcmp(buf, "### IA")) in_ia = 1;
            else if (!strcmp(buf, "### Tú")) in_ia = 0;
            else if (in_ia && buf[0]) {   /* vista previa: la última línea de la IA, sin marcas */
                size_t j = 0;
                char *p = buf;
                while (*p == '>' || *p == '-' || *p == ' ') p++;
                for (; *p && j + 1 < sizeof c->preview; p++) if (*p != '*' && *p != '#' && *p != '_') c->preview[j++] = *p;
                c->preview[j] = 0;
            }
        }
        if (f) fclose(f);
        if (!c->title[0]) snprintf(c->title, sizeof c->title, "Chat");
        nchats++;
    }
    closedir(d);
    qsort(chats, nchats, sizeof *chats, by_date);
}

static void free_msgs(void) { for (int i = 0; i < nmsgs; i++) free(msgs[i].text); nmsgs = 0; }

static void add_msg(int ia, const char *text)
{
    if (nmsgs >= MAX_MSGS) return;
    msgs[nmsgs].ia = ia;
    msgs[nmsgs].text = strdup(text);
    nmsgs++;
}

static void load_chat(const char *path)
{
    free_msgs();
    snprintf(cur_path, sizeof cur_path, "%s", path);
    cur_title[0] = 0;
    chat_cost = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char buf[4096];
    size_t cap = 0, len = 0;
    char *acc = NULL;
    int role = -1;
    while (fgets(buf, sizeof buf, f)) {
        int is_tu = !strncmp(buf, "### Tú", 7) && (buf[7] == '\n' || !buf[7]);
        int is_ia = !strncmp(buf, "### IA", 6) && (buf[6] == '\n' || !buf[6]);
        if (!cur_title[0] && !strncmp(buf, "# ", 2) && role < 0) { snprintf(cur_title, sizeof cur_title, "%s", buf + 2); trim_nl(cur_title); continue; }
        if (role < 0 && !strncmp(buf, "<!-- coste_usd=", 15)) { chat_cost = atof(buf + 15); continue; }
        if (is_tu || is_ia) {
            if (role >= 0 && acc) { while (len && (acc[len - 1] == '\n')) acc[--len] = 0; add_msg(role, acc); }
            role = is_ia; len = 0; if (acc) acc[0] = 0;
            continue;
        }
        if (role < 0) continue;
        size_t bl = strlen(buf);
        if (len + bl + 1 > cap) { cap = (len + bl + 1) * 2; acc = realloc(acc, cap); }
        if (len == 0 && (buf[0] == '\n')) continue;   /* línea en blanco tras el encabezado */
        memcpy(acc + len, buf, bl + 1);
        len += bl;
    }
    if (role >= 0 && acc) { while (len && acc[len - 1] == '\n') acc[--len] = 0; add_msg(role, acc); }
    free(acc);
    fclose(f);
}

static void save_chat(void)
{
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", cur_path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "# %s\n<!-- coste_usd=%.6f -->\n", cur_title, chat_cost);
    for (int i = 0; i < nmsgs; i++) fprintf(f, "\n### %s\n\n%s\n", msgs[i].ia ? "IA" : "Tú", msgs[i].text);
    fclose(f);
    rename(tmp, cur_path);
}

/* ---------- JSON mínimo ---------- */
static void json_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') { fputc('\\', f); fputc(*p, f); }
        else if (*p == '\n') fputs("\\n", f);
        else if (*p == '\r') fputs("\\r", f);
        else if (*p == '\t') fputs("\\t", f);
        else if (*p < 0x20) fprintf(f, "\\u%04x", *p);
        else fputc(*p, f);
    }
    fputc('"', f);
}

static void put_utf8(char **o, unsigned cp)
{
    char *q = *o;
    if (cp < 0x80) *q++ = cp;
    else if (cp < 0x800) { *q++ = 0xC0 | (cp >> 6); *q++ = 0x80 | (cp & 63); }
    else if (cp < 0x10000) { *q++ = 0xE0 | (cp >> 12); *q++ = 0x80 | ((cp >> 6) & 63); *q++ = 0x80 | (cp & 63); }
    else { *q++ = 0xF0 | (cp >> 18); *q++ = 0x80 | ((cp >> 12) & 63); *q++ = 0x80 | ((cp >> 6) & 63); *q++ = 0x80 | (cp & 63); }
    *o = q;
}

/* Busca "key":"..." en s y copia el valor ya desescapado en out. Devuelve 1 si lo encuentra. */
static int json_get(const char *s, const char *key, char *out, size_t n)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(s, pat);
    if (!p) return 0;
    p += strlen(pat);
    char *o = out, *end = out + n - 5;
    while (*p && *p != '"' && o < end) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': *o++ = '\n'; break;
            case 't': *o++ = '\t'; break;
            case 'r': break;
            case 'u': {
                unsigned cp = strtoul((char[]){ p[1], p[2], p[3], p[4], 0 }, NULL, 16);
                p += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && p[1] == '\\' && p[2] == 'u') {
                    unsigned lo = strtoul((char[]){ p[3], p[4], p[5], p[6], 0 }, NULL, 16);
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
                put_utf8(&o, cp);
                break;
            }
            default: *o++ = *p;
            }
            p++;
        } else *o++ = *p++;
    }
    *o = 0;
    return 1;
}

static double json_num(const char *s, const char *key)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(s, pat);
    return p ? atof(p + strlen(pat)) : 0;
}

/* ---------- pantalla del chat ---------- */
static MdCtx md;
static int body_top(void) { return TOP_BAR + 10; }
static int rows_on_page(void) { return (H - TOOLBAR - body_top()) / LH; }

static void setup_md(void)
{
    md = (MdCtx){ f_body, f_bold, f_ital, f_boldital, f_h1, f_h2, f_mono, M, W - M, body_top(), H - TOOLBAR, LH, BASE, scroll_row, SOFT };
}

/* Coloca todos los mensajes; con draw=1 los pinta. Devuelve el total de filas. */
static int layout_chat(int draw)
{
    setup_md();
    int row = 0;
    for (int i = 0; i < nmsgs; i++) {
        Msg *m = &msgs[i];
        m->row0 = row;
        if (draw && md_row_visible(&md, row)) {   /* etiqueta */
            SetFont(f_smallb, SOFT);
            DrawString(M, md_row_y(&md, row) + LH - BASE - f_smallb->baseline, m->ia ? "IA" : "TÚ");
            if (m->ia) { SetFont(f_small, SOFT); DrawTextRect(M, md_row_y(&md, row) + LH - BASE - f_small->baseline - 4, W - 2 * M, 40, "Guardar en el Cuaderno  ›", ALIGN_RIGHT); }
        }
        row++;
        MdState st = { 0, 0 };
        const char *p = m->text;
        char line[4096];
        if (!m->ia) { md.left = M + 30; }
        while (*p) {
            const char *nl = strchr(p, '\n');
            size_t l = nl ? (size_t)(nl - p) : strlen(p);
            if (l > sizeof line - 1) l = sizeof line - 1;
            memcpy(line, p, l);
            line[l] = 0;
            int r0 = row;
            if (!m->ia) {   /* lo tuyo, en cursiva gris y con una barra al lado */
                md.left = M + 30;
                row += md_flow(&md, line, M + 30, row, SOFT, 0, draw, f_ital);
                if (draw) for (int r = r0; r < row; r++) if (md_row_visible(&md, r)) FillArea(M + 4, md_row_y(&md, r) + 8, 6, LH - 8, PAPER_LINE);
            } else row += md_line(&md, &st, line, row, draw);
            p += l;
            if (*p == '\n') p++;
        }
        md.left = M;
        m->row1 = row;
        row++;   /* aire entre mensajes */
    }
    return row;
}

static void draw_toolbar(const char *main_label)
{
    int y = H - TOOLBAR;
    FillArea(0, y, W, TOOLBAR, WHITE);
    DrawLine(0, y, W, y, BLACK);
    for (int i = 0; i < 2; i++) DrawRect(M + i, y + 22 + i, W - 2 * M - 2 * i, TOOLBAR - 44 - 2 * i, BLACK);
    SetFont(f_head, SOFT);
    DrawTextRect(M + 30, y + 22, W - 2 * M - 60, TOOLBAR - 44, main_label, ALIGN_LEFT | VALIGN_MIDDLE);
}

static void draw_chat_body(const char *status)
{
    ClearScreen();
    SetFont(f_head, BLACK);
    DrawString(M - 20, 42, "‹ Chats");
    SetFont(f_small, SOFT);
    DrawTextRect(W / 3, 34, W / 3, 40, cur_title, ALIGN_CENTER | DOTS);
    char cost[80], mny[32];
    money(chat_cost, mny, sizeof mny);
    total_rows = layout_chat(0);
    int pages = (total_rows + rows_on_page() - 1) / rows_on_page();
    if (pages > 1)   /* la página va con el coste, debajo del título: así no choca con «Borrar» */
        snprintf(cost, sizeof cost, "%s · %s · pág. %d de %d", MODELS[model].name, mny, scroll_row / rows_on_page() + 1, pages);
    else
        snprintf(cost, sizeof cost, "%s · %s", MODELS[model].name, mny);
    SetFont(f_small, SOFT);
    DrawTextRect(W / 4, 76, W / 2, 40, cost, ALIGN_CENTER);
    SetFont(f_head, BLACK);
    DrawString(W - M + 20 - StringWidth("Borrar"), 42, "Borrar");
    DrawLine(0, TOP_BAR, W, TOP_BAR, PAPER_LINE);
    layout_chat(1);
    draw_toolbar(status ? status : "Escribe tu pregunta…");
}

static void draw_chat(void) { draw_chat_body(NULL); refresh(); }

/* Lleva la vista al principio del último mensaje, alineado a página. */
static void scroll_to_msg(int i)
{
    total_rows = layout_chat(0);
    if (i < 0 || i >= nmsgs) return;
    int rp = rows_on_page();
    scroll_row = msgs[i].row0;
    if (scroll_row + rp > total_rows) scroll_row = total_rows > rp ? total_rows - rp : 0;
    if (scroll_row < 0) scroll_row = 0;
}

/* ---------- llamada a la API ---------- */
static void ask(void)
{
    if (!(QueryNetwork() & NET_CONNECTED)) {
        draw_chat_body("Conectando a la wifi…");
        SoftUpdate();
        if (NetConnect2(NULL, true) != 0 && !(QueryNetwork() & NET_CONNECTED)) {
            add_msg(1, "*No hay conexión. Activa la wifi y vuelve a preguntar.*");
            save_chat();
            draw_chat();
            return;
        }
    }
    FILE *f = fopen("/tmp/ia-peticion.json", "w");
    FILE *h = fopen("/tmp/ia-cabecera.txt", "w");
    if (!f || !h) { if (f) fclose(f); if (h) fclose(h); return; }
    fprintf(h, "Authorization: Bearer %s\nContent-Type: application/json\n", api_key);
    fclose(h);
    fprintf(f, "{\"model\":\"%s\",\"stream\":true,\"stream_options\":{\"include_usage\":true},\"thinking\":{\"type\":\"disabled\"},\"messages\":[{\"role\":\"system\",\"content\":", MODELS[model].id);
    json_str(f, SYSTEM_PROMPT);
    fputs("}", f);
    for (int i = 0; i < nmsgs; i++) {
        fprintf(f, ",{\"role\":\"%s\",\"content\":", msgs[i].ia ? "assistant" : "user");
        json_str(f, msgs[i].text);
        fputs("}", f);
    }
    fputs("]}", f);
    fclose(f);

    add_msg(1, "");
    int me = nmsgs - 1;
    scroll_to_msg(me - 1);
    draw_chat_body("Pensando…");
    SoftUpdate();

    FILE *p = popen("curl -sN -m 180 -H @/tmp/ia-cabecera.txt -d @/tmp/ia-peticion.json " API_URL " 2>&1", "r");
    if (!p) return;
    size_t cap = 4096, len = 0;
    char *acc = malloc(cap);
    acc[0] = 0;
    char line[16384], piece[16384], raw[2048] = "";
    double last = now_s();
    while (fgets(line, sizeof line, p)) {
        if (!strncmp(line, "data: ", 6)) {
            if (!strncmp(line + 6, "[DONE]", 6)) break;
            const char *u = strstr(line + 6, "\"usage\":{");
            if (u) {   /* último trozo: tokens gastados */
                double hit = json_num(u, "prompt_cache_hit_tokens"), miss = json_num(u, "prompt_cache_miss_tokens");
                double pin = json_num(u, "prompt_tokens"), outk = json_num(u, "completion_tokens");
                if (hit + miss <= 0) miss = pin;
                const Model *m = &MODELS[model];
                chat_cost += price_factor() * (hit * m->hit + miss * m->in + outk * m->out) / 1e6;
            }
            if (json_get(line + 6, "content", piece, sizeof piece)) {
                size_t pl = strlen(piece);
                if (len + pl + 1 > cap) { cap = (len + pl + 1) * 2; acc = realloc(acc, cap); }
                memcpy(acc + len, piece, pl + 1);
                len += pl;
            }
        } else if (strlen(raw) + strlen(line) < sizeof raw - 1) strcat(raw, line);   /* error u otra cosa */
        if (now_s() - last > 1.5 && len) {   /* ir enseñando lo que llega */
            free(msgs[me].text);
            msgs[me].text = strdup(acc);
            int rp = rows_on_page();
            total_rows = layout_chat(0);
            if (total_rows - scroll_row > rp && msgs[me - 1].row0 + rp < total_rows) { /* la pregunta deja de caber: se mira la respuesta */
                if (scroll_row < msgs[me].row0) scroll_row = msgs[me].row0;
            }
            draw_chat_body("Escribiendo…");
            SoftUpdate();
            last = now_s();
        }
    }
    pclose(p);
    free(msgs[me].text);
    if (len) msgs[me].text = acc;
    else {
        char msg[1024] = "";
        if (!json_get(raw, "message", msg, sizeof msg)) snprintf(msg, sizeof msg, "%s", raw[0] ? raw : "sin respuesta");
        char out[1200];
        snprintf(out, sizeof out, "*No se pudo obtener respuesta: %s*", msg);
        msgs[me].text = strdup(out);
        free(acc);
    }
    remove("/tmp/ia-cabecera.txt");
    save_chat();
    scroll_to_msg(me);
    draw_chat();
}

/* ---------- lista de chats ---------- */
#define ROW_H 150
#define LIST_TOP 230
static int list_rows(void) { return (H - LIST_TOP - 40) / ROW_H; }

static void draw_list(void)
{
    ClearScreen();
    SetFont(f_title, BLACK);
    DrawString(M, 60, "Chats");
    DrawTextRect(W - M - 120, 40, 120, 100, "+", ALIGN_RIGHT | VALIGN_MIDDLE);
    char cnt[200], mny[32];
    money(typical(&MODELS[model]), mny, sizeof mny);
    snprintf(cnt, sizeof cnt, "Modelo %s · ≈ %s por respuesta%s  ›", MODELS[model].name, mny, peak_now() ? " (hora punta)" : "");
    SetFont(f_small, SOFT);
    DrawString(M, 150, cnt);
    DrawLine(M, LIST_TOP - 20, W - M, LIST_TOP - 20, PAPER_LINE);
    if (!nchats) {
        SetFont(f_body, SOFT);
        DrawTextRect(M, LIST_TOP + 200, W - 2 * M, 200, "Toca + para empezar a preguntar.\nMantén pulsado un chat para borrarlo.", ALIGN_CENTER);
    }
    for (int k = 0; k < list_rows() && list_scroll + k < nchats; k++) {
        ChatInfo *c = &chats[list_scroll + k];
        int y = LIST_TOP + k * ROW_H;
        char ds[40];
        date_str(c->mtime, ds, sizeof ds);
        SetFont(f_bold, BLACK);
        DrawTextRect(M, y + 10, W - 2 * M, 50, c->title, ALIGN_LEFT | DOTS);
        SetFont(f_small, SOFT);
        int dw = StringWidth(ds);
        DrawString(M, y + 72, ds);
        DrawTextRect(M + dw + 24, y + 72, W - 2 * M - dw - 24, 44, c->preview, ALIGN_LEFT | DOTS);
        DrawLine(M, y + ROW_H - 10, W - M, y + ROW_H - 10, PAPER_LINE);
    }
    refresh();
}

static void draw_nokey(void)
{
    ClearScreen();
    SetFont(f_title, BLACK);
    DrawString(M, 60, "IA");
    SetFont(f_body, BLACK);
    DrawTextRect(M, 260, W - 2 * M, 600,
                 "Para usar el chat necesitas una clave de DeepSeek (platform.deepseek.com).\n\n"
                 "La forma fácil: conecta el lector al ordenador y deja un archivo «clave.txt» con la clave dentro de la carpeta Chats.\n\n"
                 "O toca aquí para escribirla.", ALIGN_LEFT);
    refresh();
}

/* ---------- acciones ---------- */
static void back_to_list(void) { screen = LIST; scan_chats(); draw_list(); }

static void open_chat(const char *path)
{
    load_chat(path);
    screen = CHAT;
    scroll_to_msg(nmsgs - 1);
    draw_chat();
}

static void on_question(char *s)
{
    if (!s || !s[0]) { if (screen == CHAT) draw_chat(); else draw_list(); return; }
    if (screen != CHAT) {   /* chat nuevo */
        free_msgs();
        snprintf(cur_path, sizeof cur_path, CHATS_DIR "/%ld.md", (long)time(NULL));
        snprintf(cur_title, sizeof cur_title, "%.60s%s", s, strlen(s) > 60 ? "…" : "");
        screen = CHAT;
    }
    add_msg(0, s);
    save_chat();
    ask();
}

static void on_key(char *s)
{
    if (s && s[0]) { snprintf(api_key, sizeof api_key, "%s", s); save_cfg(); }
    if (api_key[0]) back_to_list(); else draw_nokey();
}

static int pending_save = -1;
static int long_pressed;
static void on_save_answer(int button)
{
    if (button == 1 && pending_save >= 0) {
        mkdir(NOTES_DIR, 0755);
        char path[512];
        snprintf(path, sizeof path, NOTES_DIR "/%ld.md", (long)time(NULL));
        FILE *f = fopen(path, "w");
        if (f) {
            const char *q = pending_save > 0 && !msgs[pending_save - 1].ia ? msgs[pending_save - 1].text : cur_title;
            char title[200];
            snprintf(title, sizeof title, "%.80s", q);
            for (char *t = title; *t; t++) if (*t == '\n') *t = ' ';
            fprintf(f, "# %s\n%s\n", title, msgs[pending_save].text);
            fclose(f);
            Message(ICON_INFORMATION, "Cuaderno", "Respuesta guardada como nota.", 2000);
        }
    }
    pending_save = -1;
    draw_chat();
}

static void page(int dir);
static char delete_path[512];
static void on_delete(int button)
{
    if (button == 1 && delete_path[0]) { remove(delete_path); delete_path[0] = 0; back_to_list(); return; }
    delete_path[0] = 0;
    if (screen == CHAT) draw_chat(); else draw_list();
}

static void ask_delete(const char *path, const char *title)
{
    static char text[400];
    snprintf(delete_path, sizeof delete_path, "%s", path);
    snprintf(text, sizeof text, "¿Borrar «%.120s»? No se puede deshacer.", title);
    Dialog(ICON_QUESTION, "Borrar chat", text, "Borrar", "Cancelar", on_delete);
}

static void chat_tap(int x, int y)
{
    if (y < TOP_BAR) { if (x < W / 3) back_to_list(); else if (x > W - M - 240) ask_delete(cur_path, cur_title); return; }
    if (y > H - TOOLBAR) { kbd[0] = 0; OpenKeyboard("Pregunta", kbd, sizeof kbd - 1, KBD_NORMAL | KBD_NOEVTSHOW, on_question); return; }
    setup_md();
    int row = scroll_row + (y - body_top()) / LH;
    /* solo la etiqueta «Guardar en el Cuaderno ›» guarda; el resto de la página pasa hojas */
    for (int i = 0; i < nmsgs; i++)
        if (msgs[i].ia && row == msgs[i].row0 && msgs[i].text[0] && x > W / 2) {
            pending_save = i;
            Dialog(ICON_QUESTION, "Cuaderno", "¿Guardar esta respuesta como nota en el Cuaderno?", "Guardar", "Cancelar", on_save_answer);
            return;
        }
    page(x > W / 2 ? 1 : -1);
}

static void on_model(int button)
{
    if (button >= 1 && button <= NMODELS) { model = button - 1; save_cfg(); }
    draw_list();
}

static void choose_model(void)
{
    static char text[600];
    char a[32], b[32];
    money(typical(&MODELS[0]), a, sizeof a);
    money(typical(&MODELS[1]), b, sizeof b);
    snprintf(text, sizeof text,
             "%s: %s. ≈ %s por respuesta.\n(%.2f $ / %.2f $ por millón de tokens de entrada / salida)\n\n"
             "%s: %s. ≈ %s por respuesta.\n(%.2f $ / %.2f $ por millón de tokens)\n\n%s",
             MODELS[0].name, MODELS[0].desc, a, MODELS[0].in * price_factor(), MODELS[0].out * price_factor(),
             MODELS[1].name, MODELS[1].desc, b, MODELS[1].in * price_factor(), MODELS[1].out * price_factor(),
             peak_now() ? "Ahora es hora punta: a otras horas cuesta la mitad." : "Ahora es hora valle (precio a mitad). En hora punta, el doble.");
    Dialog(ICON_QUESTION, "Modelo", text, MODELS[0].name, MODELS[1].name, on_model);
}

static void list_tap(int x, int y)
{
    if (y > 120 && y < LIST_TOP - 20 && x < W - M - 160) { choose_model(); return; }
    if (y < LIST_TOP - 20) {
        if (x > W - M - 160) { kbd[0] = 0; OpenKeyboard("Nueva pregunta", kbd, sizeof kbd - 1, KBD_NORMAL | KBD_NOEVTSHOW, on_question); }
        return;
    }
    int k = (y - LIST_TOP) / ROW_H;
    if (k >= 0 && list_scroll + k < nchats) open_chat(chats[list_scroll + k].path);
}

static void page(int dir)
{
    if (screen == CHAT) {
        int rp = rows_on_page();
        if (dir > 0 && scroll_row + rp < total_rows) scroll_row += rp;
        else if (dir < 0 && scroll_row > 0) scroll_row = scroll_row > rp ? scroll_row - rp : 0;
        else return;
        draw_chat();
    } else if (screen == LIST) {
        if (dir > 0 && list_scroll + list_rows() < nchats) list_scroll += list_rows();
        else if (dir < 0 && list_scroll > 0) list_scroll = list_scroll > list_rows() ? list_scroll - list_rows() : 0;
        else return;
        draw_list();
    }
}

static char *auto_question;
static void run_auto(void) { static char q[2048]; snprintf(q, sizeof q, "%s", auto_question); auto_question = NULL; on_question(q); }

static int handler(int type, int par1, int par2)
{
    switch (type) {
    case EVT_INIT:
        SetPanelType(PANEL_DISABLED);   /* sin barra de estado: si no, el primer pintado sale desplazado */
        W = ScreenWidth(); H = ScreenHeight();
        f_title = OpenFont(DEFAULTFONTB, 64, 1);
        f_head = OpenFont(DEFAULTFONT, 38, 1);
        f_h1 = OpenFont(DEFAULTFONTB, 46, 1);
        f_h2 = OpenFont(DEFAULTFONTB, 38, 1);
        f_body = OpenFont(DEFAULTFONT, 34, 1);
        f_bold = OpenFont(DEFAULTFONTB, 34, 1);
        f_ital = OpenFont(DEFAULTFONTI, 34, 1);
        f_boldital = OpenFont(DEFAULTFONTBI, 34, 1);
        f_small = OpenFont(DEFAULTFONT, 28, 1);
        f_smallb = OpenFont(DEFAULTFONTB, 28, 1);
        f_mono = OpenFont(DEFAULTFONTM, 30, 1);
        mkdir(CHATS_DIR, 0755);
        load_key();
        if (!api_key[0] && screen == LIST) screen = NOKEY;
        scan_chats();
        break;
    case EVT_SHOW:
        if (auto_question && api_key[0]) { screen = LIST; SetWeakTimer("ia_auto", run_auto, 500); }
        if (screen == CHAT) draw_chat(); else if (screen == NOKEY) draw_nokey(); else draw_list();
        break;
    case EVT_POINTERLONG:
        if (screen == LIST && par2 >= LIST_TOP) {
            int k = (par2 - LIST_TOP) / ROW_H;
            if (list_scroll + k < nchats) { long_pressed = 1; ask_delete(chats[list_scroll + k].path, chats[list_scroll + k].title); }
        }
        return 1;
    case EVT_POINTERUP:
        if (long_pressed) { long_pressed = 0; return 1; }
        if (screen == CHAT) chat_tap(par1, par2);
        else if (screen == NOKEY) { kbd[0] = 0; OpenKeyboard("Clave de DeepSeek", kbd, 200, KBD_ENTEXT | KBD_NOEVTSHOW, on_key); }
        else list_tap(par1, par2);
        return 1;
    case EVT_KEYPRESS:
        if (par1 == IV_KEY_BACK || par1 == IV_KEY_HOME || par1 == IV_KEY_MENU) {
            if (screen == CHAT) back_to_list(); else CloseApp();
            return 1;
        }
        if (par1 == IV_KEY_NEXT || par1 == IV_KEY_NEXT2 || par1 == IV_KEY_DOWN || par1 == IV_KEY_RIGHT) { page(1); return 1; }
        if (par1 == IV_KEY_PREV || par1 == IV_KEY_PREV2 || par1 == IV_KEY_UP || par1 == IV_KEY_LEFT) { page(-1); return 1; }
        break;
    case EVT_EXIT:
        free_msgs();
        break;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc > 2 && !strcmp(argv[1], "--preguntar")) { auto_question = argv[2]; argc = 1; }
    /* Para las capturas del emulador: «ia ruta.md» abre ese chat directamente. */
    if (argc > 1) { screen = CHAT; load_chat(argv[1]); scroll_row = 0; }
    InkViewMain(handler);
    return 0;
}
