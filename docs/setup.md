# qmkscript: setup

## requisitos

- **flex** 2.6+ (`dnf install flex`)
- **bison** 3.5+ (`dnf install bison`)
- **gcc**, **make**
- **hidapi** para `qks-push` (`dnf install hidapi-devel`, el Makefile lo resuelve
  con `pkg-config --cflags/--libs hidapi-hidraw`)
- opcional: `jq` para mirar la salida vial
- opcional: `entr` para los targets `watch` / `watch-push` (`dnf install entr`)
- opcional: `python3` para regenerar las tablas (`regen-keycodes`, `regen-layouts`)

verificado en fedora 43 con flex 2.6.4 / bison 3.8.2 / gcc 15.

## build

```bash
make          # build/qmkscript, build/qks-run, build/qks-push, build/lex-test
make clean    # borra build/
make test     # golden tests del compilador + unit tests de la VM
```

`build/` está en `.gitignore`: es todo generado (flex/bison + objetos +
binarios). versionar eso es de amateur.

## modos de salida

el binario principal es `build/qmkscript`. agarra un `.qks` como argumento
posicional, o lee stdin si no le pasás nada.

```bash
qmkscript --emit=dump      f.qks   # pretty-print del AST (default)
qmkscript --emit=c         f.qks   # bloque C SEND_STRING(...) para el keymap
qmkscript --emit=vial      f.qks   # macro vial como json
qmkscript --emit=bytecode  f.qks   # bytecode binario (pipe a xxd o > out.bin)
qmkscript --emit=bcdump    f.qks   # desensamblado del bytecode (legible)
qmkscript --push           f.qks   # compila a bytecode y lo empuja al AN360
qmkscript -h                       # ayuda
```

stdin y pipes unix andan:
```bash
echo 'type "pwn"
tap enter' | qmkscript --emit=c

qmkscript --emit=vial examples/hola.qks | jq .
```

## atajos del Makefile

| target             | qué hace                                              |
|--------------------|------------------------------------------------------|
| `make run`         | dump del AST de `examples/hola.qks`                  |
| `make run-c`       | emite C                                               |
| `make run-vial`    | emite json vial                                       |
| `make run-bc`      | bytecode a `xxd`                                      |
| `make run-bcdump`  | bytecode desensamblado                                |
| `make run-vm`      | compila y corre el bytecode en `qks-run` (host)      |
| `make run-push`    | compila y empuja al AN360 con `qks-push`             |
| `make run-push-native` | lo mismo con el modo `--push` nativo             |
| `make watch`       | auto-bcdump en cada save de `examples/*.qks` (entr)  |
| `make watch-push`  | auto-push en cada save (entr)                         |
| `make lex`         | stream de tokens del lexer (herramienta de debug)    |

`make watch WATCH=examples/foo.qks` para mirar otro fichero.

## ruta al AN360 (raw-HID)

`qks-push` (de `tools/qks_push.c`) empuja el bytecode al AN360 por raw-HID, sin
reflashear. `--push` en el compilador hace el pipe solo: compila y llama a
`qks-push -` con `popen`, resolviendo `qks-push` en su propio directorio y con
fallback al PATH.

`qks-run` es el intérprete de mesa: corre el MISMO `vm.c` que el firmware, pero
en el host. sirve para validar el pipeline sin tocar hardware:
```bash
qmkscript --emit=bytecode examples/hola.qks | ./build/qks-run -
```

si solo querés prototipar payloads sin tocar el firmware, `--emit=vial` alcanza.
la integración completa de la VM en el teclado está en la próxima sección.

## la VM adentro del firmware (ejemplo real: AN360)

hasta acá todo fue el compilador. la otra mitad es la VM corriendo DENTRO del
teclado. el AN360 es el ejemplo real: su firmware es un keyboard QMK, en
<https://github.com/4nt11/an360-firmware>.

**vendor drop**: `vm.c` y `vm.h` del firmware son el mismo `src/vm.c` / `src/vm.h`
de qmkscript, copiados sin cambios. es justo lo que promete el header de `vm.c`:
copiás los dos ficheros, das tus callbacks, y corre. cuando cambie la ISA,
recopiás los dos y listo.

**los ficheros que lo integran** (de `rules.mk`):

| fichero          | rol                                                    |
|------------------|--------------------------------------------------------|
| `vm.c` / `vm.h`  | la VM, copiada de qmkscript sin cambios                |
| `qks_hid.c`      | receptor raw-HID (`raw_hid_receive_kb`)                |
| `qks_storage.c`  | 8 slots de payload en flash                            |
| `qks_dispatch.c` | tabla RAM (target_kc, mods) a slot, sale del `bind()`  |
| `qks_run.c`      | carga + valida un slot y lo dispara                    |
| `qks_worker.c` / `qks_multicore.c` | offload de `vm_exec` al core 1       |

**cómo se cablea la VM**: `qks_run.c` y el worker de core 1 (`qks_worker.c`) arman
un `vm_ops_t` con los callbacks apuntando a funciones de QMK, hacen
`vm_load_and_validate()` y después `vm_exec()`. cómo se escriben esos callbacks,
con el twist del offload a core 1, está en [`vm.md`](vm.md).

**el viaje de un payload**, punta a punta:

1. `qmkscript --push foo.qks` compila a bytecode y lo pipea a `qks-push`.
2. `qks-push` lo manda por raw-HID en chunks. wire v2 (multi-slot): magic `0xE0`,
   cmd `WRITE` (0x01) por chunk, `FLUSH` (0x02) para commit-ear, cada uno con su
   `slot_idx`. también hay `DUMP` (0x03) y `RESET` (0x04).
3. `qks_hid.c` acumula los `WRITE` en un buffer RAM, y en el `FLUSH` los escribe
   al slot en flash (`qks_storage_write`). tras el commit re-escanea el dispatch.
4. `qks_dispatch` mapea el `bind(target_kc, mods)` del header a ese slot.
5. apretás la tecla que matchea, `qks_run_slot(slot)` valida el bytecode y
   dispara `qks_worker_dispatch`, que corre `vm_exec` en el core 1. los callbacks
   teclean en la ventana activa.

todo el receiver loguea por `qmk console` (`CONSOLE_ENABLE = yes`), útil para
debuggear el transporte.

**build + flash** (RP2040): es un keyboard QMK como cualquier otro.

```bash
make an360:vial                       # -> an360_vial.uf2
picotool load an360_vial.uf2 && picotool reboot
```

ojo: el toolchain de qmk/vial necesita python 3.11. las versiones más nuevas se
mueren con `ast.Num`.

## ficheros

| fichero                | rol                                                     |
|------------------------|---------------------------------------------------------|
| `src/lex.l`            | lexer flex integrado con bison                          |
| `src/lex-debug.l`      | lexer standalone, `build/lex-test` (debug)             |
| `src/parse.y`          | gramática LALR(1) de bison                              |
| `src/ast.h` / `.c`     | AST: tagged union + constructores + pretty-print        |
| `src/emit_c.c`         | backend AST a `SEND_STRING(...)`                        |
| `src/emit_vial.c`      | backend AST a macro vial (json)                         |
| `src/emit_bytecode.c`  | backend AST a bytecode de la VM                        |
| `src/vm.c` / `.h`      | la VM: ejecuta el bytecode (host y firmware)            |
| `src/vm_host.c`        | callbacks de efecto para correr la VM en el host        |
| `src/qks_run.c`        | `qks-run`: el intérprete de mesa                        |
| `src/main.c`           | CLI: getopt_long + dispatch de backend                  |
| `src/keycodes_generated.h` | tabla `KC_*`/`QK_*` (regen desde vial-qmk)          |
| `src/layouts_generated.h`  | tabla `LAYOUT_*` (regen desde vial-qmk)             |
| `tools/qks_push.c`     | `qks-push`: el sender raw-HID al AN360                  |
| `tools/gen_keycodes.py`| regenera `keycodes_generated.h`                         |
| `tools/gen_layouts.py` | regenera `layouts_generated.h`                          |
| `tests/`               | golden tests + unit tests de la VM (`make test`)        |

meter un backend nuevo es un `emit_X.c` que consume el mismo AST, el frontend no
se toca. `-Wswitch-enum` en el Makefile te OBLIGA a manejar cada nodo AST nuevo
en todos los backends, es tu red.

## regenerar tablas

cuando QMK upstream mete keycodes nuevos, o para agregar un layout:
```bash
make regen-keycodes            # busca tu checkout de vial-qmk (default del Makefile)
make regen-layouts
make regen-keycodes QMK=<tu vial-qmk>    # override si lo tenés en otro lado
```
