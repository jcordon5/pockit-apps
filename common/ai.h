/*
 * ai.h: IA de Pockit (DeepSeek) compartida por todos los módulos.
 *
 *  - Configuración en system/config/ia.cfg (deepseek_key, model, price.<id>.hit/in/out).
 *    La escriben la web (pockit.yous.dev) y los Ajustes de Pockit.
 *  - Precios oficiales de hora punta; fuera de la hora punta cuestan la mitad.
 *  - Cada llamada apunta su coste en system/config/pockit/gasto.tsv, para los Ajustes.
 *  - ai_complete() hace una llamada corta y sin streaming, con el modo «pensar» apagado y
 *    un tope de tokens de salida: es lo que usan las funciones de lectura.
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

#define AI_CFG FLASHDIR "/system/config/ia.cfg"
#define AI_DROP FLASHDIR "/Chats/clave.txt"
#define POCKIT_DIR FLASHDIR "/system/config/pockit"
#define AI_SPENT POCKIT_DIR "/gasto.tsv"
#define AI_URL "https://api.deepseek.com/chat/completions"

typedef struct { const char *id, *name, *desc; double hit, in, out; } AiModel;
static AiModel AI_MODELS[] = {
    { "deepseek-flash", "Flash", "rápido y muy barato", 0.006, 0.3, 1.2 },
    { "deepseek-v4-pro", "Pro", "más listo, más caro", 0.044, 1.32, 3.96 },
};
#define AI_NMODELS (int)(sizeof AI_MODELS / sizeof *AI_MODELS)
static char ai_key[256];
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
static double ai_factor(void) { return ai_peak() ? 1.0 : 0.5; }

static void ai_save_cfg(void)
{
    FILE *o = fopen(AI_CFG, "w");
    if (!o) return;
    fprintf(o, "deepseek_key=%s\nmodel=%s\n", ai_key, AI_MODELS[ai_model].id);
    for (int i = 0; i < AI_NMODELS; i++)
        fprintf(o, "price.%s.hit=%g\nprice.%s.in=%g\nprice.%s.out=%g\n",
                AI_MODELS[i].id, AI_MODELS[i].hit, AI_MODELS[i].id, AI_MODELS[i].in, AI_MODELS[i].id, AI_MODELS[i].out);
    fclose(o);
}

/* Lee la configuración. Devuelve 1 si hay clave. */
static int ai_load_cfg(void)
{
    char buf[512];
    ai_key[0] = 0;
    FILE *f = fopen(AI_CFG, "r");
    if (f) {
        while (fgets(buf, sizeof buf, f)) {
            ai_trim(buf);
            char *eq = strchr(buf, '=');
            if (!eq) continue;
            *eq = 0;
            const char *k = buf, *v = eq + 1;
            if (!strcmp(k, "deepseek_key")) snprintf(ai_key, sizeof ai_key, "%s", v);
            else if (!strcmp(k, "model")) { for (int i = 0; i < AI_NMODELS; i++) if (!strcmp(v, AI_MODELS[i].id)) ai_model = i; }
            else if (!strncmp(k, "price.", 6))
                for (int i = 0; i < AI_NMODELS; i++) {
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
    if ((f = fopen(AI_DROP, "r"))) {   /* clave dejada a mano en Chats/clave.txt */
        if (fgets(buf, sizeof buf, f)) { ai_trim(buf); char *p = buf; while (*p == ' ') p++; if (*p) snprintf(ai_key, sizeof ai_key, "%s", p); }
        fclose(f);
        if (ai_key[0]) { ai_save_cfg(); remove(AI_DROP); }
    }
    return ai_key[0] != 0;
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

/* ---------- JSON mínimo ---------- */
static void ai_json_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p >= 0x80) {   /* solo UTF-8 válido: una letra partida haría fallar la petición entera */
            int n = (*p & 0xE0) == 0xC0 ? 1 : (*p & 0xF0) == 0xE0 ? 2 : (*p & 0xF8) == 0xF0 ? 3 : -1, ok = n > 0;
            for (int k = 1; ok && k <= n; k++) if ((p[k] & 0xC0) != 0x80) ok = 0;
            if (!ok) { fputc('?', f); continue; }
            fwrite(p, 1, n + 1, f);
            p += n;
            continue;
        }
        if (*p == '"' || *p == '\\') { fputc('\\', f); fputc(*p, f); }
        else if (*p == '\n') fputs("\\n", f);
        else if (*p == '\r') continue;
        else if (*p == '\t') fputs("\\t", f);
        else if (*p < 0x20) fprintf(f, "\\u%04x", *p);
        else fputc(*p, f);
    }
    fputc('"', f);
}

static void ai_put_utf8(char **o, unsigned cp)
{
    char *q = *o;
    if (cp < 0x80) *q++ = cp;
    else if (cp < 0x800) { *q++ = 0xC0 | (cp >> 6); *q++ = 0x80 | (cp & 63); }
    else if (cp < 0x10000) { *q++ = 0xE0 | (cp >> 12); *q++ = 0x80 | ((cp >> 6) & 63); *q++ = 0x80 | (cp & 63); }
    else { *q++ = 0xF0 | (cp >> 18); *q++ = 0x80 | ((cp >> 12) & 63); *q++ = 0x80 | ((cp >> 6) & 63); *q++ = 0x80 | (cp & 63); }
    *o = q;
}

/* Valor desescapado de "key":"…" (el primero que aparece). */
static int ai_json_get(const char *s, const char *key, char *out, size_t n)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(s, pat);
    if (!p) { snprintf(pat, sizeof pat, "\"%s\": \"", key); p = strstr(s, pat); }
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
                char h[5] = { p[1], p[2], p[3], p[4], 0 };
                unsigned cp = strtoul(h, NULL, 16);
                p += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && p[1] == '\\' && p[2] == 'u') {
                    char l[5] = { p[3], p[4], p[5], p[6], 0 };
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (strtoul(l, NULL, 16) - 0xDC00);
                    p += 6;
                }
                ai_put_utf8(&o, cp);
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

static double ai_json_num(const char *s, const char *key)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(s, pat);
    return p ? atof(p + strlen(pat)) : 0;
}

/* Llamada corta y sin streaming. Devuelve el texto (malloc) o NULL con el error en err.
 * what: concepto para el registro de gasto. */
static char *ai_complete(const char *system_prompt, const char *user, int max_tokens, const char *what, char *err, size_t errn)
{
    if (!ai_key[0] && !ai_load_cfg()) { snprintf(err, errn, "Falta la clave de DeepSeek."); return NULL; }
    if (!(QueryNetwork() & NET_CONNECTED) && NetConnect2(NULL, true) != 0 && !(QueryNetwork() & NET_CONNECTED)) {
        snprintf(err, errn, "No hay conexión. Activa la wifi y vuelve a intentarlo.");
        return NULL;
    }
    FILE *f = fopen("/tmp/pk-peticion.json", "w"), *h = fopen("/tmp/pk-cabecera.txt", "w");
    if (!f || !h) { if (f) fclose(f); if (h) fclose(h); snprintf(err, errn, "No se pudo preparar la petición."); return NULL; }
    fprintf(h, "Authorization: Bearer %s\nContent-Type: application/json\n", ai_key);
    fclose(h);
    /* el mensaje de sistema va primero y siempre igual: así DeepSeek lo cachea y cuesta mucho menos */
    fprintf(f, "{\"model\":\"%s\",\"stream\":false,\"max_tokens\":%d,\"temperature\":0.3,\"thinking\":{\"type\":\"disabled\"},"
               "\"messages\":[{\"role\":\"system\",\"content\":", AI_MODELS[ai_model].id, max_tokens);
    ai_json_str(f, system_prompt);
    fputs("},{\"role\":\"user\",\"content\":", f);
    ai_json_str(f, user);
    fputs("}]}", f);
    fclose(f);
    FILE *p = popen("curl -s -m 120 -H @/tmp/pk-cabecera.txt -d @/tmp/pk-peticion.json " AI_URL " 2>&1", "r");
    if (!p) { snprintf(err, errn, "No se pudo llamar a la IA."); return NULL; }
    size_t cap = 65536, len = 0;
    char *raw = malloc(cap);
    size_t r;
    while ((r = fread(raw + len, 1, cap - len - 1, p)) > 0) { len += r; if (len + 1 >= cap) { cap *= 2; raw = realloc(raw, cap); } }
    raw[len] = 0;
    pclose(p);
    remove("/tmp/pk-cabecera.txt");
    char *out = malloc(len + 16);
    if (!ai_json_get(raw, "content", out, len + 16) || !out[0]) {
        char msg[400] = "";
        if (!ai_json_get(raw, "message", msg, sizeof msg)) snprintf(msg, sizeof msg, "%.200s", raw[0] ? raw : "sin respuesta");
        snprintf(err, errn, "La IA no respondió: %s", msg);
        free(out); free(raw);
        return NULL;
    }
    const char *u = strstr(raw, "\"usage\"");
    if (u) {
        double hit = ai_json_num(u, "prompt_cache_hit_tokens"), miss = ai_json_num(u, "prompt_cache_miss_tokens");
        double in = ai_json_num(u, "prompt_tokens"), outk = ai_json_num(u, "completion_tokens");
        if (hit + miss <= 0) miss = in;
        const AiModel *m = &AI_MODELS[ai_model];
        ai_log_cost(ai_factor() * (hit * m->hit + miss * m->in + outk * m->out) / 1e6, what);
    }
    free(raw);
    return out;
}

#endif
