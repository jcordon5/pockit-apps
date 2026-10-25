/*
 * pockit.h: lo que comparten los módulos de la app Pockit.
 *
 * Pockit es una sola app con varios módulos (Inicio, Lectura, Cuaderno, IA, Fondos y
 * Ajustes). main.c tiene el manejador de eventos de InkView y se los pasa al módulo
 * activo. Cada módulo es un .c con su estado privado y expone:
 *   int  <mod>_handler(int type, int par1, int par2);
 *   void <mod>_args(int argc, char *argv[]);   (para abrirlo directamente en el emulador)
 * Cuando un módulo «se cierra» (botón Atrás en su pantalla principal) vuelve al Inicio.
 */
#ifndef POCKIT_H
#define POCKIT_H

#define POCKIT_VERSION "2.0.0"

enum { MOD_HOME, MOD_LECTURA, MOD_CUADERNO, MOD_IA, MOD_FONDOS, MOD_AJUSTES, MOD_COUNT };

void pockit_open(int mod);   /* cambia de módulo */
void pockit_home(void);      /* vuelve al Inicio */
extern int pockit_full_next; /* el siguiente pintado, con destello (al cambiar de módulo) */

/* Refresco común: con destello solo al entrar en un módulo; si no, sin destello. */
#define POCKIT_REFRESH(shown) do { if (!(shown) || pockit_full_next) { FullUpdate(); (shown) = 1; pockit_full_next = 0; } else SoftUpdate(); } while (0)

int lectura_handler(int, int, int);
int cuaderno_handler(int, int, int);
int ia_handler(int, int, int);
int fondos_handler(int, int, int);
int ajustes_handler(int, int, int);
void lectura_args(int, char **);
void cuaderno_args(int, char **);
void ia_args(int, char **);

/* Para el Inicio */
typedef struct { char title[300], author[200], cover[1200]; double progress; int page, pages; long opened; int ok; } PockitReading;
int lectura_now(PockitReading *out);        /* el libro a medias abierto más recientemente */
int lectura_week(int active[7]);            /* días con lectura de lunes a domingo; devuelve el de hoy */
void lectura_open_current(int sub);         /* abre la ficha del libro actual en una pestaña */
int cuaderno_tasks(char out[][200], int max); /* tareas pendientes de todas las notas */
void cuaderno_open_note(const char *path);  /* abre una nota concreta (los Apuntes de un libro) */

#endif
