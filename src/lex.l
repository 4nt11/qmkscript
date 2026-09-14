/* lex.l -- lexer INTEGRADO con bison, sintaxis v1.
 *
 * Cambios vs v0:
 *   - keywords lowercase: type, tap, delay, chord
 *   - strings "..."  con escapes \" \\ \n \t \r  (via start condition IN_STRING)
 *   - chars '.'      con escapes \' \\ \n \t \r  (via start condition IN_CHAR)
 *   - comentarios #  al inicio o inline hasta \n
 *   - ; y newline separan statements (mismo token SEP)
 *   - IDENT lowercase para nombres de teclas y modificadores
 *   - chord [gui, r]  -> LBRACK, RBRACK, COMMA
 */

%option noyywrap
%option nodefault
%option yylineno

%x IN_STRING IN_CHAR

%{
#include "ast.h"
#include "parse.tab.h"
#include "keycodes_generated.h"   /* qks_lookup_keycode() para tokens UPPER */
#include "layouts_generated.h"    /* qks_lookup_layout()  para LAYOUT_* */
#include "qks_errors.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Buffer dinámico compartido por IN_STRING / IN_CHAR. Un solo buffer alcanza:
 * el lexer solo está en uno de los dos estados a la vez. Se reinicia por token.
 */
static char  *sb = NULL;
static size_t sb_len = 0, sb_cap = 0;

static void sb_reset(void) {
    if (sb_cap == 0) { sb_cap = 64; sb = malloc(sb_cap); }
    sb_len = 0; sb[0] = '\0';
}
static void sb_push(char c) {
    if (sb_len + 1 >= sb_cap) {
        sb_cap *= 2;
        sb = realloc(sb, sb_cap);
    }
    sb[sb_len++] = c;
    sb[sb_len]   = '\0';
}
%}

%%

"#".*                    { /* comentario a fin de línea: descartar */ }

"type"                   { return KW_TYPE;  }
"tap"                    { return KW_TAP;   }
"delay"                  { return KW_DELAY; }
"chord"                  { return KW_CHORD; }
"var"                    { return KW_VAR;   }
"if"                     { return KW_IF;      }
"else"                   { return KW_ELSE;    }
"while"                  { return KW_WHILE;   }
"switch"                 { return KW_SWITCH;  }
"case"                   { return KW_CASE;    }
"default"                { return KW_DEFAULT; }
"bind"                   { return KW_BIND;    }
"layout"                 { return KW_LAYOUT;  }
"layer"                  { return KW_LAYER;   }

"["                      { return LBRACK; }
"]"                      { return RBRACK; }
"("                      { return LPAREN; }
")"                      { return RPAREN; }
"{"                      { return LBRACE; }
"}"                      { return RBRACE; }
","                      { return COMMA;  }
";"                      { return SEP;    }
"="                      { return ASSIGN; }
":"                      { return COLON;  }
"+"                      { return PLUS;   }
"-"                      { return MINUS;  }
"*"                      { return STAR;   }
"/"                      { return SLASH;  }
"=="                     { return EQ_EQ;  }
"!="                     { return BANG_EQ; }
"<"                      { return LT_OP;  }
">"                      { return GT_OP;  }

\"                       { sb_reset(); BEGIN(IN_STRING); }
<IN_STRING>\\n           { sb_push('\n'); }
<IN_STRING>\\t           { sb_push('\t'); }
<IN_STRING>\\r           { sb_push('\r'); }
<IN_STRING>\\\"          { sb_push('"');  }
<IN_STRING>\\\\          { sb_push('\\'); }
<IN_STRING>\n            { fprintf(stderr, "qmkscript: unterminated string at line %d\n", yylineno);
                           BEGIN(INITIAL); }
<IN_STRING>\"            { BEGIN(INITIAL);
                           yylval.str = strdup(sb ? sb : "");
                           return STRING_LIT; }
<IN_STRING>.             { sb_push(yytext[0]); }

\'                       { sb_reset(); BEGIN(IN_CHAR); }
<IN_CHAR>\\n             { sb_push('\n'); }
<IN_CHAR>\\t             { sb_push('\t'); }
<IN_CHAR>\\r             { sb_push('\r'); }
<IN_CHAR>\\\'            { sb_push('\''); }
<IN_CHAR>\\\\            { sb_push('\\'); }
<IN_CHAR>\n              { fprintf(stderr, "qmkscript: unterminated char at line %d\n", yylineno);
                           BEGIN(INITIAL); }
<IN_CHAR>\'              { BEGIN(INITIAL);
                           if (sb_len != 1) {
                               fprintf(stderr, "qmkscript: char literal debe tener exactamente 1 char (línea %d, %zu)\n",
                                       yylineno, sb_len);
                               yylval.chr = '\0';
                           } else {
                               yylval.chr = sb[0];
                           }
                           return CHAR_LIT; }
<IN_CHAR>.               { sb_push(yytext[0]); }

[0-9]+                   { yylval.num = atoi(yytext); return NUMBER; }
[a-z_][a-z0-9_]*         { yylval.str = strdup(yytext); return IDENT; }

    /* Uppercase identifier (KC_*, QK_*, LAYOUT_*, etc.) -- resuelto contra
     * las dos tablas generadas: keycodes primero, layouts como fallback.
     * Si no se encuentra en ninguna, error compile-time.
     */
[A-Z][A-Z0-9_]+          {
                             int32_t v = qks_lookup_keycode(yytext);
                             if (v < 0) v = qks_lookup_layout(yytext);
                             if (v < 0) {
                                 qks_err_report(QKS_ERR_KEYCODE_UNKNOWN, yylineno,
                                                "identifier UPPER desconocido '%s' "
                                                "(no está en QMK keycodes.h ni en layouts)",
                                                yytext);
                             }
                             yylval.num = v;
                             return NUMBER;
                         }

[ \t]+                   { /* whitespace horizontal ignorado */ }
\r?\n                    { return SEP; }

.                        { fprintf(stderr, "qmkscript: unexpected '%c' at line %d\n",
                                   *yytext, yylineno); }

%%
