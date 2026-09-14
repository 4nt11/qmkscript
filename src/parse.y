/* parse.y -- gramática LALR(1) de qmkscript v1.
 *
 * v1 admite CUATRO statements con sintaxis python-ish:
 *   type "..."         -- teclea la string en la ventana activa
 *   tap ident          -- toca 1 tecla nombrada (enter, esc, tab, r, a, 1, ...)
 *   delay N            -- espera N ms
 *   chord [m, m, ..., k]  -- mods held mientras se ejecuta k (última posición)
 *
 * Separadores de statement: ; o newline (mismo token SEP).
 * Comentarios #  a fin de línea (los come el lexer).
 *
 * NOTA v1: el chord SOLO acepta 1 tecla al final. Multi-tecla + strings dentro
 * del array llegan en fase B (nuevos opcodes OP_REG_MODS/OP_UNREG_MODS).
 */

%{
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ast.h"

extern int   yylex(void);
extern int   yylineno;
extern FILE *yyin;
void         yyerror(ast_t **root, const char *msg);

/* Definido en emit_bytecode.c. Guarda el target del bind() para emitir en
 * el header al final. Duplicate detection + line reporting adentro. */
extern void qks_bind_set(ast_t *target, uint8_t layer, int line);
/* Layer semantics para bind():
 *   0..15 -> match solo si ese layer está on (IS_LAYER_ON)
 *   0xFF  -> match en cualquier layer (wildcard)
 *   sin layer= en source -> default 3 (OFFSEC) por convención. */
#define QKS_BIND_LAYER_ANY     0xFF
#define QKS_BIND_LAYER_DEFAULT 3

/* --- helpers ---------------------------------------------------------
 * Conversión nombre -> bits del bitmap HID. Devuelve 0 si el nombre no es
 * un modificador conocido.
 *
 * Convención dispatch: los nombres genéricos ("gui", "ctrl", "shift", "alt")
 * emiten AMBOS bits (L|R) -> el firmware los interpreta como "cualquier lado".
 * Los nombres explícitos ("lgui", "rgui", "lctl", ...) emiten solo su bit ->
 * matchean solo ese lado. Ver qks_dispatch_lookup en el firmware.
 */
static uint8_t mod_bit_of(const char *name) {
    if (!strcmp(name, "gui"))   return 0x08 | 0x80;  // L|R generic
    if (!strcmp(name, "ctrl"))  return 0x01 | 0x10;
    if (!strcmp(name, "alt"))   return 0x04 | 0x40;
    if (!strcmp(name, "shift")) return 0x02 | 0x20;
    if (!strcmp(name, "lgui"))  return 0x08;
    if (!strcmp(name, "lctl"))  return 0x01;
    if (!strcmp(name, "lalt"))  return 0x04;
    if (!strcmp(name, "lsft"))  return 0x02;
    if (!strcmp(name, "rgui"))  return 0x80;
    if (!strcmp(name, "rctl"))  return 0x10;
    if (!strcmp(name, "ralt"))  return 0x40;
    if (!strcmp(name, "rsft"))  return 0x20;
    return 0;
}

/* Construye una string de 1 char (para pasarle a ast_chord). Caller libera. */
static char *one_char_str(char c) {
    char *s = malloc(2); s[0] = c; s[1] = '\0'; return s;
}
%}

%parse-param { ast_t **root }

%union {
    int      num;
    char     chr;
    char    *str;
    ast_t   *node;
}

%token <str>   IDENT STRING_LIT
%token <chr>   CHAR_LIT
%token <num>   NUMBER
%token         KW_TYPE KW_TAP KW_DELAY KW_CHORD KW_VAR KW_IF KW_ELSE KW_WHILE
%token         KW_SWITCH KW_CASE KW_DEFAULT KW_BIND KW_LAYOUT KW_LAYER
%token         LBRACK RBRACK LPAREN RPAREN LBRACE RBRACE COMMA SEP ASSIGN COLON
%token         PLUS MINUS STAR SLASH EQ_EQ BANG_EQ LT_OP GT_OP

/* Precedencia de expr, orden = precedencia (ARRIBA = MÁS BAJA).
 * %left     -> asocia izquierda: a-b-c = (a-b)-c
 * %nonassoc -> NO asociativo: a<b<c es ERROR de parseo (contra el bug
 *              clásico de C donde a<b<c evalúa (a<b)<c = 0 o 1 <c).
 * Comparaciones tienen precedencia MENOR que aritmética (a+b < c*d
 * parsea como (a+b) < (c*d)).
 */
%nonassoc EQ_EQ BANG_EQ LT_OP GT_OP
%left     PLUS MINUS
%left     STAR SLASH

%type <node>   program stmt block block_body if_stmt chord_elems expr
%type <node>   switch_body case_clause bind_target

%%

program : /* vacío */         { *root = ast_program(); $$ = *root; }
        | program stmt        { ast_program_append($1, $2); $$ = $1; }
        | program SEP         { $$ = $1; }
        ;

stmt    : KW_TYPE  STRING_LIT           { $$ = ast_string($2); $$->line = yylineno; free($2); }
        | KW_TAP   IDENT                { $$ = ast_tap($2);    $$->line = yylineno; free($2); }
        | KW_TAP   CHAR_LIT             { char *s = one_char_str($2);
                                          $$ = ast_tap(s); $$->line = yylineno; free(s); }
        | KW_DELAY NUMBER               { $$ = ast_delay($2); $$->line = yylineno; }
        | KW_VAR IDENT ASSIGN expr      { $$ = ast_var_decl($2, $4); $$->line = yylineno; free($2); }
        | IDENT ASSIGN expr             { $$ = ast_assign($1, $3);   $$->line = yylineno; free($1); }
        | block                         { $$ = $1; }
        | if_stmt                       { $$ = $1; }
        | KW_WHILE expr block           {
                                          /* C.3.d: while cond { body }. Misma sintaxis
                                           * que if: sin parens en cond, braces obligatorias.
                                           * LBRACE termina la expr por FIRST-set disjoint. */
                                          $$ = ast_while($2, $3); $$->line = yylineno;
                                        }
        | KW_BIND LPAREN bind_target RPAREN
                                        {
                                          /* Sin `layer=` -> default OFFSEC (layer 3). */
                                          qks_bind_set($3, QKS_BIND_LAYER_DEFAULT, yylineno);
                                          $$ = ast_block();
                                          $$->line = yylineno;
                                        }
        | KW_BIND LPAREN KW_LAYER ASSIGN NUMBER COMMA bind_target RPAREN
                                        {
                                          /* bind(layer=N, ...): match solo si layer N está on.
                                           * N debe caber en 0..15 (layers de QMK). */
                                          if ($5 < 0 || $5 > 15) {
                                              yyerror(root, "bind(layer=N): N fuera de rango [0..15]");
                                              YYERROR;
                                          }
                                          qks_bind_set($7, (uint8_t)$5, yylineno);
                                          $$ = ast_block();
                                          $$->line = yylineno;
                                        }
        | KW_BIND LPAREN KW_LAYER ASSIGN IDENT COMMA bind_target RPAREN
                                        {
                                          /* bind(layer=any, ...): match en cualquier layer. */
                                          if (strcmp($5, "any") != 0) {
                                              yyerror(root, "bind(layer=IDENT): solo 'any' es valido (o un numero)");
                                              free($5); YYERROR;
                                          }
                                          free($5);
                                          qks_bind_set($7, QKS_BIND_LAYER_ANY, yylineno);
                                          $$ = ast_block();
                                          $$->line = yylineno;
                                        }
        | KW_LAYOUT LPAREN expr RPAREN  {
                                          /* C.4.b: runtime layout switch. EXPR se evalúa,
                                           * TOS pasa a OP_LAYOUT que actualiza current_layout
                                           * en la VM. `layout(LAYOUT_LATAM)` es lo típico
                                           * pero `layout(i)` con i variable también funciona
                                           * (usa case del while iterate-layouts). */
                                          $$ = ast_layout($3); $$->line = yylineno;
                                        }
        | KW_SWITCH expr LBRACE switch_body RBRACE
                                        {
                                          /* C.3.g: switch key { case N: ... default: ... }
                                           * key es expr arbitraria (LBRACE termina).
                                           * switch_body es un AST_SWITCH que fue
                                           * incrementalmente acumulando cases; ahora
                                           * le pegamos la key y devolvemos. */
                                          ast_switch_set_key($4, $2);
                                          $$ = $4; $$->line = yylineno;
                                        }
        | KW_CHORD LBRACK chord_elems RBRACK
                                        {
                                          ast_t *cs = $3;
                                          if (cs->s_chord_seq.n == 0) {
                                              yyerror(root, "chord [...] necesita al menos una tecla o string");
                                              YYERROR;
                                          }
                                          /* FOLD: si es 1 sola action tipo TAP -> AST_CHORD (3B en bytecode).
                                           * Si no, se queda como AST_CHORD_SEQ (REG_MODS + seq + UNREG_MODS). */
                                          if (cs->s_chord_seq.n == 1 &&
                                              cs->s_chord_seq.actions[0]->kind == AST_TAP) {
                                              $$ = ast_chord(cs->s_chord_seq.mods,
                                                             cs->s_chord_seq.actions[0]->s_tap.key);
                                              /* ponytail: el cs y su action interno quedan como garbage
                                               * hasta que termine el proceso (sin ast_free en v0). */
                                          } else {
                                              $$ = cs;
                                          }
                                          $$->line = yylineno;
                                        }
        ;

/* chord_elems: acumula elementos en un AST_CHORD_SEQ (mods bitmap + lista
 * de actions). Regla semántica: los mods deben venir ANTES de cualquier
 * tecla/string. `mod` después de una action = error (semánticamente raro).
 *
 * El fold "colapsar a AST_CHORD" ocurre en la regla del stmt, no aquí --
 * aquí solo acumulamos. Menos acoplamiento.
 */
chord_elems
    : IDENT
        {
            $$ = ast_chord_seq(0);
            uint8_t bit = mod_bit_of($1);
            if (bit) $$->s_chord_seq.mods |= bit;
            else     ast_chord_seq_append($$, ast_tap($1));
            free($1);
        }
    | CHAR_LIT
        {
            $$ = ast_chord_seq(0);
            char *k = one_char_str($1);
            ast_chord_seq_append($$, ast_tap(k));
            free(k);
        }
    | STRING_LIT
        {
            $$ = ast_chord_seq(0);
            ast_chord_seq_append($$, ast_string($1));
            free($1);
        }
    | chord_elems COMMA IDENT
        {
            $$ = $1;
            uint8_t bit = mod_bit_of($3);
            if (bit) {
                if ($$->s_chord_seq.n > 0) {
                    yyerror(root, "modificador después de una tecla (los mods van al principio)");
                    free($3); YYERROR;
                }
                $$->s_chord_seq.mods |= bit;
            } else {
                ast_chord_seq_append($$, ast_tap($3));
            }
            free($3);
        }
    | chord_elems COMMA CHAR_LIT
        {
            $$ = $1;
            char *k = one_char_str($3);
            ast_chord_seq_append($$, ast_tap(k));
            free(k);
        }
    | chord_elems COMMA STRING_LIT
        {
            $$ = $1;
            ast_chord_seq_append($$, ast_string($3));
            free($3);
        }
    ;

/* C.3.a: bloques { stmt; stmt; ... }.
 * `block_body` acumula stmts idéntico a `program`: tolerá SEP sueltos y
 * cuerpo vacío. Un `block` es SINTAX PURA — no ejecuta ni contiene nada
 * propio en bytecode; su walker delega en cada stmt hijo. Existe como
 * container para que if/while (C.3.b+) tengan un `then/else/body` con tipo
 * unívoco. Sin esto, cada estructura tendría que llevar su propio vector
 * de stmts, duplicando la lógica de append.
 *
 * Como es un stmt más, `{ type "hola" }` en top-level es sintácticamente
 * válido y semánticamente idéntico a `type "hola"`. YAGNI: sin scope de
 * vars hasta fase D.
 */
block   : LBRACE block_body RBRACE   { $$ = $2; }
        ;

block_body
        : /* vacío */                { $$ = ast_block(); }
        | block_body stmt            { ast_block_append($1, $2); $$ = $1; }
        | block_body SEP             { $$ = $1; }
        ;

/* C.3.b/c: if / else / else-if.
 *
 * Tres alternativas -- ninguna genera shift/reduce porque LBRACE es
 * fondo distinto de KW_IF y de KW_ELSE, y `block` obliga a cerrar el
 * then antes de que KW_ELSE pueda aparecer. NO hay dangling-else aquí,
 * las braces obligatorias lo mataron a nivel léxico.
 *
 * La tercera regla (chaining) desazucara `else if X` a `else { if X }`:
 * envuelve el if_stmt recursivo en un AST_BLOCK sintético para mantener
 * el invariante "else_body es un AST_BLOCK". Cero código nuevo en emit
 * -- se recurre igual que cualquier else normal. Ese es el pago del
 * diseño: la sintaxis se enriquece sin tocar el back-end.
 */
if_stmt : KW_IF expr block                   {
                                               $$ = ast_if($2, $3, NULL);
                                               $$->line = yylineno;
                                             }
        | KW_IF expr block KW_ELSE block     {
                                               $$ = ast_if($2, $3, $5);
                                               $$->line = yylineno;
                                             }
        | KW_IF expr block KW_ELSE if_stmt   {
                                               /* `else if X { }` -- envolver el if_stmt
                                                * hijo en un AST_BLOCK sintético mantiene
                                                * el invariante y no requiere rama nueva
                                                * en emit_bytecode. */
                                               ast_t *wrap = ast_block();
                                               ast_block_append(wrap, $5);
                                               $$ = ast_if($2, $3, wrap);
                                               $$->line = yylineno;
                                             }
        ;

/* bind() target: 3 sabores. Todos producen un AST_TAP / AST_CHORD / AST_NUM_LIT
 * que emit_bytecode inspeccionará para extraer (kc, mods).
 *
 *   bind(KC_R)          -> NUMBER (lexer resolvió KC_R desde la tabla generada)
 *   bind(tap enter)     -> AST_TAP con key="enter"; emit hace name_to_kc
 *   bind(tap 'r')       -> AST_TAP con key="r"
 *   bind(chord [gui,r]) -> AST_CHORD (folded desde chord_elems)
 */
bind_target
    : NUMBER                           { $$ = ast_num_lit((uint32_t)$1); $$->line = yylineno; }
    | KW_TAP IDENT                     { $$ = ast_tap($2); $$->line = yylineno; free($2); }
    | KW_TAP CHAR_LIT                  { char *s = one_char_str($2);
                                         $$ = ast_tap(s); $$->line = yylineno; free(s); }
    | KW_CHORD LBRACK chord_elems RBRACK
                                       {
                                         /* mismo fold que en el stmt normal de chord:
                                          * si es 1 sola tecla, colapsa a AST_CHORD; si
                                          * multi-elem, queda AST_CHORD_SEQ -- que emit_bytecode
                                          * rechazará como E_BIND_INVALID (bind no soporta seq). */
                                         ast_t *cs = $3;
                                         if (cs->s_chord_seq.n == 1 &&
                                             cs->s_chord_seq.actions[0]->kind == AST_TAP) {
                                             $$ = ast_chord(cs->s_chord_seq.mods,
                                                            cs->s_chord_seq.actions[0]->s_tap.key);
                                         } else {
                                             $$ = cs;
                                         }
                                         $$->line = yylineno;
                                       }
    ;

/* C.3.g: switch body -- acumula cases y default en un AST_SWITCH.
 *
 * `switch_body` empieza como AST_SWITCH vacío. Cada case_clause es un
 * AST_CASE transitorio (nunca sobrevive al reduce -- se absorbe en los
 * vectores paralelos values[]/bodies[] del AST_SWITCH). default_clause
 * setea el default_body.
 *
 * `stmt` como body: cabe cualquier statement (single tap/type, bloque, if,
 * while, otro switch). Multi-stmt requiere `{ ... }` (usar block).
 *
 * NUMBER es literal solo -- expresiones runtime como `case x+1:` no se
 * aceptan (mantener la tabla determinística compile-time).
 */
switch_body
    : /* vacío */                       { $$ = ast_switch_new(); }
    | switch_body case_clause           { ast_switch_add_case_node($1, $2); $$ = $1; }
    | switch_body KW_DEFAULT COLON stmt { ast_switch_set_default($1, $4); $$ = $1; }
    | switch_body SEP                   { $$ = $1; }
    ;

case_clause
    : KW_CASE NUMBER COLON stmt         { $$ = ast_case_new((uint32_t)$2, $4);
                                          $$->line = yylineno; }
    ;

/* Expresiones. C.2.a: literales numéricos. C.2.b: + var refs.
 * C.2.c: + aritmética con precedencia. C.2.d: + comparación.
 *
 * Nota: la gramática `expr : expr OP expr` es AMBIGUA (`1+2*3` puede ser
 * `(1+2)*3` o `1+(2*3)`). Bison la acepta gracias a las directivas
 * `%left` de arriba: los shift/reduce se resuelven en favor del operador
 * de MAYOR precedencia. Idiom clásico de bison.
 */
expr : NUMBER                  { $$ = ast_num_lit((uint32_t)$1); $$->line = yylineno; }
     | IDENT                   { $$ = ast_var_ref($1); $$->line = yylineno; free($1); }
     | LPAREN expr RPAREN      { $$ = $2; }
     | expr PLUS    expr       { $$ = ast_bin_op(BIN_ADD, $1, $3); $$->line = yylineno; }
     | expr MINUS   expr       { $$ = ast_bin_op(BIN_SUB, $1, $3); $$->line = yylineno; }
     | expr STAR    expr       { $$ = ast_bin_op(BIN_MUL, $1, $3); $$->line = yylineno; }
     | expr SLASH   expr       { $$ = ast_bin_op(BIN_DIV, $1, $3); $$->line = yylineno; }
     | expr EQ_EQ   expr       { $$ = ast_bin_op(BIN_EQ,  $1, $3); $$->line = yylineno; }
     | expr BANG_EQ expr       { $$ = ast_bin_op(BIN_NEQ, $1, $3); $$->line = yylineno; }
     | expr LT_OP   expr       { $$ = ast_bin_op(BIN_LT,  $1, $3); $$->line = yylineno; }
     | expr GT_OP   expr       { $$ = ast_bin_op(BIN_GT,  $1, $3); $$->line = yylineno; }
     ;

%%

void yyerror(ast_t **root, const char *msg) {
    (void)root;
    fprintf(stderr, "qmkscript: parse error at line %d: %s\n", yylineno, msg);
}
