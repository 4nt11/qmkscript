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

## ejemplo

`examples/hola.qks` desensamblado con `--emit=bcdump` te muestra el mapeo
statement a opcodes. un `type "hola"` es `01 04 00 68 6f 6c 61` (STR len=4
"hola"), `tap enter` es `02 28`, y un `while i < 5 { ... }` mezcla `LOAD_N`,
`PUSH_*`, `LT`, `JMPZ_S` y `JMP_S`.
