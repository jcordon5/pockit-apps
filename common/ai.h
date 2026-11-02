/*
 * ai.h: IA de Pockit compartida por todos los módulos.
 *
 *  - Habla con cualquier API compatible con OpenAI (/chat/completions). De serie, DeepSeek
 *    Flash y Pro; un tercer modelo opcional («custom.*» en ia.cfg) puede ser de otro
 *    proveedor, con su URL y su clave.
 *  - Configuración en system/config/ia.cfg (deepseek_key, model, price.<id>.hit/in/out,
 *    custom.name/url/key/model/hit/in/out). La escriben la web (pockit.yous.dev) y Pockit.
 *  - DeepSeek: precios oficiales de hora punta; fuera de ella cuestan la mitad.
 *  - Cada llamada apunta su coste en system/config/pockit/gasto.tsv y cada error en
 *    errores.tsv, para los Ajustes y la web.
 *  - ai_stream() hace una petición en streaming (texto y herramientas) y explica los errores;
 *    ai_complete() es la llamada corta de las funciones de lectura.
 *
 * Incluir en un solo .c por módulo; todo es static.
 */
#ifndef AI_H
#define AI_H

#include "inkview.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "json.h"

#define AI_CFG FLASHDIR "/system/config/ia.cfg"
#define AI_DROP FLASHDIR "/Chats/clave.txt"
#define POCKIT_DIR FLASHDIR "/system/config/pockit"
#define AI_SPENT POCKIT_DIR "/gasto.tsv"
#define AI_ERRORS POCKIT_DIR "/errores.tsv"
#define AI_DEEPSEEK "https://api.deepseek.com/chat/completions"

typedef struct { char id[80], name[48], desc[64], url[256]; double hit, in, out; int custom; } AiModel;
static AiModel AI_MODELS[3] = {
    { "deepseek-flash", "Flash", "rápido y muy barato", AI_DEEPSEEK, 0.006, 0.3, 1.2, 0 },
    { "deepseek-v4-pro", "Pro", "más listo, más caro", AI_DEEPSEEK, 0.044, 1.32, 3.96, 0 },
    { "", "", "otro proveedor", "", 0, 0, 0, 1 },
};
static int ai_nmodels = 2;   /* 3 si ia.cfg trae un modelo «custom» */
#define AI_NMODELS ai_nmodels
static char ai_key[256], ai_custom_key[256];
static int ai_model;

static void ai_trim(char *s) { size_t n = strlen(s); while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) s[--n] = 0; }

/* Hora punta de DeepSeek: 01-04 y 06-10 UTC, de lunes a viernes. */
static int ai_peak(void)
{
    time_t t = time(NULL);
    struct tm u;
    gmtime_r(&t, &u);
    if (u.tm_wday == 0 || u.tm_wday == 6) return 0;
    return (u.tm_hour >= 1 && u.tm_hour < 4) || (u.tm_hour >= 6 && u.tm_hour < 10);
}
/* Lo que se paga de la tarifa: la hora valle solo existe en DeepSeek. */
static double ai_factor_of(int m) { return AI_MODELS[m].custom ? 1.0 : ai_peak() ? 1.0 : 0.5; }
static double ai_factor(void) { return ai_factor_of(ai_model); }
static const char *ai_bearer(void) { return AI_MODELS[ai_model].custom ? ai_custom_key : ai_key; }
/* «DeepSeek» o el nombre del host del proveedor, para los mensajes de error */
static void ai_provider(char *out, size_t n)
{
    const char *u = AI_MODELS[ai_model].url, *h = strstr(u, "://");
    h = h ? h + 3 : u;
    if (strstr(h, "deepseek.com")) { snprintf(out, n, "DeepSeek"); return; }
    size_t l = strcspn(h, "/:");
    snprintf(out, n, "%.*s", (int)(l < n ? l : n - 1), h);
}

static void ai_save_cfg(void)
{
    FILE *o = fopen(AI_CFG, "w");
    if (!o) return;
    fprintf(o, "deepseek_key=%s\nmodel=%s\n", ai_key, AI_MODELS[ai_model].id);
    for (int i = 0; i < 2; i++)
        fprintf(o, "price.%s.hit=%g\nprice.%s.in=%g\nprice.%s.out=%g\n",
                AI_MODELS[i].id, AI_MODELS[i].hit, AI_MODELS[i].id, AI_MODELS[i].in, AI_MODELS[i].id, AI_MODELS[i].out);
    if (ai_nmodels > 2) {
        const AiModel *c = &AI_MODELS[2];
        fprintf(o, "custom.name=%s\ncustom.url=%s\ncustom.key=%s\ncustom.model=%s\ncustom.hit=%g\ncustom.in=%g\ncustom.out=%g\n",
                c->name, c->url, ai_custom_key, c->id, c->hit, c->in, c->out);
    }
    fclose(o);
}

/* Lee la configuración. Devuelve 1 si el modelo elegido tiene clave. */
static int ai_load_cfg(void)
{
    char buf[600], model_id[80] = "";
    ai_key[0] = ai_custom_key[0] = 0;
    AiModel *c = &AI_MODELS[2];
    c->id[0] = c->url[0] = 0;
    FILE *f = fopen(AI_CFG, "r");
    if (f) {
        while (fgets(buf, sizeof buf, f)) {
            ai_trim(buf);
            char *eq = strchr(buf, '=');
            if (!eq) continue;
            *eq = 0;
            const char *k = buf, *v = eq + 1;
            if (!strcmp(k, "deepseek_key")) snprintf(ai_key, sizeof ai_key, "%s", v);
            else if (!strcmp(k, "model")) snprintf(model_id, sizeof model_id, "%s", v);
            else if (!strcmp(k, "custom.name")) snprintf(c->name, sizeof c->name, "%s", v);
            else if (!strcmp(k, "custom.url")) snprintf(c->url, sizeof c->url, "%s", v);
            else if (!strcmp(k, "custom.key")) snprintf(ai_custom_key, sizeof ai_custom_key, "%s", v);
            else if (!strcmp(k, "custom.model")) snprintf(c->id, sizeof c->id, "%s", v);
            else if (!strcmp(k, "custom.hit")) c->hit = atof(v);
            else if (!strcmp(k, "custom.in")) c->in = atof(v);
            else if (!strcmp(k, "custom.out")) c->out = atof(v);
            else if (!strncmp(k, "price.", 6))
                for (int i = 0; i < 2; i++) {
                    size_t l = strlen(AI_MODELS[i].id);
                    if (strncmp(k + 6, AI_MODELS[i].id, l) || k[6 + l] != '.') continue;
                    const char *what = k + 7 + l;
                    double x = atof(v);
                    if (!strcmp(what, "hit")) AI_MODELS[i].hit = x;
                    else if (!strcmp(what, "in")) AI_MODELS[i].in = x;
                    else if (!strcmp(what, "out")) AI_MODELS[i].out = x;
                }
        }
        fclose(f);
    }
    ai_nmodels = c->id[0] && c->url[0] ? 3 : 2;
    if (ai_nmodels == 3 && !c->name[0]) snprintf(c->name, sizeof c->name, "%.40s", c->id);
    ai_model = 0;
    for (int i = 0; i < ai_nmodels; i++) if (!strcmp(model_id, AI_MODELS[i].id)) ai_model = i;
    if ((f = fopen(AI_DROP, "r"))) {   /* clave dejada a mano en Chats/clave.txt */
        if (fgets(buf, sizeof buf, f)) { ai_trim(buf); char *p = buf; while (*p == ' ') p++; if (*p) snprintf(ai_key, sizeof ai_key, "%s", p); }
        fclose(f);
        if (ai_key[0]) { ai_save_cfg(); remove(AI_DROP); }
    }
    return ai_bearer()[0] != 0;
}

/* «0,0012 $» */
static void ai_money(double usd, char *out, size_t n)
{
    if (usd <= 0) { snprintf(out, n, "0 $"); return; }
    if (usd < 0.0001) { snprintf(out, n, "< 0,0001 $"); return; }
    int dec = usd >= 1 ? 2 : usd >= 0.01 ? 3 : 4;
    snprintf(out, n, "%.*f $", dec, usd);
    for (char *p = out; *p; p++) if (*p == '.') *p = ',';
}

/* Coste aproximado de mandar `chars` caracteres y recibir `out_tokens` (español: ~4 caracteres por token). */
static double ai_estimate(long chars, int out_tokens)
{
    const AiModel *m = &AI_MODELS[ai_model];
    return ai_factor() * ((chars / 4.0) * m->in + out_tokens * m->out) / 1e6;
}

static void ai_log_cost(double usd, const char *what)
{
    if (usd <= 0) return;
    mkdir(POCKIT_DIR, 0755);
    FILE *f = fopen(AI_SPENT, "a");
    if (!f) return;
    fprintf(f, "%ld\t%.7f\t%s\n", (long)time(NULL), usd, what);
    fclose(f);
}

static void ai_log_error(const char *what, const char *msg)
{
    mkdir(POCKIT_DIR, 0755);
    FILE *f = fopen(AI_ERRORS, "a");
    if (!f) return;
    fprintf(f, "%ld\t%s\t%s\t", (long)time(NULL), what, AI_MODELS[ai_model].id);
    for (const char *p = msg; *p; p++) fputc(*p == '\n' || *p == '\t' ? ' ' : *p, f);
    fputc('\n', f);
    fclose(f);
}

/* Total gastado y gastado este mes, en dólares. */
static double ai_spent(double *month)
{
    double total = 0, mon = 0;
    time_t now = time(NULL);
    struct tm tn;
    localtime_r(&now, &tn);
    FILE *f = fopen(AI_SPENT, "r");
    char buf[512];
    while (f && fgets(buf, sizeof buf, f)) {
        time_t t = atol(buf);
        char *tab = strchr(buf, '\t');
        if (!tab) continue;
        double c = atof(tab + 1);
        total += c;
        struct tm tt;
        localtime_r(&t, &tt);
        if (tt.tm_year == tn.tm_year && tt.tm_mon == tn.tm_mon) mon += c;
    }
    if (f) fclose(f);
    if (month) *month = mon;
    return total;
}

/* ---------- petición ---------- */
#define AI_MAX_CALLS 8
typedef struct { char id[80], name[64]; JBuf args; } AiCall;
typedef struct {
    JBuf text;                    /* lo que ha escrito el modelo */
    AiCall calls[AI_MAX_CALLS];   /* herramientas que pide usar */
    int ncalls;
    double cost;
    long tok_in, tok_hit, tok_out;
    int http;
    char finish[24];
    char err[480];                /* vacío si fue bien; si no, qué pasó y qué hacer */
} AiResult;

static void ai_result_free(AiResult *r)
{
    free(r->text.s);
    for (int i = 0; i < r->ncalls; i++) free(r->calls[i].args.s);
    memset(r, 0, sizeof *r);
}

/* Principio del cuerpo de la petición: «{"model":…,"stream":true,…,». Lo demás lo pone quien llama. */
static void ai_body_begin(JBuf *b, int max_tokens, double temperature)
{
    char head[400];
    snprintf(head, sizeof head, "{\"model\":\"%s\",\"stream\":true,\"stream_options\":{\"include_usage\":true},\"temperature\":%.2f,",
             AI_MODELS[ai_model].id, temperature);
    jb_cat(b, head);
    if (max_tokens > 0) { snprintf(head, sizeof head, "\"max_tokens\":%d,", max_tokens); jb_cat(b, head); }
    if (!AI_MODELS[ai_model].custom) jb_cat(b, "\"thinking\":{\"type\":\"disabled\"},");   /* DeepSeek piensa por defecto */
}

/* Error de la API o de la red, explicado: qué pasó y qué hacer. */
static void ai_explain(AiResult *r, int curl_exit, const char *body)
{
    char prov[80], msg[300] = "";
    ai_provider(prov, sizeof prov);
    JTok t[64];
    int n = json_parse(body, (int)strlen(body), t, 64);
    if (n > 0) {
        int e = json_key(body, t, n, 0, "error");
        int m = e >= 0 && t[e].type == J_OBJ ? json_key(body, t, n, e, "message") : e;
        if (m < 0) m = json_key(body, t, n, 0, "message");
        if (m >= 0) json_str(body, &t[m], msg, sizeof msg);
    } else if (body[0] && body[0] != '<') snprintf(msg, sizeof msg, "%.200s", body);
    const char *model = AI_MODELS[ai_model].name;
    if (curl_exit == 6 || curl_exit == 7) snprintf(r->err, sizeof r->err, "No hay conexión con %s. Comprueba la wifi y vuelve a intentarlo.", prov);
    else if (curl_exit == 28) snprintf(r->err, sizeof r->err, "%s tardó demasiado en responder. Vuelve a intentarlo en un momento.", prov);
    else if (curl_exit == 35 || curl_exit == 60) snprintf(r->err, sizeof r->err, "No se pudo abrir una conexión segura con %s (error %d de curl). Revisa la fecha y hora del lector.", prov, curl_exit);
    else if (curl_exit) snprintf(r->err, sizeof r->err, "No se pudo hablar con %s (error %d de curl).", prov, curl_exit);
    else if (r->http == 401 || r->http == 403) snprintf(r->err, sizeof r->err, "%s no acepta la clave: es incorrecta o ha caducado. Cámbiala en «Uso y ajustes» o en pockit.yous.dev.", prov);
    else if (r->http == 402) snprintf(r->err, sizeof r->err, "Tu cuenta de %s no tiene saldo. Recarga en su web y vuelve a intentarlo.", prov);
    else if (r->http == 429) snprintf(r->err, sizeof r->err, "%s dice que hay demasiadas peticiones o que has llegado a tu límite. Espera un poco.%s%s", prov, msg[0] ? " Detalle: " : "", msg);
    else if (r->http == 404 || strstr(msg, "odel not") || strstr(msg, "does not exist")) snprintf(r->err, sizeof r->err, "%s no tiene el modelo «%s». Elige otro en «Uso y ajustes».", prov, model);
    else if (r->http == 400) snprintf(r->err, sizeof r->err, "%s rechazó la petición: %s", prov, msg[0] ? msg : "sin detalle");
    else if (r->http >= 500) snprintf(r->err, sizeof r->err, "El modelo %s no está disponible ahora mismo (error %d de %s). Prueba en un rato o cambia de modelo.", model, r->http, prov);
    else snprintf(r->err, sizeof r->err, "%s no respondió bien%s%s.", prov, msg[0] ? ": " : "", msg);
}

static int ai_online(char *err, size_t n)
{
    if (QueryNetwork() & NET_CONNECTED) return 1;
    if (NetConnect2(NULL, true) == 0 || (QueryNetwork() & NET_CONNECTED)) return 1;
    snprintf(err, n, "No hay conexión. Activa la wifi y vuelve a intentarlo.");
    return 0;
}

/* Manda el cuerpo (que acaba quien llama con «]}») y lee la respuesta en streaming.
 * on_text recibe todo el texto acumulado cada vez que llega más. what: concepto para el gasto.
 * Devuelve 1 si fue bien; si no, r->err explica qué pasó. */
static int ai_stream(JBuf *body, const char *what, void (*on_text)(const char *text, void *ctx), void *ctx, AiResult *r)
{
    memset(r, 0, sizeof *r);
    if (!ai_bearer()[0]) { snprintf(r->err, sizeof r->err, "Falta la clave de la IA. Ponla en pockit.yous.dev → Tu lector → Apps → IA."); return 0; }
    if (!ai_online(r->err, sizeof r->err)) return 0;
    FILE *f = fopen("/tmp/pk-peticion.json", "w"), *h = fopen("/tmp/pk-cabecera.txt", "w");
    if (!f || !h) { if (f) fclose(f); if (h) fclose(h); snprintf(r->err, sizeof r->err, "No se pudo preparar la petición."); return 0; }
    fprintf(h, "Authorization: Bearer %s\nContent-Type: application/json\n", ai_bearer());
    fclose(h);
    fwrite(body->s, 1, body->len, f);
    fclose(f);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "curl -sN -m 180 -w '\\n__HTTP__%%{http_code}\\n' -H @/tmp/pk-cabecera.txt -d @/tmp/pk-peticion.json '%s'", AI_MODELS[ai_model].url);
    FILE *p = popen(cmd, "r");
    if (!p) { remove("/tmp/pk-cabecera.txt"); snprintf(r->err, sizeof r->err, "No se pudo llamar a la IA."); return 0; }
    static char line[65536];
    static JTok t[1024];
    char other[2048] = "";
    double hit = 0, miss = 0, in = 0, outk = 0;
    int got_usage = 0;
    while (fgets(line, sizeof line, p)) {
        if (!strncmp(line, "__HTTP__", 8)) { r->http = atoi(line + 8); continue; }
        if (strncmp(line, "data: ", 6)) {   /* un error en JSON normal, o basura */
            if (strlen(other) + strlen(line) < sizeof other - 1) strcat(other, line);
            continue;
        }
        const char *js = line + 6;
        if (!strncmp(js, "[DONE]", 6)) continue;
        int n = json_parse(js, (int)strlen(js), t, 1024);
        if (n <= 0) continue;
        int u = json_key(js, t, n, 0, "usage");
        if (u >= 0 && t[u].type == J_OBJ) {
            got_usage = 1;
            in = json_num_key(js, t, n, u, "prompt_tokens");
            outk = json_num_key(js, t, n, u, "completion_tokens");
            int k = json_key(js, t, n, u, "prompt_cache_hit_tokens");   /* DeepSeek */
            if (k >= 0) { hit = json_num(js, &t[k]); miss = json_num_key(js, t, n, u, "prompt_cache_miss_tokens"); }
            else {   /* OpenAI y compatibles */
                int d = json_key(js, t, n, u, "prompt_tokens_details");
                int c = d >= 0 ? json_key(js, t, n, d, "cached_tokens") : -1;
                hit = c >= 0 ? json_num(js, &t[c]) : 0;
                miss = in - hit;
            }
        }
        int ch = json_at(t, n, json_key(js, t, n, 0, "choices"), 0);
        if (ch < 0) continue;
        int fr = json_key(js, t, n, ch, "finish_reason");
        if (fr >= 0 && t[fr].type == J_STR) json_str(js, &t[fr], r->finish, sizeof r->finish);
        int d = json_key(js, t, n, ch, "delta");
        if (d < 0) continue;
        int c = json_key(js, t, n, d, "content");
        if (c >= 0 && t[c].type == J_STR && t[c].end > t[c].start) {
            char *piece = json_strdup(js, &t[c]);
            jb_cat(&r->text, piece);
            free(piece);
            if (on_text) on_text(r->text.s, ctx);
        }
        int tc = json_key(js, t, n, d, "tool_calls");
        for (int k = 0; tc >= 0 && k < t[tc].size; k++) {
            int e = json_at(t, n, tc, k);
            int ix = json_key(js, t, n, e, "index");
            int idx = ix >= 0 ? (int)json_num(js, &t[ix]) : json_key(js, t, n, e, "id") >= 0 ? r->ncalls : r->ncalls - 1;
            if (idx < 0 || idx >= AI_MAX_CALLS) continue;
            if (idx >= r->ncalls) r->ncalls = idx + 1;
            AiCall *call = &r->calls[idx];
            int id = json_key(js, t, n, e, "id");
            if (id >= 0 && t[id].type == J_STR) json_str(js, &t[id], call->id, sizeof call->id);
            int fn = json_key(js, t, n, e, "function");
            if (fn < 0) continue;
            int nm = json_key(js, t, n, fn, "name");
            if (nm >= 0 && t[nm].type == J_STR) json_str(js, &t[nm], call->name, sizeof call->name);
            int ar = json_key(js, t, n, fn, "arguments");
            if (ar >= 0 && t[ar].type == J_STR) { char *piece = json_strdup(js, &t[ar]); jb_cat(&call->args, piece); free(piece); }
        }
    }
    int st = pclose(p);
    int curl_exit = WIFEXITED(st) ? WEXITSTATUS(st) : 0;
    remove("/tmp/pk-cabecera.txt");
    if (got_usage) {
        const AiModel *m = &AI_MODELS[ai_model];
        if (hit + miss <= 0) miss = in;
        r->tok_in = (long)in; r->tok_hit = (long)hit; r->tok_out = (long)outk;
        r->cost = ai_factor() * (hit * m->hit + miss * m->in + outk * m->out) / 1e6;
        ai_log_cost(r->cost, what);
    }
    for (int i = 0; i < r->ncalls; i++) if (!r->calls[i].args.s) jb_cat(&r->calls[i].args, "{}");
    if (curl_exit || (r->http && r->http != 200) || (!r->text.len && !r->ncalls)) {
        if (!curl_exit && r->http == 200 && !other[0]) snprintf(r->err, sizeof r->err, "La IA no devolvió nada. Vuelve a intentarlo.");
        else ai_explain(r, curl_exit, other);
        ai_log_error(what, r->err);
        return 0;
    }
    if (!r->text.s) jb_cat(&r->text, "");
    return 1;
}

/* Llamada corta sin herramientas. Devuelve el texto (malloc) o NULL con el error en err.
 * what: concepto para el registro de gasto. */
static char *ai_complete(const char *system_prompt, const char *user, int max_tokens, const char *what, char *err, size_t errn)
{
    if (!ai_bearer()[0] && !ai_load_cfg()) { snprintf(err, errn, "Falta la clave de la IA."); return NULL; }
    JBuf b = { 0 };
    ai_body_begin(&b, max_tokens, 0.3);
    /* el mensaje de sistema va primero y siempre igual: así se cachea y cuesta mucho menos */
    jb_cat(&b, "\"messages\":[{\"role\":\"system\",\"content\":");
    jb_str(&b, system_prompt);
    jb_cat(&b, "},{\"role\":\"user\",\"content\":");
    jb_str(&b, user);
    jb_cat(&b, "}]}");
    AiResult r;
    int ok = ai_stream(&b, what, NULL, NULL, &r);
    free(b.s);
    char *out = NULL;
    if (ok && r.text.len) { out = r.text.s; r.text.s = NULL; }
    else snprintf(err, errn, "%s", r.err[0] ? r.err : "La IA no respondió.");
    ai_result_free(&r);
    return out;
}

#endif
