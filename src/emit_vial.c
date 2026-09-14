#include "emit_vial.h"
#include "vm.h"     // VMMOD_* bitmap layout
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>

// Nombres de keycode Vial (KC_*). Mismo mapping que emit_c pero prefijo KC_.
static const char *vial_kc_of(const char *name) {
    if (!strcmp(name, "enter") || !strcmp(name, "ent"))   return "KC_ENTER";
    if (!strcmp(name, "esc"))                              return "KC_ESCAPE";
    if (!strcmp(name, "tab"))                              return "KC_TAB";
    if (!strcmp(name, "space") || !strcmp(name, "spc"))   return "KC_SPACE";
    if (!strcmp(name, "bspc")  || !strcmp(name, "bs"))    return "KC_BSPC";
    if (name[0] && name[1] == '\0') {
        static char buf[8];
        char c = name[0];
        if (c >= 'a' && c <= 'z') { snprintf(buf, sizeof buf, "KC_%c", c - 32); return buf; }
        if (c >= '0' && c <= '9') { snprintf(buf, sizeof buf, "KC_%c", c);       return buf; }
    }
    return "KC_NO";
}

// escribe una accion JSON. `first` controla la coma que la separa de la anterior.
static void action_string(FILE *out, int *first, const char *fmt, ...) {
    if (!*first) fputs(",\n    ", out); else fputs("\n    ", out);
    *first = 0;
    va_list ap; va_start(ap, fmt); vfprintf(out, fmt, ap); va_end(ap);
}

// Emite ["down", "KC_XXX"] por cada bit del bitmap. Vial macro accepts esto
// como acciones individuales dentro de la lista principal.
static void emit_mods_down(FILE *out, int *first, uint8_t b) {
    if (b & VMMOD_LCTL) action_string(out, first, "[\"down\", \"KC_LCTL\"]");
    if (b & VMMOD_LSFT) action_string(out, first, "[\"down\", \"KC_LSFT\"]");
    if (b & VMMOD_LALT) action_string(out, first, "[\"down\", \"KC_LALT\"]");
    if (b & VMMOD_LGUI) action_string(out, first, "[\"down\", \"KC_LGUI\"]");
    if (b & VMMOD_RCTL) action_string(out, first, "[\"down\", \"KC_RCTL\"]");
    if (b & VMMOD_RSFT) action_string(out, first, "[\"down\", \"KC_RSFT\"]");
    if (b & VMMOD_RALT) action_string(out, first, "[\"down\", \"KC_RALT\"]");
    if (b & VMMOD_RGUI) action_string(out, first, "[\"down\", \"KC_RGUI\"]");
}
static void emit_mods_up(FILE *out, int *first, uint8_t b) {
    if (b & VMMOD_RGUI) action_string(out, first, "[\"up\", \"KC_RGUI\"]");
    if (b & VMMOD_RALT) action_string(out, first, "[\"up\", \"KC_RALT\"]");
    if (b & VMMOD_RSFT) action_string(out, first, "[\"up\", \"KC_RSFT\"]");
    if (b & VMMOD_RCTL) action_string(out, first, "[\"up\", \"KC_RCTL\"]");
    if (b & VMMOD_LGUI) action_string(out, first, "[\"up\", \"KC_LGUI\"]");
    if (b & VMMOD_LALT) action_string(out, first, "[\"up\", \"KC_LALT\"]");
    if (b & VMMOD_LSFT) action_string(out, first, "[\"up\", \"KC_LSFT\"]");
    if (b & VMMOD_LCTL) action_string(out, first, "[\"up\", \"KC_LCTL\"]");
}

// El chord [gui, r] se traduce a "down GUI, tap r, up GUI" en macros Vial.
static void emit_node(FILE *out, const ast_t *n, int *first) {
    switch (n->kind) {
        case AST_STRING:
            action_string(out, first, "[\"text\", \"%s\"]", n->s_string.text);
            break;
        case AST_TAP:
            action_string(out, first, "[\"tap\", \"%s\"]", vial_kc_of(n->s_tap.key));
            break;
        case AST_DELAY:
            action_string(out, first, "[\"delay\", %d]", n->s_delay.ms);
            break;
        case AST_CHORD: {
            emit_mods_down(out, first, n->s_chord.mods);
            action_string(out, first, "[\"tap\", \"%s\"]", vial_kc_of(n->s_chord.key));
            emit_mods_up(out, first, n->s_chord.mods);
            break;
        }
        case AST_CHORD_SEQ:
            emit_mods_down(out, first, n->s_chord_seq.mods);
            for (size_t i = 0; i < n->s_chord_seq.n; i++)
                emit_node(out, n->s_chord_seq.actions[i], first);
            emit_mods_up(out, first, n->s_chord_seq.mods);
            break;
        case AST_NUM_LIT:
        case AST_VAR_REF:
        case AST_BIN_OP:
        case AST_VAR_DECL:
        case AST_ASSIGN:
            // Vial macros no tienen estado runtime -- vars y expresiones se
            // pierden. Skip silencioso; el backend real es bytecode.
            break;
        case AST_BLOCK:
            for (size_t i = 0; i < n->s_block.n; i++)
                emit_node(out, n->s_block.stmts[i], first);
            break;
        case AST_IF:
        case AST_WHILE:
        case AST_SWITCH:
        case AST_CASE:
        case AST_LAYOUT:
            // Macros Vial no tienen control flow -- se pierde. Backend demo.
            break;
        case AST_PROGRAM:
            for (size_t i = 0; i < n->s_prog.n; i++)
                emit_node(out, n->s_prog.stmts[i], first);
            break;
    }
}

void emit_vial(FILE *out, const ast_t *n) {
    fputs("[", out);
    int first = 1;
    emit_node(out, n, &first);
    fputs("\n]\n", out);
}
