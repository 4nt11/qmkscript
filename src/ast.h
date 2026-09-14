// qmkscript AST -- cinco tipos de nodo, un tagged union en C.
// Sin visitors, sin OOP. La lista de statements de PROGRAM es un vector
// dinamico plano.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef enum {
    AST_STRING,     // type "hola mundo"           -> string literal (quoted)
    AST_TAP,        // tap enter                   -> nombre de tecla (lowercase)
    AST_DELAY,      // delay 500                   -> milisegundos
    AST_CHORD,      // chord [gui, r]              -> mods bitmap + 1 tecla (simple, OP_CHORD)
    AST_CHORD_SEQ,  // chord [gui, r, "hola"]      -> mods bitmap + N acciones (REG+seq+UNREG)
    AST_NUM_LIT,    // 42, 0, 65535                -> literal entero (u32)
    AST_VAR_REF,    // x  (dentro de una expr)     -> ref a var declarada
    AST_VAR_DECL,   // var x = <expr>              -> nombre + AST de la expresión inicial
    AST_ASSIGN,     // x = <expr>                  -> reasigna var YA declarada
    AST_BIN_OP,     // a + b, x * 2, ...           -> op + lhs + rhs (postorder = RPN)
    AST_BLOCK,      // { stmt; stmt; ... }         -> container sintáctico (misma shape que PROGRAM)
    AST_IF,         // if cond { then } [else { else }]  -> C.3.b/c: cond + then + else_or_NULL
    AST_WHILE,      // while cond { body }         -> C.3.d: cond + body (loop hasta cond==0)
    AST_SWITCH,     // switch key { case N: ... }  -> C.3.g: key + vector de (value, body) + default_or_NULL
    AST_CASE,       // case N: body                -> C.3.g: nodo TRANSITORIO (carrier del parser, nunca sobrevive al reduce del switch_body)
    AST_LAYOUT,     // layout(EXPR)                -> C.4.b: emit(EXPR) + OP_LAYOUT
    AST_PROGRAM,    // secuencia top-level         -> vector de statements
} ast_kind_t;

// Enum de operadores binarios. El valor NO es el opcode de la VM: la
// conversión bin_op -> OP_ADD/SUB/... se hace en emit_bytecode.c.
// Manteniendo esta capa aislada, mañana podemos añadir un operador
// (SHL, MOD) sin tocar la ISA -- solo el emitter mapea.
typedef enum {
    BIN_ADD, BIN_SUB, BIN_MUL, BIN_DIV,
    BIN_EQ,  BIN_NEQ, BIN_LT,  BIN_GT,
} bin_op_t;

typedef struct ast_node ast_t;

struct ast_node {
    ast_kind_t kind;
    int        line;   // línea del .qks donde se originó (para errores del backend)
    union {
        struct { char *text; }                     s_string;
        struct { char *key;  }                     s_tap;    // "enter", "tab", "r", ...
        struct { int ms; }                         s_delay;
        struct { uint8_t mods; char *key; }        s_chord;  // bitmap HID mods, "r" o "enter"
        struct { uint8_t mods;
                 ast_t **actions;
                 size_t n, cap; }                  s_chord_seq;  // mods + N acciones
        struct { uint32_t value; }                 s_num;        // literal entero
        struct { char *name; }                     s_var_ref;    // referencia a var
        struct { char *name; ast_t *init; }        s_var_decl;   // 'var name = init_expr'
        struct { char *name; ast_t *value; }       s_assign;     // 'name = expr' (reasign)
        struct { bin_op_t op; ast_t *lhs, *rhs; }  s_bin_op;     // aritmética/comparación
        struct { ast_t **stmts; size_t n, cap; }   s_block;      // C.3.a: { ... } (misma shape que s_prog)
        struct { ast_t *cond; ast_t *then_body; ast_t *else_body; }  s_if;  // C.3.b/c: else_body es NULL si no hay else
        struct { ast_t *cond; ast_t *body; }       s_while;                   // C.3.d: while cond { body }
        struct { ast_t *key;
                 uint32_t *values;
                 ast_t   **bodies;
                 size_t    n, cap;
                 ast_t    *default_body; }         s_switch;                  // C.3.g: switch key { ... }
        struct { uint32_t value; ast_t *body; }    s_case;                    // C.3.g: transient carrier (parser only)
        struct { ast_t *expr; }                    s_layout;                  // C.4.b: layout(EXPR) -- EXPR runtime-evaluated
        struct { ast_t **stmts; size_t n, cap; }   s_prog;
    };
};

// constructores -- cada uno reserva un nodo del tipo correspondiente.
// Todos los strings los adopta el nodo (strdup interno). Caller libera su copia.
ast_t *ast_string(const char *text);
ast_t *ast_tap   (const char *key);
ast_t *ast_delay (int ms);
ast_t *ast_chord (uint8_t mods, const char *key);
ast_t *ast_chord_seq(uint8_t mods);                    // fase B: chord multi-elemento
void   ast_chord_seq_append(ast_t *cs, ast_t *action); // añade acción a un chord_seq
ast_t *ast_num_lit(uint32_t value);                    // C.2.a: literal entero
ast_t *ast_var_ref(const char *name);                  // C.2.b: referencia a var
ast_t *ast_var_decl(const char *name, ast_t *init);    // C.2.a: 'var name = init'
ast_t *ast_assign(const char *name, ast_t *value);      // C.2.e: reasign
ast_t *ast_bin_op(bin_op_t op, ast_t *lhs, ast_t *rhs); // C.2.c: op binaria
ast_t *ast_block(void);                                 // C.3.a: { ... } container
void   ast_block_append(ast_t *blk, ast_t *stmt);
ast_t *ast_if(ast_t *cond, ast_t *then_body, ast_t *else_body); // C.3.b/c: else_body NULL si no hay else
ast_t *ast_while(ast_t *cond, ast_t *body);                    // C.3.d: while cond { body }

// C.3.g: switch construction. Se construye incrementalmente durante el parse:
// ast_switch_new() crea el nodo vacío, cada case se agrega vía _add_case_node
// (que absorbe un AST_CASE transitorio del parser), y default via _set_default.
// La expresión key se pega al final con ast_switch_set_key.
ast_t *ast_switch_new(void);
void   ast_switch_set_key(ast_t *sw, ast_t *key);
void   ast_switch_add_case_node(ast_t *sw, ast_t *case_node);  // absorbe AST_CASE
void   ast_switch_set_default(ast_t *sw, ast_t *body);
ast_t *ast_case_new(uint32_t value, ast_t *body);              // AST_CASE transitorio
ast_t *ast_layout(ast_t *expr);                                // C.4.b: layout(EXPR)
ast_t *ast_program(void);
void   ast_program_append(ast_t *prog, ast_t *stmt);

// debug: imprime el arbol indentado a stdout
void   ast_dump(const ast_t *node, int indent);

// ponytail: sin ast_free() en v0. El proceso termina, el SO limpia.
