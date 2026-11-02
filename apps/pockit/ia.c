/*
 * IA: Pockit AI, el chat con el agente de Pockit (agent.c), al estilo de Claude o ChatGPT.
 *
 * Por fuera es un chat con historial; por dentro, cada pregunta puede usar herramientas
 * (notas, tareas, libros, estadísticas, Wikipedia, internet). Lo que hace se ve en el chat
 * como pasos en gris («» Buscado «Gregor» en «La metamorfosis»: 4 resultados»); si un paso
 * cambió una nota, se puede tocar para deshacerlo.
 *
 * Cada chat es un archivo .md en Chats/ ("### Tú" / "### IA" separan los mensajes), así
 * que se pueden leer en el ordenador. «Uso y ajustes» enseña el modelo, la clave, el gasto
 * y los últimos errores.
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
#include "pockit.h"
#include "../../common/ai.h"
/* en Pockit, «cerrar» un módulo es volver al Inicio */
#define CloseApp pockit_home

#define CHATS_DIR FLASHDIR "/Chats"
#define NOTES_DIR FLASHDIR "/Notas"

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
static double chat_cost;   /* dólares gastados en el chat abierto */

typedef struct { char path[512]; char title[256]; char preview[256]; time_t mtime; } ChatInfo;
static ChatInfo chats[MAX_CHATS];
static int nchats, list_scroll;

typedef struct { int ia; char *text; int row0, row1; } Msg;
static Msg msgs[MAX_MSGS];
static int nmsgs;
static char cur_path[512], cur_title[256];
static int scroll_row, total_rows;

static enum { LIST, CHAT, NOKEY, USAGE } screen = LIST;
static int shown_once;
static char kbd[4096];

static const char *MESES[] = { "ene", "feb", "mar", "abr", "may", "jun", "jul", "ago", "sep", "oct", "nov", "dic" };

/* Preguntas de un toque: escribir en tinta electrónica cuesta */
static const char *IDEAS[] = {
    "¿Qué tareas tengo pendientes?",
    "Recuérdame qué ha pasado hasta ahora en el libro",
    "Repasa mis subrayados de esta semana",
    "¿Cómo voy de lectura este mes?",
};
#define NIDEAS 4

static void refresh(void) { POCKIT_REFRESH(shown_once); }
static void trim_nl(char *s) { size_t n = strlen(s); while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) s[--n] = 0; }
static int is_step(const char *s) { return !strncmp(s, "» ", 3); }

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

/* Lo que cuesta una consulta típica al agente: el prefijo fijo (~4.000 tokens) casi siempre
 * en caché, ~1.500 tokens nuevos y ~400 de respuesta. */
static double typical(int m)
{
    const AiModel *x = &AI_MODELS[m];
    return ai_factor_of(m) * (4000 * x->hit + 1500 * x->in + 400 * x->out) / 1e6;
}

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
            else if (in_ia && buf[0] && !is_step(buf) && strncmp(buf, "<!--", 4)) {   /* vista previa: la última línea de la IA, sin marcas */
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

/* ---------- pantalla del chat ---------- */
static MdCtx md;
static int body_top(void) { return TOP_BAR + 10; }
static int rows_on_page(void) { return (H - TOOLBAR - body_top()) / LH; }

/* Pasos que se pueden deshacer, para los toques: fila, mensaje y dónde empieza la línea */
typedef struct { int row, msg; long off; } UndoHit;
static UndoHit undo_hits[64];
static int nundo;

static void setup_md(void)
{
    md = (MdCtx){ f_body, f_bold, f_ital, f_boldital, f_h1, f_h2, f_mono, M, W - M, body_top(), H - TOOLBAR, LH, BASE, scroll_row, SOFT };
}

/* Un paso del agente: gris y pequeño; «Deshacer» a la derecha si cambió una nota. */
static int layout_step(int i, const char *line, long off, int row, int draw)
{
    char shown[600];
    const char *cm = strstr(line, " <!--");
    int undoable = cm && strstr(cm, "deshacer:");
    snprintf(shown, sizeof shown, "%.*s", (int)(cm ? cm - line : (long)strlen(line)), line);
    if (undoable && nundo < 64) undo_hits[nundo++] = (UndoHit){ row, i, off };
    int right = undoable ? 220 : 0;
    MdCtx save = md;
    md.right = W - M - right;
    int rows = md_flow(&md, shown, M, row, SOFT, 0, draw, f_small);
    md = save;
    if (draw && undoable && md_row_visible(&md, row)) {
        SetFont(f_smallb, BLACK);
        DrawTextRect(W - M - right, md_row_y(&md, row) + LH - BASE - f_smallb->baseline - 4, right, 40, "Deshacer", ALIGN_RIGHT);
    }
    return rows;
}

/* Coloca todos los mensajes; con draw=1 los pinta. Devuelve el total de filas. */
static int layout_chat(int draw)
{
    setup_md();
    nundo = 0;
    int row = 0;
    for (int i = 0; i < nmsgs; i++) {
        Msg *m = &msgs[i];
        m->row0 = row;
        if (draw && md_row_visible(&md, row)) {   /* etiqueta */
            SetFont(f_smallb, SOFT);
            DrawString(M, md_row_y(&md, row) + LH - BASE - f_smallb->baseline, m->ia ? "POCKIT AI" : "TÚ");
            if (m->ia && m->text[0]) { SetFont(f_small, SOFT); DrawTextRect(M, md_row_y(&md, row) + LH - BASE - f_small->baseline - 4, W - 2 * M, 40, "Guardar en el Cuaderno  ›", ALIGN_RIGHT); }
        }
        row++;
        MdState st = { 0, 0 };
        const char *p = m->text;
        char line[4096];
        while (*p) {
            const char *nl = strchr(p, '\n');
            size_t l = nl ? (size_t)(nl - p) : strlen(p);
            const char *start = p;
            if (l > sizeof line - 1) l = sizeof line - 1;
            memcpy(line, p, l);
            line[l] = 0;
            int r0 = row;
            if (!m->ia) {   /* lo tuyo, en cursiva gris y con una barra al lado */
                md.left = M + 30;
                row += md_flow(&md, line, M + 30, row, SOFT, 0, draw, f_ital);
                if (draw) for (int r = r0; r < row; r++) if (md_row_visible(&md, r)) FillArea(M + 4, md_row_y(&md, r) + 8, 6, LH - 8, PAPER_LINE);
            } else if (is_step(line)) row += layout_step(i, line, start - m->text, row, draw);
            else row += md_line(&md, &st, line, row, draw);
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
    DrawTextRect(M + 30, y + 22, W - 2 * M - 60, TOOLBAR - 44, main_label, ALIGN_LEFT | VALIGN_MIDDLE | DOTS);
}

static void draw_chat_body(const char *status)
{
    ClearScreen();
    SetFont(f_head, BLACK);
    DrawString(M - 20, 42, "‹ Chats");
    SetFont(f_small, SOFT);
    DrawTextRect(W / 3, 34, W / 3, 40, cur_title, ALIGN_CENTER | DOTS);
    char cost[120], mny[32];
    ai_money(chat_cost, mny, sizeof mny);
    total_rows = layout_chat(0);
    int pages = (total_rows + rows_on_page() - 1) / rows_on_page();
    if (pages > 1)   /* la página va con el coste, debajo del título: así no choca con «Borrar» */
        snprintf(cost, sizeof cost, "%s · %s · pág. %d de %d", AI_MODELS[ai_model].name, mny, scroll_row / rows_on_page() + 1, pages);
    else
        snprintf(cost, sizeof cost, "%s · %s", AI_MODELS[ai_model].name, mny);
    SetFont(f_small, SOFT);
    DrawTextRect(W / 4, 76, W / 2, 40, cost, ALIGN_CENTER);
    SetFont(f_head, BLACK);
    DrawString(W - M + 20 - StringWidth("Borrar"), 42, "Borrar");
    DrawLine(0, TOP_BAR, W, TOP_BAR, PAPER_LINE);
    layout_chat(1);
    draw_toolbar(status ? status : "Pídele algo a Pockit AI…");
}

static void draw_chat(void) { draw_chat_body(NULL); refresh(); }

/* Lleva la vista al principio del mensaje i; si es largo, a su última página. */
static void scroll_to_msg(int i)
{
    total_rows = layout_chat(0);
    if (i < 0 || i >= nmsgs) return;
    int rp = rows_on_page();
    scroll_row = msgs[i].row0;
    if (scroll_row + rp > total_rows) scroll_row = total_rows > rp ? total_rows - rp : 0;
    if (scroll_row < 0) scroll_row = 0;
}

/* ---------- un turno con el agente ---------- */
typedef struct { int me; char *steps, *text; double last; const char *status; } Live;

static void live_show(Live *lv, int force)
{
    if (!force && now_s() - lv->last < 1.5) return;
    JBuf b = { 0 };
    if (lv->steps && lv->steps[0]) jb_cat(&b, lv->steps);
    if (lv->text && lv->text[0]) { if (b.len) jb_cat(&b, "\n"); jb_cat(&b, lv->text); }
    free(msgs[lv->me].text);
    msgs[lv->me].text = b.s ? b.s : strdup("");
    int rp = rows_on_page();
    total_rows = layout_chat(0);
    if (total_rows - scroll_row > rp) {   /* lo nuevo no cabe: se pasa a la página siguiente */
        int want = total_rows - rp;
        if (want > scroll_row) scroll_row = want < msgs[lv->me].row0 ? want : msgs[lv->me].row0 > scroll_row ? msgs[lv->me].row0 : want;
        if (total_rows - scroll_row > rp) scroll_row = total_rows - rp;
    }
    draw_chat_body(lv->status);
    SoftUpdate();
    lv->last = now_s();
}

static void on_step(const char *steps, void *ctx)
{
    Live *lv = ctx;
    free(lv->steps);
    lv->steps = strdup(steps);
    free(lv->text);   /* lo que escribió antes de usar herramientas ya no es la respuesta */
    lv->text = NULL;
    lv->status = "Trabajando…";
    live_show(lv, 1);
}

static void on_text(const char *text, void *ctx)
{
    Live *lv = ctx;
    free(lv->text);
    lv->text = strdup(text);
    lv->status = "Escribiendo…";
    live_show(lv, 0);
}

static void ask(void)
{
    add_msg(1, "");
    int me = nmsgs - 1;
    scroll_to_msg(me - 1);
    if (!(QueryNetwork() & NET_CONNECTED)) { draw_chat_body("Conectando a la wifi…"); SoftUpdate(); }
    else { draw_chat_body("Pensando…"); SoftUpdate(); }
    static int roles[MAX_MSGS];
    static char *texts[MAX_MSGS];
    for (int i = 0; i < me; i++) { roles[i] = msgs[i].ia; texts[i] = msgs[i].text; }
    Live lv = { me, NULL, NULL, 0, "Pensando…" };
    AgentUI ui = { on_step, on_text, &lv };
    AgentOut out;
    int ok = agent_turn(me, roles, texts, &ui, &out);
    chat_cost += out.cost;
    JBuf b = { 0 };
    if (out.steps && out.steps[0]) jb_cat(&b, out.steps);
    if (ok) { if (b.len) jb_cat(&b, "\n"); jb_cat(&b, out.text && out.text[0] ? out.text : "*(sin respuesta)*"); }
    else {   /* nada de fallos silenciosos: qué pasó y qué hacer */
        if (b.len) jb_cat(&b, "\n");
        jb_cat(&b, "**No se pudo completar.** ");
        jb_cat(&b, out.err[0] ? out.err : "Error desconocido.");
    }
    free(msgs[me].text);
    msgs[me].text = b.s;
    free(lv.steps); free(lv.text);
    agent_out_free(&out);
    save_chat();
    scroll_to_msg(me);
    draw_chat();
}

/* ---------- lista de chats ---------- */
#define ROW_H 150
static int ideas_top(void) { return 205; }
static int idea_h(void) { return 110; }
static int list_top(void) { return ideas_top() + 2 * idea_h() + 50; }
static int list_rows(void) { return (H - list_top() - 40) / ROW_H; }

static void draw_list(void)
{
    ClearScreen();
    SetFont(f_small, SOFT);
    DrawString(M - 20, 10, "‹ Pockit");
    SetFont(f_title, BLACK);
    DrawString(M, 50, "Pockit AI");
    DrawTextRect(W - M - 120, 30, 120, 100, "+", ALIGN_RIGHT | VALIGN_MIDDLE);
    char cnt[200], mny[32];
    ai_money(typical(ai_model), mny, sizeof mny);
    snprintf(cnt, sizeof cnt, "%s · ≈ %s por consulta · Uso y ajustes  ›", AI_MODELS[ai_model].name, mny);
    SetFont(f_small, SOFT);
    DrawTextRect(M, 135, W - 2 * M, 40, cnt, ALIGN_LEFT | DOTS);
    /* ideas: 2 × 2 */
    int gw = (W - 2 * M - 20) / 2;
    SetFont(f_small, BLACK);
    for (int k = 0; k < NIDEAS; k++) {
        int x = M + (k % 2) * (gw + 20), y = ideas_top() + (k / 2) * idea_h();
        DrawRect(x, y, gw, idea_h() - 16, PAPER_LINE);
        DrawRect(x + 1, y + 1, gw - 2, idea_h() - 18, PAPER_LINE);
        DrawTextRect(x + 20, y, gw - 40, idea_h() - 16, IDEAS[k], ALIGN_LEFT | VALIGN_MIDDLE);
    }
    DrawLine(M, list_top() - 20, W - M, list_top() - 20, PAPER_LINE);
    if (!nchats) {
        SetFont(f_body, SOFT);
        DrawTextRect(M, list_top() + 120, W - 2 * M, 300,
                     "Toca + o una idea para empezar.\n\nPockit AI puede mirar tus libros, subrayados, notas y tareas, apuntar cosas en el Cuaderno y buscar en internet.",
                     ALIGN_CENTER);
    }
    for (int k = 0; k < list_rows() && list_scroll + k < nchats; k++) {
        ChatInfo *c = &chats[list_scroll + k];
        int y = list_top() + k * ROW_H;
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
    SetFont(f_small, SOFT);
    DrawString(M - 20, 10, "‹ Pockit");
    SetFont(f_title, BLACK);
    DrawString(M, 60, "Pockit AI");
    SetFont(f_body, BLACK);
    DrawTextRect(M, 220, W - 2 * M, 900,
                 "Pockit AI es un asistente que mira tus libros, subrayados, notas y tareas, apunta cosas en el Cuaderno y busca en internet por ti.\n\n"
                 "Es opcional: todo Pockit funciona sin ella. Para activarla hace falta una clave de un proveedor de IA (por ejemplo, DeepSeek: platform.deepseek.com). "
                 "Cada consulta cuesta una fracción de céntimo y siempre ves lo que gastas.\n\n"
                 "La forma fácil: pockit.yous.dev → Tu lector → Apps → IA, pega la clave y guarda.\n\n"
                 "O toca aquí para escribirla.", ALIGN_LEFT);
    refresh();
}

/* ---------- uso y ajustes ---------- */
static int us_model_y, us_key_y;
#define US_ROW 110

static void draw_usage(void)
{
    ClearScreen();
    SetFont(f_head, BLACK);
    DrawString(M - 20, 42, "‹ Chats");
    SetFont(f_title, BLACK);
    DrawString(M, 110, "Uso y ajustes");
    int y = 230;
    char line[400], a[32], b[32];
    SetFont(f_smallb, SOFT);
    DrawString(M, y, "MODELO");
    y += 50;
    us_model_y = y;
    for (int i = 0; i < AI_NMODELS; i++) {
        int on = i == ai_model;
        ai_money(typical(i), a, sizeof a);
        DrawRect(M, y + 6, 40, 40, BLACK);
        if (on) FillArea(M + 9, y + 15, 22, 22, BLACK);
        SetFont(on ? f_bold : f_body, BLACK);
        snprintf(line, sizeof line, "%s · %s", AI_MODELS[i].name, AI_MODELS[i].desc);
        DrawTextRect(M + 64, y + 4, W - 2 * M - 64, 44, line, ALIGN_LEFT | DOTS);
        SetFont(f_small, SOFT);
        snprintf(line, sizeof line, "≈ %s por consulta%s", a, !AI_MODELS[i].custom && ai_peak() ? " (hora punta)" : "");
        DrawString(M + 64, y + 52, line);
        y += US_ROW;
    }
    y += 10;
    us_key_y = y;
    SetFont(f_body, BLACK);
    const char *k = ai_bearer();
    size_t kl = strlen(k);
    char prov[80];
    ai_provider(prov, sizeof prov);
    if (kl) snprintf(line, sizeof line, "Clave de %s: guardada (…%s)", prov, kl > 4 ? k + kl - 4 : k);
    else snprintf(line, sizeof line, "Clave de %s: sin configurar", prov);
    DrawString(M, y, line);
    SetFont(f_small, SOFT);
    DrawTextRect(M, y + 50, W - 2 * M, 40, AI_MODELS[ai_model].custom ? "Se cambia en pockit.yous.dev." : "Toca para cambiarla.", ALIGN_LEFT);
    y += 120;
    DrawLine(M, y, W - M, y, PAPER_LINE);
    y += 30;
    SetFont(f_smallb, SOFT);
    DrawString(M, y, "GASTO");
    y += 50;
    double mon, tot = ai_spent(&mon);
    ai_money(mon, a, sizeof a);
    ai_money(tot, b, sizeof b);
    snprintf(line, sizeof line, "Este mes: %s  ·  en total: %s", a, b);
    SetFont(f_body, BLACK);
    DrawString(M, y, line);
    y += 60;
    /* desglose del mes por concepto */
    struct { char what[40]; double usd; int n; } by[12];
    int nby = 0;
    time_t now = time(NULL);
    struct tm tn;
    localtime_r(&now, &tn);
    FILE *f = fopen(AI_SPENT, "r");
    char buf[512];
    while (f && fgets(buf, sizeof buf, f)) {
        trim_nl(buf);
        time_t t = atol(buf);
        char *t1 = strchr(buf, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (!t2) continue;
        struct tm tt;
        localtime_r(&t, &tt);
        if (tt.tm_year != tn.tm_year || tt.tm_mon != tn.tm_mon) continue;
        int j = 0;
        while (j < nby && strcmp(by[j].what, t2 + 1)) j++;
        if (j == nby) { if (nby == 12) continue; snprintf(by[nby].what, sizeof by[nby].what, "%s", t2 + 1); by[nby].usd = 0; by[nby].n = 0; nby++; }
        by[j].usd += atof(t1 + 1);
        by[j].n++;
    }
    if (f) fclose(f);
    SetFont(f_small, SOFT);
    for (int j = 0; j < nby && j < 6; j++) {
        ai_money(by[j].usd, a, sizeof a);
        snprintf(line, sizeof line, "%s: %s en %d consulta%s", by[j].what, a, by[j].n, by[j].n == 1 ? "" : "s");
        DrawString(M, y, line);
        y += 44;
    }
    y += 20;
    DrawLine(M, y, W - M, y, PAPER_LINE);
    y += 30;
    SetFont(f_smallb, SOFT);
    DrawString(M, y, "ÚLTIMOS ERRORES");
    y += 50;
    char errs[3][400];
    int ne = 0;
    if ((f = fopen(AI_ERRORS, "r"))) {   /* los tres últimos */
        while (fgets(buf, sizeof buf, f)) {
            trim_nl(buf);
            char *t1 = strchr(buf, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL, *t3 = t2 ? strchr(t2 + 1, '\t') : NULL;
            if (!t3) continue;
            char ds[40];
            date_str(atol(buf), ds, sizeof ds);
            if (ne == 3) { memmove(errs[0], errs[1], sizeof errs[0] * 2); ne = 2; }
            snprintf(errs[ne++], sizeof errs[0], "%s · %s", ds, t3 + 1);
        }
        fclose(f);
    }
    SetFont(f_small, ne ? BLACK : SOFT);
    if (!ne) { DrawString(M, y, "Ninguno."); y += 50; }
    for (int j = ne - 1; j >= 0 && y < H - 120; j--) {
        int h = TextRectHeight(W - 2 * M, errs[j], 0);
        if (h > 130) h = 130;
        DrawTextRect(M, y, W - 2 * M, h, errs[j], ALIGN_LEFT | DOTS);
        y += h + 16;
    }
    SetFont(f_small, SOFT);
    snprintf(line, sizeof line, "%d chat%s guardado%s en la carpeta Chats. Mantén pulsado uno en la lista para borrarlo.", nchats, nchats == 1 ? "" : "s", nchats == 1 ? "" : "s");
    DrawTextRect(M, H - 110, W - 2 * M, 90, line, ALIGN_LEFT);
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
        chat_cost = 0;
        screen = CHAT;
    }
    add_msg(0, s);
    save_chat();
    ask();
}

static void on_key(char *s)
{
    if (s && s[0]) { snprintf(ai_key, sizeof ai_key, "%s", s); ai_save_cfg(); }
    if (screen == USAGE) { draw_usage(); return; }
    if (ai_bearer()[0]) back_to_list(); else draw_nokey();
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
            fprintf(f, "# %s\n", title);
            for (const char *p = msgs[pending_save].text; *p; ) {   /* sin los pasos del agente */
                const char *nl = strchr(p, '\n');
                size_t l = nl ? (size_t)(nl - p) : strlen(p);
                if (!is_step(p)) fprintf(f, "%.*s\n", (int)l, p);
                p += l + (nl ? 1 : 0);
            }
            fclose(f);
            Message(ICON_INFORMATION, "Cuaderno", "Respuesta guardada como nota.", 2000);
        }
    }
    pending_save = -1;
    draw_chat();
}

static int pending_undo = -1;
static void on_undo(int button)
{
    if (button == 1 && pending_undo >= 0 && pending_undo < nundo) {
        UndoHit *u = &undo_hits[pending_undo];
        char *line = msgs[u->msg].text + u->off;
        if (agent_undo(line)) {   /* la línea queda como «» Deshecho: …», sin la copia */
            char *cm = strstr(line, " <!--"), *nl = strchr(line, '\n');
            JBuf b = { 0 };
            jb_put(&b, msgs[u->msg].text, u->off);
            jb_cat(&b, "» Deshecho: ");
            jb_put(&b, line + 3, (cm ? cm : nl ? nl : line + strlen(line)) - (line + 3));
            if (nl) jb_cat(&b, nl);
            free(msgs[u->msg].text);
            msgs[u->msg].text = b.s;
            save_chat();
            Message(ICON_INFORMATION, "Pockit AI", "Cambio deshecho.", 1500);
        } else Message(ICON_WARNING, "Pockit AI", "No se pudo deshacer: la copia ya no está.", 2500);
    }
    pending_undo = -1;
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
    if (y > H - TOOLBAR) { kbd[0] = 0; OpenKeyboard("Pockit AI", kbd, sizeof kbd - 1, KBD_NORMAL | KBD_NOEVTSHOW, on_question); return; }
    setup_md();
    int row = scroll_row + (y - body_top()) / LH;
    total_rows = layout_chat(0);   /* para tener las filas de los pasos */
    if (x > W - M - 260)
        for (int k = 0; k < nundo; k++)
            if (undo_hits[k].row == row) {
                pending_undo = k;
                Dialog(ICON_QUESTION, "Deshacer", "¿Deshacer este cambio en tu nota? Vuelve a como estaba antes.", "Deshacer", "Cancelar", on_undo);
                return;
            }
    /* solo la etiqueta «Guardar en el Cuaderno ›» guarda; el resto de la página pasa hojas */
    for (int i = 0; i < nmsgs; i++)
        if (msgs[i].ia && row == msgs[i].row0 && msgs[i].text[0] && x > W / 2) {
            pending_save = i;
            Dialog(ICON_QUESTION, "Cuaderno", "¿Guardar esta respuesta como nota en el Cuaderno?", "Guardar", "Cancelar", on_save_answer);
            return;
        }
    page(x > W / 2 ? 1 : -1);
}

static void usage_tap(int x, int y)
{
    if (y < 90 && x < W / 3) { back_to_list(); return; }
    if (y >= us_model_y && y < us_model_y + AI_NMODELS * US_ROW) {
        int m = (y - us_model_y) / US_ROW;
        if (m != ai_model) { ai_model = m; ai_save_cfg(); draw_usage(); }
        return;
    }
    if (y >= us_key_y - 10 && y < us_key_y + 110 && !AI_MODELS[ai_model].custom) {
        kbd[0] = 0;
        OpenKeyboard("Clave de DeepSeek", kbd, 200, KBD_ENTEXT | KBD_NOEVTSHOW, on_key);
    }
}

static void list_tap(int x, int y)
{
    if (y < 50 && x < W / 3) { CloseApp(); return; }
    if (y > 120 && y < ideas_top() - 10) { screen = USAGE; scan_chats(); draw_usage(); return; }
    if (y < ideas_top()) {
        if (x > W - M - 160) { kbd[0] = 0; OpenKeyboard("Nueva pregunta", kbd, sizeof kbd - 1, KBD_NORMAL | KBD_NOEVTSHOW, on_question); }
        return;
    }
    if (y < list_top() - 20) {
        int gw = (W - 2 * M - 20) / 2, col = x > M + gw + 10, rowi = (y - ideas_top()) / idea_h();
        int k = rowi * 2 + col;
        if (k >= 0 && k < NIDEAS) { static char q[200]; snprintf(q, sizeof q, "%s", IDEAS[k]); on_question(q); }
        return;
    }
    int k = (y - list_top()) / ROW_H;
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

static void draw_current(void)
{
    if (screen == CHAT) draw_chat(); else if (screen == NOKEY) draw_nokey(); else if (screen == USAGE) draw_usage(); else draw_list();
}

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
        ai_load_cfg();
        if (!ai_bearer()[0] && screen == LIST) screen = NOKEY;
        scan_chats();
        break;
    case EVT_SHOW:
        if (screen == NOKEY && ai_load_cfg()) screen = LIST;   /* la clave pudo llegar desde la web o los Ajustes */
        if (auto_question && ai_bearer()[0]) { screen = LIST; SetWeakTimer("ia_auto", run_auto, 500); }
        draw_current();
        break;
    case EVT_POINTERLONG:
        if (screen == LIST && par2 >= list_top()) {
            int k = (par2 - list_top()) / ROW_H;
            if (list_scroll + k < nchats) { long_pressed = 1; ask_delete(chats[list_scroll + k].path, chats[list_scroll + k].title); }
        }
        return 1;
    case EVT_POINTERUP:
        if (long_pressed) { long_pressed = 0; return 1; }
        if (screen == CHAT) chat_tap(par1, par2);
        else if (screen == USAGE) usage_tap(par1, par2);
        else if (screen == NOKEY && par2 < 50 && par1 < W / 3) CloseApp();
        else if (screen == NOKEY) { kbd[0] = 0; OpenKeyboard("Clave de DeepSeek", kbd, 200, KBD_ENTEXT | KBD_NOEVTSHOW, on_key); }
        else list_tap(par1, par2);
        return 1;
    case EVT_KEYPRESS:
        if (par1 == IV_KEY_BACK || par1 == IV_KEY_HOME || par1 == IV_KEY_MENU) {
            if (screen == CHAT || screen == USAGE) back_to_list(); else CloseApp();
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

void ia_args(int argc, char *argv[])
{
    if (argc > 2 && !strcmp(argv[1], "--preguntar")) { auto_question = argv[2]; argc = 1; }
    if (argc > 1 && !strcmp(argv[1], "--uso")) { screen = USAGE; return; }
    /* Para las capturas del emulador: «ia ruta.md» abre ese chat directamente. */
    if (argc > 1) { screen = CHAT; load_chat(argv[1]); scroll_row = 0; }
}

int ia_handler(int type, int par1, int par2) { return handler(type, par1, par2); }
