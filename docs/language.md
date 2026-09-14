# qmkscript — referencia del lenguaje

Sintaxis v1: lowercase, python-ish. Fuente de verdad: `src/lex.l` (tokens) y
`src/parse.y` (gramática LALR(1)). Todo lo de acá está en `examples/`.

## Léxico

- **Comentarios**: `#` hasta fin de línea.
- **Separadores de statement**: `;` o newline (el mismo token `SEP`). Una
  statement por línea, o varias separadas por `;`:
  ```
  delay 200; type "hola "; type "mundo"; tap enter
  ```
- **Strings** `"..."` con escapes `\"` `\\` `\n` `\t` `\r`. String sin cerrar
  antes del newline = error.
- **Char literals** `'x'`: exactamente 1 carácter, mismos escapes (`\'` en vez de
  `\"`). Más de 1 char = error.
- **Números**: enteros decimales (`atoi`, sin signo en la fuente).
- **Identificadores lowercase** `[a-z_][a-z0-9_]*`: nombres de vars, teclas y
  modificadores.
- **Identificadores UPPER** `[A-Z][A-Z0-9_]+`: se resuelven en compile-time
  contra las tablas generadas (`KC_*`, `QK_*` en `keycodes_generated.h`, y
  `LAYOUT_*` en `layouts_generated.h`). Un UPPER desconocido es error de
  compilación, no se pasa como texto.

## Statements

### `type "..."`
Teclea la string en la ventana activa (bytecode `OP_STR`).
```
type "pwnedbyanti"
```

### `tap <tecla>`
Toca una tecla, por nombre lowercase (`enter`, `esc`, `tab`, `r`, `a`, `1`, ...)
o por char literal (`'r'`).
```
tap enter
tap 'r'
```

### `delay N`
Espera `N` milisegundos.
```
delay 500
```

### `var name = expr` / `name = expr`
Declara o reasigna una variable. El symbol table asigna slots `0..N-1`
automáticamente; reasignar reusa el slot (no leakea vars).
```
var i = 0
i = i + 1
```
Las vars viven solo en el estado de la VM, no se ven en el output HID.

### bloque `{ ... }`
Agrupa statements. No ejecuta nada por sí mismo: emite el mismo bytecode que sus
stmts sueltos. Existe para dar a `if`/`while`/`switch` un cuerpo con tipo único.
Anidable.
```
{
    type "hola"
    tap enter
}
```

### `if` / `else` / `else if`
Condición **sin paréntesis**, braces **obligatorias** (esto mata el
dangling-else a nivel léxico).
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

**Gotcha**: `} else {` va SIEMPRE en la misma línea. El `else` en línea aparte
del `}` no compila (LALR(1) no puede esperar past `SEP`). Mismo criterio que Go.

### `while expr block`
Misma forma que `if`: cond sin parens, braces obligatorias.
```
var i = 0
while i < 5 {
    type "x"
    i = i + 1
}
```
`while` + var + reasign hace a qmkscript **Turing-completo** (Böhm-Jacopini).

### `switch expr { case N: stmt ... default: stmt }`
La key es una expr arbitraria. Los valores de `case` deben ser **literales**
(no `case x+1:`, para mantener la tabla determinística en compile-time). Sin
fall-through: cada case salta al final. Detecta cases duplicados en compile-time.
```
var x = 2
switch x {
    case 1: type "uno"
    case 2: type "dos"
    case 3: type "tres"
    default: type "otro"
}
```
El body de un case es UN stmt; para varios, usar bloque `{ ... }`.

### `chord [ ... ]`
Modificadores held mientras se ejecutan una o más acciones (taps/strings). Los
mods van **primero**; un mod después de una tecla es error.
```
chord [gui, r]            # LGUI/RGUI + tap r
chord [shift, "pwn"]      # shift held mientras teclea "pwn" -> "PWN"
```
Nombres de mod:
- Genéricos: `gui`, `ctrl`, `alt`, `shift` — emiten **ambos** bits L|R, el
  firmware los toma como "cualquier lado".
- Explícitos: `lgui`/`rgui`, `lctl`/`rctl`, `lalt`/`ralt`, `lsft`/`rsft` — solo
  su lado.

Si el chord es un solo mod + una sola tecla, colapsa a un `OP_CHORD` compacto;
si no, se emite como `OP_REG_MODS` + secuencia + `OP_UNREG_MODS`.

### `bind(...)`
Metadata compile-time: qué tecla dispara este payload cuando se cargue en el
firmware. Tres sabores de target:
```
bind(KC_R)                # constante keycode QMK (resuelta por el lexer)
bind(tap enter)           # tap por nombre o char
bind(chord [gui, r])      # mods + 1 tecla
```
Opcional `layer=`:
```
bind(layer=3, tap enter)  # match solo si el layer 3 está on. N en [0..15]
bind(layer=any, tap r)    # match en cualquier layer
```
Sin `layer=`, default = layer 3 (OFFSEC) por convención. `N` fuera de `[0..15]`
es error.

### `layout(expr)`
Cambia el layout activo en runtime. La expr se evalúa y actualiza el layout de la
VM (afecta cómo se mapean chars a scancodes: el drama de las tildes).
```
layout(LAYOUT_LATAM)
```
Layouts disponibles (de `layouts_generated.h`): `LAYOUT_DEFAULT`, `LAYOUT_ES`,
`LAYOUT_LATAM`. La expr puede ser variable (`layout(i)`) también.

## Expresiones

Operandos: números, refs a var (`ident`), constantes UPPER (resueltas a número),
y sub-expresiones entre paréntesis.

Operadores y precedencia (de menor a mayor):

| Nivel | Operadores          | Asociatividad |
|-------|---------------------|---------------|
| 1     | `==` `!=` `<` `>`   | **no asociativa** |
| 2     | `+` `-`             | izquierda     |
| 3     | `*` `/`             | izquierda     |

- La comparación es **no asociativa** a propósito: `a < b < c` es error de
  parseo (contra el bug clásico de C donde `a<b<c` evalúa `(a<b)<c`).
- La comparación tiene menor precedencia que la aritmética: `a + b > c * d`
  parsea como `(a+b) > (c*d)`.
- Comparación devuelve `0` (false) o `1` (true) en el stack.

```
var mixed  = a + b * 2        # 20: * antes que +
var forced = (a + b) * 2      # 30: paréntesis fuerzan
var less   = a < b            # 0 o 1
```

## Backends

El mismo AST alimenta cuatro backends (`--emit=`): `dump`, `c`, `vial`,
`bytecode`/`bcdump`. Ver [`setup.md`](setup.md) para los modos y la ruta `--push`
al AN360.
