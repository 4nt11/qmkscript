// qmkscript VM -- ISA v1. Source of truth compartido entre:
//   - host: emit_bytecode.c (compilador) + intérprete de mesa
//   - firmware AN360: intérprete on-device (RP2040)
//
// Regla dura: SOLO <stdint.h>. Nada de libc. Este header tiene que compilar
// dentro de QMK sin arrastrar dependencias.
//
// Fichero .bin:
//   0x00  magic "QKSC"        (4 B)
//   0x04  version = 1         (1 B)
//   0x05  reserved 0x00 x3    (3 B, padding a 8)
//   0x08  code_len u32 LE     (4 B)
//   0x0C  code_len bytes de opcodes...
//
// Multi-byte = little-endian (RP2040 nativo -> cero swaps).
// HALT explícito al final (bucle intérprete más limpio, permite salir antes vía JMP).
#pragma once
#include <stdint.h>
#include <stddef.h>

#define QKS_MAGIC        0x43534B51u   // 'QKSC' little-endian
// Header layout (16 bytes):
//   0x00 magic "QKSC"      (4B)
//   0x04 version = 1       (1B)
//   0x05 flags = 0         (1B, reservado)
//   0x06 target_mods       (1B, bind() -- HID modifier bitmap, 0 si no hay bind)
//   0x07 target_layer      (1B, bind() -- 0..15 layer específico, 0xFF wildcard)
//   0x08 code_len u32 LE   (4B)
//   0x0C target_kc u16 LE  (2B, bind() -- QMK keycode, 0 si no hay bind)
//   0x0E reserved u16      (2B, pad a 16B)
// Backward compat: none. Sin usuarios reales, cambios de layout van
// directamente al firmware -- flash a la nueva versión, push .bin nuevo.
#define QKS_VERSION      1
#define QKS_HEADER_SIZE  16

// Límites duros de la VM (32 valores en el stack, 32 vars globales).
// Cabemos 128 + 128 = 256 B de RAM extra para la VM. Trivial en RP2040.
// Cuando el compilador tenga verification pass (fase C.2), rechazará
// estáticamente los .qks que necesiten más.
#define VM_STACK_SIZE    32
#define VM_VARS_SIZE     32

// Máximo tamaño del code segment (bytes DESPUÉS del header de 12B). El
// loader rechaza bytecode más grande antes de ejecutar. 4096B = 4KB alcanza
// para los payloads reales (nuestro pool en flash del AN360 es 4084B por
// slot single -- coincidencia intencional). El verifier de jumps (C.3.e2)
// asigna un bitset stack-allocated de VM_MAX_CODE_LEN/8 bytes, cap explícito
// = cero VLAs = firmware-safe.
#define VM_MAX_CODE_LEN  4096

typedef enum {
    // --- control ---
    OP_HALT   = 0x00,

    // --- I/O de teclado (v0: emitidos por el compilador) ---
    OP_STR    = 0x01,   // u16 len, bytes[len]     -> send_string(bytes)
    OP_TAP    = 0x02,   // u8 kc                   -> tap_code(kc)
    OP_DELAY  = 0x03,   // u16 ms                  -> wait_ms(ms)
    OP_CHORD  = 0x04,   // u8 mods, u8 kc          -> register mods, tap kc, unregister

    // Opcodes atómicos para chords "gordos" (mods held mientras corre una
    // secuencia de acciones). El compilador los emite alrededor de
    // STR/TAP/... cuando el chord tiene >1 tecla o strings dentro.
    OP_REG_MODS   = 0x05,  // u8 mods              -> register_mods(mods)
    OP_UNREG_MODS = 0x06,  // u8 mods              -> unregister_mods(mods)

    // --- control de flujo (v1: IF/WHILE del compilador) ---
    OP_JMP    = 0x10,   // i16 offset  (relativo al byte tras el operando)
    OP_JMPZ   = 0x11,   // i16 offset  (salta si TOS == 0, pop siempre)

    // C.3.f: short jumps -- i8 offset (2B total en vez de 3B).
    // Semántica idéntica a JMP/JMPZ pero rango de offset [-128..127].
    // El compilador elige short/long via relax_jumps() (fixpoint post-emit).
    OP_JMP_S  = 0x12,   // i8 offset   (relativo al byte tras el operando)
    OP_JMPZ_S = 0x13,   // i8 offset   (salta si TOS == 0, pop siempre)

    // C.3.g: switch con jump table inline. Layout tras el opcode:
    //   u16 n_cases
    //   [u32 key][i16 off]   x n_cases
    //   i16 default_off
    // Longitud total: 3 + 6*n_cases + 2 = 5 + 6*n_cases bytes.
    // Offsets relativos al byte TRAS la tabla completa.
    // Semántica: pop TOS como key, scan lineal por match, si no hay match
    // usa default_off. Sin fall-through (estilo Rust match / Go switch).
    OP_SWITCH = 0x14,

    // C.4.b: cambio de layout runtime. Pop TOS a `current_layout` de la VM.
    //   current_layout = 0 -> OP_STR usa send_string (US-hardcoded en QMK).
    //   current_layout > 0 -> OP_STR itera byte-por-byte usando la tabla
    //   layouts_generated.h::qks_layouts[current_layout - 1].
    // Payloads pueden hacer `layout(LAYOUT_LATAM)` y todos los `type "..."`
    // subsecuentes typean correcto en target LatAm. El "iterate over layouts"
    // pattern (idea ANTI): var i=0; while i<3 { layout(i); type "..."; i++ }
    OP_LAYOUT = 0x15,

    // --- pila y variables ---
    OP_PUSH   = 0x20,   // u32 val
    OP_LOAD   = 0x21,   // u8 idx      -> push vars[idx]
    OP_STORE  = 0x22,   // u8 idx      -> pop  vars[idx]
    OP_POP    = 0x23,   // -            -> descarta TOS (reservado; solo útil
                        //                 cuando aparezcan expression stmts
                        //                 o funciones con return, ninguno
                        //                 existe en el DSL actual)

    // --- density pass (C.3.e): short encoding de constantes y var access ---
    // Filosofía JVM/PDP-11: los patrones más comunes se pagan en 1 byte, el
    // resto sigue por su forma larga. El compilador ELIGE la más corta que
    // quepa; la VM despacha ambas. Zero cambio de semántica, solo de bytes.
    OP_PUSH_0   = 0x24,   // -            -> push 0                (1B: constante más común)
    OP_PUSH_1   = 0x25,   // -            -> push 1                (1B: segunda más común)
    OP_PUSH_U8  = 0x26,   // u8 val       -> push (u32)val         (2B: cubre 0..255)
    OP_PUSH_U16 = 0x27,   // u16 val LE   -> push (u32)val         (3B: cubre 0..65535)

    // LOAD_N/STORE_N: idx codificado en el propio opcode (bits del opcode
    // SON el operando, estilo x86 register encoding). 4 valores contiguos
    // permiten `idx = op - OP_LOAD_0` en el dispatch, un solo case.
    OP_LOAD_0  = 0x28, OP_LOAD_1, OP_LOAD_2, OP_LOAD_3,  // 1B: primeras 4 vars
    OP_STORE_0 = 0x2C, OP_STORE_1, OP_STORE_2, OP_STORE_3,

    // --- aritmética (binaria, TOS = b, TOS-1 = a; empuja a OP b) ---
    OP_ADD    = 0x30,
    OP_SUB    = 0x31,
    OP_MUL    = 0x32,
    OP_DIV    = 0x33,

    // --- comparación (empuja 0 o 1) ---
    OP_EQ     = 0x40,
    OP_NEQ    = 0x41,
    OP_LT     = 0x42,
    OP_GT     = 0x43,
} vm_opcode_t;

// Modificadores: mismo bitmap que el HID report modifier byte.
// Cero traducción en firmware -- se pasan tal cual a register_mods().
typedef enum {
    VMMOD_LCTL = 0x01,
    VMMOD_LSFT = 0x02,
    VMMOD_LALT = 0x04,
    VMMOD_LGUI = 0x08,
    VMMOD_RCTL = 0x10,
    VMMOD_RSFT = 0x20,
    VMMOD_RALT = 0x40,
    VMMOD_RGUI = 0x80,
} vm_mod_t;

// Keycodes = HID Usage IDs. Coinciden con KC_* internos de QMK.
// Solo el subset que v0 emite; el resto se añaden por goteo.
typedef enum {
    VMKC_A     = 0x04, VMKC_B, VMKC_C, VMKC_D, VMKC_E, VMKC_F,
    VMKC_G, VMKC_H, VMKC_I, VMKC_J, VMKC_K, VMKC_L, VMKC_M,
    VMKC_N, VMKC_O, VMKC_P, VMKC_Q, VMKC_R, VMKC_S, VMKC_T,
    VMKC_U, VMKC_V, VMKC_W, VMKC_X, VMKC_Y, VMKC_Z,          // 0x04..0x1D

    VMKC_1     = 0x1E, VMKC_2, VMKC_3, VMKC_4, VMKC_5,
    VMKC_6, VMKC_7, VMKC_8, VMKC_9,                          // 0x1E..0x26
    VMKC_0     = 0x27,

    VMKC_ENTER = 0x28,
    VMKC_ESC   = 0x29,
    VMKC_BSPC  = 0x2A,
    VMKC_TAB   = 0x2B,
    VMKC_SPACE = 0x2C,
} vm_kc_t;

// ============================================================
// RUNTIME API -- todo lo de abajo es el "vendor drop" portable.
// vm.h + vm.c compilan idénticos en host y en firmware AN360.
// La única cosa específica del entorno son los callbacks (vm_ops_t).
// ============================================================

typedef enum {
    VM_OK = 0,
    // Errores del loader (parse-time)
    VM_ERR_TOO_SHORT,      // fichero < 12 bytes
    VM_ERR_BAD_MAGIC,      // primeros 4 bytes != "QKSC"
    VM_ERR_BAD_VERSION,    // version != QKS_VERSION
    VM_ERR_LEN_MISMATCH,   // 12 + code_len != file_size
    VM_ERR_NO_HALT,        // el código no termina en OP_HALT
    VM_ERR_CODE_TOO_LONG,  // code_len > VM_MAX_CODE_LEN (evita bitset overflow)
    VM_ERR_JMP_MISALIGNED, // JMP/JMPZ target cae en el MEDIO de otra instrucción
                           //   (weird-machine / gadget attack -- Bratus 2011)
    // Errores del dispatch (run-time)
    VM_ERR_TRUNCATED,      // opcode con operando cortado por final de code
    VM_ERR_UNKNOWN_OP,     // opcode no reconocido (bytecode de v mayor?)
    // Errores del stack machine (C.1)
    VM_ERR_STACK_OVERFLOW,   // push cuando sp == VM_STACK_SIZE
    VM_ERR_STACK_UNDERFLOW,  // pop cuando sp == 0 (bytecode malformado)
    VM_ERR_VAR_OOB,          // idx de LOAD/STORE >= VM_VARS_SIZE
    VM_ERR_JMP_OOB,          // target de JMP/JMPZ fuera de code
    VM_ERR_DIV_ZERO,         // divisor == 0 en DIV
    VM_ERR_LAYOUT_OOB,       // OP_LAYOUT con TOS > número de layouts registrados
} vm_result_t;

typedef struct {
    const uint8_t *code;
    size_t         len;
    // v2 metadata (bind()). En v1 estos son 0.
    uint16_t       target_kc;    // QMK keycode que dispara este payload (0 = default/QK_KB_0)
    uint8_t        target_mods;  // bitmap HID de modifiers requeridos (0 = ninguno)
    uint8_t        target_layer; // 0..15 layer específico, 0xFF wildcard (any)
} vm_program_t;
#define QKS_LAYER_ANY 0xFF

// Policy: qué hacer con cada evento. NULL en un puntero == no-op silencioso
// (útil para dry-run). El firmware AN360 rellena estos con las funciones
// homónimas de QMK; el host los rellena con impresoras.
typedef struct {
    void (*send_string)     (const uint8_t *bytes, uint16_t len, void *ctx);
    void (*tap_code)        (uint8_t kc, void *ctx);
    void (*wait_ms)         (uint16_t ms, void *ctx);
    void (*register_mods)   (uint8_t mods, void *ctx);
    void (*unregister_mods) (uint8_t mods, void *ctx);
} vm_ops_t;

// Valida un blob completo. En éxito: rellena *prog_out (aliasing al blob).
// En fallo: *err_out apunta a un literal estático explicando qué falló
// (puede pasarse NULL si no interesa).
vm_result_t vm_load_and_validate(const uint8_t *blob, size_t blob_len,
                                 vm_program_t *prog_out,
                                 const char **err_out);

// Ejecuta un programa ya validado. Cada opcode dispara ops-><callback>(ctx).
// Los handlers hacen chequeo de bounds sobre operandos -- el loader NO
// verifica el interior, solo la envoltura, así que aquí somos defensivos.
vm_result_t vm_exec(const vm_program_t *prog, const vm_ops_t *ops, void *ctx);
