# Pockit AI

El agente de Pockit. Vive en el módulo **IA**: por fuera es un chat con historial, por dentro un
agente con herramientas que lee y escribe en el lector (notas, tareas, libros, estadísticas) y,
si se le pide, consulta internet. El resto de Pockit no tiene botones de IA «sueltos» nuevos: sin
clave, Pockit funciona entero y la única diferencia es que el chat no está disponible.

## Principios

- **Transparente.** Cada paso que da el agente se ve en el chat («» Buscando «Gregor» en
  *La metamorfosis*… 4 resultados»). Los cambios en tus notas se pueden deshacer tocando el paso.
- **Sin fallos silenciosos.** Clave mala, sin saldo, modelo caído, sin wifi o tiempo agotado se
  explican con su causa y qué hacer. Los errores se apuntan en `system/config/pockit/errores.tsv`.
- **Gasto a la vista.** Cada chat lleva su coste; el total y el del mes, en *Uso y ajustes*.
- **Sin destripes.** Las herramientas de libros solo leen hasta donde vas, salvo que lo pidas.
- **Barato en un lector.** Prompt de sistema y herramientas fijos (se cachean), el contexto que
  cambia va al final, los resultados se recortan y el historial solo guarda texto, no resultados.

## Arquitectura

```
ia.c          chat (lista, conversación, uso y ajustes)       UI e-ink
agent.c       bucle del agente: petición → herramientas → …   máx. 8 pasos por turno
tools.c       catálogo de herramientas y su implementación
common/ai.h   proveedor OpenAI-compatible: streaming, tool calls, coste, errores
common/json.h lector de JSON por tokens (sin memoria dinámica)
```

`ai.h` habla con cualquier API compatible con OpenAI (`/chat/completions` con `tools`). De serie,
DeepSeek (Flash y Pro). Un tercer modelo opcional se define en `ia.cfg`:

```
custom.name=Qwen Flash
custom.url=https://…/v1/chat/completions
custom.key=…
custom.model=qwen3.7-flash
custom.hit=0.006
custom.in=0.03
custom.out=0.13
```

## Herramientas

| Herramienta | Qué hace | Escribe |
|---|---|---|
| `list_books` | Libros leyendo / terminados / todos, con progreso | |
| `reading_stats` | Días de lectura, racha, libros terminados, subrayados y palabras en N días | |
| `book_chapters` | Capítulos o relatos de un libro, cuáles has leído y por cuál vas | |
| `search_book` | Busca un texto (un personaje, una frase) en lo leído y da fragmentos con su capítulo | |
| `read_book` | Lee un capítulo o relato (o un trozo) | |
| `book_memory` | Lo que Pockit ya sabe del libro: resúmenes de «Dónde voy» y personajes | |
| `list_highlights` | Subrayados y notas del lector | |
| `list_words` | Palabras consultadas en el diccionario, con definición y si están aprendidas | |
| `list_notes` / `read_note` / `search_notes` | Cuaderno | |
| `create_note` / `edit_note` | Crear o cambiar notas (añadir, insertar tras un título, reemplazar) | ✎ |
| `list_tasks` / `add_task` / `set_task` | Tareas `- [ ]` de cualquier nota (por defecto, *Tareas*) | ✎ |
| `calculate` | Aritmética y funciones (`sqrt`, `pow`, `sin`, `log`…) | |
| `wikipedia` | Busca y resume un artículo de Wikipedia (es/en/…) | |
| `web_search` / `read_web` | Busca en internet (DuckDuckGo) y lee una página como texto | |

Antes de cambiar una nota se guarda una copia en `system/config/pockit/deshacer/`.

## Chats

Siguen siendo `.md` en `Chats/`. Los pasos del agente son líneas que empiezan por `» `; al
mandar el historial al modelo se quitan. Así se leen bien en el ordenador o en el móvil.

## Fases

1. **Agente en el lector** (esta): herramientas locales, Wikipedia y web, errores y gasto.
2. **Proveedor a elegir desde pockit.yous.dev** (DeepSeek o compatible con OpenAI).
3. **Dropbox**: Notas y Chats sincronizados; la web los pinta y permite seguir un chat.
4. Imágenes en las notas.
