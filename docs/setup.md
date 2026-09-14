# qmkscript — setup

## Requisitos

- **flex** 2.6+ (`dnf install flex`)
- **bison** 3.5+ (`dnf install bison`)
- **gcc**, **make**
- **hidapi** para `qks-push` (`dnf install hidapi-devel`; el Makefile lo resuelve
  con `pkg-config --cflags/--libs hidapi-hidraw`)
- opcional: `jq` para inspeccionar la salida Vial
- opcional: `entr` para los targets `watch` / `watch-push` (`dnf install entr`)
- opcional: `python3` para regenerar las tablas (`regen-keycodes`, `regen-layouts`)

Verificado en Fedora 43 con flex 2.6.4 / bison 3.8.2 / gcc 15.

## Build

```bash
make          # build/qmkscript, build/qks-run, build/qks-push, build/lex-test
make clean    # borra build/
make test     # golden tests del compilador + unit tests de la VM
```

`build/` está en `.gitignore`: es todo generado (flex/bison + objetos + binarios).

## Modos de salida

El binario principal es `build/qmkscript`. Toma un `.qks` como argumento
posicional, o lee stdin si se omite.

```bash
qmkscript --emit=dump      f.qks   # pretty-print del AST (default)
qmkscript --emit=c         f.qks   # bloque C SEND_STRING(...) para el keymap
qmkscript --emit=vial      f.qks   # macro Vial como JSON
qmkscript --emit=bytecode  f.qks   # bytecode binario (pipe a xxd o > out.bin)
qmkscript --emit=bcdump    f.qks   # desensamblado del bytecode (legible)
qmkscript --push           f.qks   # compila a bytecode y lo empuja al AN360
qmkscript -h                       # ayuda
```

stdin y pipes Unix funcionan:
```bash
echo 'type "pwn"
tap enter' | qmkscript --emit=c

qmkscript --emit=vial examples/hola.qks | jq .
```

## Atajos del Makefile

| Target             | Qué hace                                              |
|--------------------|------------------------------------------------------|
| `make run`         | dump del AST de `examples/hola.qks`                  |
| `make run-c`       | emite C                                               |
| `make run-vial`    | emite JSON Vial                                       |
| `make run-bc`      | bytecode \| `xxd`                                     |
| `make run-bcdump`  | bytecode desensamblado                                |
| `make run-vm`      | compila y corre el bytecode en `qks-run` (host)      |
| `make run-push`    | compila y empuja al AN360 vía `qks-push`             |
| `make run-push-native` | idem con el modo `--push` nativo                 |
| `make watch`       | auto-bcdump en cada save de `examples/*.qks` (entr)  |
| `make watch-push`  | auto-push en cada save (entr)                         |
| `make lex`         | stream de tokens del lexer (herramienta de debug)    |

`make watch WATCH=examples/foo.qks` para mirar otro fichero.

## Ruta al AN360 (raw-HID)

`qks-push` (de `tools/qks_push.c`) empuja el bytecode al AN360 por raw-HID, sin
reflashear. `--push` en el compilador hace el pipe internamente (compila y
llama a `qks-push -` vía `popen`; resuelve `qks-push` en su propio directorio,
fallback al PATH).

`qks-run` es el intérprete de mesa: corre el mismo `vm.c` que el firmware, en el
host. Sirve para validar el pipeline sin tocar hardware:
```bash
qmkscript --emit=bytecode examples/hola.qks | ./build/qks-run -
```

El firmware del AN360 (con la VM embebida) vive aparte; ver las notas de ese
proyecto. Para prototipar payloads sin nada de eso, `--emit=vial` alcanza.

## Ficheros

| Fichero                | Rol                                                     |
|------------------------|---------------------------------------------------------|
| `src/lex.l`            | Lexer flex integrado con bison                          |
| `src/lex-debug.l`      | Lexer standalone → `build/lex-test` (debug)            |
| `src/parse.y`          | Gramática LALR(1) de bison                              |
| `src/ast.h` / `.c`     | AST: tagged union + constructores + pretty-print        |
| `src/emit_c.c`         | Backend AST → `SEND_STRING(...)`                        |
| `src/emit_vial.c`      | Backend AST → macro Vial (JSON)                         |
| `src/emit_bytecode.c`  | Backend AST → bytecode de la VM                        |
| `src/vm.c` / `.h`      | La VM: ejecuta el bytecode (host y firmware)            |
| `src/vm_host.c`        | Callbacks de efecto para correr la VM en el host        |
| `src/qks_run.c`        | `qks-run`: intérprete de mesa                          |
| `src/main.c`           | CLI: getopt_long + dispatch de backend                  |
| `src/keycodes_generated.h` | Tabla `KC_*`/`QK_*` (regen desde vial-qmk)          |
| `src/layouts_generated.h`  | Tabla `LAYOUT_*` (regen desde vial-qmk)             |
| `tools/qks_push.c`     | `qks-push`: sender raw-HID al AN360                     |
| `tools/gen_keycodes.py`| Regenera `keycodes_generated.h`                         |
| `tools/gen_layouts.py` | Regenera `layouts_generated.h`                          |
| `tests/`               | Golden tests + unit tests de la VM (`make test`)        |

**Añadir un backend nuevo** es un `emit_X.c` que consume el mismo AST; el
frontend no se toca. `-Wswitch-enum` en el Makefile te obliga a manejar cada
nodo AST nuevo en todos los backends.

## Regenerar tablas

Cuando QMK upstream añade keycodes, o para añadir un layout:
```bash
make regen-keycodes            # default: parsea ~/vial-qmk
make regen-layouts
make regen-keycodes QMK=/otro/vial-qmk   # override del path
```
