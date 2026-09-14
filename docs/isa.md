# qmkscript: ISA (bytecode)

el set de instrucciones de la VM. fuente de verdad: `src/vm.h` (el enum de
opcodes + el formato) y `src/vm.c::opcode_length` (las longitudes canónicas). la
semántica de ejecución vive en [`vm.md`](vm.md), acá está el **encoding**.

se emite con `--emit=bytecode` (binario) y lo inspeccionás con `--emit=bcdump`
(desensamblado legible).

## convenciones

- **little-endian** en todo lo multi-byte. RP2040 es LE nativo, cero swaps.
- **stack machine**: sin registros, los operandos van en un value stack de
  `uint32_t`.
- **orden binario**: para ops de dos operandos, `TOS-1 OP TOS`. o sea `a` se
  empuja primero, `b` (el TOS) después. `SUB` computa `a - b`, `DIV` `a / b`,
  `LT` `a < b`.
- **HALT explícito** cierra todo programa (`0x00`). el loader lo exige.

## formato del `.bin`

header de 16 bytes (`QKS_HEADER_SIZE`), y atrás el code segment:

| offset | tam | campo          | notas                                        |
|--------|-----|----------------|----------------------------------------------|
| `0x00` | 4   | magic          | `"QKSC"` (`0x43534B51` LE)                    |
| `0x04` | 1   | version        | `1` (`QKS_VERSION`)                            |
| `0x05` | 1   | flags          | reservado, `0`                                |
| `0x06` | 1   | `target_mods`  | bitmap HID del `bind()`, `0` si no hay bind   |
| `0x07` | 1   | `target_layer` | layer `0..15` del `bind()`, `0xFF` = wildcard |
| `0x08` | 4   | `code_len` u32 | bytes del code segment                        |
| `0x0C` | 2   | `target_kc` u16| keycode QMK del `bind()`, `0` = default       |
| `0x0E` | 2   | reserved       | pad a 16B, `0`                                |
| `0x10` | ... | code           | `code_len` bytes de opcodes, terminan en HALT |

`16 + code_len == file_size`, invariante que el loader chequea. los tres campos
de `bind()` son metadata que el firmware lee al load-time. en un `.qks` sin
`bind()` valen 0.

sin backward compat, a propósito: no hay usuarios reales, un cambio de layout va
directo al firmware (flash nuevo + push del `.bin` nuevo). fuck la retrocompat
por ahora.

## tabla de opcodes

### control
| op        | byte  | operandos      | efecto                                   |
|-----------|-------|----------------|------------------------------------------|
| `HALT`    | `0x00`| ninguno        | termina la ejecución (`VM_OK`)           |

### i/o de teclado
| op            | byte  | operandos          | efecto                                        |
|---------------|-------|--------------------|-----------------------------------------------|
| `STR`         | `0x01`| u16 len, byte[len] | teclea la string (dispatch de layout en vm.md)|
| `TAP`         | `0x02`| u8 kc              | `tap_code(kc)`                                |
| `DELAY`       | `0x03`| u16 ms             | `wait_ms(ms)`                                 |
| `CHORD`       | `0x04`| u8 mods, u8 kc     | register mods, tap kc, unregister             |
| `REG_MODS`    | `0x05`| u8 mods            | `register_mods(mods)`                         |
| `UNREG_MODS`  | `0x06`| u8 mods            | `unregister_mods(mods)`                       |

`CHORD` es el chord compacto (1 mod-set + 1 tecla). los chords "gordos" (>1 tecla
o strings adentro) salen como `REG_MODS` + secuencia normal + `UNREG_MODS`.

### control de flujo
| op        | byte  | operandos | efecto                                          |
|-----------|-------|-----------|-------------------------------------------------|
| `JMP`     | `0x10`| i16 off   | salta                                           |
| `JMPZ`    | `0x11`| i16 off   | pop TOS, salta si era `0`                        |
| `JMP_S`   | `0x12`| i8 off    | igual que `JMP`, offset corto `[-128..127]`      |
| `JMPZ_S`  | `0x13`| i8 off    | igual que `JMPZ`, offset corto                   |
| `SWITCH`  | `0x14`| tabla     | jump table inline (abajo)                        |
| `LAYOUT`  | `0x15`| ninguno   | pop TOS, va a `current_layout` de la VM          |

**offsets**: relativos al byte que sigue al operando (para `SWITCH`, al byte
después de toda la tabla). `off == 0` no salta, positivo salta adelante, negativo
atrás. el compilador elige `JMP`/`JMP_S` con `relax_jumps()` (fixpoint post-emit),
la VM despacha los dos.

### pila y variables
| op        | byte  | operandos | efecto                          |
|-----------|-------|-----------|---------------------------------|
| `PUSH`    | `0x20`| u32 val   | push val                        |
| `LOAD`    | `0x21`| u8 idx    | push `vars[idx]`                |
| `STORE`   | `0x22`| u8 idx    | pop a `vars[idx]`               |
| `POP`     | `0x23`| ninguno   | tira el TOS (reservado, sin uso en el DSL de hoy) |

### density (short encodings, C.3.e)
mismo efecto, menos bytes. el compilador elige la forma más corta que quepa, la
VM despacha todas. filosofía JVM/PDP-11: lo común se paga en 1 byte.

| op          | byte  | operandos | efecto            |
|-------------|-------|-----------|-------------------|
| `PUSH_0`    | `0x24`| ninguno   | push `0`          |
| `PUSH_1`    | `0x25`| ninguno   | push `1`          |
| `PUSH_U8`   | `0x26`| u8 val    | push val (0..255) |
| `PUSH_U16`  | `0x27`| u16 val   | push val (0..65535)|
| `LOAD_0..3` | `0x28`..`0x2B`| ninguno | push `vars[op - 0x28]` (el idx va en el opcode) |
| `STORE_0..3`| `0x2C`..`0x2F`| ninguno | pop a `vars[op - 0x2C]`             |

`LOAD_N`/`STORE_N` codifican el índice en los bits del opcode (estilo x86 register
encoding). las primeras 4 vars cuestan 1 byte.

### aritmética (u32, `TOS-1 OP TOS`)
| op    | byte  | efecto        |
|-------|-------|---------------|
| `ADD` | `0x30`| push `a + b`  |
| `SUB` | `0x31`| push `a - b`  |
| `MUL` | `0x32`| push `a * b`  |
| `DIV` | `0x33`| push `a / b` (b==0 da `VM_ERR_DIV_ZERO`) |

### comparación (push `0` o `1`)
| op    | byte  | efecto        |
|-------|-------|---------------|
| `EQ`  | `0x40`| `a == b`      |
| `NEQ` | `0x41`| `a != b`      |
| `LT`  | `0x42`| `a < b`       |
| `GT`  | `0x43`| `a > b`       |

## encodings de longitud variable

### `STR` (`0x01`)
```
01  <len:u16 LE>  <bytes[len]>
```
longitud total `3 + len`. los bytes son el texto crudo (UTF-8 cuando hay layout
no-default, ver vm.md).

### `SWITCH` (`0x14`)
```
14  <n_cases:u16 LE>  [ <key:u32 LE> <off:i16 LE> ] x n_cases  <default_off:i16 LE>
```
longitud total `3 + 6*n_cases + 2`. ejecución: pop TOS como key, scan lineal por
match, tomás su `off`. sin match, va el `default_off`. sin fall-through. los
offsets son relativos al byte DESPUÉS de toda la tabla.

## modificadores y keycodes

los bytes de `mods` usan el MISMO bitmap que el HID report modifier byte (cero
traducción en el firmware, se pasan tal cual):

| bit    | mod     | bit    | mod     |
|--------|---------|--------|---------|
| `0x01` | LCTL    | `0x10` | RCTL    |
| `0x02` | LSFT    | `0x20` | RSFT    |
| `0x04` | LALT    | `0x40` | RALT    |
| `0x08` | LGUI    | `0x80` | RGUI    |

los keycodes son **HID Usage IDs** (los mismos `KC_*` internos de QMK): `A..Z` =
`0x04..0x1D`, `1..9` = `0x1E..0x26`, `0` = `0x27`, `ENTER` = `0x28`, `ESC` =
`0x29`, `BSPC` = `0x2A`, `TAB` = `0x2B`, `SPACE` = `0x2C`. el resto entra por
goteo (`vm_kc_t` en `vm.h`).

## ejemplo: un desensamblado con caña

`--emit=bcdump` te muestra el bytecode instrucción por instrucción.
`fib_stress.qks` (fibonacci iterativo 300000 veces, después teclea "done")
ejercita medio set de opcodes de una sola pasada: density encodings, una
constante grande, aritmética, y los dos jumps del `while`.

```
; qmkscript bytecode v1 -- 41 bytes de code
; header: magic=QKSC version=1 code_len=41

0000  24                      PUSH_0
0001  2c                      STORE_0
0002  25                      PUSH_1
0003  2d                      STORE_1
0004  24                      PUSH_0
0005  2e                      STORE_2
0006  24                      PUSH_0
0007  2f                      STORE_3
0008  2b                      LOAD_3
0009  20 e0 93 04 00          PUSH 300000
000e  42                      LT
000f  13 0e                   JMPZ_S +14
0011  28                      LOAD_0
0012  29                      LOAD_1
0013  30                      ADD
0014  2e                      STORE_2
0015  29                      LOAD_1
0016  2c                      STORE_0
0017  2a                      LOAD_2
0018  2d                      STORE_1
0019  2b                      LOAD_3
001a  25                      PUSH_1
001b  30                      ADD
001c  2f                      STORE_3
001d  12 e9                   JMP_S  -23
001f  01 04 00 ...            STR len=4 "done"
0026  02 28                   TAP kc=0x28
0028  00                      HALT
```

qué está pasando:

- **el setup de las 4 vars** (`a b t i`) compila a puro 1-byte. `var a = 0` es
  `PUSH_0` + `STORE_0`, `var b = 1` es `PUSH_1` + `STORE_1`. las primeras 4 vars
  viven en `LOAD_N`/`STORE_N`, sin gastar ni un byte de operando. el density pass
  laburando.
- **la única constante gorda**: `300000` no entra en un u16, así que sale como
  `PUSH` de 5 bytes (`20 e0 93 04 00`, little-endian). si fuera `<= 255` sería
  `PUSH_U8` (2B), si `<= 65535` sería `PUSH_U16` (3B). el compilador elige la más
  corta que quepa.
- **el test del while** (`i < 300000`): `LOAD_3` (la `i`) + `PUSH 300000` + `LT`,
  y después `JMPZ_S +14`. si `i < 300000` da falso, salta +14 derecho al
  `STR "done"`. si no, cae al body. el `+14` es relativo al byte que sigue al
  operando.
- **el body** es la recurrencia en RPN: `t = a + b` es `LOAD_0` `LOAD_1` `ADD`
  `STORE_2`, y siguen los shifts. `i = i + 1` es `LOAD_3` `PUSH_1` `ADD`
  `STORE_3`.
- **el `JMP_S -23`** cierra el loop: backward jump de vuelta al `LOAD_3` de
  arriba (offset negativo). el compilador ya sabía el target, sin backpatch.
- **el cierre**: `STR len=4 "done"`, `TAP kc=0x28` (enter), `HALT`.

fijate que los dos saltos son la forma CORTA (`JMP_S`/`JMPZ_S`, 1 byte de
offset): los bodies son chicos y entran en `[-128..127]`, así que `relax_jumps()`
los dejó short. si el body fuera más grande, el mismo compilador tiraría
`JMP`/`JMPZ` de i16 sin que toques nada.

lo que el fib no toca lo ves en `hola.qks`, que es más de I/O:

```
; qmkscript bytecode v1 -- 23 bytes de code
; header: magic=QKSC version=1 code_len=23

0000  03 f4 01                DELAY ms=500
0003  04 88 15                CHORD mods=0x88 kc=0x15
0006  03 c8 00                DELAY ms=200
0009  01 08 00 ...            STR len=8 "calc.exe"
0014  02 28                   TAP kc=0x28
0016  00                      HALT
```

- `DELAY ms=500` es `03 f4 01` (`0x01f4` = 500, little-endian).
- `CHORD mods=0x88 kc=0x15` es el `chord [gui, r]`. `0x88` = `0x08 | 0x80` =
  LGUI|RGUI, o sea el `gui` genérico que matchea cualquier lado. `0x15` es la `r`.

entre los dos ejemplos cubrís casi toda la máquina. lo que falta (los mods
sueltos `REG_MODS`/`UNREG_MODS`, el `SWITCH`, el `LAYOUT`, las comparaciones
`EQ`/`NEQ`/`GT`, el resto de la aritmética) sale igual: `--emit=bcdump` sobre el
`.qks` que corresponda y lo ves. para eso está.
