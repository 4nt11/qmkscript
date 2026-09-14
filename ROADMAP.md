# qmkscript — ROADMAP (mapa, no construir aún)

Un clon de DuckyScript que compila a QMK. Proyecto de ANTI para (a) aprender flex/bison
y (b) convertir su WK Kinesis 360 (clon chino, QMK/Vial, 2.4G) en un "ducky" propio.
Ver [[wk360-keyboard]] y [[anti-tooling-preferences]].

## Veredicto de diseño
- DuckyScript 1.0 (línea-a-línea) NO justifica flex/bison — un `for linea: split()` basta.
- Justificación real: **aprender el toolchain** + **apuntar a DuckyScript 3.0** (VAR, IF/ELSE,
  WHILE, funciones, aritmética). *Ahí* bison se gana el sueldo: expresiones con precedencia,
  bloques anidados, recursión.

## Mapeo base DuckyScript → QMK
| Ducky | QMK |
|---|---|
| `STRING x` | `SEND_STRING("x")` |
| `ENTER` | `SS_TAP(X_ENTER)` |
| `GUI r` | `SEND_STRING(SS_LGUI("r"))` |
| `DELAY n` | `SS_DELAY(n)` |
| `REM ...` | `// ...` |

## Arquitectura
```
qmkscript.l   flex: tokens (STRING, GUI, IF, números, ident, ops)
qmkscript.y   bison: gramática → AST en C
ast.c/.h      nodos
codegen.c     AST → bloque SEND_STRING(...) para keymap.c
layout/       tablas char→keycode por layout (us, latam...) ★ KILLER FEATURE
Makefile
```
La carpeta `layout/` resuelve de raíz la **dependencia de layout** (el drama de las tildes):
HID manda scancodes, no caracteres; el compilador mapea char→keycode según el layout del
target. Eso hace a qmkscript mejor que escribir macros a mano.

## Orden de trabajo (vertical slice primero — NO diseñar el lenguaje entero antes)
1. Slice mínimo end-to-end: `STRING`/`ENTER`/`DELAY`/`GUI` → C que compile y teclee.
2. Recién ahí: `VAR`, `IF/ELSE`, `WHILE`, expresiones.
3. Backend alternativo: emitir macro Vial (sin recompilar) además del bloque C.

## Dependencia / bloqueo
- La ruta **compilada** necesita firmware QMK construible para el board. El vendor dice que
  el proveedor NO tiene la fuente pública → hay que **reversear/reconstruir** el firmware
  (track paralelo, ver abajo).
- PERO: los payloads qmkscript se pueden prototipar **HOY en macros de Vial** sin nada de RE.
  El RE NO bloquea el diseño del lenguaje; solo la ruta de salida compilada.

## Track paralelo: reverse del firmware WK360
Objetivo: obtener una base QMK construible (o entender el binario). Fases:
0. Recon: identificar MCU, bootloader, qué hay flasheado, qué artefactos tenemos. ← EMPEZANDO
1. ¿Flash leíble? (MCU + bootloader + read-protection) → intentar dump.
2. Si hay binario: strings + Ghidra → hallar `raw_hid_receive`, protocolo de pantalla, string "Peace and love".
3. Reconstruir QMK desde la definición Vial embebida (matriz/pines) o parchear el binario.
Nota: para SOLO macros (qmkscript payloads), Vial basta; el RE es para control total / ruta compilada / cambiar la pantalla.
