# qmkscript

un compilador tipo duckyscript, en C, con **flex + bison**. agarra un `.qks` y lo
escupe como código QMK (`SEND_STRING(...)`), como macro de vial (json), o como
**bytecode** para una VM que corre DENTRO del firmware del AN360 (el WK Kinesis
360 que reverseé). el bytecode se carga por raw-HID sin reflashear, ese es el
truco.

proyecto de aprendizaje: overengineering a propósito con flex/bison, en la línea
de langsec / hammer / meredith l. patterson. la gramática valida lo más temprano
posible y no deja escape hatches. nada de "ya lo chequeo después".

## ⚠️ fase experimental

esto está en pañales. el lenguaje **VA A CAMBIAR**, casi seguro de formas que
rompen tus `.qks`. no hay retrocompat y no me interesa mantenerla. lo que
funciona hoy mañana puede reventar. no construyas nada serio encima todavía.
estás avisado :)

## qué hay hoy

el lenguaje ya pasó el vertical slice y es **turing-completo**: variables,
aritmética con precedencia, `if/else/else if`, `while`, `switch`, `chord`,
`bind()` declarativo, y `layout()` en runtime (latam/es/us). el backend de
bytecode alimenta una VM con guards langsec (stack over/underflow, var OOB, jmp
OOB, div por cero) y todo el pipeline está cubierto por golden tests + unit
tests de la VM (`make test`).

## docs

- [`docs/language.md`](docs/language.md): el lenguaje. statements, expresiones,
  precedencia, `chord`, `bind`, `switch`, `layout`.
- [`docs/isa.md`](docs/isa.md): el bytecode. formato del `.bin`, tabla de
  opcodes, encodings.
- [`docs/vm.md`](docs/vm.md): la VM. loader, safety pass langsec, dispatch,
  guards, y el vendor drop portable al firmware.
- [`docs/setup.md`](docs/setup.md): toolchain, build, los modos `--emit`, y la
  ruta `--push` al AN360.

## quick start

```bash
make                                       # build/qmkscript + qks-run + qks-push + lex-test
./build/qmkscript examples/hola.qks        # dump del AST (default)
./build/qmkscript --emit=c   examples/hola.qks   # bloque C para pegar en el keymap
./build/qmkscript --emit=vial examples/hola.qks  # macro vial json
./build/qmkscript --emit=bcdump examples/hola.qks # bytecode desensamblado
make test                                  # golden tests + unit tests de la VM
```

`hola.qks` es el payload clásico: abre el "run" de windows y lanza calc.

```
delay 500
chord [gui, r]
delay 200
type "calc.exe"
tap enter
```

## arquitectura

```
  .qks ─▶  lex.l  ──tokens──▶  parse.y  ──AST──▶  emit_c.c        ──▶ QMK C
          (flex)              (bison)             emit_vial.c      ──▶ vial json
                                                 emit_bytecode.c  ──▶ blob VM
                                                                        │
                                              qks-push (raw-HID) ◀──────┘
                                                    │
                                              firmware AN360 ── vm.c corre el blob
```

meter un backend nuevo es un `emit_X.c` que consume el mismo AST. el frontend no
se toca. la tabla de ficheros está en [`docs/setup.md`](docs/setup.md).

## origen

repo canónico: <https://github.com/4nt11/qmkscript>.
