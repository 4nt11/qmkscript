// emit_bytecode -- AST -> blob VM (formato ver vm.h).
// Diseño: recorremos el AST empujando bytes a un buffer dinámico, luego
// escupimos header + code + OP_HALT al FILE*. El disassembler recorre el
// mismo buffer decodificando -- reusable como base del intérprete de mesa (v1.5 paso 2).
#include "emit_bytecode.h"
#include "vm.h"          // OP_* + VM_VARS_SIZE
#include "qks_errors.h"  // qks_err_report + tipos
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

// ============================================================
// buffer dinámico -- crece x2, aborta si OOM (v0, sin recuperación).
// ============================================================
typedef struct { uint8_t *data; size_t len, cap; } buf_t;

static void buf_reserve(buf_t *b, size_t extra) {
    if (b->len + extra <= b->cap) return;
    size_t nc = b->cap ? b->cap : 64;
    while (nc < b->len + extra) nc *= 2;
    uint8_t *p = realloc(b->data, nc);
    if (!p) qks_err_report(QKS_ERR_OOM, 0, "realloc(%zu) falló en emit buffer", nc);
    b->data = p; b->cap = nc;
}
static void buf_u8 (buf_t *b, uint8_t  v) { buf_reserve(b, 1); b->data[b->len++] = v; }
static void buf_u16(buf_t *b, uint16_t v) { buf_reserve(b, 2);
    b->data[b->len++] = v & 0xFF; b->data[b->len++] = (v >> 8) & 0xFF; }
static void buf_u32(buf_t *b, uint32_t v) { buf_reserve(b, 4);
    for (int i = 0; i < 4; i++) b->data[b->len++] = (v >> (8*i)) & 0xFF; }
static void buf_bytes(buf_t *b, const void *src, size_t n) {
    buf_reserve(b, n); memcpy(b->data + b->len, src, n); b->len += n;
}

// ============================================================
// helpers de traducción -- el compilador resuelve nombres lowercase a HID.
// Aborta con mensaje si el input no es válido. v1: strict.
// (mods ya vienen como bitmap del parser -- no hay mods_to_bitmap aquí)
// ============================================================
static uint8_t name_to_kc(const char *k, int line) {
    // teclas nombradas
    if (!strcmp(k, "enter") || !strcmp(k, "ent"))  return VMKC_ENTER;
    if (!strcmp(k, "esc"))                          return VMKC_ESC;
    if (!strcmp(k, "bspc") || !strcmp(k, "bs"))    return VMKC_BSPC;
    if (!strcmp(k, "tab"))                          return VMKC_TAB;
    if (!strcmp(k, "space") || !strcmp(k, "spc"))  return VMKC_SPACE;
    // letras / dígitos (case-insensitive por defensa; el lexer siempre lower)
    if (k[0] && k[1] == '\0') {
        char c = k[0];
        if (c >= 'a' && c <= 'z') return VMKC_A + (c - 'a');
        if (c >= 'A' && c <= 'Z') return VMKC_A + (c - 'A');
        if (c == '0')             return VMKC_0;
        if (c >= '1' && c <= '9') return VMKC_1 + (c - '1');
    }
    qks_err_report(QKS_ERR_KEY_UNKNOWN, line, "tecla desconocida '%s'", k);
}

// ============================================================
// Symbol table -- mapa nombre-de-var -> idx (0..VM_VARS_SIZE-1).
// Vive solo durante emit_bytecode() top-level; se resetea antes de cada
// compilación. Búsqueda lineal: 32 entries máx, ~O(32) por lookup, trivial.
// ============================================================
typedef struct { char *name; } sym_entry_t;
static sym_entry_t g_syms[VM_VARS_SIZE];
static size_t      g_n_syms;

static void sym_reset(void) {
    for (size_t i = 0; i < g_n_syms; i++) { free(g_syms[i].name); g_syms[i].name = NULL; }
    g_n_syms = 0;
}
// Devuelve idx (0..) si name está declarado, o -1 si no.
static int sym_lookup(const char *name) {
    for (size_t i = 0; i < g_n_syms; i++)
        if (!strcmp(g_syms[i].name, name)) return (int)i;
    return -1;
}
// Declara una var nueva. Devuelve el idx o aborta con error si:
//   - name ya declarado (evita ambigüedad de idx)
//   - excede VM_VARS_SIZE (verification LangSec temprano)
static int sym_declare(const char *name, int line) {
    if (sym_lookup(name) != -1) {
        qks_err_report(QKS_ERR_VAR_REDECLARED, line,
                       "variable '%s' ya declarada", name);
    }
    if (g_n_syms >= VM_VARS_SIZE) {
        qks_err_report(QKS_ERR_VAR_LIMIT_EXCEEDED, line,
                       "excedido máximo de %d variables (declarando '%s')",
                       VM_VARS_SIZE, name);
    }
    g_syms[g_n_syms].name = strdup(name);
    return (int)g_n_syms++;
}

// ============================================================
// bind() state -- guarda el target al parsear, resuelve al momento de emit
// para escribirlo en el header v2. Global porque bind() es metadata singular
// por programa (duplicate detection = fail).
// ============================================================
static ast_t *g_bind_target = NULL;
static int    g_bind_line   = 0;
static uint8_t g_bind_layer = 0;   // 0 = "no bind"; sino QKS_BIND_LAYER_*

// NO se usa desde emit -- el estado del bind viene del parse. Preservado
// para tests que quisieran reiniciar entre compilaciones en el mismo proceso.
static void bind_reset(void) { g_bind_target = NULL; g_bind_line = 0; g_bind_layer = 0; }
__attribute__((unused)) static void (*bind_reset_ref)(void) = bind_reset;

// Llamado desde parse.y en la acción semántica de KW_BIND LPAREN ... RPAREN.
// Reject temprano si aparece un segundo bind() -- un solo target por payload.
void qks_bind_set(ast_t *target, uint8_t layer, int line) {
    if (g_bind_target) {
        qks_err_report(QKS_ERR_BIND_DUPLICATE, line,
                       "bind() aparece más de una vez en el .qks");
    }
    g_bind_target = target;
    g_bind_line = line;
    g_bind_layer = layer;
}

// Resuelve el AST bind_target a (kc, mods). Llamado en emit_bytecode antes
// del write_header. Si no hay bind() en el .qks, retorna (0, 0) = defaults.
//
// Enumera todos los AST kinds explícitamente por -Wswitch-enum. Los cases
// "no válidos como bind_target" caen al mismo error via fallthrough.
static void bind_resolve(uint16_t *out_kc, uint8_t *out_mods) {
    *out_kc = 0;
    *out_mods = 0;
    if (!g_bind_target) return;
    switch (g_bind_target->kind) {
        case AST_NUM_LIT:
            // bind(KC_R) o bind(0x15). Valor directo.
            if (g_bind_target->s_num.value > 0xFFFFu) {
                qks_err_report(QKS_ERR_BIND_INVALID, g_bind_line,
                               "bind(value=%u): keycode > 0xFFFF no cabe en u16",
                               g_bind_target->s_num.value);
            }
            *out_kc = (uint16_t)g_bind_target->s_num.value;
            return;
        case AST_TAP:
            // bind(tap enter) -- resolver el nombre a keycode via name_to_kc.
            *out_kc = name_to_kc(g_bind_target->s_tap.key, g_bind_line);
            return;
        case AST_CHORD:
            // bind(chord [gui, r]) -- simple chord (mods + 1 tecla).
            *out_kc   = name_to_kc(g_bind_target->s_chord.key, g_bind_line);
            *out_mods = g_bind_target->s_chord.mods;
            return;
        // Todos los siguientes NO son válidos como bind target. Fallthrough
        // al error handler. -Wswitch-enum obliga a enumerar cada uno.
        case AST_CHORD_SEQ: /* chord multi-elem o con string -- no válido */
        case AST_STRING:
        case AST_DELAY:
        case AST_VAR_REF:
        case AST_VAR_DECL:
        case AST_ASSIGN:
        case AST_BIN_OP:
        case AST_BLOCK:
        case AST_IF:
        case AST_WHILE:
        case AST_SWITCH:
        case AST_CASE:
        case AST_LAYOUT:
        case AST_PROGRAM:
            qks_err_report(QKS_ERR_BIND_INVALID, g_bind_line,
                           "bind() target inválido para el kind AST %d "
                           "(esperado: NUMBER/KC_X, `tap X`, o `chord [mods, X]`)",
                           (int)g_bind_target->kind);
    }
}

// ============================================================
// DENSITY helpers (C.3.e) -- short encoding estilo JVM/PDP-11.
// El compilador ELIGE la forma más corta que quepa; la VM despacha ambas.
// La elección aquí es TRIVIAL porque depende del VALOR/IDX que ya conocemos
// en el emit -- no del offset final (los jumps son un problema aparte, C.3.f).
// ============================================================
static void emit_push_const(buf_t *b, uint32_t v) {
    if      (v == 0)         buf_u8(b, OP_PUSH_0);                                      // 1B
    else if (v == 1)         buf_u8(b, OP_PUSH_1);                                      // 1B
    else if (v <= 0xFFu)   { buf_u8(b, OP_PUSH_U8);  buf_u8 (b, (uint8_t)v);  }         // 2B
    else if (v <= 0xFFFFu) { buf_u8(b, OP_PUSH_U16); buf_u16(b, (uint16_t)v); }         // 3B
    else                   { buf_u8(b, OP_PUSH);     buf_u32(b, v); }                   // 5B (fallback)
}
static void emit_load_var(buf_t *b, uint8_t idx) {
    if (idx < 4) buf_u8(b, (uint8_t)(OP_LOAD_0 + idx));                                 // 1B
    else       { buf_u8(b, OP_LOAD); buf_u8(b, idx); }                                  // 2B
}
static void emit_store_var(buf_t *b, uint8_t idx) {
    if (idx < 4) buf_u8(b, (uint8_t)(OP_STORE_0 + idx));                                // 1B
    else       { buf_u8(b, OP_STORE); buf_u8(b, idx); }                                 // 2B
}

// ============================================================
// JUMP TABLE + RELAXATION (C.3.b + C.3.f).
//
// C.3.b introdujo backpatching: emit del jump con placeholder, patch cuando
// se conoce el target. Escribía los offset bytes al momento del patch.
// C.3.f añade JMP_S/JMPZ_S (i8 offset). Ahora los emit_* de jumps SOLO
// REGISTRAN el jump en g_jumps[] -- los offset bytes se escriben al FINAL,
// después de relax_jumps() (fixpoint pesimista de shrinks).
//
// Fases del emit:
//   1. walker recursivo (emit_node): emite todo, incluyendo jumps con opcode
//      LONG y placeholder 0x00 0x00. Cada jump se registra en g_jumps.
//   2. relax_jumps(): iteración pesimista -- todos empiezan LONG (siempre
//      cabe). Para cada uno, intentar shrink a SHORT si offset entra en i8.
//      Cada shrink es un byte splice + actualización de todas las positions.
//      Terminación: cada shrink reduce el buffer 1 byte, monotónico, ≤N iters.
//   3. finalize_offsets(): con positions finales, escribir offset bytes.
//
// Pesimismo > optimismo porque el estado inicial "todos long" es VÁLIDO
// POR CONSTRUCCIÓN. Cada intento de shrink es INDEPENDIENTE, sin backtrack.
// LLVM BranchRelaxation, GNU `as` desde 1988, todos usan variantes de esto.
//
// Offset RELATIVO al byte tras el operando:
//   long form: pc_after = pos + 3
//   short form: pc_after = pos + 2
// ============================================================

// C.3.g: dos kinds de records. JR_JUMP = jump normal con opcode al frente,
// candidato a shrink en relax_jumps. JR_TABLE_ENTRY = 2 bytes de offset
// dentro de una tabla de OP_SWITCH, SIN opcode delante -- pos apunta a
// los 2 bytes de offset directamente. NUNCA shrink (fixed-size table).
typedef enum { JR_JUMP, JR_TABLE_ENTRY } jr_kind_t;

typedef struct {
    jr_kind_t kind;
    size_t    pos;     // JR_JUMP: opcode pos. JR_TABLE_ENTRY: offset-bytes pos.
    uint8_t   opcode;  // solo JR_JUMP: OP_JMP/OP_JMPZ (long) o OP_JMP_S/OP_JMPZ_S (short)
    size_t    target;  // posición ABSOLUTA del target (actualizada en shrinks)
    size_t    ref_pc;  // JR_TABLE_ENTRY: posición absoluta del byte tras la tabla
                       //   (aka table_end). offset final = target - ref_pc.
                       //   Se actualiza junto con pos/target en shifts porque
                       //   ambos están DENTRO del switch (o después). Ignored for JR_JUMP.
    int       line;    // para errores
} jump_rec_t;

#define MAX_JUMPS_PER_PROG 256
static jump_rec_t g_jumps[MAX_JUMPS_PER_PROG];
static size_t     g_n_jumps;

static void jumps_reset(void) { g_n_jumps = 0; }

static void jump_record(size_t pos, uint8_t op, size_t target, int line) {
    if (g_n_jumps >= MAX_JUMPS_PER_PROG) {
        qks_err_report(QKS_ERR_OOM, line,
                       "más de %d jumps en un programa (raise MAX_JUMPS_PER_PROG)",
                       MAX_JUMPS_PER_PROG);
    }
    g_jumps[g_n_jumps++] = (jump_rec_t){JR_JUMP, pos, op, target, 0, line};
}
// C.3.g: registra un slot de tabla de switch (2 bytes de offset sin opcode).
// pos = posición de los 2 bytes de offset. target = case body pos absoluto.
// ref_pc = posición absoluta del byte tras la tabla del switch (offset se
// computa como target - ref_pc). Nunca shrinkea; se resuelve como i16.
static void jump_record_table_entry(size_t pos, size_t target, size_t ref_pc, int line) {
    if (g_n_jumps >= MAX_JUMPS_PER_PROG) {
        qks_err_report(QKS_ERR_OOM, line,
                       "más de %d jumps en un programa", MAX_JUMPS_PER_PROG);
    }
    g_jumps[g_n_jumps++] = (jump_rec_t){JR_TABLE_ENTRY, pos, 0, target, ref_pc, line};
}
static jump_rec_t *jump_find_by_hole(size_t hole_pos) {
    // hole_pos == opcode_pos + 1
    for (size_t i = 0; i < g_n_jumps; i++) {
        if (g_jumps[i].pos + 1 == hole_pos) return &g_jumps[i];
    }
    return NULL;
}

// Emite un jump forward con placeholder de 2 bytes (forma long). Se registra
// en g_jumps con target=0 (se fijará por patch_jump_to_here). Devuelve la
// posición del hueco (== opcode_pos + 1) para que el caller lo backpatchee.
static size_t emit_jump_placeholder(buf_t *b, uint8_t op) {
    size_t pos = b->len;
    buf_u8(b, op);
    size_t hole = b->len;
    buf_u8(b, 0x00);
    buf_u8(b, 0x00);
    jump_record(pos, op, 0, 0);   // target=0, será actualizado por patch
    return hole;
}
// Fija el target del jump identificado por hole_pos. NO escribe offset bytes
// (los offsets se escriben en finalize_offsets() tras relax).
static void patch_jump_to_here(buf_t *b, size_t hole_pos, int line) {
    jump_rec_t *j = jump_find_by_hole(hole_pos);
    if (!j) {
        qks_err_report(QKS_ERR_OOM, line,
                       "bug interno: hueco de jump en pos %zu no encontrado", hole_pos);
    }
    j->target = b->len;
    j->line   = line;
}
// C.3.d: jump backward con target ya conocido (loop start del while). Emite
// forma long + registra con target absoluto. relax_jumps luego decidirá short.
static void emit_jump_to_target(buf_t *b, uint8_t op, size_t target_pos, int line) {
    size_t pos = b->len;
    buf_u8(b, op);
    buf_u8(b, 0x00);
    buf_u8(b, 0x00);
    jump_record(pos, op, target_pos, line);
}

// ============================================================
// C.3.f: relax_jumps -- fixpoint pesimista.
// Cada shrink: opcode long -> short, byte splice, actualiza positions.
// Monotónico: buffer solo mengua. Termina en ≤ N iteraciones.
// ============================================================
static void relax_jumps(buf_t *b) {
    int changed = 1;
    while (changed) {
        changed = 0;
        for (size_t i = 0; i < g_n_jumps; i++) {
            jump_rec_t *j = &g_jumps[i];
            // Table entries de switch nunca se shrinkean -- la regularidad
            // 6 bytes por slot (u32 key + i16 off) es NO negociable.
            if (j->kind == JR_TABLE_ENTRY) continue;
            if (j->opcode == OP_JMP_S || j->opcode == OP_JMPZ_S) continue;

            // Offset en el layout POST-shrink hipotético:
            //   forward (target > pos+2): target shifta -1 al remover byte;
            //     pc_after también shifta -- neto: new_off = old_long_off
            //     = target - (pos + 3).
            //   backward (target <= pos+2): target no shifta; pc_after es
            //     pos+2 (era pos+3) -- neto: new_off = target - (pos + 2)
            //     = old_long_off + 1.
            long new_off = (j->target > j->pos + 2)
                         ? (long)j->target - (long)(j->pos + 3)
                         : (long)j->target - (long)(j->pos + 2);
            if (new_off < -128 || new_off > 127) continue;

            // Shrink.
            j->opcode = (j->opcode == OP_JMP) ? OP_JMP_S : OP_JMPZ_S;
            b->data[j->pos] = j->opcode;
            memmove(&b->data[j->pos + 2], &b->data[j->pos + 3], b->len - (j->pos + 3));
            b->len -= 1;
            // Positions > (pos+2) shiftan -1 (nuestro propio pos NO shifta;
            // nuestro propio target shifta si estaba después del splice).
            size_t threshold = j->pos + 2;
            for (size_t k = 0; k < g_n_jumps; k++) {
                if (k != i && g_jumps[k].pos    > threshold) g_jumps[k].pos    -= 1;
                if           (g_jumps[k].target > threshold) g_jumps[k].target -= 1;
                // ref_pc solo usado para JR_TABLE_ENTRY; para JR_JUMP es 0.
                // Un ref_pc=0 nunca supera threshold (threshold ≥ 2), safe.
                if           (g_jumps[k].ref_pc > threshold) g_jumps[k].ref_pc -= 1;
            }
            changed = 1;
        }
    }
}
// Escribe los offset bytes finales con las positions ya estables.
static void finalize_offsets(buf_t *b) {
    for (size_t i = 0; i < g_n_jumps; i++) {
        jump_rec_t *j = &g_jumps[i];
        if (j->kind == JR_TABLE_ENTRY) {
            // 2 bytes de offset en j->pos (sin opcode). Offset relativo a
            // j->ref_pc (== byte tras la tabla del switch). Ambos pos y
            // ref_pc shiftan juntos si algún shrink cae entre la tabla y
            // el target (dentro del body de un case).
            long off = (long)j->target - (long)j->ref_pc;
            if (off < INT16_MIN || off > INT16_MAX) {
                qks_err_report(QKS_ERR_JUMP_TOO_FAR, j->line,
                               "offset de switch (%ld) excede rango i16", off);
            }
            b->data[j->pos]     = (uint8_t)(off & 0xFF);
            b->data[j->pos + 1] = (uint8_t)((off >> 8) & 0xFF);
            continue;
        }
        int is_short = (j->opcode == OP_JMP_S || j->opcode == OP_JMPZ_S);
        size_t pc_after = j->pos + (is_short ? 2u : 3u);
        long off = (long)j->target - (long)pc_after;
        if (is_short) {
            if (off < -128 || off > 127) {
                qks_err_report(QKS_ERR_JUMP_TOO_FAR, j->line,
                               "bug interno: short jump fuera de rango post-relax");
            }
            b->data[j->pos + 1] = (uint8_t)(off & 0xFF);
        } else {
            if (off < INT16_MIN || off > INT16_MAX) {
                qks_err_report(QKS_ERR_JUMP_TOO_FAR, j->line,
                               "salto de %ld bytes excede el rango i16 [-32768..32767]",
                               off);
            }
            b->data[j->pos + 1] = (uint8_t)(off & 0xFF);
            b->data[j->pos + 2] = (uint8_t)((off >> 8) & 0xFF);
        }
    }
}

// ============================================================
// walker AST -> buffer
// ============================================================
static void emit_node(buf_t *b, const ast_t *n) {
    switch (n->kind) {
        case AST_STRING: {
            size_t len = strlen(n->s_string.text);
            if (len > 0xFFFF) {
                qks_err_report(QKS_ERR_STRING_TOO_LONG, n->line,
                               "STRING de %zu bytes no cabe en OP_STR u16 (max 65535)", len);
            }
            buf_u8(b, OP_STR);
            buf_u16(b, (uint16_t)len);
            buf_bytes(b, n->s_string.text, len);
            break;
        }
        case AST_TAP:
            buf_u8(b, OP_TAP);
            buf_u8(b, name_to_kc(n->s_tap.key, n->line));
            break;
        case AST_DELAY:
            if (n->s_delay.ms < 0 || n->s_delay.ms > 0xFFFF) {
                qks_err_report(QKS_ERR_DELAY_OUT_OF_RANGE, n->line,
                               "delay %d fuera de rango (0..65535)", n->s_delay.ms);
            }
            buf_u8(b, OP_DELAY);
            buf_u16(b, (uint16_t)n->s_delay.ms);
            break;
        case AST_CHORD:
            buf_u8(b, OP_CHORD);
            buf_u8(b, n->s_chord.mods);            // bitmap ya listo del parser
            buf_u8(b, name_to_kc(n->s_chord.key, n->line));
            break;
        case AST_CHORD_SEQ:
            // Chord "gordo": mods held durante una secuencia de acciones.
            // Emitido como REG_MODS + emit_node(cada action) + UNREG_MODS.
            // La recursión sobre las actions reutiliza los cases de STR/TAP
            // sin código nuevo -- cada action se codifica igual que si
            // estuviera en el top-level.
            buf_u8(b, OP_REG_MODS);
            buf_u8(b, n->s_chord_seq.mods);
            for (size_t i = 0; i < n->s_chord_seq.n; i++)
                emit_node(b, n->s_chord_seq.actions[i]);
            buf_u8(b, OP_UNREG_MODS);
            buf_u8(b, n->s_chord_seq.mods);
            break;
        case AST_NUM_LIT:
            // C.3.e: emit_push_const elige la forma más corta según el valor.
            // Constantes 0/1 -> 1B, 2..255 -> 2B, 256..65535 -> 3B, resto -> 5B.
            emit_push_const(b, n->s_num.value);
            break;
        case AST_VAR_REF: {
            // OP_LOAD idx -- busca en symbol table. Var no declarada ->
            // error compile-time. NO se emite un LOAD a idx "por defecto"
            // ni nada por el estilo -- filosofía "NO ESCAPE HATCHES".
            int idx = sym_lookup(n->s_var_ref.name);
            if (idx < 0) {
                qks_err_report(QKS_ERR_VAR_UNDECLARED, n->line,
                               "variable '%s' no declarada", n->s_var_ref.name);
            }
            emit_load_var(b, (uint8_t)idx);   // C.3.e: LOAD_0..3 (1B) o LOAD (2B)
            break;
        }
        case AST_VAR_DECL: {
            // emit(init_expr)  -> deja el valor en TOS
            // OP_STORE idx     -> pop TOS a vars[idx]
            emit_node(b, n->s_var_decl.init);
            int idx = sym_declare(n->s_var_decl.name, n->line);
            emit_store_var(b, (uint8_t)idx);  // C.3.e: STORE_0..3 (1B) o STORE (2B)
            break;
        }
        case AST_ASSIGN: {
            // Reasignación: la var DEBE existir ya. Var no declarada ->
            // error compile-time (mismo enum que VAR_REF). NO se crea
            // silenciosamente la var como en Python -- filosofía "todo
            // explícito".
            int idx = sym_lookup(n->s_assign.name);
            if (idx < 0) {
                qks_err_report(QKS_ERR_VAR_UNDECLARED, n->line,
                               "no se puede reasignar '%s': variable no declarada (usa 'var' primero)",
                               n->s_assign.name);
            }
            emit_node(b, n->s_assign.value);
            emit_store_var(b, (uint8_t)idx);
            break;
        }
        case AST_BIN_OP: {
            // POSTORDER walk = RPN. emit(lhs) deja lhs en stack, emit(rhs)
            // deja rhs encima, y el opcode binario pop-ea 2 y push-ea 1.
            // 1 línea de emit + 1 opcode por operador. Aquí es donde el
            // stack machine + AST recursivo se alinean con naturalidad.
            emit_node(b, n->s_bin_op.lhs);
            emit_node(b, n->s_bin_op.rhs);
            static const uint8_t bin_to_op[] = {
                [BIN_ADD] = OP_ADD, [BIN_SUB] = OP_SUB,
                [BIN_MUL] = OP_MUL, [BIN_DIV] = OP_DIV,
                [BIN_EQ]  = OP_EQ,  [BIN_NEQ] = OP_NEQ,
                [BIN_LT]  = OP_LT,  [BIN_GT]  = OP_GT,
            };
            buf_u8(b, bin_to_op[n->s_bin_op.op]);
            break;
        }
        case AST_BLOCK:
            // C.3.a: un bloque emite EXACTAMENTE los mismos bytes que sus stmts
            // sueltos. Cero overhead runtime. La existencia del nodo AST es
            // puramente para tener un container tipado que if/while (C.3.b+)
            // puedan usar como then/else/body.
            for (size_t i = 0; i < n->s_block.n; i++) emit_node(b, n->s_block.stmts[i]);
            break;
        case AST_IF: {
            // C.3.b/c: if cond { then } [else { else }]
            //
            // Layout SIN else (C.3.b):
            //   emit(cond)
            //   JMPZ hole1     ; salta al final si cond es 0
            //   emit(then)
            //   patch(hole1) --> aquí
            //
            // Layout CON else (C.3.c) -- dos backpatches:
            //   emit(cond)
            //   JMPZ hole1     ; salta al ELSE si cond es 0
            //   emit(then)
            //   JMP  hole2     ; después del then, salta al FINAL (evita else)
            //   patch(hole1) --> aquí (inicio del else)
            //   emit(else)
            //   patch(hole2) --> aquí (final total)
            //
            // Reusamos el MISMO helper emit_jump_placeholder para JMPZ y JMP
            // (toma el opcode como arg). Este es el pago del diseño de C.3.b:
            // añadir else es 3 líneas de emit, cero código nuevo en helpers.
            emit_node(b, n->s_if.cond);
            size_t hole1 = emit_jump_placeholder(b, OP_JMPZ);
            emit_node(b, n->s_if.then_body);
            if (n->s_if.else_body) {
                size_t hole2 = emit_jump_placeholder(b, OP_JMP);
                patch_jump_to_here(b, hole1, n->line);   // else empieza aquí
                emit_node(b, n->s_if.else_body);
                patch_jump_to_here(b, hole2, n->line);   // final total aquí
            } else {
                patch_jump_to_here(b, hole1, n->line);   // sin else: final = target de JMPZ
            }
            break;
        }
        case AST_SWITCH: {
            // C.3.g: switch key { case N: body ... default: body }
            //
            // Layout emitido:
            //   emit(key)                    ; TOS = key
            //   OP_SWITCH + u16 n_cases      ; 3 bytes
            //   [key1:u32][off1:i16]         ; 6 bytes por case (n veces)
            //   ...
            //   [default_off:i16]            ; 2 bytes
            // table_end:                     ; ref_pc para todos los offsets
            //   emit(case1_body); JMP end
            //   emit(case2_body); JMP end
            //   ...
            //   emit(default_body)           ; sin JMP (cae a end natural)
            // end:
            //
            // Detección compile-time de duplicate cases: linear scan sobre
            // los values ya vistos. Reject temprano con E_DUPLICATE_CASE.
            //
            // Cada offset de la tabla se registra como JR_TABLE_ENTRY con
            // ref_pc = table_end. Cada JMP-a-end tras case body es un
            // JR_JUMP normal (posible shrink a JMP_S en relax_jumps).

            // Duplicate case check
            for (size_t i = 0; i < n->s_switch.n; i++) {
                for (size_t j = i + 1; j < n->s_switch.n; j++) {
                    if (n->s_switch.values[i] == n->s_switch.values[j]) {
                        qks_err_report(QKS_ERR_DUPLICATE_CASE, n->line,
                                       "case duplicado: valor %u aparece dos veces en el mismo switch",
                                       n->s_switch.values[i]);
                    }
                }
            }
            if (n->s_switch.n > 0xFFFF) {
                qks_err_report(QKS_ERR_SWITCH_TOO_LARGE, n->line,
                               "switch con %zu cases (max 65535)", n->s_switch.n);
            }

            emit_node(b, n->s_switch.key);
            size_t switch_op_pos = b->len;
            buf_u8(b, OP_SWITCH);
            buf_u16(b, (uint16_t)n->s_switch.n);

            // Reservar la tabla completa (placeholders 0x00). Anotamos las
            // posiciones de cada slot de offset para el backpatch después.
            size_t entry_off_positions[MAX_JUMPS_PER_PROG];   // slots [4..5] de cada entry
            if (n->s_switch.n > MAX_JUMPS_PER_PROG) {
                qks_err_report(QKS_ERR_OOM, n->line,
                               "switch con más de %d cases (raise MAX_JUMPS_PER_PROG)",
                               MAX_JUMPS_PER_PROG);
            }
            for (size_t i = 0; i < n->s_switch.n; i++) {
                buf_u32(b, n->s_switch.values[i]);   // key: 4 bytes
                entry_off_positions[i] = b->len;
                buf_u16(b, 0);                       // offset placeholder: 2 bytes
            }
            size_t default_off_pos = b->len;
            buf_u16(b, 0);                           // default offset placeholder
            size_t table_end = b->len;
            (void)switch_op_pos;

            // Emitir cada case body seguido de un JMP a `end` (backpatch tras).
            // Anotamos también cada case_body_start para backpatchear la tabla.
            size_t jmp_holes[MAX_JUMPS_PER_PROG];   // hole positions de los JMP-a-end
            for (size_t i = 0; i < n->s_switch.n; i++) {
                size_t body_start = b->len;
                // Backpatch table entry offset a body_start.
                jump_record_table_entry(entry_off_positions[i], body_start,
                                        table_end, n->line);
                emit_node(b, n->s_switch.bodies[i]);
                jmp_holes[i] = emit_jump_placeholder(b, OP_JMP);
            }
            // Default body (si existe) — su target es aquí; sin JMP-a-end
            // porque naturalmente cae al end.
            size_t default_target = b->len;
            if (n->s_switch.default_body) {
                jump_record_table_entry(default_off_pos, default_target,
                                        table_end, n->line);
                emit_node(b, n->s_switch.default_body);
            } else {
                // Sin default: el default_off apunta a `end` (donde caemos
                // sin hacer nada). end == b->len tras emit de bodies.
                // Lo backpatcheamos con target = b->len al final (después
                // del loop). Registramos con target provisional.
                jump_record_table_entry(default_off_pos, 0, table_end, n->line);
                // El target se actualiza abajo -- no; en realidad, si no hay
                // default body, el "end" del switch == b->len ahora (tras
                // el último JMP). Lo fijamos justo:
                g_jumps[g_n_jumps - 1].target = b->len;
            }
            size_t end_pos = b->len;
            // Backpatch todos los JMP-a-end de cada case body.
            for (size_t i = 0; i < n->s_switch.n; i++) {
                jump_rec_t *j = jump_find_by_hole(jmp_holes[i]);
                j->target = end_pos;
                j->line = n->line;
            }
            // Si NO había default y el default_target se registró como
            // provisional, lo ajustamos ahora que sabemos el end real.
            if (!n->s_switch.default_body) {
                // El último JR_TABLE_ENTRY registrado antes del end_pos
                // check era el default sin body. Lo re-target-eamos a end.
                // (Ya lo hicimos arriba pero end pudo haber shifteado por
                // JMP holes. En realidad no -- end_pos = b->len y JMPs no
                // se han shrinkeado aún.)
                for (size_t i = g_n_jumps; i > 0; i--) {
                    if (g_jumps[i-1].kind == JR_TABLE_ENTRY &&
                        g_jumps[i-1].pos == default_off_pos) {
                        g_jumps[i-1].target = end_pos;
                        break;
                    }
                }
            }
            break;
        }
        case AST_CASE:
            // AST_CASE es transient del parser -- se absorbe en
            // ast_switch_add_case_node y nunca sobrevive al reduce.
            // Si esto se ejecuta, hay un bug en el parser.
            qks_err_report(QKS_ERR_OOM, n->line,
                           "bug interno: AST_CASE llegó al emit (no absorbido por el parser)");
        case AST_LAYOUT:
            // C.4.b: emit(expr) deja el layout id en TOS, luego OP_LAYOUT
            // lo popea y actualiza current_layout de la VM.
            emit_node(b, n->s_layout.expr);
            buf_u8(b, OP_LAYOUT);
            break;
        case AST_WHILE: {
            // C.3.d: while cond { body }
            //
            // Layout:
            //   loop_start:              <-- posición anotada ANTES de emit(cond)
            //       emit(cond)
            //       JMPZ hole            <-- salta al POST_LOOP si cond es 0 (forward, backpatch)
            //       emit(body)
            //       JMP  loop_start      <-- salto HACIA ATRÁS con target ya conocido (offset negativo)
            //   post_loop:               <-- patch(hole) apunta aquí
            //
            // Novedad conceptual: DOS TIPOS de saltos coexisten:
            //   - forward (JMPZ): backpatch clásico, no sabemos el target hasta que
            //     emitimos el body.
            //   - backward (JMP): el target ES loop_start, anotado antes de arrancar.
            //     No hay hueco, no hay backpatch -- offset se computa y escribe directo
            //     con emit_jump_to_target. Offset será negativo por construcción.
            //
            // La VM (vm.c, OP_JMP) ya soporta ambos signos con bounds check separado
            // (test_guard_jmp_oob_backward lo valida desde C.1.1). Este emit es la
            // otra punta del contrato.
            size_t loop_start = b->len;
            emit_node(b, n->s_while.cond);
            size_t hole = emit_jump_placeholder(b, OP_JMPZ);   // exit forward, patch después
            emit_node(b, n->s_while.body);
            emit_jump_to_target(b, OP_JMP, loop_start, n->line); // vuelve al inicio (offset neg)
            patch_jump_to_here(b, hole, n->line);                // post_loop = aquí
            break;
        }
        case AST_PROGRAM:
            for (size_t i = 0; i < n->s_prog.n; i++) emit_node(b, n->s_prog.stmts[i]);
            break;
        // ponytail: sin default -- -Wswitch-enum grita si añadimos AST_IF y olvidamos.
    }
}

// ============================================================
// entry points públicos
// ============================================================
// Header (16B). Ver comentario en vm.h para layout completo.
// Offset 7 = target_layer (0..15 = layer específico, 0xFF = any).
static void write_header(FILE *out, uint32_t code_len,
                         uint16_t target_kc, uint8_t target_mods,
                         uint8_t target_layer) {
    uint8_t hdr[QKS_HEADER_SIZE] = {
        'Q','K','S','C',
        QKS_VERSION, 0, target_mods, target_layer,
        (uint8_t)(code_len      ), (uint8_t)(code_len >>  8),
        (uint8_t)(code_len >> 16), (uint8_t)(code_len >> 24),
        (uint8_t)(target_kc     ), (uint8_t)(target_kc >>  8),
        0, 0,
    };
    fwrite(hdr, 1, sizeof hdr, out);
}

void emit_bytecode(FILE *out, const ast_t *root) {
    sym_reset();                     // symbol table fresca por invocación
    jumps_reset();                   // jump table fresca por invocación
    /* NO bind_reset -- el estado del bind viene del parse (ver comentario
     * en emit_bytecode_dump). Resetear acá borraría lo que qks_bind_set almacenó. */
    buf_t b = {0};
    emit_node(&b, root);
    buf_u8(&b, OP_HALT);
    relax_jumps(&b);                 // C.3.f: pesimistic shrink to short forms
    finalize_offsets(&b);            // escribir offset bytes con positions finales
    uint16_t target_kc; uint8_t target_mods;
    bind_resolve(&target_kc, &target_mods);
    write_header(out, (uint32_t)b.len, target_kc, target_mods, g_bind_layer);
    fwrite(b.data, 1, b.len, out);
    free(b.data);
}

// ============================================================
// disassembler -- decodifica el mismo buffer que emit_bytecode produce.
// Formato por línea: `OFFSET  HEX_BYTES  MNEMONICO ARGS`
// Reusable como esqueleto del intérprete (paso 2 del plan).
// ============================================================
static void dump_hex_str(FILE *out, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) fprintf(out, "%02x ", p[i]);
}
static void dump_ascii_str(FILE *out, const uint8_t *p, size_t n) {
    fputc('"', out);
    for (size_t i = 0; i < n; i++)
        fputc(isprint(p[i]) ? p[i] : '.', out);
    fputc('"', out);
}

void emit_bytecode_dump(FILE *out, const ast_t *root) {
    sym_reset();                     // symbol table fresca por invocación
    jumps_reset();
    /* NO bind_reset aquí -- el estado del bind viene del parse (setea ANTES
     * que corra emit). Reset-ear acá borraría lo que qks_bind_set almacenó. */
    buf_t b = {0};
    emit_node(&b, root);
    buf_u8(&b, OP_HALT);
    relax_jumps(&b);
    finalize_offsets(&b);

    fprintf(out, "; qmkscript bytecode v%d -- %zu bytes de code\n", QKS_VERSION, b.len);
    fprintf(out, "; header: magic=QKSC version=%d code_len=%zu\n\n", QKS_VERSION, b.len);

    size_t pc = 0;
    while (pc < b.len) {
        size_t start = pc;
        uint8_t op = b.data[pc++];
        fprintf(out, "%04zx  ", start);

        switch ((vm_opcode_t)op) {
            case OP_HALT:
                fprintf(out, "%-24s", "00");
                fprintf(out, "HALT\n");
                break;
            case OP_STR: {
                uint16_t len = b.data[pc] | (b.data[pc+1] << 8);
                pc += 2;
                // hex de op + len (3 bytes), luego payload en línea separada si es largo
                char hex[64]; snprintf(hex, sizeof hex, "01 %02x %02x ...",
                                       b.data[start+1], b.data[start+2]);
                fprintf(out, "%-24s", hex);
                fprintf(out, "STR len=%u ", len);
                dump_ascii_str(out, b.data + pc, len);
                fputc('\n', out);
                pc += len;
                break;
            }
            case OP_TAP: {
                uint8_t kc = b.data[pc++];
                char hex[16]; snprintf(hex, sizeof hex, "02 %02x", kc);
                fprintf(out, "%-24s", hex);
                fprintf(out, "TAP kc=0x%02x\n", kc);
                break;
            }
            case OP_DELAY: {
                uint16_t ms = b.data[pc] | (b.data[pc+1] << 8);
                pc += 2;
                char hex[16]; snprintf(hex, sizeof hex, "03 %02x %02x",
                                       b.data[start+1], b.data[start+2]);
                fprintf(out, "%-24s", hex);
                fprintf(out, "DELAY ms=%u\n", ms);
                break;
            }
            case OP_CHORD: {
                uint8_t mods = b.data[pc++], kc = b.data[pc++];
                char hex[16]; snprintf(hex, sizeof hex, "04 %02x %02x", mods, kc);
                fprintf(out, "%-24s", hex);
                fprintf(out, "CHORD mods=0x%02x kc=0x%02x\n", mods, kc);
                break;
            }
            case OP_REG_MODS: {
                uint8_t mods = b.data[pc++];
                char hex[16]; snprintf(hex, sizeof hex, "05 %02x", mods);
                fprintf(out, "%-24s", hex);
                fprintf(out, "REG_MODS mods=0x%02x\n", mods);
                break;
            }
            case OP_UNREG_MODS: {
                uint8_t mods = b.data[pc++];
                char hex[16]; snprintf(hex, sizeof hex, "06 %02x", mods);
                fprintf(out, "%-24s", hex);
                fprintf(out, "UNREG_MODS mods=0x%02x\n", mods);
                break;
            }
            case OP_PUSH: {
                uint32_t v = (uint32_t)b.data[pc]
                           | ((uint32_t)b.data[pc+1] <<  8)
                           | ((uint32_t)b.data[pc+2] << 16)
                           | ((uint32_t)b.data[pc+3] << 24);
                pc += 4;
                char hex[32]; snprintf(hex, sizeof hex, "20 %02x %02x %02x %02x",
                    b.data[start+1], b.data[start+2], b.data[start+3], b.data[start+4]);
                fprintf(out, "%-24s", hex);
                fprintf(out, "PUSH %u\n", v);
                break;
            }
            case OP_LOAD: {
                uint8_t idx = b.data[pc++];
                char hex[16]; snprintf(hex, sizeof hex, "21 %02x", idx);
                fprintf(out, "%-24s", hex);
                fprintf(out, "LOAD var[%u]\n", idx);
                break;
            }
            case OP_STORE: {
                uint8_t idx = b.data[pc++];
                char hex[16]; snprintf(hex, sizeof hex, "22 %02x", idx);
                fprintf(out, "%-24s", hex);
                fprintf(out, "STORE var[%u]\n", idx);
                break;
            }
            case OP_POP:
                fprintf(out, "%-24s", "23");
                fprintf(out, "POP\n");
                break;
            // Density (C.3.e). Cada case imprime el mnemónico corto y su valor.
            case OP_PUSH_0: fprintf(out, "%-24s%s\n", "24", "PUSH_0"); break;
            case OP_PUSH_1: fprintf(out, "%-24s%s\n", "25", "PUSH_1"); break;
            case OP_PUSH_U8: {
                uint8_t v = b.data[pc++];
                char hex[16]; snprintf(hex, sizeof hex, "26 %02x", v);
                fprintf(out, "%-24s", hex);
                fprintf(out, "PUSH_U8 %u\n", v);
                break;
            }
            case OP_PUSH_U16: {
                uint16_t v = b.data[pc] | (b.data[pc+1] << 8);
                pc += 2;
                char hex[16]; snprintf(hex, sizeof hex, "27 %02x %02x",
                                       b.data[start+1], b.data[start+2]);
                fprintf(out, "%-24s", hex);
                fprintf(out, "PUSH_U16 %u\n", v);
                break;
            }
            case OP_LOAD_0: case OP_LOAD_1: case OP_LOAD_2: case OP_LOAD_3: {
                unsigned idx = op - OP_LOAD_0;
                char hex[8]; snprintf(hex, sizeof hex, "%02x", op);
                fprintf(out, "%-24s", hex);
                fprintf(out, "LOAD_%u\n", idx);
                break;
            }
            case OP_STORE_0: case OP_STORE_1: case OP_STORE_2: case OP_STORE_3: {
                unsigned idx = op - OP_STORE_0;
                char hex[8]; snprintf(hex, sizeof hex, "%02x", op);
                fprintf(out, "%-24s", hex);
                fprintf(out, "STORE_%u\n", idx);
                break;
            }
            case OP_ADD: fprintf(out, "%-24s%s\n", "30", "ADD"); break;
            case OP_SUB: fprintf(out, "%-24s%s\n", "31", "SUB"); break;
            case OP_MUL: fprintf(out, "%-24s%s\n", "32", "MUL"); break;
            case OP_DIV: fprintf(out, "%-24s%s\n", "33", "DIV"); break;
            case OP_EQ:  fprintf(out, "%-24s%s\n", "40", "EQ");  break;
            case OP_NEQ: fprintf(out, "%-24s%s\n", "41", "NEQ"); break;
            case OP_LT:  fprintf(out, "%-24s%s\n", "42", "LT");  break;
            case OP_GT:  fprintf(out, "%-24s%s\n", "43", "GT");  break;
            case OP_JMP: {
                int16_t off = (int16_t)((uint16_t)b.data[pc] | ((uint16_t)b.data[pc+1] << 8));
                pc += 2;
                char hex[16]; snprintf(hex, sizeof hex, "10 %02x %02x",
                    b.data[start+1], b.data[start+2]);
                fprintf(out, "%-24s", hex);
                fprintf(out, "JMP  %+d\n", off);
                break;
            }
            case OP_JMPZ: {
                int16_t off = (int16_t)((uint16_t)b.data[pc] | ((uint16_t)b.data[pc+1] << 8));
                pc += 2;
                char hex[16]; snprintf(hex, sizeof hex, "11 %02x %02x",
                    b.data[start+1], b.data[start+2]);
                fprintf(out, "%-24s", hex);
                fprintf(out, "JMPZ %+d\n", off);
                break;
            }
            case OP_JMP_S: {
                int8_t off = (int8_t)b.data[pc++];
                char hex[16]; snprintf(hex, sizeof hex, "12 %02x", (uint8_t)off);
                fprintf(out, "%-24s", hex);
                fprintf(out, "JMP_S  %+d\n", off);
                break;
            }
            case OP_JMPZ_S: {
                int8_t off = (int8_t)b.data[pc++];
                char hex[16]; snprintf(hex, sizeof hex, "13 %02x", (uint8_t)off);
                fprintf(out, "%-24s", hex);
                fprintf(out, "JMPZ_S %+d\n", off);
                break;
            }
            case OP_LAYOUT: {
                fprintf(out, "%-24s", "15");
                fprintf(out, "LAYOUT (pop TOS -> current_layout)\n");
                break;
            }
            case OP_SWITCH: {
                uint16_t nc = b.data[pc] | (b.data[pc+1] << 8);
                pc += 2;
                fprintf(out, "%-24s", "14 ..");
                fprintf(out, "SWITCH  n_cases=%u\n", nc);
                for (uint16_t i = 0; i < nc; i++) {
                    uint32_t key = (uint32_t)b.data[pc]
                                 | ((uint32_t)b.data[pc+1] <<  8)
                                 | ((uint32_t)b.data[pc+2] << 16)
                                 | ((uint32_t)b.data[pc+3] << 24);
                    int16_t  off = (int16_t)((uint16_t)b.data[pc+4] | ((uint16_t)b.data[pc+5] << 8));
                    fprintf(out, "%04zx  %-24s   case %u -> %+d\n", pc, "..", key, off);
                    pc += 6;
                }
                int16_t doff = (int16_t)((uint16_t)b.data[pc] | ((uint16_t)b.data[pc+1] << 8));
                fprintf(out, "%04zx  %-24s   default -> %+d\n", pc, "..", doff);
                pc += 2;
                break;
            }
        }
    }
    free(b.data);
}
