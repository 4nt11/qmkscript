# qmkscript

Un compilador estilo DuckyScript escrito en C con **flex + bison**. Toma un
`.qks` y lo emite como código QMK (`SEND_STRING(...)`), como macro de Vial
(JSON), o como **bytecode** para una VM que corre dentro del firmware del AN360
(WK Kinesis 360 reverseado por ANTI). El bytecode se carga por raw-HID **sin
reflashear**.

Proyecto de aprendizaje: overengineering deliberado con flex/bison, en línea con
el trabajo de LangSec / Hammer / Meredith L. Patterson. La gramática valida al
momento más temprano posible y no deja escape hatches.

## ⚠️ FASE EXPERIMENTAL

esto está en pañales. el lenguaje **VA A CAMBIAR**, casi seguro de formas que
rompen tus `.qks`. no hay retrocompatibilidad y no me interesa mantenerla. si
algo funciona hoy, mañana puede no. no construyas nada serio encima todavía.
estás avisado :)

## Estado

El lenguaje ya pasó el vertical slice y es **Turing-completo**: variables,
aritmética con precedencia, `if/else/else-if`, `while`, `switch`, `chord`,
`bind()` declarativo, y `layout()` runtime (LATAM/ES/US). El backend de bytecode
alimenta una VM con guards LangSec (stack over/underflow, var OOB, jmp OOB, div
por cero) y el pipeline entero está cubierto por golden tests + micro-tests de la
VM (`make test`).

## Docs

- [`docs/language.md`](docs/language.md) — referencia del lenguaje: statements,
  expresiones, precedencia, `chord`, `bind`, `switch`, `layout`.
- [`docs/setup.md`](docs/setup.md) — toolchain, build, los modos `--emit`, y la
  ruta `--push` al AN360.

## Quick start

```bash
make                                       # build/qmkscript + qks-run + qks-push + lex-test
./build/qmkscript examples/hola.qks        # dump del AST (default)
./build/qmkscript --emit=c   examples/hola.qks   # bloque C para pegar en keymap
./build/qmkscript --emit=vial examples/hola.qks  # macro Vial JSON
./build/qmkscript --emit=bcdump examples/hola.qks # bytecode desensamblado
make test                                  # golden tests + unit tests VM
```

`hola.qks` es el payload clásico: abre el "Run" de Windows y lanza calc.

```
delay 500
chord [gui, r]
delay 200
type "calc.exe"
tap enter
```

## Arquitectura

```
  .qks ─▶  lex.l  ──tokens──▶  parse.y  ──AST──▶  emit_c.c        ──▶ QMK C
          (flex)              (bison)             emit_vial.c      ──▶ Vial JSON
                                                 emit_bytecode.c  ──▶ blob VM
                                                                        │
                                              qks-push (raw-HID) ◀──────┘
                                                    │
                                              AN360 firmware ── vm.c corre el blob
```

Añadir un backend nuevo es un `emit_X.c` que consume el mismo AST. El frontend no
se toca. Ver la tabla de ficheros en [`docs/setup.md`](docs/setup.md).

## Origen

Repo canónico: <https://github.com/4nt11/qmkscript>.
