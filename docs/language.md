# qmkscript: referencia del lenguaje

sintaxis v1: lowercase, python-ish. la fuente de verdad es `src/lex.l` (tokens) y
`src/parse.y` (gramática LALR(1)). todo lo de acá sale de `examples/`, nada
inventado.

## léxico

- **comentarios**: `#` hasta fin de línea.
- **separadores de statement**: `;` o newline (el mismo token `SEP`). una
  statement por línea, o varias con `;`:
  ```
  delay 200; type "hola "; type "mundo"; tap enter
  ```
- **strings** `"..."` con escapes `\"` `\\` `\n` `\t` `\r`. string sin cerrar
  antes del newline = error.
- **char literals** `'x'`: EXACTAMENTE 1 carácter, mismos escapes (`\'` en vez de
  `\"`). más de 1 char = error.
- **números**: enteros decimales (`atoi`, sin signo en la fuente).
- **identificadores lowercase** `[a-z_][a-z0-9_]*`: nombres de vars, teclas y
  modificadores.
- **identificadores UPPER** `[A-Z][A-Z0-9_]+`: se resuelven en compile-time
  contra las tablas generadas (`KC_*`, `QK_*` en `keycodes_generated.h`, y
  `LAYOUT_*` en `layouts_generated.h`). un UPPER desconocido es error de
  compilación, NO se pasa como texto. sin escape hatches.

## statements

### `type "..."`
teclea la string en la ventana activa (bytecode `OP_STR`).
```
type "pwnedbyanti"
```

### `tap <tecla>`
toca una tecla, por nombre lowercase (`enter`, `esc`, `tab`, `r`, `a`, `1`, ...)
o por char literal (`'r'`).
```
tap enter
tap 'r'
```

### `delay N`
espera `N` ms.
```
delay 500
```

### `var name = expr` / `name = expr`
declara o reasigna una variable. el symbol table asigna slots `0..N-1` solo;
reasignar reusa el slot, no leakea vars.
```
var i = 0
i = i + 1
```
las vars viven solo en el estado de la VM, no se ven en el output HID.

### bloque `{ ... }`
agrupa statements pero NO ejecuta nada por sí mismo: emite el mismo bytecode que
los stmts sueltos. existe para darle a `if`/`while`/`switch` un cuerpo con tipo
único. anidable.
```
{
    type "hola"
    tap enter
}
```

### `if` / `else` / `else if`
condición SIN paréntesis, braces OBLIGATORIAS (así mato el dangling-else a nivel
léxico, no hay ambigüedad que resolver).
```
var x = 2
if x == 1 {
    type "uno"
} else if x == 2 {
    type "dos"
} else {
    type "otro"
}
```
`else if` es azúcar: la gramática lo desazucara a `else { if ... }`.

**gotcha**: `} else {` va SIEMPRE en la misma línea. el `else` en línea aparte
del `}` no compila, LALR(1) no puede esperar past `SEP`. mismo criterio que go.

### `while expr block`
misma forma que `if`: cond sin parens, braces obligatorias.
```
var i = 0
while i < 5 {
    type "x"
    i = i + 1
}
```
`while` + var + reasign y ya sos **turing-completo** (böhm-jacopini). con eso
alcanza para computar cualquier cosa computable.

### `switch expr { case N: stmt ... default: stmt }`
la key es una expr arbitraria. los valores de `case` tienen que ser **literales**
(nada de `case x+1:`, la tabla se queda determinística en compile-time). sin
fall-through: cada case salta al final. detecta cases duplicados en compile-time.
```
var x = 2
switch x {
    case 1: type "uno"
    case 2: type "dos"
    case 3: type "tres"
    default: type "otro"
}
```
el body de un case es UN stmt. para varios, metelos en un bloque `{ ... }`.

### `chord [ ... ]`
modificadores held mientras corren una o más acciones (taps/strings). los mods
van PRIMERO. un mod después de una tecla es error.
```
chord [gui, r]            # LGUI/RGUI + tap r
chord [shift, "pwn"]      # shift held mientras teclea "pwn" -> "PWN"
```
nombres de mod:
- genéricos: `gui`, `ctrl`, `alt`, `shift`. emiten AMBOS bits L|R, el firmware
  los toma como "cualquier lado".
- explícitos: `lgui`/`rgui`, `lctl`/`rctl`, `lalt`/`ralt`, `lsft`/`rsft`. solo
  su lado.

si el chord es un solo mod + una sola tecla, colapsa a un `OP_CHORD` compacto. si
no, sale como `OP_REG_MODS` + secuencia + `OP_UNREG_MODS`.

### `bind(...)`
metadata compile-time: qué tecla dispara este payload cuando se cargue en el
firmware. tres sabores de target:
```
bind(KC_R)                # constante keycode QMK (la resuelve el lexer)
bind(tap enter)           # tap por nombre o char
bind(chord [gui, r])      # mods + 1 tecla
```
opcional `layer=`:
```
bind(layer=3, tap enter)  # match solo si el layer 3 está on. N en [0..15]
bind(layer=any, tap r)    # match en cualquier layer
```
sin `layer=`, default = layer 3 (OFFSEC) por convención. `N` fuera de `[0..15]`
es error.

### `layout(expr)`
cambia el layout activo en runtime. la expr se evalúa y actualiza el layout de la
VM (afecta cómo se mapean chars a scancodes, o sea: el drama de las tildes).
```
layout(LAYOUT_LATAM)
```
layouts que hay (de `layouts_generated.h`): `LAYOUT_DEFAULT`, `LAYOUT_ES`,
`LAYOUT_LATAM`. la expr puede ser variable también (`layout(i)`).

## expresiones

operandos: números, refs a var (`ident`), constantes UPPER (resueltas a número),
y sub-expresiones entre paréntesis.

precedencia (de menor a mayor):

| nivel | operadores          | asociatividad |
|-------|---------------------|---------------|
| 1     | `==` `!=` `<` `>`   | **no asociativa** |
| 2     | `+` `-`             | izquierda     |
| 3     | `*` `/`             | izquierda     |

- la comparación es **no asociativa** a propósito: `a < b < c` es error de
  parseo. es contra el bug clásico de C donde `a<b<c` evalúa `(a<b)<c` y te da
  cualquier cosa.
- la comparación pega MENOS fuerte que la aritmética: `a + b > c * d` parsea como
  `(a+b) > (c*d)`.
- comparar devuelve `0` (false) o `1` (true) en el stack.

```
var mixed  = a + b * 2        # 20: * antes que +
var forced = (a + b) * 2      # 30: paréntesis mandan
var less   = a < b            # 0 o 1
```

## backends

el mismo AST alimenta cuatro backends (`--emit=`): `dump`, `c`, `vial`,
`bytecode`/`bcdump`. mirá [`setup.md`](setup.md) para los modos y la ruta
`--push` al AN360, [`isa.md`](isa.md) para el bytecode y [`vm.md`](vm.md) para la
VM.
