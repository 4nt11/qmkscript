#include "emit_c.h"
#include "vm.h"     // VMMOD_* bitmap layout compartido con la VM
#include <stdio.h>
#include <string.h>
#include <stdint.h>

// Mapa: nombre lowercase -> keycode QMK SS_ macro-name.
// Cuando llegue una tabla de layout completa, esto se generaliza.
static const char *qmk_keycode_of(const char *name) {
    if (!strcmp(name, "enter") || !strcmp(name, "ent"))   return "X_ENTER";
    if (!strcmp(name, "esc"))                              return "X_ESCAPE";
    if (!strcmp(name, "tab"))                              return "X_TAB";
    if (!strcmp(name, "space") || !strcmp(name, "spc"))   return "X_SPACE";
    if (!strcmp(name, "bspc")  || !strcmp(name, "bs"))    return "X_BSPC";
    // letras/dígitos: X_A..X_Z, X_0..X_9
    if (name[0] && name[1] == '\0') {
        static char buf[8];
        char c = name[0];
        if (c >= 'a' && c <= 'z') { snprintf(buf, sizeof buf, "X_%c", c - 32); return buf; }
        if (c >= '0' && c <= '9') { snprintf(buf, sizeof buf, "X_%c", c);       return buf; }
    }
    return "X_NO"; // fallback
}

// Emite todos los SS_DOWN(X_L...)  para cada bit del bitmap. Espacio entre
// cada uno (SS_ macros son literales que se concatenan a compile-time en C).
static void emit_mods_down(FILE *out, uint8_t b) {
    if (b & VMMOD_LCTL) fputs("SS_DOWN(X_LCTL) ", out);
    if (b & VMMOD_LSFT) fputs("SS_DOWN(X_LSFT) ", out);
    if (b & VMMOD_LALT) fputs("SS_DOWN(X_LALT) ", out);
    if (b & VMMOD_LGUI) fputs("SS_DOWN(X_LGUI) ", out);
    if (b & VMMOD_RCTL) fputs("SS_DOWN(X_RCTL) ", out);
    if (b & VMMOD_RSFT) fputs("SS_DOWN(X_RSFT) ", out);
    if (b & VMMOD_RALT) fputs("SS_DOWN(X_RALT) ", out);
    if (b & VMMOD_RGUI) fputs("SS_DOWN(X_RGUI) ", out);
}
// Espejo: SS_UP en orden inverso (unwrap limpio, aunque para register_mods
// no importa realmente el orden).
static void emit_mods_up(FILE *out, uint8_t b) {
    if (b & VMMOD_RGUI) fputs("SS_UP(X_RGUI) ", out);
    if (b & VMMOD_RALT) fputs("SS_UP(X_RALT) ", out);
    if (b & VMMOD_RSFT) fputs("SS_UP(X_RSFT) ", out);
    if (b & VMMOD_RCTL) fputs("SS_UP(X_RCTL) ", out);
    if (b & VMMOD_LGUI) fputs("SS_UP(X_LGUI) ", out);
    if (b & VMMOD_LALT) fputs("SS_UP(X_LALT) ", out);
    if (b & VMMOD_LSFT) fputs("SS_UP(X_LSFT) ", out);
    if (b & VMMOD_LCTL) fputs("SS_UP(X_LCTL) ", out);
}

void emit_c(FILE *out, const ast_t *n) {
    if (!n) return;
    switch (n->kind) {
        case AST_STRING:
            fprintf(out, "    SEND_STRING(\"%s\");\n", n->s_string.text);
            break;
        case AST_TAP:
            fprintf(out, "    SEND_STRING(SS_TAP(%s));\n", qmk_keycode_of(n->s_tap.key));
            break;
        case AST_DELAY:
            fprintf(out, "    SEND_STRING(SS_DELAY(%d));\n", n->s_delay.ms);
            break;
        case AST_CHORD:
            // chord [gui, r] -> SEND_STRING(SS_DOWN(...) SS_TAP(X_R) SS_UP(...));
            // Un solo SEND_STRING; las macros SS_ son literales concatenables.
            fputs("    SEND_STRING(", out);
            emit_mods_down(out, n->s_chord.mods);
            fprintf(out, "SS_TAP(%s) ", qmk_keycode_of(n->s_chord.key));
            emit_mods_up(out, n->s_chord.mods);
            fputs(");\n", out);
            break;
        case AST_CHORD_SEQ:
            // Chord "gordo": DOWN todos los mods, cada action (recursivo), UP todos.
            // Split en varios SEND_STRING para que las actions type/tap/delay se
            // encapsulen bien -- SS_ macros no juegan con delays de longitud variable.
            fputs("    SEND_STRING(", out);
            emit_mods_down(out, n->s_chord_seq.mods);
            fputs(");\n", out);
            for (size_t i = 0; i < n->s_chord_seq.n; i++)
                emit_c(out, n->s_chord_seq.actions[i]);
            fputs("    SEND_STRING(", out);
            emit_mods_up(out, n->s_chord_seq.mods);
            fputs(");\n", out);
            break;
        case AST_NUM_LIT:
        case AST_VAR_REF:
        case AST_BIN_OP:
            // Dentro de expr contexts que emit_c no soporta (bytecode-only).
            break;
        case AST_VAR_DECL:
            // C.2+: variables NO se emiten al output SEND_STRING (macros QMK
            // no tienen estado runtime). Se pierde. El backend real de
            // qmkscript es bytecode; emit_c queda como demo legacy.
            fprintf(out, "    // var %s = <expr>;  (no soportado en emit_c, usa --emit=bytecode)\n",
                    n->s_var_decl.name);
            break;
        case AST_ASSIGN:
            fprintf(out, "    // %s = <expr>;  (no soportado en emit_c, usa --emit=bytecode)\n",
                    n->s_assign.name);
            break;
        case AST_BLOCK:
            for (size_t i = 0; i < n->s_block.n; i++)
                emit_c(out, n->s_block.stmts[i]);
            break;
        case AST_IF:
            // Control flow no tiene equivalencia en SEND_STRING (macro sin estado
            // runtime). emit_c es demo legacy; el backend real es bytecode.
            fprintf(out, "    // if <expr> { ... };  (no soportado en emit_c, usa --emit=bytecode)\n");
            break;
        case AST_WHILE:
            fprintf(out, "    // while <expr> { ... };  (no soportado en emit_c, usa --emit=bytecode)\n");
            break;
        case AST_SWITCH:
        case AST_CASE:
        case AST_LAYOUT:
            fprintf(out, "    // switch/layout (no soportado en emit_c, usa --emit=bytecode)\n");
            break;
        case AST_PROGRAM:
            fprintf(out, "// --- qmkscript v1: paste inside a QMK macro handler ---\n");
            for (size_t i = 0; i < n->s_prog.n; i++)
                emit_c(out, n->s_prog.stmts[i]);
            break;
    }
}
