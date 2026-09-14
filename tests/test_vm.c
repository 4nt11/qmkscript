// tests/test_vm.c -- micro-tests unitarios de vm.c.
// Cero frameworks. Cada test es una función void que hace asserts.
// main() las llama en orden y cuenta fallos.
//
// Dos técnicas para observar la VM sin exponer su estado interno:
//   1. capture_ops -- un vm_ops_t que loguea cada callback a un buffer.
//      Sirve para verificar la SECUENCIA y VALORES de efectos disparados.
//   2. bytecode malicioso + assert vm_result_t -- para verificar que los
//      guards LangSec disparan el error correcto ante inputs corruptos.
//
// Aritmética/comparación/jumps se testean INDIRECTAMENTE: programas que
// combinan cálculo + STR condicional, observando qué STR llegó al capture.
#include "vm.h"
#include "vm_errors.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// ============================================================
// Infraestructura: capture ops + helper para construir programas
// ============================================================

typedef enum {
    EV_SEND_STRING, EV_TAP, EV_WAIT_MS, EV_REG_MODS, EV_UNREG_MODS,
} event_kind_t;

typedef struct {
    event_kind_t kind;
    uint32_t     v1;              // kc / ms / mods
    uint16_t     v2;              // len (para send_string)
    char         str[64];         // copia del payload (send_string)
} event_t;

#define MAX_EVENTS 128
static event_t g_events[MAX_EVENTS];
static size_t  g_n_events;

static void cap_reset(void) { g_n_events = 0; }

static void cap_send_string(const uint8_t *b, uint16_t len, void *ctx) {
    (void)ctx;
    assert(g_n_events < MAX_EVENTS);
    event_t *e = &g_events[g_n_events++];
    e->kind = EV_SEND_STRING;
    e->v2 = len;
    size_t copy = len < sizeof(e->str) - 1 ? len : sizeof(e->str) - 1;
    memcpy(e->str, b, copy);
    e->str[copy] = '\0';
}
static void cap_tap(uint8_t kc, void *ctx) {
    (void)ctx;
    assert(g_n_events < MAX_EVENTS);
    g_events[g_n_events++] = (event_t){.kind = EV_TAP, .v1 = kc};
}
static void cap_wait_ms(uint16_t ms, void *ctx) {
    (void)ctx;
    assert(g_n_events < MAX_EVENTS);
    g_events[g_n_events++] = (event_t){.kind = EV_WAIT_MS, .v1 = ms};
}
static void cap_reg_mods(uint8_t m, void *ctx) {
    (void)ctx;
    assert(g_n_events < MAX_EVENTS);
    g_events[g_n_events++] = (event_t){.kind = EV_REG_MODS, .v1 = m};
}
static void cap_unreg_mods(uint8_t m, void *ctx) {
    (void)ctx;
    assert(g_n_events < MAX_EVENTS);
    g_events[g_n_events++] = (event_t){.kind = EV_UNREG_MODS, .v1 = m};
}
static const vm_ops_t capture_ops = {
    .send_string     = cap_send_string,
    .tap_code        = cap_tap,
    .wait_ms         = cap_wait_ms,
    .register_mods   = cap_reg_mods,
    .unregister_mods = cap_unreg_mods,
};

// Envuelve `code` con header QKSC + valida. Aborta el test si el loader
// rechaza (bytecode del test mal construido).
static vm_result_t run(const uint8_t *code, size_t code_len) {
    static uint8_t blob[512];
    assert(code_len + 16 <= sizeof(blob));
    memcpy(blob, "QKSC", 4);
    blob[4] = 1; blob[5] = 0; blob[6] = 0 /*target_mods*/; blob[7] = 0;
    blob[8]  = (uint8_t)(code_len);
    blob[9]  = (uint8_t)(code_len >> 8);
    blob[10] = (uint8_t)(code_len >> 16);
    blob[11] = (uint8_t)(code_len >> 24);
    blob[12] = 0; blob[13] = 0;   /* target_kc = 0 */
    blob[14] = 0; blob[15] = 0;   /* reserved */
    memcpy(blob + 16, code, code_len);

    vm_program_t prog;
    const char *err = NULL;
    vm_result_t r = vm_load_and_validate(blob, 16 + code_len, &prog, &err);
    // C.3.e2: si el loader rechaza (incluye ahora el safety pass de jumps),
    // devolvemos ese error igual. Los guard tests que asertan VM_ERR_JMP_OOB /
    // VM_ERR_TRUNCATED / VM_ERR_UNKNOWN_OP siguen funcionando -- el error se
    // dispara en el loader en vez del dispatch, pero el CÓDIGO es el mismo.
    if (r != VM_OK) return r;
    cap_reset();
    return vm_exec(&prog, &capture_ops, NULL);
}

// Variante que ejecuta SOLO el loader/verifier -- útil para los tests que
// verifican rechazo temprano (C.3.e2 safety pass). Devuelve el resultado
// del loader sin abortar en caso de error.
static vm_result_t load_only(const uint8_t *code, size_t code_len) {
    static uint8_t blob[512];
    assert(code_len + 16 <= sizeof(blob));
    memcpy(blob, "QKSC", 4);
    blob[4] = 1; blob[5] = 0; blob[6] = 0 /*target_mods*/; blob[7] = 0;
    blob[8]  = (uint8_t)(code_len);
    blob[9]  = (uint8_t)(code_len >> 8);
    blob[10] = (uint8_t)(code_len >> 16);
    blob[11] = (uint8_t)(code_len >> 24);
    blob[12] = 0; blob[13] = 0;   /* target_kc = 0 */
    blob[14] = 0; blob[15] = 0;   /* reserved */
    memcpy(blob + 16, code, code_len);

    vm_program_t prog;
    const char *err = NULL;
    return vm_load_and_validate(blob, 16 + code_len, &prog, &err);
}

// ============================================================
// TESTS: efectos observables (opcodes que disparan callbacks)
// ============================================================

static void test_str(void) {
    uint8_t code[] = {
        0x01, 0x05, 0x00, 'h','o','l','a','!',   // STR len=5 "hola!"
        0x00,                                     // HALT
    };
    assert(run(code, sizeof code) == VM_OK);
    assert(g_n_events == 1);
    assert(g_events[0].kind == EV_SEND_STRING);
    assert(g_events[0].v2 == 5);
    assert(memcmp(g_events[0].str, "hola!", 5) == 0);
    puts(" OK   test_str");
}

static void test_tap_delay_chord(void) {
    uint8_t code[] = {
        0x02, 0x28,             // TAP enter
        0x03, 0xC8, 0x00,       // DELAY 200
        0x04, 0x08, 0x15,       // CHORD LGUI + R
        0x00,                   // HALT
    };
    assert(run(code, sizeof code) == VM_OK);
    assert(g_n_events == 5);
    assert(g_events[0].kind == EV_TAP && g_events[0].v1 == 0x28);
    assert(g_events[1].kind == EV_WAIT_MS && g_events[1].v1 == 200);
    assert(g_events[2].kind == EV_REG_MODS && g_events[2].v1 == 0x08);
    assert(g_events[3].kind == EV_TAP && g_events[3].v1 == 0x15);
    assert(g_events[4].kind == EV_UNREG_MODS && g_events[4].v1 == 0x08);
    puts(" OK   test_tap_delay_chord");
}

static void test_reg_unreg_mods_direct(void) {
    // Fase B: REG_MODS + STR + UNREG_MODS (chord con string dentro)
    uint8_t code[] = {
        0x05, 0x02,                   // REG_MODS LSFT
        0x01, 0x03, 0x00, 'p','w','n',// STR "pwn"
        0x06, 0x02,                   // UNREG_MODS LSFT
        0x00,                         // HALT
    };
    assert(run(code, sizeof code) == VM_OK);
    assert(g_n_events == 3);
    assert(g_events[0].kind == EV_REG_MODS && g_events[0].v1 == 0x02);
    assert(g_events[1].kind == EV_SEND_STRING && g_events[1].v2 == 3);
    assert(memcmp(g_events[1].str, "pwn", 3) == 0);
    assert(g_events[2].kind == EV_UNREG_MODS && g_events[2].v1 == 0x02);
    puts(" OK   test_reg_unreg_mods_direct");
}

// ============================================================
// TESTS: aritmética + control (indirecto via JMPZ que decide qué STR)
// ============================================================

static void test_add_then_jmpz_not_taken(void) {
    // PUSH 3, PUSH 4, ADD -> stack=[7]; JMPZ +N no salta porque 7 != 0.
    // Ejecuta STR "hit". Luego HALT antes de STR "miss".
    uint8_t hit[]  = "hit";
    uint8_t miss[] = "miss";
    uint8_t code[] = {
        0x20, 3,0,0,0,                  // PUSH 3
        0x20, 4,0,0,0,                  // PUSH 4
        0x30,                            // ADD -> 7
        0x11, 0x06, 0x00,               // JMPZ +6 (target = HALT en 0x14, boundary
                                         //   post C.3.e2. Antes era +9 -> caía en el
                                         //   medio de STR "miss", ahora eso lo caza
                                         //   el safety pass como weird-machine).
        0x01, 3,0, hit[0],hit[1],hit[2],// STR "hit" (6 bytes)
        0x00,                            // HALT (1 byte)
        0x01, 4,0, miss[0],miss[1],miss[2],miss[3],   // STR "miss" (7 bytes, unreachable)
        0x00,                            // HALT
    };
    assert(run(code, sizeof code) == VM_OK);
    assert(g_n_events == 1);
    assert(g_events[0].kind == EV_SEND_STRING);
    assert(memcmp(g_events[0].str, "hit", 3) == 0);
    puts(" OK   test_add_then_jmpz_not_taken");
}

static void test_sub_zero_jmpz_taken(void) {
    // PUSH 5, PUSH 5, SUB -> stack=[0]; JMPZ SALTA.
    // Salta sobre STR "hit" (6B), ejecuta STR "landed".
    uint8_t code[] = {
        0x20, 5,0,0,0,                            // PUSH 5
        0x20, 5,0,0,0,                            // PUSH 5
        0x31,                                      // SUB -> 0
        0x11, 0x07, 0x00,                         // JMPZ +7 (salta STR "hit"+HALT)
        0x01, 3,0, 'h','i','t',                    // STR "hit" (6B)
        0x00,                                      // HALT (1B) -- offset +7 salta desde el próximo byte
        0x01, 6,0, 'l','a','n','d','e','d',       // STR "landed"
        0x00,                                      // HALT
    };
    assert(run(code, sizeof code) == VM_OK);
    assert(g_n_events == 1);
    assert(g_events[0].kind == EV_SEND_STRING);
    assert(g_events[0].v2 == 6);
    assert(memcmp(g_events[0].str, "landed", 6) == 0);
    puts(" OK   test_sub_zero_jmpz_taken");
}

static void test_store_load_roundtrip(void) {
    // PUSH 42, STORE 0, LOAD 0, JMPZ +N (no salta, 42 != 0), STR "roundtrip"
    uint8_t code[] = {
        0x20, 42,0,0,0,                              // PUSH 42
        0x22, 0x00,                                   // STORE 0
        0x21, 0x00,                                   // LOAD 0
        0x11, 0x01, 0x00,                            // JMPZ +1 (skip HALT si != 0)
        0x00,                                         // HALT (0-offset target)
        0x01, 9,0, 'r','o','u','n','d','t','r','i','p',   // STR "roundtrip" (12B)
        0x00,                                         // HALT
    };
    // 42 != 0 -> JMPZ NO salta. Cae en HALT inmediato.
    // Sin STR emitido. (Test que STORE/LOAD no ensucian el stack.)
    assert(run(code, sizeof code) == VM_OK);
    assert(g_n_events == 0);
    puts(" OK   test_store_load_roundtrip");
}

static void test_eq_true(void) {
    // PUSH 5, PUSH 5, EQ -> stack=[1]. JMPZ NO salta.
    uint8_t code[] = {
        0x20, 5,0,0,0,
        0x20, 5,0,0,0,
        0x40,                                         // EQ -> 1
        0x11, 0x06, 0x00,                            // JMPZ +6
        0x01, 2,0, 'e','q',                          // STR "eq"
        0x00,                                         // HALT
        0x00,                                         // HALT fallback
    };
    assert(run(code, sizeof code) == VM_OK);
    assert(g_n_events == 1);
    assert(memcmp(g_events[0].str, "eq", 2) == 0);
    puts(" OK   test_eq_true");
}

static void test_lt_false(void) {
    // PUSH 5, PUSH 3, LT -> 5 < 3 == false, stack=[0]. JMPZ SALTA.
    uint8_t code[] = {
        0x20, 5,0,0,0,
        0x20, 3,0,0,0,
        0x42,                                         // LT
        0x11, 0x07, 0x00,                            // JMPZ +7
        0x01, 3,0, 'l','t','!',                      // STR "lt!"
        0x00,                                         // HALT
        0x01, 4,0, 'g','o','o','d',                  // STR "good"
        0x00,                                         // HALT
    };
    assert(run(code, sizeof code) == VM_OK);
    assert(g_n_events == 1);
    assert(memcmp(g_events[0].str, "good", 4) == 0);
    puts(" OK   test_lt_false");
}

// ============================================================
// TESTS: guards LangSec (bytecode malicioso)
// ============================================================

static void test_guard_stack_overflow(void) {
    // 33 PUSHes seguidos (VM_STACK_SIZE = 32)
    uint8_t code[1 + 33*5];
    size_t i = 0;
    for (int k = 0; k < 33; k++) {
        code[i++] = 0x20;               // PUSH
        code[i++] = 0; code[i++] = 0; code[i++] = 0; code[i++] = 0;
    }
    code[i++] = 0x00;                    // HALT
    assert(run(code, i) == VM_ERR_STACK_OVERFLOW);
    puts(" OK   test_guard_stack_overflow");
}

static void test_guard_stack_underflow(void) {
    uint8_t code[] = { 0x23, 0x00 };    // POP, HALT
    assert(run(code, sizeof code) == VM_ERR_STACK_UNDERFLOW);
    puts(" OK   test_guard_stack_underflow");
}

static void test_guard_div_zero(void) {
    uint8_t code[] = {
        0x20, 5,0,0,0,                   // PUSH 5
        0x20, 0,0,0,0,                   // PUSH 0
        0x33,                             // DIV
        0x22, 0,                         // STORE 0 (no llega)
        0x00,                             // HALT
    };
    assert(run(code, sizeof code) == VM_ERR_DIV_ZERO);
    puts(" OK   test_guard_div_zero");
}

static void test_guard_var_oob(void) {
    uint8_t code[] = { 0x21, 99, 0x22, 0, 0x00 };  // LOAD 99, STORE 0, HALT
    assert(run(code, sizeof code) == VM_ERR_VAR_OOB);
    puts(" OK   test_guard_var_oob");
}

static void test_guard_jmp_oob_forward(void) {
    uint8_t code[] = { 0x10, 0xFF, 0x7F, 0x00 };   // JMP +32767, HALT
    assert(run(code, sizeof code) == VM_ERR_JMP_OOB);
    puts(" OK   test_guard_jmp_oob_forward");
}

static void test_guard_jmp_oob_backward(void) {
    // JMP -100 (con code_len ~4, offset -100 pasa antes del inicio)
    uint8_t code[] = { 0x10, 0x9C, 0xFF, 0x00 };   // JMP -100
    assert(run(code, sizeof code) == VM_ERR_JMP_OOB);
    puts(" OK   test_guard_jmp_oob_backward");
}

static void test_guard_truncated_str(void) {
    // OP_STR dice len=10 pero solo hay 3 bytes tras el len
    uint8_t code[] = { 0x01, 10, 0, 'a','b','c', 0x00 };
    assert(run(code, sizeof code) == VM_ERR_TRUNCATED);
    puts(" OK   test_guard_truncated_str");
}

static void test_guard_unknown_op(void) {
    // Opcode 0xFF (nunca definido) -> UNKNOWN_OP
    uint8_t code[] = { 0xFF, 0x00 };
    assert(run(code, sizeof code) == VM_ERR_UNKNOWN_OP);
    puts(" OK   test_guard_unknown_op");
}

// ============================================================
// C.3.e: density pass -- short encoding.
// Verificamos que los opcodes cortos producen resultados IDÉNTICOS a las
// formas largas. Semántica invariante, solo cambian los bytes.
// ============================================================
static void test_density_push_short(void) {
    // PUSH_0 + PUSH_1 + ADD -> 1. JMPZ NO salta. STR "d1".
    uint8_t code[] = {
        0x24,                                 // PUSH_0     (1B, era 5B)
        0x25,                                 // PUSH_1     (1B, era 5B)
        0x30,                                 // ADD
        0x11, 0x05, 0x00,                     // JMPZ +5
        0x01, 2, 0, 'd', '1',                 // STR "d1"
        0x00,                                 // HALT
    };
    assert(run(code, sizeof code) == VM_OK);
    assert(g_n_events == 1);
    assert(memcmp(g_events[0].str, "d1", 2) == 0);
    puts(" OK   test_density_push_short");
}

static void test_density_push_u8_u16(void) {
    // PUSH_U8 200 + PUSH_U16 40000 + LT -> 200<40000 = 1. Verifica que
    // la extensión de valor a u32 es CORRECTA (zero-extend, no sign).
    uint8_t code[] = {
        0x26, 200,                            // PUSH_U8 200
        0x27, 0x40, 0x9C,                     // PUSH_U16 40000 (LE)
        0x42,                                 // LT
        0x11, 0x05, 0x00,                     // JMPZ +5
        0x01, 2, 0, 'o', 'k',                 // STR "ok"
        0x00,
    };
    assert(run(code, sizeof code) == VM_OK);
    assert(g_n_events == 1);
    assert(memcmp(g_events[0].str, "ok", 2) == 0);
    puts(" OK   test_density_push_u8_u16");
}

static void test_density_jmp_s_forward(void) {
    // JMP_S +5 salta sobre STR "no" y aterriza en STR "yes".
    //   0000  12 05           JMP_S +5  (pc_after=2, +5=7)
    //   0002  01 02 00 n o    STR "no"  (5 bytes) [unreachable]
    //   0007  01 03 00 y e s  STR "yes" (6 bytes)
    //   000d  00              HALT
    uint8_t code[] = {
        0x12, 0x05,
        0x01, 0x02, 0x00, 'n', 'o',
        0x01, 0x03, 0x00, 'y', 'e', 's',
        0x00,
    };
    assert(run(code, sizeof code) == VM_OK);
    assert(g_n_events == 1);
    assert(memcmp(g_events[0].str, "yes", 3) == 0);
    puts(" OK   test_density_jmp_s_forward");
}
static void test_density_jmpz_s_backward(void) {
    // Loop chico: decrementa var[0] de 3 a 0, cada iter emite STR "x".
    // El body es pequeño -> JMP backward cabe en i8.
    //   0000  26 03           PUSH_U8 3
    //   0002  2c              STORE_0            (var[0] = 3)
    //   0003  28              LOAD_0             <-- loop_start
    //   0004  13 07           JMPZ_S +7          (exit si TOS==0)
    //   0006  01 01 00 x      STR "x"
    //   000a  28              LOAD_0
    //   000b  25              PUSH_1
    //   000c  31              SUB                (var[0]--)
    //   000d  2c              STORE_0
    //   000e  12 f3           JMP_S -13          (vuelve a loop_start = 0x0003)
    //   0010  00              HALT
    uint8_t code[] = {
        0x26, 0x03,
        0x2C,
        0x28,
        0x13, 0x0A,        // JMPZ_S +10 -> pc_after=6, +10=0x10 = HALT
        0x01, 0x01, 0x00, 'x',
        0x28,
        0x25,
        0x31,
        0x2C,
        0x12, 0xF3,        // JMP_S -13 -> pc_after=0x10, -13=0x03 = loop_start
        0x00,
    };
    assert(run(code, sizeof code) == VM_OK);
    // Esperado: 3 iteraciones que emiten "x"
    assert(g_n_events == 3);
    for (int i = 0; i < 3; i++) {
        assert(g_events[i].kind == EV_SEND_STRING);
        assert(memcmp(g_events[i].str, "x", 1) == 0);
    }
    puts(" OK   test_density_jmpz_s_backward");
}
static void test_switch_case_match(void) {
    // switch con key=2 matchea case 2 -> STR "dos". Sin default_body -> el
    // default apunta al end (fall-out) pero no lo tomamos.
    //
    // Layout crafted:
    //   0000 26 02       PUSH_U8 2  (key)
    //   0002 14 02 00    SWITCH n=2
    //   0005 01 00 00 00 key=1
    //   0009 0a 00       off = +10 -> table_end + 10 = 0x0011 + 10 = 0x001b = STR "one"
    //   000b 02 00 00 00 key=2
    //   000f 03 00       off = +3  -> 0x0011 + 3  = 0x0014 = STR "two"
    //   0011 05 00       default_off = +5 -> 0x0016 = end
    //   0013 ??  actually let me recompute...
    //
    // Simpler layout:
    //   0000 26 02          PUSH_U8 2
    //   0002 14 02 00       SWITCH n=2
    //   0005 01 00 00 00    key=1
    //   0009 XX XX          off1
    //   000b 02 00 00 00    key=2
    //   000f XX XX          off2
    //   0011 XX XX          default_off
    //   0013 <-- table_end (all offsets relative here)
    //   0013 01 03 00 o n e STR "one" (6B) [case 1 body]
    //   0019 00             HALT (fall-out)
    //   001a 01 03 00 t w o STR "two" (6B) [case 2 body]
    //   0020 00             HALT
    //   0021 00             HALT (default = fall out to here, unused)
    //
    // off1 (case 1) = 0x0013 - 0x0013 = +0
    // off2 (case 2) = 0x001a - 0x0013 = +7
    // default_off  = 0x0021 - 0x0013 = +14
    uint8_t code[] = {
        0x26, 0x02,                              // PUSH_U8 2
        0x14, 0x02, 0x00,                        // SWITCH n=2
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00,      // case 1 -> +0
        0x02, 0x00, 0x00, 0x00, 0x07, 0x00,      // case 2 -> +7
        0x0E, 0x00,                              // default -> +14
        0x01, 0x03, 0x00, 'o', 'n', 'e',         // STR "one"
        0x00,                                     // HALT (post case 1)
        0x01, 0x03, 0x00, 't', 'w', 'o',         // STR "two"
        0x00,                                     // HALT (post case 2)
        0x00,                                     // HALT (default target)
    };
    assert(run(code, sizeof code) == VM_OK);
    assert(g_n_events == 1);
    assert(memcmp(g_events[0].str, "two", 3) == 0);
    puts(" OK   test_switch_case_match");
}
static void test_switch_default_taken(void) {
    // key=99 no matchea ningún case -> default -> STR "def".
    // Reuso layout: cambio PUSH a 99, mismo resto.
    uint8_t code[] = {
        0x26, 0x63,                              // PUSH_U8 99
        0x14, 0x02, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00,      // case 1 -> +0
        0x02, 0x00, 0x00, 0x00, 0x07, 0x00,      // case 2 -> +7
        0x0E, 0x00,                              // default -> +14
        0x01, 0x03, 0x00, 'o', 'n', 'e',
        0x00,
        0x01, 0x03, 0x00, 't', 'w', 'o',
        0x00,
        0x01, 0x03, 0x00, 'd', 'e', 'f',         // default body
        0x00,
    };
    // NOTA: default_off en el layout previo (+14) apunta a HALT sin body.
    // Aquí necesito recomputar: default_off apunta al inicio del default body.
    // table_end = 0x0013. default body empieza donde antes había HALT solo
    // (offset 0x0021). Necesito +14 -> +14 = 0x0021 exact -- pero ahora tengo
    // 5 bytes menos porque agregué STR "def". Voy a recomputar.
    //   0013 01 03 00 one    STR (6B)
    //   0019 00              HALT
    //   001a 01 03 00 two    STR (6B)
    //   0020 00              HALT
    //   0021 01 03 00 def    STR (6B)  <-- default body starts here
    //   0027 00              HALT
    // default_off = 0x0021 - 0x0013 = +14. OK, layout above es correcto.
    assert(run(code, sizeof code) == VM_OK);
    assert(g_n_events == 1);
    assert(memcmp(g_events[0].str, "def", 3) == 0);
    puts(" OK   test_switch_default_taken");
}
static void test_verify_reject_switch_offset_misaligned(void) {
    // SWITCH con un case offset que aterriza en el medio de STR "hi".
    // Safety pass debe rechazar antes de ejecutar.
    //   0000 26 01          PUSH_U8 1
    //   0002 14 01 00       SWITCH n=1
    //   0005 01 00 00 00    key=1
    //   0009 01 00          off = +1 -> table_end + 1 = 0x000d + 1 = 0x000e (MEDIO de STR "hi")
    //   000b 00 00          default_off = +0 -> 0x000d = start of STR (boundary OK)
    //   000d 01 02 00 h i   STR "hi"
    //   0012 00             HALT
    uint8_t code[] = {
        0x26, 0x01,
        0x14, 0x01, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x01, 0x00,      // key=1, off=+1 (MISALIGNED)
        0x00, 0x00,                               // default = +0 (OK)
        0x01, 0x02, 0x00, 'h', 'i',
        0x00,
    };
    assert(load_only(code, sizeof code) == VM_ERR_JMP_MISALIGNED);
    puts(" OK   test_verify_reject_switch_offset_misaligned");
}
static void test_verify_reject_jmp_s_misaligned(void) {
    // JMP_S +2 aterriza en pos 4, MEDIO del payload de PUSH_U16.
    //   0000  12 02           JMP_S +2  (pc_after=2, +2=4)
    //   0002  27 25 02        PUSH_U16 con bytes que forman opcodes en offset 4
    //   0005  00              HALT
    // Target 0x04 NO es boundary -> safety pass rechaza.
    uint8_t code[] = {
        0x12, 0x02,
        0x27, 0x25, 0x02,
        0x00,
    };
    assert(load_only(code, sizeof code) == VM_ERR_JMP_MISALIGNED);
    puts(" OK   test_verify_reject_jmp_s_misaligned");
}
static void test_density_load_store_short(void) {
    // STORE_2 pop-ea a var[2]; LOAD_2 lo recupera; PUSH_U8 42; EQ -> 1.
    // Verifica que idx codificado en el opcode se decodifica correcto.
    uint8_t code[] = {
        0x26, 42,                             // PUSH_U8 42
        0x2E,                                 // STORE_2   (vars[2] = 42)
        0x2A,                                 // LOAD_2    (push vars[2] = 42)
        0x26, 42,                             // PUSH_U8 42
        0x40,                                 // EQ -> 1
        0x11, 0x05, 0x00,                     // JMPZ +5
        0x01, 2, 0, 'v', '2',                 // STR "v2"
        0x00,
    };
    assert(run(code, sizeof code) == VM_OK);
    assert(g_n_events == 1);
    assert(memcmp(g_events[0].str, "v2", 2) == 0);
    puts(" OK   test_density_load_store_short");
}

// ============================================================
// C.3.e2 SAFETY PASS: verifier de boundaries + jumps.
// Estos tests son la regresión del "weird machine" attack: sin el pass,
// bytecode hostil crafteado podía saltar al medio de instrucciones y
// re-interpretar bytes de operando como opcodes. Con el pass, rechazado
// al load time -- la VM nunca ejecuta un byte.
// ============================================================
static void test_verify_reject_jmp_into_push_u16(void) {
    // JMP +1 aterriza en el byte 0x0004, MEDIO del payload de PUSH_U16.
    //   0000  10 01 00      JMP +1  (pc_after_operand = 3, +1 = 4)
    //   0003  27 25 02      PUSH_U16 0x0225
    //   0006  00            HALT
    // El target (0x0004) NO es boundary -> rechazado.
    uint8_t code[] = {
        0x10, 0x01, 0x00,
        0x27, 0x25, 0x02,
        0x00,
    };
    assert(load_only(code, sizeof code) == VM_ERR_JMP_MISALIGNED);
    puts(" OK   test_verify_reject_jmp_into_push_u16");
}
static void test_verify_reject_jmp_into_str_payload(void) {
    // JMP +2 aterriza dentro del payload de STR "hola".
    //   0000  10 02 00           JMP +2  (pc_after_operand = 3, +2 = 5)
    //   0003  01 04 00 h o l a   STR "hola"
    //   000a  00                 HALT
    // Target 0x0005 = 'h' del payload, no boundary.
    uint8_t code[] = {
        0x10, 0x02, 0x00,
        0x01, 0x04, 0x00, 'h','o','l','a',
        0x00,
    };
    assert(load_only(code, sizeof code) == VM_ERR_JMP_MISALIGNED);
    puts(" OK   test_verify_reject_jmp_into_str_payload");
}
static void test_verify_accept_jmp_to_code_end(void) {
    // JMP a code_len (exit natural del loop) DEBE aceptarse.
    //   0000  10 01 00      JMP +1  (target = 3+1 = 4 = code_len)
    //   0003  00            HALT
    uint8_t code[] = {
        0x10, 0x01, 0x00,
        0x00,
    };
    assert(load_only(code, sizeof code) == VM_OK);
    puts(" OK   test_verify_accept_jmp_to_code_end");
}
static void test_verify_accept_backward_jump_to_boundary(void) {
    // Loop clásico: JMP -N a loop_start. Target ES boundary. Debe pasar.
    //   0000  01 01 00 x    STR "x"   (4 bytes)
    //   0004  10 fc ff      JMP -4    (target = 4+3-4 = 3? no: pc_after=7, -4=3)
    // Hmm, calculo: JMP en pos 4, pc_after_operand = 7, +(-4) = 3. Byte 3 = 'x'
    // del payload -> NO boundary. Cambio a algo que sea boundary real:
    //   0000  02 28         TAP enter (2 bytes)
    //   0002  10 fc ff      JMP -4    (target = 2+3-4 = 1 -> byte 0x28, NO boundary)
    // De nuevo mal. Ajusto: JMP -5 -> target = 5-5 = 0 = boundary (inicio TAP)
    //   0000  02 28         TAP enter
    //   0002  10 fb ff      JMP -5    (target = 5 + (-5) = 0, boundary)
    //   0005  00            HALT (inalcanzable en teoría; el loader NO verifica reachability, ok)
    uint8_t code[] = {
        0x02, 0x28,
        0x10, 0xFB, 0xFF,
        0x00,
    };
    assert(load_only(code, sizeof code) == VM_OK);
    puts(" OK   test_verify_accept_backward_jump_to_boundary");
}
static void test_verify_reject_unknown_op_in_stream(void) {
    // Byte 0xF7 no es opcode. El verifier lo detecta en PASS 1 antes de
    // que el dispatch runtime lo vea.
    uint8_t code[] = {
        0xF7,                        // opcode inválido
        0x00,
    };
    assert(load_only(code, sizeof code) == VM_ERR_UNKNOWN_OP);
    puts(" OK   test_verify_reject_unknown_op_in_stream");
}
static void test_verify_reject_str_length_inflado(void) {
    // STR con length 99 pero solo hay ~3 bytes de payload. Detectado
    // en PASS 1 al pasarse del code segment.
    uint8_t code[] = {
        0x01, 0x63, 0x00, 'a', 'b',   // STR len=99 "ab" ... [se pasa]
        0x00,
    };
    assert(load_only(code, sizeof code) == VM_ERR_TRUNCATED);
    puts(" OK   test_verify_reject_str_length_inflado");
}

// ============================================================
// main
// ============================================================
int main(void) {
    puts("test_vm:");
    // efectos
    test_str();
    test_tap_delay_chord();
    test_reg_unreg_mods_direct();
    // aritmética + control (indirecto)
    test_add_then_jmpz_not_taken();
    test_sub_zero_jmpz_taken();
    test_store_load_roundtrip();
    test_eq_true();
    test_lt_false();
    // density (C.3.e): short encoding
    test_density_push_short();
    test_density_push_u8_u16();
    test_density_load_store_short();
    test_density_jmp_s_forward();
    test_density_jmpz_s_backward();
    test_verify_reject_jmp_s_misaligned();
    // C.3.e2 safety pass: verifier de boundaries y jumps
    test_verify_reject_jmp_into_push_u16();
    test_verify_reject_jmp_into_str_payload();
    test_verify_accept_jmp_to_code_end();
    test_verify_accept_backward_jump_to_boundary();
    test_verify_reject_unknown_op_in_stream();
    test_verify_reject_str_length_inflado();
    // C.3.g: switch dispatch + safety pass
    test_switch_case_match();
    test_switch_default_taken();
    test_verify_reject_switch_offset_misaligned();
    // guards LangSec
    test_guard_stack_overflow();
    test_guard_stack_underflow();
    test_guard_div_zero();
    test_guard_var_oob();
    test_guard_jmp_oob_forward();
    test_guard_jmp_oob_backward();
    test_guard_truncated_str();
    test_guard_unknown_op();
    puts("--- test_vm: all passed ---");
    return 0;
}
