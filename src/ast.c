#include "ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// helper: reserva un nodo cero-inicializado de tipo k
static ast_t *node(ast_kind_t k) {
    ast_t *n = calloc(1, sizeof *n);
    n->kind = k;
    return n;
}

ast_t *ast_string(const char *text) {
    ast_t *n = node(AST_STRING);
    n->s_string.text = strdup(text);
    return n;
}

ast_t *ast_tap(const char *key) {
    ast_t *n = node(AST_TAP);
    n->s_tap.key = strdup(key);
    return n;
}

ast_t *ast_delay(int ms) {
    ast_t *n = node(AST_DELAY);
    n->s_delay.ms = ms;
    return n;
}

ast_t *ast_chord(uint8_t mods, const char *key) {
    ast_t *n = node(AST_CHORD);
    n->s_chord.mods = mods;
    n->s_chord.key  = strdup(key);
    return n;
}

ast_t *ast_chord_seq(uint8_t mods) {
    ast_t *n = node(AST_CHORD_SEQ);
    n->s_chord_seq.mods = mods;
    // actions/n/cap ya vienen a 0 gracias a calloc en node().
    return n;
}

void ast_chord_seq_append(ast_t *cs, ast_t *action) {
    if (cs->s_chord_seq.n == cs->s_chord_seq.cap) {
        cs->s_chord_seq.cap = cs->s_chord_seq.cap ? cs->s_chord_seq.cap * 2 : 4;
        cs->s_chord_seq.actions = realloc(cs->s_chord_seq.actions,
                                          cs->s_chord_seq.cap * sizeof(ast_t *));
    }
    cs->s_chord_seq.actions[cs->s_chord_seq.n++] = action;
}

ast_t *ast_num_lit(uint32_t value) {
    ast_t *n = node(AST_NUM_LIT);
    n->s_num.value = value;
    return n;
}

ast_t *ast_var_ref(const char *name) {
    ast_t *n = node(AST_VAR_REF);
    n->s_var_ref.name = strdup(name);
    return n;
}

ast_t *ast_var_decl(const char *name, ast_t *init) {
    ast_t *n = node(AST_VAR_DECL);
    n->s_var_decl.name = strdup(name);
    n->s_var_decl.init = init;   // init: ast_t ownership passes to el nodo (no strdup)
    return n;
}

ast_t *ast_assign(const char *name, ast_t *value) {
    ast_t *n = node(AST_ASSIGN);
    n->s_assign.name  = strdup(name);
    n->s_assign.value = value;
    return n;
}

ast_t *ast_bin_op(bin_op_t op, ast_t *lhs, ast_t *rhs) {
    ast_t *n = node(AST_BIN_OP);
    n->s_bin_op.op  = op;
    n->s_bin_op.lhs = lhs;
    n->s_bin_op.rhs = rhs;
    return n;
}

ast_t *ast_program(void) {
    return node(AST_PROGRAM);
}

void ast_program_append(ast_t *prog, ast_t *stmt) {
    if (prog->s_prog.n == prog->s_prog.cap) {
        prog->s_prog.cap = prog->s_prog.cap ? prog->s_prog.cap * 2 : 8;
        prog->s_prog.stmts = realloc(prog->s_prog.stmts,
                                     prog->s_prog.cap * sizeof(ast_t *));
    }
    prog->s_prog.stmts[prog->s_prog.n++] = stmt;
}

// C.3.a: bloques. Misma shape/append que PROGRAM. Kind separado para
// que -Wswitch-enum obligue a pensar cada sitio si mañana los blocks
// introducen scope de vars (fase D+) y PROGRAM sigue siendo global.
ast_t *ast_block(void) {
    return node(AST_BLOCK);
}

void ast_block_append(ast_t *blk, ast_t *stmt) {
    if (blk->s_block.n == blk->s_block.cap) {
        blk->s_block.cap = blk->s_block.cap ? blk->s_block.cap * 2 : 8;
        blk->s_block.stmts = realloc(blk->s_block.stmts,
                                     blk->s_block.cap * sizeof(ast_t *));
    }
    blk->s_block.stmts[blk->s_block.n++] = stmt;
}

// C.3.b/c: if cond { then } [else { else }]. `then_body` es AST_BLOCK (braces
// obligatorias). `else_body` es AST_BLOCK, o NULL cuando no hay else, o un
// AST_IF envuelto en un AST_BLOCK sintético en el caso `else if` chaining
// (ver la regla `if_stmt : ... KW_ELSE if_stmt` en parse.y).
// Ownership: cond/then_body/else_body pasan al nodo (sin strdup, son ast_t *).
ast_t *ast_if(ast_t *cond, ast_t *then_body, ast_t *else_body) {
    ast_t *n = node(AST_IF);
    n->s_if.cond      = cond;
    n->s_if.then_body = then_body;
    n->s_if.else_body = else_body;   // NULL = no hay else
    return n;
}

// C.3.d: while cond { body }. `body` es siempre AST_BLOCK (braces obligatorias).
// Ownership: cond y body pasan al nodo.
ast_t *ast_while(ast_t *cond, ast_t *body) {
    ast_t *n = node(AST_WHILE);
    n->s_while.cond = cond;
    n->s_while.body = body;
    return n;
}

// ================================================================
// C.3.g: switch construction. Ver comentario en ast.h.
// ================================================================
ast_t *ast_switch_new(void) {
    return node(AST_SWITCH);   // key/values/bodies/default_body a NULL via calloc
}
void ast_switch_set_key(ast_t *sw, ast_t *key) {
    sw->s_switch.key = key;
}
ast_t *ast_case_new(uint32_t value, ast_t *body) {
    ast_t *n = node(AST_CASE);
    n->s_case.value = value;
    n->s_case.body  = body;
    return n;
}
void ast_switch_add_case_node(ast_t *sw, ast_t *case_node) {
    // absorbe el value/body del AST_CASE transitorio en los vectores paralelos.
    if (sw->s_switch.n == sw->s_switch.cap) {
        sw->s_switch.cap = sw->s_switch.cap ? sw->s_switch.cap * 2 : 4;
        sw->s_switch.values = realloc(sw->s_switch.values,
                                      sw->s_switch.cap * sizeof(uint32_t));
        sw->s_switch.bodies = realloc(sw->s_switch.bodies,
                                      sw->s_switch.cap * sizeof(ast_t *));
    }
    sw->s_switch.values[sw->s_switch.n] = case_node->s_case.value;
    sw->s_switch.bodies[sw->s_switch.n] = case_node->s_case.body;
    sw->s_switch.n++;
    // El AST_CASE transitorio queda como garbage (ponytail: sin ast_free en v0).
}
void ast_switch_set_default(ast_t *sw, ast_t *body) {
    sw->s_switch.default_body = body;
}

// C.4.b: layout(EXPR). EXPR se evalúa en runtime, TOS pasa a OP_LAYOUT.
ast_t *ast_layout(ast_t *expr) {
    ast_t *n = node(AST_LAYOUT);
    n->s_layout.expr = expr;
    return n;
}

// --- pretty-print --------------------------------------------------

static void ind(int n) { for (int i = 0; i < n; i++) putchar(' '); }

// Formato humano del bitmap de mods: "gui+ctrl", "shift", "" si 0.
static void print_mods(uint8_t m) {
    const char *first = NULL;
    #define PM(bit, name) do { \
        if (m & (bit)) { if (first) putchar('+'); else first = name; fputs(name, stdout); } \
    } while(0)
    PM(0x01, "lctl"); PM(0x02, "lsft"); PM(0x04, "lalt"); PM(0x08, "lgui");
    PM(0x10, "rctl"); PM(0x20, "rsft"); PM(0x40, "ralt"); PM(0x80, "rgui");
    #undef PM
    if (!first) fputs("(none)", stdout);
}

void ast_dump(const ast_t *n, int indent) {
    if (!n) { ind(indent); puts("(null)"); return; }
    ind(indent);
    switch (n->kind) {
        case AST_STRING:  printf("STRING  \"%s\"\n", n->s_string.text); break;
        case AST_TAP:     printf("TAP     %s\n",     n->s_tap.key);     break;
        case AST_DELAY:   printf("DELAY   %d ms\n",  n->s_delay.ms);    break;
        case AST_CHORD:
            fputs("CHORD   ", stdout);
            print_mods(n->s_chord.mods);
            printf(" + %s\n", n->s_chord.key);
            break;
        case AST_CHORD_SEQ:
            fputs("CHORD_SEQ ", stdout);
            print_mods(n->s_chord_seq.mods);
            printf(" (%zu actions)\n", n->s_chord_seq.n);
            for (size_t i = 0; i < n->s_chord_seq.n; i++)
                ast_dump(n->s_chord_seq.actions[i], indent + 2);
            break;
        case AST_NUM_LIT:
            printf("NUM_LIT %u\n", n->s_num.value);
            break;
        case AST_VAR_REF:
            printf("VAR_REF %s\n", n->s_var_ref.name);
            break;
        case AST_VAR_DECL:
            printf("VAR_DECL %s =\n", n->s_var_decl.name);
            ast_dump(n->s_var_decl.init, indent + 2);
            break;
        case AST_ASSIGN:
            printf("ASSIGN %s =\n", n->s_assign.name);
            ast_dump(n->s_assign.value, indent + 2);
            break;
        case AST_BIN_OP: {
            static const char *op_names[] = {
                [BIN_ADD]="+", [BIN_SUB]="-", [BIN_MUL]="*", [BIN_DIV]="/",
                [BIN_EQ]="==",[BIN_NEQ]="!=",[BIN_LT]="<", [BIN_GT]=">",
            };
            printf("BIN_OP %s\n", op_names[n->s_bin_op.op]);
            ast_dump(n->s_bin_op.lhs, indent + 2);
            ast_dump(n->s_bin_op.rhs, indent + 2);
            break;
        }
        case AST_BLOCK:
            printf("BLOCK (%zu stmts)\n", n->s_block.n);
            for (size_t i = 0; i < n->s_block.n; i++)
                ast_dump(n->s_block.stmts[i], indent + 2);
            break;
        case AST_IF:
            printf("IF\n");
            ind(indent + 2); printf("cond:\n");
            ast_dump(n->s_if.cond, indent + 4);
            ind(indent + 2); printf("then:\n");
            ast_dump(n->s_if.then_body, indent + 4);
            if (n->s_if.else_body) {
                ind(indent + 2); printf("else:\n");
                ast_dump(n->s_if.else_body, indent + 4);
            }
            break;
        case AST_WHILE:
            printf("WHILE\n");
            ind(indent + 2); printf("cond:\n");
            ast_dump(n->s_while.cond, indent + 4);
            ind(indent + 2); printf("body:\n");
            ast_dump(n->s_while.body, indent + 4);
            break;
        case AST_SWITCH:
            printf("SWITCH (%zu cases%s)\n", n->s_switch.n,
                   n->s_switch.default_body ? " + default" : "");
            ind(indent + 2); printf("key:\n");
            ast_dump(n->s_switch.key, indent + 4);
            for (size_t i = 0; i < n->s_switch.n; i++) {
                ind(indent + 2); printf("case %u:\n", n->s_switch.values[i]);
                ast_dump(n->s_switch.bodies[i], indent + 4);
            }
            if (n->s_switch.default_body) {
                ind(indent + 2); printf("default:\n");
                ast_dump(n->s_switch.default_body, indent + 4);
            }
            break;
        case AST_CASE:
            // No debería aparecer post-parse. Debug output por si aparece.
            printf("CASE %u (transient -- bug si esto imprime post-parse)\n", n->s_case.value);
            ast_dump(n->s_case.body, indent + 2);
            break;
        case AST_LAYOUT:
            printf("LAYOUT\n");
            ast_dump(n->s_layout.expr, indent + 2);
            break;
        case AST_PROGRAM:
            printf("PROGRAM (%zu stmts)\n", n->s_prog.n);
            for (size_t i = 0; i < n->s_prog.n; i++)
                ast_dump(n->s_prog.stmts[i], indent + 2);
            break;
    }
}
