// vm.c -- intérprete portable de bytecode qmkscript.
//
// ██ VENDOR DROP: este fichero + vm.h van juntos, cero libc.
// ██ Copia ambos a tu firmware, provee un vm_ops_t con tus funciones
// ██ (tap_code, wait_ms, register_mods, unregister_mods, send_string),
// ██ y llama vm_load_and_validate() -> vm_exec().
//
// Únicas dependencias:
//   <stdint.h>  -- uint8/16/32_t
//   <stddef.h>  -- size_t, NULL
//   <string.h>  -- memcmp (4 bytes de magic). Disponible en newlib RP2040.
//                  Si tu toolchain no lo tiene, sustituye por 4 comparaciones.
#include "vm.h"
#include "vm_internal.h"
#include "layouts_generated.h"   // qks_layouts[N][128], qks_n_layouts (C.4.b)
#include <string.h>

// ============================================================
// helpers de lectura -- little-endian explícito, endianness-agnóstico.
// ============================================================
static uint16_t rd_u16_le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t rd_u32_le(const uint8_t *p) {
    return  (uint32_t)p[0]
         | ((uint32_t)p[1] <<  8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

// ============================================================
// Tabla de longitudes de instrucción -- necesaria para el verifier (C.3.e2)
// y como declaración canónica de la ISA (single source of truth).
//
// Devuelve VM_OK con *out_len = longitud total de la instrucción (opcode +
// operandos). Devuelve VM_ERR_TRUNCATED si el operando (o el length de STR)
// no cabe en el buffer restante. VM_ERR_UNKNOWN_OP si el byte no es opcode.
//
// -Wswitch-enum garantiza que añadir un opcode al enum obligue a añadirlo
// aquí. `default:` cazará bytes de bytecode hostil fuera del rango del enum.
// Defensa en profundidad: guardián estructural (compile-time) + guard runtime.
// ============================================================
static vm_result_t opcode_length(uint8_t op, const uint8_t *at, size_t remaining,
                                 size_t *out_len) {
    size_t n;
    switch ((vm_opcode_t)op) {
        // 1 byte: opcode puro, sin operando
        case OP_HALT:
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV:
        case OP_EQ:  case OP_NEQ: case OP_LT:  case OP_GT:
        case OP_POP:
        case OP_PUSH_0: case OP_PUSH_1:
        case OP_LOAD_0: case OP_LOAD_1: case OP_LOAD_2: case OP_LOAD_3:
        case OP_STORE_0: case OP_STORE_1: case OP_STORE_2: case OP_STORE_3:
        case OP_LAYOUT:   /* C.4.b: pop TOS -> current_layout, sin operando inline */
            n = 1; break;
        // 2 bytes: opcode + 1B operando (u8)
        case OP_TAP:
        case OP_REG_MODS: case OP_UNREG_MODS:
        case OP_LOAD: case OP_STORE:
        case OP_PUSH_U8:
            n = 2; break;
        // 3 bytes: opcode + 2B operando (u16 o i16)
        case OP_DELAY: case OP_CHORD:
        case OP_JMP:  case OP_JMPZ:
        case OP_PUSH_U16:
            n = 3; break;
        // C.3.f: JMP_S/JMPZ_S -- misma familia que TAP (opcode + 1B), pero
        // el 1 byte es un offset SIGNED (i8), no un keycode. Tratamiento
        // idéntico en longitud; el verifier los distingue por opcode.
        case OP_JMP_S: case OP_JMPZ_S:
            n = 2; break;
        // C.3.g: SWITCH -- longitud variable (segundo opcode variable, tras STR).
        // Layout: opcode + u16 n_cases + n_cases*6 (case entries) + 2 (default).
        case OP_SWITCH: {
            if (remaining < 3) return VM_ERR_TRUNCATED;
            uint16_t nc = (uint16_t)at[1] | ((uint16_t)at[2] << 8);
            n = 3u + (size_t)nc * 6u + 2u;
            break;
        }
        // 5 bytes: opcode + 4B operando (u32)
        case OP_PUSH:
            n = 5; break;
        // Variable: opcode + u16 len + len bytes
        case OP_STR: {
            if (remaining < 3) return VM_ERR_TRUNCATED;
            uint16_t slen = (uint16_t)at[1] | ((uint16_t)at[2] << 8);
            n = 3u + slen;
            break;
        }
        default:
            return VM_ERR_UNKNOWN_OP;
    }
    if (n > remaining) return VM_ERR_TRUNCATED;
    *out_len = n;
    return VM_OK;
}

// ============================================================
// C.3.e2 (LangSec safety pass): verifier de boundaries + jumps.
//
// Problema: con opcodes de longitud variable, un JMP/JMPZ que aterrice EN
// EL MEDIO de otra instrucción hace que la VM reinterprete los bytes de
// operando como opcodes (Bratus/Sassaman "weird machine", 2011). Es la
// misma familia de bug que x86 ROP y ARM Thumb/ARM confusion.
//
// Nuestro compilador NUNCA emite jumps mal alineados por construcción,
// pero raw-HID acepta bytecode arbitrario del host -- una vez que sale de
// nuestro control, el operador tiene que asumir hostilidad.
//
// Fix: al load time, hacemos un walk lineal decodificando cada opcode y
// marcando su posición inicial en un bitset. Después verificamos que cada
// target de JMP/JMPZ caiga en una boundary marcada. Si algún target cae
// en el medio de una instrucción, rechazamos el .bin COMPLETO antes de
// ejecutar. LangSec puro: recognize BEFORE interpret.
//
// Costo: O(n) tiempo, O(n/8) memoria stack-allocated. Para code_len máx
// = 4096 => 512 B en stack, trivial en host y RP2040. Cero overhead runtime.
// ============================================================
static vm_result_t vm_verify_jumps(const uint8_t *c, size_t code_len,
                                   const char **err_out) {
    // Bitset: bit i = 1 si el byte i es INICIO de una instrucción válida.
    // Tamaño fijo para evitar VLAs (portable a firmware sin C99 VLA).
    uint8_t boundary[(VM_MAX_CODE_LEN + 7) / 8] = {0};
    #define MARK(pos)     (boundary[(pos) >> 3] |= (uint8_t)(1u << ((pos) & 7)))
    #define IS_BOUND(pos) (boundary[(pos) >> 3] &  (uint8_t)(1u << ((pos) & 7)))

    // PASS 1: decodificar linealmente, marcar boundaries.
    // Si un opcode inválido o un operando cortado aparece en el camino, la
    // función `opcode_length` devuelve el error correspondiente -- basta con
    // propagarlo. Al final pc DEBE ser == code_len exacto; cualquier otro
    // valor significa STR con length inflado que se comería el resto.
    size_t pc = 0;
    while (pc < code_len) {
        MARK(pc);
        size_t ilen;
        vm_result_t r = opcode_length(c[pc], c + pc, code_len - pc, &ilen);
        if (r != VM_OK) {
            *err_out = (r == VM_ERR_UNKNOWN_OP)
                     ? "opcode desconocido: bytecode corrupto o de version futura"
                     : "operando cortado por final de code: bytecode truncado";
            return r;
        }
        pc += ilen;
    }
    if (pc != code_len) {
        *err_out = "STR con length inflado se pasa del code segment";
        return VM_ERR_TRUNCATED;
    }

    // PASS 2: verificar cada JMP/JMPZ. Solo estos dos opcodes cambian pc a
    // un target arbitrario -- todos los demás avanzan secuencialmente.
    // Helper macro para el check de UN target -- reutilizado por jumps y por
    // cada entry de la tabla de OP_SWITCH.
    #define CHECK_TARGET(target)                                            \
        do {                                                                \
            if ((target) < 0 || (size_t)(target) > code_len) {              \
                *err_out = "target de JMP/JMPZ/SWITCH fuera de code";       \
                return VM_ERR_JMP_OOB;                                      \
            }                                                               \
            if ((size_t)(target) < code_len && !IS_BOUND((size_t)(target))) {\
                *err_out = "target aterriza en el MEDIO de otra "           \
                           "instrucción (weird-machine attack): rechazado"; \
                return VM_ERR_JMP_MISALIGNED;                               \
            }                                                               \
        } while (0)

    pc = 0;
    while (pc < code_len) {
        uint8_t op = c[pc];
        if (op == OP_JMP || op == OP_JMPZ) {
            int16_t off = (int16_t)((uint16_t)c[pc+1] | ((uint16_t)c[pc+2] << 8));
            long target = (long)pc + 3 + off;
            CHECK_TARGET(target);
        } else if (op == OP_JMP_S || op == OP_JMPZ_S) {
            int8_t off = (int8_t)c[pc+1];
            long target = (long)pc + 2 + off;
            CHECK_TARGET(target);
        } else if (op == OP_SWITCH) {
            // Cada entry de la tabla trae un target: N cases + 1 default.
            // TODOS deben caer en boundary o == code_len.
            uint16_t nc = (uint16_t)c[pc+1] | ((uint16_t)c[pc+2] << 8);
            size_t table_start = pc + 3;
            size_t table_end   = table_start + (size_t)nc * 6u + 2u;
            for (size_t i = 0; i < (size_t)nc; i++) {
                size_t off_pos = table_start + i * 6u + 4u;   // skip 4B key
                int16_t off = (int16_t)((uint16_t)c[off_pos]
                                      | ((uint16_t)c[off_pos+1] << 8));
                long target = (long)table_end + off;
                CHECK_TARGET(target);
            }
            // default
            size_t def_pos = table_end - 2;
            int16_t def_off = (int16_t)((uint16_t)c[def_pos]
                                      | ((uint16_t)c[def_pos+1] << 8));
            long def_target = (long)table_end + def_off;
            CHECK_TARGET(def_target);
        }
        size_t ilen;
        (void)opcode_length(op, c + pc, code_len - pc, &ilen);
        pc += ilen;
    }
    #undef CHECK_TARGET
    #undef MARK
    #undef IS_BOUND
    return VM_OK;
}

// ============================================================
// LangSec: validación de shape ANTES de dejar que el intérprete toque nada.
// ============================================================
vm_result_t vm_load_and_validate(const uint8_t *blob, size_t blob_len,
                                 vm_program_t *prog_out,
                                 const char **err_out) {
    const char *dummy = NULL;
    if (!err_out) err_out = &dummy;

    if (blob_len < QKS_HEADER_SIZE) {
        *err_out = "fichero < 16 bytes: no cabe ni el header";
        return VM_ERR_TOO_SHORT;
    }
    static const uint8_t magic[4] = {'Q','K','S','C'};
    if (memcmp(blob, magic, 4) != 0) {
        *err_out = "magic != 'QKSC': no es un .bin qmkscript";
        return VM_ERR_BAD_MAGIC;
    }
    if (blob[4] != QKS_VERSION) {
        *err_out = "version incompatible: bytecode de otro compilador";
        return VM_ERR_BAD_VERSION;
    }
    uint32_t code_len = rd_u32_le(blob + 8);
    if ((size_t)QKS_HEADER_SIZE + code_len != blob_len) {
        *err_out = "code_len + header != file_size: fichero truncado o con basura";
        return VM_ERR_LEN_MISMATCH;
    }
    if (code_len > VM_MAX_CODE_LEN) {
        *err_out = "code_len > VM_MAX_CODE_LEN: bytecode más grande que el cap";
        return VM_ERR_CODE_TOO_LONG;
    }
    if (code_len == 0 || blob[QKS_HEADER_SIZE + code_len - 1] != OP_HALT) {
        *err_out = "el código no termina en OP_HALT (0x00)";
        return VM_ERR_NO_HALT;
    }
    // C.3.e2 safety pass sobre el segmento de code.
    vm_result_t vr = vm_verify_jumps(blob + QKS_HEADER_SIZE, code_len, err_out);
    if (vr != VM_OK) return vr;

    prog_out->code        = blob + QKS_HEADER_SIZE;
    prog_out->len         = code_len;
    prog_out->target_mods  = blob[6];
    prog_out->target_layer = blob[7];
    prog_out->target_kc    = (uint16_t)blob[12] | ((uint16_t)blob[13] << 8);
    *err_out = NULL;
    return VM_OK;
}

// ============================================================
// dispatch loop
// ============================================================
// Bounds: el loader garantiza el HALT final; NEED garantiza que cada
// operando cabe. Entre los dos, `c[pc]` es siempre válido.
// NEED / CALL están definidos en vm_internal.h (dependen de scope local).
vm_result_t vm_exec(const vm_program_t *prog, const vm_ops_t *ops, void *ctx) {
    const uint8_t *c = prog->code;
    size_t pc = 0;

    // Estado del stack machine (C.1). Ambos arrays en stack C (~256 B).
    // vars se zero-inicializa: todas las vars empiezan en 0.
    uint32_t stack[VM_STACK_SIZE];
    uint8_t  sp = 0;
    uint32_t vars[VM_VARS_SIZE] = {0};
    // C.4.b: layout runtime. 0 = default (send_string, US-hardcoded). >0 =
    // tables[current_layout-1]. OP_LAYOUT lo actualiza (pop TOS).
    uint8_t  current_layout = 0;

    while (pc < prog->len) {
        uint8_t op = c[pc++];
        switch ((vm_opcode_t)op) {

        case OP_HALT:
            return VM_OK;

        case OP_STR: {
            NEED(2);
            uint16_t slen = rd_u16_le(c + pc); pc += 2;
            NEED(slen);
            if (current_layout == 0) {
                // Default: US via send_string callback (backward-compat con
                // firmware existente, y con test_vm que observa SEND_STRING).
                CALL(send_string, c + pc, slen, ctx);
            } else {
                // C.4.b: per-char lookup via layout table. Cada byte ASCII
                // -> (kc, mods, dead). Emite tap_code y register_mods/
                // unregister_mods según la entry. Dead keys se sellan con
                // KC_SPACE (0x2C) para que el OS los materialize como char.
                const qks_layout_entry_t *table  = qks_layouts[current_layout - 1];
                const qks_layout_extra_t *extras = qks_layouts_extras[current_layout - 1];
                for (uint16_t i = 0; i < slen; i++) {
                    uint8_t b = c[pc + i];
                    if (b < 0x80) {
                        // ASCII: lookup directo en la tabla base.
                        qks_layout_entry_t e = table[b];
                        if (e.kc == 0) continue;
                        if (e.mods) CALL(register_mods, e.mods, ctx);
                        CALL(tap_code, e.kc, ctx);
                        if (e.mods) CALL(unregister_mods, e.mods, ctx);
                        if (e.dead) CALL(tap_code, 0x2C /*KC_SPACE*/, ctx);
                        continue;
                    }
                    // UTF-8: solo aceptamos 2-byte para Latin-1 (0x80..0xFF).
                    // Formato: 110xxxxx 10xxxxxx  -> codepoint = (b&0x1F)<<6 | (b2&0x3F).
                    if ((b & 0xE0) != 0xC0 || i + 1 >= slen) continue;
                    uint8_t b2 = c[pc + i + 1];
                    if ((b2 & 0xC0) != 0x80) continue;
                    uint16_t cp = ((uint16_t)(b & 0x1F) << 6) | (b2 & 0x3F);
                    i++;   // consumimos el 2do byte
                    if (cp > 0xFF) continue;   // fuera de Latin-1
                    qks_layout_extra_t x = extras[cp - 0x80];
                    if (x.n == 0) continue;
                    if (x.mods1) CALL(register_mods,   x.mods1, ctx);
                    CALL(tap_code, x.kc1, ctx);
                    if (x.mods1) CALL(unregister_mods, x.mods1, ctx);
                    if (x.n >= 2) {
                        if (x.mods2) CALL(register_mods,   x.mods2, ctx);
                        CALL(tap_code, x.kc2, ctx);
                        if (x.mods2) CALL(unregister_mods, x.mods2, ctx);
                    }
                }
            }
            pc += slen;
            break;
        }
        case OP_TAP:
            NEED(1);
            CALL(tap_code, c[pc], ctx);
            pc += 1;
            break;

        case OP_DELAY: {
            NEED(2);
            uint16_t ms = rd_u16_le(c + pc); pc += 2;
            CALL(wait_ms, ms, ctx);
            break;
        }
        case OP_CHORD: {
            NEED(2);
            uint8_t mods = c[pc++], kc = c[pc++];
            CALL(register_mods,   mods, ctx);
            CALL(tap_code,        kc,   ctx);
            CALL(unregister_mods, mods, ctx);
            break;
        }

        // Chord "gordo" (fase B): el compilador emite REG_MODS al inicio,
        // luego los items del chord como opcodes normales (STR/TAP/...),
        // y UNREG_MODS al final. Cada uno es atómico y de longitud fija.
        case OP_REG_MODS:
            NEED(1);
            CALL(register_mods, c[pc], ctx);
            pc += 1;
            break;
        case OP_UNREG_MODS:
            NEED(1);
            CALL(unregister_mods, c[pc], ctx);
            pc += 1;
            break;

        // ============================================================
        // Stack + variables (C.1)
        // ============================================================
        case OP_PUSH: {
            NEED(4);
            uint32_t v = (uint32_t)c[pc]
                       | ((uint32_t)c[pc+1] <<  8)
                       | ((uint32_t)c[pc+2] << 16)
                       | ((uint32_t)c[pc+3] << 24);
            pc += 4;
            PUSH(v);
            break;
        }
        case OP_LOAD: {
            NEED(1);
            uint8_t idx = c[pc++];
            if (idx >= VM_VARS_SIZE) return VM_ERR_VAR_OOB;
            PUSH(vars[idx]);
            break;
        }
        case OP_STORE: {
            NEED(1);
            uint8_t idx = c[pc++];
            if (idx >= VM_VARS_SIZE) return VM_ERR_VAR_OOB;
            uint32_t v; POP(v);
            vars[idx] = v;
            break;
        }
        case OP_POP: {
            uint32_t discard; POP(discard); (void)discard;
            break;
        }

        // ============================================================
        // C.4.b: layout switch. Pop TOS, valida rango, actualiza state.
        // TOS = 0 -> default (send_string). > 0 -> tables[TOS - 1].
        // OOB -> VM_ERR_LAYOUT_OOB. Defense in depth: bytecode hostil
        // no puede desviar OP_STR a memoria fuera de qks_layouts[].
        // ============================================================
        case OP_LAYOUT: {
            uint32_t idx; POP(idx);
            if (idx > qks_n_layouts) return VM_ERR_LAYOUT_OOB;
            current_layout = (uint8_t)idx;
            break;
        }

        // ============================================================
        // Density pass (C.3.e): short encoding de constantes y vars.
        // Cero cambio de semántica -- solo bytes. El compilador ELIGE la
        // forma más corta que quepa, la VM despacha ambas.
        // ============================================================
        case OP_PUSH_0: PUSH(0); break;
        case OP_PUSH_1: PUSH(1); break;
        case OP_PUSH_U8: {
            NEED(1);
            PUSH((uint32_t)c[pc]);
            pc += 1;
            break;
        }
        case OP_PUSH_U16: {
            NEED(2);
            PUSH((uint32_t)c[pc] | ((uint32_t)c[pc+1] << 8));
            pc += 2;
            break;
        }
        // Truco 70s: bits del opcode SON el operando. `op - OP_LOAD_0`
        // extrae idx ∈ {0,1,2,3} sin leer un byte extra. Fast path SIN
        // bounds check porque idx < 4 < VM_VARS_SIZE=32 por construcción
        // (los 4 opcodes son contiguos y no hay valor fuera del rango).
        case OP_LOAD_0: case OP_LOAD_1: case OP_LOAD_2: case OP_LOAD_3:
            PUSH(vars[op - OP_LOAD_0]);
            break;
        case OP_STORE_0: case OP_STORE_1: case OP_STORE_2: case OP_STORE_3: {
            uint32_t v; POP(v);
            vars[op - OP_STORE_0] = v;
            break;
        }

        // ============================================================
        // Aritmética. Convención: TOS-1 OP TOS, resultado sustituye ambos.
        // ============================================================
        case OP_ADD: { uint32_t a, b; POP2(a, b); PUSH(a + b); break; }
        case OP_SUB: { uint32_t a, b; POP2(a, b); PUSH(a - b); break; }
        case OP_MUL: { uint32_t a, b; POP2(a, b); PUSH(a * b); break; }
        case OP_DIV: {
            uint32_t a, b; POP2(a, b);
            if (b == 0) return VM_ERR_DIV_ZERO;
            PUSH(a / b);
            break;
        }

        // ============================================================
        // Comparación: pop 2, push 0 (false) o 1 (true).
        // ============================================================
        case OP_EQ:  { uint32_t a, b; POP2(a, b); PUSH(a == b ? 1u : 0u); break; }
        case OP_NEQ: { uint32_t a, b; POP2(a, b); PUSH(a != b ? 1u : 0u); break; }
        case OP_LT:  { uint32_t a, b; POP2(a, b); PUSH(a <  b ? 1u : 0u); break; }
        case OP_GT:  { uint32_t a, b; POP2(a, b); PUSH(a >  b ? 1u : 0u); break; }

        // ============================================================
        // Control de flujo. Offset i16 relativo al byte TRAS el operando.
        // offset == 0 -> no salta (continúa siguiente opcode).
        // offset positivo -> salta adelante, negativo -> atrás.
        // ============================================================
        case OP_JMP: {
            NEED(2);
            int16_t off = (int16_t)((uint16_t)c[pc] | ((uint16_t)c[pc+1] << 8));
            pc += 2;
            // Chequeo de bounds: pc + off debe caer en [0, prog->len].
            // == prog->len es OK (siguiente iteración termina el loop, HALT sentinel).
            // Split en positivo/negativo para evitar aritmética signed sobre size_t
            // (ssize_t no está en <stdint.h>, no queremos dep POSIX en firmware).
            if (off < 0) {
                size_t back = (size_t)(-off);
                if (back > pc) return VM_ERR_JMP_OOB;
                pc -= back;
            } else if (off > 0) {
                size_t fwd = (size_t)off;
                if (pc + fwd > prog->len) return VM_ERR_JMP_OOB;
                pc += fwd;
            }
            break;
        }
        case OP_JMPZ: {
            NEED(2);
            int16_t off = (int16_t)((uint16_t)c[pc] | ((uint16_t)c[pc+1] << 8));
            pc += 2;
            uint32_t cond; POP(cond);
            if (cond == 0) {
                if (off < 0) {
                    size_t back = (size_t)(-off);
                    if (back > pc) return VM_ERR_JMP_OOB;
                    pc -= back;
                } else if (off > 0) {
                    size_t fwd = (size_t)off;
                    if (pc + fwd > prog->len) return VM_ERR_JMP_OOB;
                    pc += fwd;
                }
            }
            break;
        }
        // C.3.f: short jumps -- i8 offset. Semántica idéntica a JMP/JMPZ,
        // solo cambia el tamaño del operando (1B vs 2B). El verifier al
        // load-time garantiza que target ES boundary; los bounds check
        // runtime siguen aquí como defensa en profundidad.
        case OP_JMP_S: {
            NEED(1);
            int8_t off = (int8_t)c[pc];
            pc += 1;
            if (off < 0) {
                size_t back = (size_t)(-(int)off);
                if (back > pc) return VM_ERR_JMP_OOB;
                pc -= back;
            } else if (off > 0) {
                size_t fwd = (size_t)off;
                if (pc + fwd > prog->len) return VM_ERR_JMP_OOB;
                pc += fwd;
            }
            break;
        }
        case OP_JMPZ_S: {
            NEED(1);
            int8_t off = (int8_t)c[pc];
            pc += 1;
            uint32_t cond; POP(cond);
            if (cond == 0) {
                if (off < 0) {
                    size_t back = (size_t)(-(int)off);
                    if (back > pc) return VM_ERR_JMP_OOB;
                    pc -= back;
                } else if (off > 0) {
                    size_t fwd = (size_t)off;
                    if (pc + fwd > prog->len) return VM_ERR_JMP_OOB;
                    pc += fwd;
                }
            }
            break;
        }
        // C.3.g: SWITCH -- dispatch por linear scan de la tabla inline.
        // Layout post-opcode: u16 n_cases, [u32 key, i16 off] x n, i16 default.
        // Semántica: pop key, buscar primer case cuya key coincide, tomar su
        // offset; si ninguno, tomar default. Offset relativo al byte tras la
        // tabla completa (== pc adelantado en table_size).
        case OP_SWITCH: {
            NEED(2);
            uint16_t nc = rd_u16_le(c + pc);
            pc += 2;
            size_t table_bytes = (size_t)nc * 6u + 2u;
            NEED(table_bytes);
            uint32_t key; POP(key);
            int16_t off_taken;
            size_t table_start = pc;
            uint16_t i;
            for (i = 0; i < nc; i++) {
                uint32_t case_key = rd_u32_le(c + table_start + i * 6u);
                if (case_key == key) break;
            }
            if (i < nc) {
                off_taken = (int16_t)rd_u16_le(c + table_start + i * 6u + 4u);
            } else {
                off_taken = (int16_t)rd_u16_le(c + table_start + (size_t)nc * 6u);
            }
            pc += table_bytes;   // avanzar past table
            if (off_taken < 0) {
                size_t back = (size_t)(-off_taken);
                if (back > pc) return VM_ERR_JMP_OOB;
                pc -= back;
            } else if (off_taken > 0) {
                size_t fwd = (size_t)off_taken;
                if (pc + fwd > prog->len) return VM_ERR_JMP_OOB;
                pc += fwd;
            }
            break;
        }

        // default: opcode fuera del enum -> bytecode desconocido o corrupto.
        // NO rompe -Wswitch-enum (que solo grita si un valor DEL ENUM no
        // se maneja). Los dos guardianes coexisten: -Wswitch-enum contra
        // enum values olvidados, `default:` contra valores fuera del enum.
        default:
            return VM_ERR_UNKNOWN_OP;
        }
    }

    // Fin del buffer sin HALT. El loader lo garantizó -- si llegamos aquí,
    // algún handler malformado dejó pc pasarse. Sanity net.
    return VM_ERR_NO_HALT;
}
