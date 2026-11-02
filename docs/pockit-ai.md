# Pockit AI

El agente de Pockit. Vive en su propia app, **Pockit AI**: por fuera es un chat con historial,
por dentro un agente con herramientas que lee y escribe en el lector (notas, tareas, libros,
estadísticas) y, si se le pide, consulta internet. Sin clave, Pockit funciona entero.

La relación es **IA ↔ módulos**, nunca módulos → IA: ningún módulo tiene botones de IA. La IA
usa lo que saben Lectura y el Cuaderno y lo enriquece: el resumen de lo leído queda en
*Dónde voy*, quién es alguien en *Personajes*, y las notas que crea llevan la etiqueta del libro,
así que salen en su ficha. Lo que haces con la IA no es una lista de acciones fijas: se lo pides
con tus palabras.

## Principios

- **Transparente.** Cada paso que da el agente se ve en el chat («» Buscando «Gregor» en
  *La metamorfosis*… 4 resultados»). Los cambios en tus notas se pueden deshacer tocando el paso.
- **Sin fallos silenciosos.** Clave mala, sin saldo, modelo caído, sin wifi o tiempo agotado se
  explican con su causa y qué hacer. Los errores se apuntan en `system/config/pockit/errores.tsv`.
- **Gasto a la vista.** Cada chat lleva su coste; el total y el del mes, en *Uso y ajustes*.
- **Sin destripes.** Las herramientas de libros solo leen hasta donde vas, salvo que lo pidas.
- **Barato en un lector.** Prompt de sistema y herramientas fijos (se cachean), el contexto que
  cambia va al final, los resultados se recortan y el historial solo guarda texto, no resultados.
- **Con frenos.** Una llamada que devuelve lo mismo que otra anterior no se le vuelve a dar; hay un
  tope de texto leído (60.000 caracteres) y de gasto (0,05 $) por pregunta; dos vueltas antes del
  final se le pide que cierre y en la última ya no puede usar herramientas. Si aun así no llega a
  una respuesta (o escribe una llamada como texto, cosa que hace DeepSeek), se para con un mensaje.
- **Sin reclamar atención.** No da consejos ni juicios que no se le piden.

## Arquitectura

```
ia.c          chat (lista, chat nuevo con ideas, uso y ajustes)  UI e-ink
agent.c       bucle del agente: petición → herramientas → …     máx. 8 vueltas por pregunta
tools.c       catálogo de herramientas y su implementación
datos.c       dónde viven Notas/, Chats/ y Libros/ (lector o Dropbox) y las #etiquetas
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
| `book_memory` | Lo que Pockit ya sabe del libro: resúmenes, quién es quién y notas con su etiqueta | |
| `summarize_reading` | Resume lo último antes de donde vas (o todo, avisando antes del coste) y lo deja en *Dónde voy*; lo resumido no se vuelve a pagar | ✎ |
| `review_characters` | Separa personajes de lugares y dice quién es cada uno a partir de un par de frases; solo los nombres nuevos | ✎ |
| `remember_character` | Guarda en *Personajes* quién es alguien, o que un nombre es un lugar | ✎ |
| `list_highlights` | Subrayados y notas del lector | |
| `list_words` | Palabras consultadas en el diccionario, con definición y si están aprendidas | |
| `list_notes` / `read_note` / `search_notes` | Cuaderno (con filtro por etiqueta) | |
| `create_note` / `edit_note` | Crear (con etiquetas) o cambiar notas (añadir, insertar tras un título, reemplazar) | ✎ |
| `list_tasks` / `add_task` / `set_task` | Tareas `- [ ]` de cualquier nota (por defecto, *Tareas*) | ✎ |
| `calculate` | Aritmética y funciones (`sqrt`, `pow`, `sin`, `log`…) | |
| `wikipedia` | Busca y resume un artículo de Wikipedia (es/en/…) | |
| `web_search` / `read_web` | Busca en internet (DuckDuckGo) y lee una página como texto | |

Antes de cambiar una nota se guarda una copia en `system/config/pockit/deshacer/`.

## Datos

Todo es texto en la carpeta de tus datos (`Pockit/`, que se sincroniza con `/Pockit` en tu Dropbox si lo enlazas):

```
Notas/<n>.md                   el Cuaderno; #etiquetas en cualquier línea
Chats/<n>.md                   «### Tú» / «### IA»; los pasos del agente son líneas «» »
Libros/<etiqueta>/ficha.ini    título, autor, progreso y fechas (sigue aunque borres el libro)
Libros/<etiqueta>/tramos.tsv   resúmenes de lo leído: inicio, fin y texto
Libros/<etiqueta>/personajes.tsv  nombre, tipo (personaje, lugar u otro) y quién es
```

La etiqueta de un libro sale de su título: «Niebla (Nivola)» → `niebla`. Una nota con `#niebla`
(o `#hombre-duplicado` para «El hombre duplicado»: sin artículo, o solo el principio) sale en la
ficha del libro. Al mandar el historial al modelo, los pasos del agente se quitan.

## Fases

1. **Agente en el lector**: herramientas locales, Wikipedia y web, errores y gasto. Hecho.
2. **IA ↔ módulos y Dropbox**: resúmenes y personajes en Lectura, etiquetas, datos en Dropbox. Hecho (2.2).
3. **La web**: ver y seguir notas y chats desde pockit.yous.dev, y elegir proveedor.
4. Imágenes en las notas.
