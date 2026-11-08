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

#define POCKIT_VERSION "2.10.1"

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

/* Idioma (i18n.c): español o inglés, según el idioma del lector */
const char *T(const char *es);   /* el texto en el idioma del lector */
int pockit_en(void);             /* 1 si el lector no está en español */

/* «‹ Pockit» arriba a la izquierda, igual en todos los módulos (main.c). */
void pockit_back(void);
int pockit_back_hit(int x, int y);
ibitmap *pockit_icon(int mod);              /* el icono de un módulo, 120 × 120 (liberar con free) */

/* Tus datos (datos.c): Notas/, Chats/ y Libros/ en Pockit/ (o, hasta enlazar Pockit, en Dropbox PocketBook/Pockit/). */
const char *pockit_data(void);
void pockit_path(char *out, size_t n, const char *sub);   /* <datos>/<sub>, creada si no existe */
int pockit_dropbox(void);                                  /* 1 si tus datos siguen en la carpeta de Dropbox del lector */
int pockit_dropbox_ready(char *folder, size_t n);          /* 1 si Dropbox está conectado en el lector */

/* Sincronización con Dropbox (nube.c) */
int nube_linked(void);                                     /* 1 si Pockit está enlazado con Dropbox */
int nube_pending(void);                                    /* 1 si hay cambios en el lector sin subir */
int nube_sync(int interactive, void (*progress)(const char *msg), char *out, size_t n);   /* 1 si fue bien; out: cómo fue */
long nube_last(char *msg, size_t n);                       /* cuándo fue la última (0 si nunca) y cómo fue */
typedef struct { char name[300]; long size, mod; } NubeBook;   /* name: relativo a /Pockit/Biblioteca */
int nube_books(NubeBook *out, int max);                    /* tu biblioteca en Dropbox, según la última sincronización */
int nube_book_get(const char *name, char *err, size_t n);  /* lo baja a Pockit/Biblioteca */
int nube_book_delete(const char *name, char *err, size_t n);   /* lo borra de Dropbox (no del lector) */
void nube_unlink(void);
void pockit_book_dir(const char *title, char *out, size_t n);       /* Libros/<etiqueta>/ de un libro */
void pockit_book_adopt(const char *title, const char *cache_dir);   /* trae lo guardado por la 2.1 */
void pockit_tag(const char *title, char *out, size_t n);  /* etiqueta de un libro, sin # */
int pockit_tags(const char *text, char tags[][48], int max);         /* las #etiquetas de un texto */
int pockit_tag_matches(const char *tag, const char *book_tag);      /* ¿esta etiqueta es de ese libro? */
void pockit_fold(const char *s, char *out, size_t n);     /* sin mayúsculas ni tildes */
unsigned pockit_fold_cp(unsigned c);
int pockit_u8(const unsigned char *s, unsigned *cp);

/* Para el Inicio */
typedef struct { char title[300], author[200], cover[1200]; double progress; int page, pages; long opened; int ok; } PockitReading;
int lectura_now(PockitReading *out);        /* el libro a medias abierto más recientemente */
int lectura_week(int active[7]);            /* días con lectura de lunes a domingo; devuelve el de hoy */
void lectura_open_current(void);            /* abre la ficha del libro que lees */
int pockit_books_missing(int notify);       /* libros de Pockit/Biblioteca que la biblioteca del lector aún no tiene */
int pockit_books_index(void (*progress)(const char *msg));   /* tras sincronizar; 1 si hay que repintar */
void pockit_books_scan(void);               /* al cerrar: que el lector busque los que falten */
void pockit_marks_merge(void);              /* rehace subrayados.md con los de todos tus lectores */
void cuaderno_open_note(const char *path);  /* abre una nota concreta */
void cuaderno_open_tag(const char *tag);    /* abre el Cuaderno filtrado por una etiqueta */

/* Para Pockit AI: los datos de Lectura, de solo lectura. i va de 0 hasta que devuelven 0.
 * Los libros van del abierto más recientemente al más antiguo; las marcas, de la más nueva a la más vieja. */
typedef struct {
    const char *title, *author, *path, *position;
    double progress;   /* de 0 a 1; -1 si no se sabe */
    int done, page, pages, marks, words;
    long added, opened, completed_ts;
    int gone;          /* ya no está en el lector: queda su ficha */
} PockitBook;
typedef struct {
    const char *text, *note, *book, *context;   /* subrayado o palabra; nota o definición; libro; frase donde estaba la palabra */
    long ts;
    int word;    /* 1 si es una palabra del diccionario */
    char state;  /* palabras: 0 por aprender, 'a' aprendida, 'q' quitada */
} PockitMark;
int lectura_book(int i, PockitBook *out);
int lectura_mark(int i, PockitMark *out);

/* Pockit AI (agent.c): un turno de conversación con herramientas. */
typedef struct {
    void (*on_step)(const char *steps, void *ctx);  /* todas las líneas «» …» hasta ahora, la última quizá en marcha */
    void (*on_text)(const char *text, void *ctx);   /* la respuesta según llega */
    void *ctx;
} AgentUI;
typedef struct {
    char *steps;    /* líneas «» …» de lo que hizo (malloc) */
    char *text;     /* respuesta final (malloc), o NULL si falló */
    char err[480];  /* qué falló y qué hacer */
    double cost;
} AgentOut;
/* roles[i]: 0 tú, 1 IA. Los textos de la IA pueden traer líneas «» »: se quitan antes de mandarlas. */
int agent_turn(int n, const int *roles, char *const *texts, const AgentUI *ui, AgentOut *out);
void agent_out_free(AgentOut *o);
/* Deshace el cambio de un paso «» …<!-- deshacer:copia|nota -->». 1 si lo deshizo. */
int agent_undo(const char *step_line);

#endif
