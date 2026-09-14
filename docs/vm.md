# qmkscript: la VM

el intérprete de bytecode. fuente de verdad: `src/vm.c` + `src/vm.h`. el encoding
de las instrucciones está en [`isa.md`](isa.md), acá está la **ejecución**:
loader, verifier, dispatch, guards.

## vendor drop

`vm.h` + `vm.c` van juntos y son **portables**: solo dependen de `<stdint.h>`,
`<stddef.h>` y `<string.h>` (un `memcmp` de 4 bytes para el magic). cero libc
más, cero POSIX, cero heap. compilan idénticos en el host y adentro del firmware
del AN360 (RP2040). lo único que cambia por entorno son los callbacks. copiás los
dos ficheros a tu firmware, le pasás tus funciones, y listo.

el mismo `vm.c` corre en:
- **host**: `qks-run` (intérprete de mesa) y `build/test_vm` (unit tests).
- **firmware AN360**: el intérprete on-device, cargado por raw-HID.

## API

```c
vm_result_t vm_load_and_validate(const uint8_t *blob, size_t blob_len,
                                 vm_program_t *prog_out, const char **err_out);
vm_result_t vm_exec(const vm_program_t *prog, const vm_ops_t *ops, void *ctx);
```

el flujo: `vm_load_and_validate()` valida el shape del `.bin` COMPLETO antes de
ejecutar nada. si pasa, rellena un `vm_program_t` que aliasa el blob. recién ahí
`vm_exec()` lo corre, disparando un callback por cada efecto. recognize before
interpret, siempre en ese orden.

### callbacks (`vm_ops_t`)
```c
typedef struct {
    void (*send_string)     (const uint8_t *bytes, uint16_t len, void *ctx);
    void (*tap_code)        (uint8_t kc, void *ctx);
    void (*wait_ms)         (uint16_t ms, void *ctx);
    void (*register_mods)   (uint8_t mods, void *ctx);
    void (*unregister_mods) (uint8_t mods, void *ctx);
} vm_ops_t;
```
cada opcode con efecto llama a uno de estos. la VM no sabe NADA del entorno: vos
le decís qué significa "teclear" pasándole las funciones. un puntero `NULL` (o
`ops` entero en `NULL`) es no-op silencioso, bueno para dry-run y testing. `ctx`
es opaco, se forwardea igual a cada callback (te sirve para pasar estado sin
globales).

### cómo se escriben

la versión mínima es directa: cada callback llama a la función de QMK que hace la
cosa. algo así:

```c
static void my_tap_code(uint8_t kc, void *ctx)   { (void)ctx; tap_code(kc); }
static void my_wait_ms(uint16_t ms, void *ctx)   { (void)ctx; wait_ms(ms); }
static void my_reg_mods(uint8_t m, void *ctx)    { (void)ctx; register_mods(m); }
static void my_unreg_mods(uint8_t m, void *ctx)  { (void)ctx; unregister_mods(m); }
static void my_send_string(const uint8_t *b, uint16_t n, void *ctx) {
    (void)ctx;
    char tmp[N]; memcpy(tmp, b, n); tmp[n] = '\0';   // send_string quiere \0
    send_string(tmp);
}

static const vm_ops_t ops = {
    .send_string = my_send_string, .tap_code = my_tap_code,
    .wait_ms = my_wait_ms, .register_mods = my_reg_mods,
    .unregister_mods = my_unreg_mods,
};
```
ojo con `send_string`: QMK lo quiere null-terminated y el `bytes` que te pasa la
VM NO lo está. copiás a un buffer local y le pegás el `\0`.

### el ejemplo real (AN360)

el AN360 corre la VM en el **core 1** para no congelar el teclado durante el
payload, y eso complica los callbacks: el USB solo se toca desde el core 0. así
que los callbacks que corren en core 1 NO tocan QMK, ENCOLAN un evento en una
cola lock-free (SPSC), y el core 0 la drena y ahí sí llama a las funciones de
QMK:

```c
// core 1: el callback solo encola, no toca el USB.
static void c1_tap_code(uint8_t kc, void *ctx) {
    (void)ctx;
    evq_push((qks_event_t){QKS_EV_TAP, kc, 0, NULL});
}

// core 0: drena la cola y ahí sí teclea de verdad.
case QKS_EV_TAP: tap_code(ev->b1); break;
```

la excepción es `wait_ms`: ese SÍ corre en el core 1 (busy-wait sobre el timer de
1 MHz del RP2040), porque bloquear el core 1 es justo lo que querés. el core 0
sigue escaneando la matriz, el USB y la LCD como si nada. ese es todo el punto del
offload.

el patrón general: si tu firmware es single-core y simple, los callbacks son
one-liners a QMK. si querés que el teclado siga vivo mientras corre un payload
largo, se vuelven un productor/consumidor entre cores. la VM no cambia, solo
cambia qué metés adentro de los cinco punteros.

### `vm_program_t`
```c
typedef struct {
    const uint8_t *code;    size_t len;
    uint16_t target_kc;     uint8_t target_mods;  uint8_t target_layer;  // bind()
} vm_program_t;
```

## estado de ejecución

todo en el stack de C, ~256 B, sin heap:
- **value stack**: `uint32_t[32]` (`VM_STACK_SIZE`), `sp` = próximo slot libre.
- **variables**: `uint32_t[32]` (`VM_VARS_SIZE`), zero-inicializadas. toda var
  arranca en 0.
- **`current_layout`**: `uint8_t`, arranca en 0 (default). lo cambia `OP_LAYOUT`.

## loader (`vm_load_and_validate`)

chequea en orden y aborta al primer fallo:

1. `blob_len >= 16` (cabe el header), si no `VM_ERR_TOO_SHORT`.
2. magic == `"QKSC"`, si no `VM_ERR_BAD_MAGIC`.
3. version == `1`, si no `VM_ERR_BAD_VERSION`.
4. `16 + code_len == blob_len`, si no `VM_ERR_LEN_MISMATCH`.
5. `code_len <= VM_MAX_CODE_LEN` (4096), si no `VM_ERR_CODE_TOO_LONG`.
6. último byte del code == `OP_HALT`, si no `VM_ERR_NO_HALT`.
7. el **safety pass** sobre el code segment (abajo).

si pasa todo, saca los tres campos del `bind()` del header y setea `prog_out`.

## safety pass langsec (`vm_verify_jumps`)

el problema: con opcodes de longitud variable, un `JMP`/`JMPZ` que aterrice EN EL
MEDIO de otra instrucción hace que la VM reinterprete bytes de operando como
opcodes. es la weird-machine de bratus/sassaman (2011), la misma familia que x86
ROP o la confusión ARM/thumb. nuestro compilador NUNCA emite jumps mal alineados,
pero raw-HID acepta bytecode arbitrario del host. una vez que sale de nuestro
control, asumís hostilidad.

dos pasadas, `O(n)` en tiempo, `O(n/8)` en memoria (bitset stack-allocated, 512 B
para el cap de 4096, cero VLAs):

1. **marcar boundaries**: walk lineal decodificando cada opcode con
   `opcode_length()`, marcando el byte inicial de cada instrucción en un bitset.
   un opcode inválido (`VM_ERR_UNKNOWN_OP`) o un operando cortado
   (`VM_ERR_TRUNCATED`) aborta acá. al final `pc` tiene que ser EXACTAMENTE
   `code_len`, si no un `STR` con length inflado se comería el resto.
2. **verificar targets**: para cada `JMP`/`JMPZ`/`JMP_S`/`JMPZ_S` y para CADA
   entry de la tabla de un `SWITCH` (los N cases + el default), el target tiene
   que caer en una boundary marcada o `== code_len`. si cae en el medio de una
   instrucción, `VM_ERR_JMP_MISALIGNED` y se rechaza el `.bin` entero. fuera de
   `[0, code_len]`, `VM_ERR_JMP_OOB`.

## dispatch (`vm_exec`)

un loop `switch` sobre el opcode. las garantías de bounds van en capas, defensa
en profundidad:
- el loader ya garantizó el `HALT` final y la alineación de jumps.
- la macro `NEED(n)` chequea que cada operando cabe antes de leerlo
  (`VM_ERR_TRUNCATED` si no).
- `PUSH`/`POP` chequean over/underflow del stack en cada uso.
- `LOAD`/`STORE` chequean `idx < 32` (`VM_ERR_VAR_OOB`). las formas `LOAD_N`/
  `STORE_N` NO chequean: `idx` está en `{0,1,2,3} < 32` por construcción.
- los jumps re-chequean bounds en runtime aunque el verifier ya los validó.
  cinturón y tiradores.
- el `default:` del switch caza cualquier byte fuera del enum
  (`VM_ERR_UNKNOWN_OP`), y `-Wswitch-enum` en el Makefile caza los del enum que
  te olvides de manejar. los dos guardianes conviven.

la aritmética es en `uint32_t` (wraparound natural). las comparaciones empujan
`0`/`1`.

## `OP_STR` y el dispatch de layout

`OP_STR` se comporta según `current_layout`:

- **`current_layout == 0`** (default): manda los bytes crudos por el callback
  `send_string` (US-hardcoded en QMK). backward-compat con el firmware existente
  y con `test_vm`.
- **`current_layout > 0`**: itera byte por byte con la tabla
  `qks_layouts[current_layout - 1]` de `layouts_generated.h`:
  - **ASCII** (`< 0x80`): lookup directo, sacás `(kc, mods, dead)`. emite
    `register_mods`/`tap_code`/`unregister_mods` según la entry. las dead keys se
    sellan con un `KC_SPACE` (`0x2C`) extra para que el OS materialice el char.
  - **latin-1** (`0x80..0xFF`, UTF-8 de 2 bytes `110xxxxx 10xxxxxx`): decodifica
    el codepoint y busca en la tabla `extras`. banca hasta 2 taps por char (ej.
    tilde muerta + vocal). codepoints fuera de latin-1 o secuencias UTF-8 rotas
    se ignoran calladas.

esto resuelve el drama de las tildes: `layout(LAYOUT_LATAM)` y todos los `type`
que siguen teclean bien (`ñ`, `¡¿`, `áéíóú`) en el target. `OP_LAYOUT` valida el
rango (`idx <= qks_n_layouts`) o aborta con `VM_ERR_LAYOUT_OOB`. bytecode hostil
no puede desviar `OP_STR` a memoria fuera de `qks_layouts[]`.

## códigos de error (`vm_result_t`)

del lado host, `vm_err_str()` (en `vm_errors.h`) traduce a texto. el firmware
expone solo el código numérico, así ahorra flash.

**loader**: `VM_ERR_TOO_SHORT`, `VM_ERR_BAD_MAGIC`, `VM_ERR_BAD_VERSION`,
`VM_ERR_LEN_MISMATCH`, `VM_ERR_NO_HALT`, `VM_ERR_CODE_TOO_LONG`,
`VM_ERR_JMP_MISALIGNED`.

**runtime**: `VM_ERR_TRUNCATED`, `VM_ERR_UNKNOWN_OP`, `VM_ERR_STACK_OVERFLOW`,
`VM_ERR_STACK_UNDERFLOW`, `VM_ERR_VAR_OOB`, `VM_ERR_JMP_OOB`, `VM_ERR_DIV_ZERO`,
`VM_ERR_LAYOUT_OOB`.

## límites

| constante          | valor | qué acota                              |
|--------------------|-------|----------------------------------------|
| `VM_STACK_SIZE`    | 32    | slots del value stack                  |
| `VM_VARS_SIZE`     | 32    | variables globales                     |
| `VM_MAX_CODE_LEN`  | 4096  | bytes del code segment (= 1 slot de flash del AN360) |

los 4096 coinciden a propósito con el pool de flash por slot single del AN360. el
cap explícito mantiene el bitset del verifier stack-allocated (cero VLAs =
firmware-safe).
