/*
 * json.h: lector de JSON por tokens, sin memoria dinámica (al estilo de jsmn).
 *
 *   JTok t[256];
 *   int n = json_parse(s, len, t, 256);          // n tokens, o -1 si no es JSON válido
 *   int i = json_key(s, t, n, 0, "choices");     // índice del valor de la clave en el objeto 0
 *   json_str(s, &t[i], buf, sizeof buf);         // cadena desescapada (UTF-8)
 *
 * Cada token guarda dónde empieza y acaba en el texto y cuántos hijos tiene (claves de un
 * objeto o elementos de una lista). json_next() salta un valor entero con todos sus hijos.
 *
 * Incluir en un solo .c por módulo; todo es static.
 */
#ifndef JSON_H
#define JSON_H

#include <stdlib.h>
#include <string.h>

enum { J_NONE, J_OBJ, J_ARR, J_STR, J_PRIM };
typedef struct { int type, start, end, size, parent; } JTok;

static int json_parse(const char *s, int len, JTok *t, int max)
{
    int n = 0, up = -1;
    for (int p = 0; p < len; p++) {
        char c = s[p];
        switch (c) {
        case '{': case '[':
            if (n >= max) return -1;
            if (up >= 0) t[up].size++;
            t[n] = (JTok){ c == '{' ? J_OBJ : J_ARR, p, -1, 0, up };
            up = n++;
            break;
        case '}': case ']':
            if (up < 0 || t[up].type != (c == '}' ? J_OBJ : J_ARR)) return -1;
            t[up].end = p + 1;
            up = t[up].parent;
            /* tras «"clave":valor», el padre de la clave es el objeto: volver a él */
            if (up >= 0 && t[up].type == J_STR) up = t[up].parent;
            break;
        case '"': {
            int a = ++p;
            for (; p < len && s[p] != '"'; p++) if (s[p] == '\\') p++;
            if (p >= len || n >= max) return -1;
            if (up >= 0) t[up].size++;
            t[n] = (JTok){ J_STR, a, p, 0, up };
            /* una cadena seguida de «:» es una clave: su valor cuelga de ella */
            int q = p + 1;
            while (q < len && (s[q] == ' ' || s[q] == '\n' || s[q] == '\r' || s[q] == '\t')) q++;
            if (q < len && s[q] == ':' && up >= 0 && t[up].type == J_OBJ) { up = n; p = q; }
            else if (up >= 0 && t[up].type == J_STR) up = t[up].parent;
            n++;
            break;
        }
        case ' ': case '\t': case '\n': case '\r': case ',': case ':':
            break;
        default: {
            int a = p;
            while (p < len && s[p] != ',' && s[p] != '}' && s[p] != ']' && s[p] != ' ' && s[p] != '\n' && s[p] != '\r' && s[p] != '\t') p++;
            if (n >= max) return -1;
            if (up >= 0) t[up].size++;
            t[n] = (JTok){ J_PRIM, a, p, 0, up };
            n++;
            p--;
            if (up >= 0 && t[up].type == J_STR) up = t[up].parent;
        }
        }
    }
    return up == -1 ? n : -1;
}

/* Índice del token que sigue a t[i] y todos sus hijos. */
static int json_next(const JTok *t, int n, int i)
{
    int end = t[i].type == J_OBJ || t[i].type == J_ARR ? t[i].end : t[i].end;
    int j = i + 1;
    if (t[i].type == J_STR && t[i].size) {   /* una clave: saltar también su valor */
        j = json_next(t, n, i + 1);
        return j;
    }
    while (j < n && t[j].start < end) j++;
    return j;
}

static int json_eq(const char *s, const JTok *k, const char *key)
{
    int l = (int)strlen(key);
    return k->type == J_STR && k->end - k->start == l && !strncmp(s + k->start, key, l);
}

/* Valor de la clave key en el objeto t[obj] (índice del token), o -1. */
static int json_key(const char *s, const JTok *t, int n, int obj, const char *key)
{
    if (obj < 0 || obj >= n || t[obj].type != J_OBJ) return -1;
    int j = obj + 1;
    for (int k = 0; k < t[obj].size && j < n; k++) {
        if (json_eq(s, &t[j], key)) return j + 1 < n ? j + 1 : -1;
        j = json_next(t, n, j);
    }
    return -1;
}

/* Elemento k de la lista t[arr], o -1. */
static int json_at(const JTok *t, int n, int arr, int k)
{
    if (arr < 0 || arr >= n || t[arr].type != J_ARR || k >= t[arr].size) return -1;
    int j = arr + 1;
    while (k-- > 0 && j < n) j = json_next(t, n, j);
    return j < n ? j : -1;
}

static void json_utf8(char **o, unsigned cp)
{
    char *q = *o;
    if (cp < 0x80) *q++ = cp;
    else if (cp < 0x800) { *q++ = 0xC0 | (cp >> 6); *q++ = 0x80 | (cp & 63); }
    else if (cp < 0x10000) { *q++ = 0xE0 | (cp >> 12); *q++ = 0x80 | ((cp >> 6) & 63); *q++ = 0x80 | (cp & 63); }
    else { *q++ = 0xF0 | (cp >> 18); *q++ = 0x80 | ((cp >> 12) & 63); *q++ = 0x80 | ((cp >> 6) & 63); *q++ = 0x80 | (cp & 63); }
    *o = q;
}

/* Copia la cadena (desescapada) o el primitivo de t en out. Devuelve la longitud. */
static int json_str(const char *s, const JTok *t, char *out, size_t n)
{
    if (!n) return 0;
    char *o = out, *end = out + n - 5;
    if (!t || t->type == J_NONE) { *out = 0; return 0; }
    if (t->type != J_STR) {   /* número, true, null… u objeto entero tal cual */
        int l = t->end - t->start;
        if (l > (int)n - 1) l = n - 1;
        memcpy(out, s + t->start, l);
        out[l] = 0;
        return l;
    }
    for (int p = t->start; p < t->end && o < end; p++) {
        if (s[p] != '\\') { *o++ = s[p]; continue; }
        p++;
        switch (s[p]) {
        case 'n': *o++ = '\n'; break;
        case 't': *o++ = '\t'; break;
        case 'r': break;
        case 'b': case 'f': break;
        case 'u': {
            char h[5] = { s[p + 1], s[p + 2], s[p + 3], s[p + 4], 0 };
            unsigned cp = strtoul(h, NULL, 16);
            p += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && s[p + 1] == '\\' && s[p + 2] == 'u') {
                char l[5] = { s[p + 3], s[p + 4], s[p + 5], s[p + 6], 0 };
                cp = 0x10000 + ((cp - 0xD800) << 10) + (strtoul(l, NULL, 16) - 0xDC00);
                p += 6;
            }
            json_utf8(&o, cp);
            break;
        }
        default: *o++ = s[p];
        }
    }
    *o = 0;
    return (int)(o - out);
}

/* Igual, pero en memoria nueva (malloc) del tamaño justo. */
static char *json_strdup(const char *s, const JTok *t)
{
    int cap = t ? t->end - t->start + 8 : 8;
    char *out = malloc(cap);
    json_str(s, t, out, cap);
    return out;
}

static double json_num(const char *s, const JTok *t) { return t && t->type == J_PRIM ? atof(s + t->start) : 0; }
static int json_true(const char *s, const JTok *t) { return t && t->type == J_PRIM && s[t->start] == 't'; }
static double json_num_key(const char *s, const JTok *t, int n, int obj, const char *key) { int k = json_key(s, t, n, obj, key); return k >= 0 ? json_num(s, &t[k]) : 0; }

/* ---------- escribir ---------- */
/* Búfer que crece, para montar peticiones. */
typedef struct { char *s; long len, cap; } JBuf;

static void jb_put(JBuf *b, const char *s, long n)
{
    if (b->len + n + 1 > b->cap) { b->cap = (b->len + n + 1) * 2 + 1024; b->s = realloc(b->s, b->cap); }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}
static void jb_cat(JBuf *b, const char *s) { jb_put(b, s, (long)strlen(s)); }

/* Cadena JSON escapada; solo deja pasar UTF-8 válido (una letra partida haría fallar la petición). */
static void jb_str(JBuf *b, const char *s)
{
    jb_put(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p >= 0x80) {
            int k = (*p & 0xE0) == 0xC0 ? 1 : (*p & 0xF0) == 0xE0 ? 2 : (*p & 0xF8) == 0xF0 ? 3 : -1, ok = k > 0;
            for (int i = 1; ok && i <= k; i++) if ((p[i] & 0xC0) != 0x80) ok = 0;
            if (!ok) { jb_put(b, "?", 1); continue; }
            jb_put(b, (const char *)p, k + 1);
            p += k;
            continue;
        }
        char e[8];
        if (*p == '"' || *p == '\\') { e[0] = '\\'; e[1] = *p; jb_put(b, e, 2); }
        else if (*p == '\n') jb_put(b, "\\n", 2);
        else if (*p == '\r') continue;
        else if (*p == '\t') jb_put(b, "\\t", 2);
        else if (*p < 0x20) { snprintf(e, sizeof e, "\\u%04x", *p); jb_put(b, e, 6); }
        else jb_put(b, (const char *)p, 1);
    }
    jb_put(b, "\"", 1);
}

#endif
