/*
 * tools.h: el catálogo de herramientas de Pockit AI (tools.c), que usa el agente (agent.c).
 *
 * Cada herramienta recibe sus argumentos ya leídos (JSON), escribe en out lo que verá el
 * modelo y en step lo que verá la persona («Buscado «Gregor» en «La metamorfosis»: 4
 * resultados»). Si cambia una nota, deja en undo «copia|nota» para poder deshacerlo.
 */
#ifndef TOOLS_H
#define TOOLS_H

#include "../../common/json.h"

typedef struct {
    const char *js;     /* argumentos */
    const JTok *t;
    int n;
    JBuf *out;          /* resultado para el modelo */
    char step[400];     /* resumen para la persona */
    char undo[1100];    /* «copia|nota» si cambió una nota («-|nota» si la creó) */
    double cost;        /* lo que gastó en IA la propia herramienta (resumir) */
    void (*doing)(void *ctx, const char *what);   /* progreso de una herramienta larga */
    void *doing_ctx;
} ToolCtx;

typedef struct {
    const char *name;
    const char *doing;    /* mientras trabaja: «Buscando en el libro» */
    const char *desc;     /* para el modelo */
    const char *params;   /* JSON Schema de los argumentos */
    void (*run)(ToolCtx *c);
} Tool;

extern const Tool TOOLS[];
extern const int NTOOLS;

#endif
