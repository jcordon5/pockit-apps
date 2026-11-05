/*
 * i18n.c: Pockit en el idioma del lector. El texto se escribe en español en el código y T("…")
 * lo devuelve en inglés si el lector no está en español (Ajustes del lector → Idioma).
 * La tabla está en en.h; la clave es el texto en español tal cual, con sus %d y %s, que en
 * inglés van en el mismo orden.
 *
 * Lo que es formato de datos (los «### Tú» de los chats, las claves de ficha.ini, las etiquetas)
 * no pasa por aquí: tiene que leerse igual en cualquier idioma, en el lector y en la web.
 */
#include "inkview.h"
#include <stdlib.h>
#include <string.h>
#include "pockit.h"

static const char *const EN[][2] = {
#include "en.h"
};
#define NEN ((int)(sizeof EN / sizeof EN[0]))
#define SLOTS 4096   /* potencia de 2, más del doble de las entradas */

static int en = -1;
static short slot[SLOTS];   /* índice + 1 en EN, 0 = libre */

int pockit_en(void)
{
    if (en < 0) {
        const char *l = getenv("PK_LANG");   /* para el emulador */
        if (!l || !*l) l = currentLang();
        en = !(l && l[0] == 'e' && l[1] == 's');
    }
    return en;
}

static unsigned hash(const char *s) { unsigned h = 2166136261u; for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; } return h; }

const char *T(const char *es)
{
    if (!es || !pockit_en()) return es;
    static int built;
    if (!built) {
        for (int i = 0; i < NEN; i++) {
            unsigned h = hash(EN[i][0]) & (SLOTS - 1);
            while (slot[h]) h = (h + 1) & (SLOTS - 1);
            slot[h] = (short)(i + 1);
        }
        built = 1;
    }
    for (unsigned h = hash(es) & (SLOTS - 1); slot[h]; h = (h + 1) & (SLOTS - 1))
        if (!strcmp(EN[slot[h] - 1][0], es)) return EN[slot[h] - 1][1];
    return es;   /* sin traducción: mejor en español que vacío */
}
