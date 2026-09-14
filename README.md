# qmkscript

Un compilador estilo DuckyScript escrito en C, con **flex + bison**, que emite
tanto código QMK (`SEND_STRING(...)`) como macros de Vial en JSON. Diseñado para
convertir el AN360 (WK Kinesis 360 reverseado por ANTI) en un dispositivo de
payload propio.

Proyecto de aprendizaje: overengineering deliberado con flex/bison en línea con
el trabajo de LangSec / Hammer / Meredith L. Patterson.

## Estado

- **v0 vertical slice — DONE** (2026-09-12).
  Compila `.qks` a C y a JSON Vial, lee ficheros y stdin, salida pipeable a `jq`.
- **v1 norte** — bytecode + VM dentro del firmware AN360, con carga de payloads
  por raw-HID *sin reflashear*. Ver `ROADMAP.md`.

## Requisitos

- flex 2.6+ (`dnf install flex` en Fedora)
- bison 3.5+ (`dnf install bison`)
- gcc, make
- opcional: `jq` para inspeccionar la salida Vial

Verificado en Fedora 43 con flex 2.6.4 / bison 3.8.2 / gcc 15.

## Build

```bash
make          # compila build/qmkscript y build/lex-test
make clean    # borra build/
```

## Uso

```bash
# CLI principal
qmkscript --emit=dump examples/hola.qks   # pretty-print del AST (default)
qmkscript --emit=c    examples/hola.qks   # bloque C para pegar en keymap
qmkscript --emit=vial examples/hola.qks   # macro Vial JSON

# stdin funciona
echo "STRING pwned
ENTER" | qmkscript --emit=c

# pipes Unix
qmkscript --emit=vial examples/hola.qks | jq .

# atajos del Makefile
make run        # ./build/qmkscript examples/hola.qks
make run-c      # emite C
make run-vial   # emite JSON
make lex        # herramienta debug: stream de tokens del lexer
```

## Sintaxis v0

```
REM comentario hasta fin de linea
STRING <palabra>          # emite el texto (una sola palabra en v0)
ENTER                     # tap ENTER
DELAY <ms>                # espera N milisegundos
GUI <palabra>             # LGUI + tecla (ej: GUI r)
```

Una statement por línea. Líneas en blanco permitidas. Ver `examples/hola.qks`
para el payload clásico `GUI r → calc.exe → ENTER`.

Multi-palabra tras STRING, más modificadores (CTRL/ALT/SHIFT), `IF`/`WHILE`/
`VAR` y expresiones aritméticas están en el roadmap (v0.x → v1).

## Arquitectura

```
                              parse.tab.h
                                   │
                                   ▼
  .qks ─▶  lex.l  ──tokens──▶  parse.y  ──AST──▶  emit_c.c   ──▶ QMK C
                (flex)          (bison)           emit_vial.c ──▶ JSON
                                                  emit_bytecode.c ─▶ blob (v1)
```

Archivos:

| Fichero              | Rol                                                           |
|----------------------|---------------------------------------------------------------|
| `src/lex.l`          | Lexer flex integrado con bison                                |
| `src/lex-debug.l`    | Lexer standalone → `build/lex-test` (herramienta de debug)    |
| `src/parse.y`        | Gramática LALR(1) de bison                                    |
| `src/ast.h/.c`       | AST: tagged union + constructores + pretty-print              |
| `src/emit_c.h/.c`    | Backend: AST → `SEND_STRING(...)` para keymap QMK             |
| `src/emit_vial.h/.c` | Backend: AST → macro Vial (JSON)                              |
| `src/main.c`         | CLI: getopt_long + dispatch de backend                        |
| `examples/hola.qks`  | Payload de ejemplo                                            |
| `Makefile`           | Reglas para flex, bison, gcc                                  |

**Añadir un backend nuevo** (Python, Go, WASM, lo que sea) es un fichero
`emit_X.c` que consume el mismo AST. El frontend no se toca.

## Gotchas útiles

- Los `%option` en `.l` **no aceptan comentarios `/* */` inline** — la sección
  de declaraciones no es C, es DSL propia de flex.
- El lexer real (`src/lex.l`) `#include`s `parse.tab.h` que genera bison. Sin
  eso, cada uno usa IDs de token distintos y todo peta silenciosamente.
- `-Wswitch-enum` en el Makefile es intencional: cuando añadas un nodo AST
  nuevo, gcc te obliga a manejarlo en todos los backends. Es tu red.
- Ownership de strings: `WORD` hace `strdup` en el lexer, `ast_string` hace
  otro `strdup` internamente (libera $2 tras la llamada), `ast_chord` NO
  duplica (el nodo se queda con la referencia). Documentado en `parse.y`.

## Roadmap resumido

Ver `ROADMAP.md` para el detalle. En corto:

- v0.1 lexer: `STRING` multi-palabra (flex start conditions).
- v0.2 chords: `CTRL/ALT/SHIFT` además de `GUI`.
- v0.3 layouts: tabla `char → (keycode, mods)` por layout (es_CL primero).
- v1 lenguaje: `VAR`, `IF/ELSE`, `WHILE`, expresiones con precedencia.
- **v1.5 bytecode + VM** ★: backend `emit_bytecode.c` + intérprete dentro del
  firmware AN360, con protocolo raw-HID para cargar payloads sin reflashear.

## Licencia

GPL-2.0-or-later (compatible con QMK).
