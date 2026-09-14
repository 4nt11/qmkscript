/* lex-debug.l -- lexer standalone, sin bison.
 * Genera build/lex-test para inspeccionar el stream de tokens de un .qks (v1).
 * Herramienta de debug: sobrevive aunque el parser evolucione.
 */

%option noyywrap
%option nodefault
%option yylineno

%x IN_STRING IN_CHAR

%{
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    T_KW_TYPE = 256, T_KW_TAP, T_KW_DELAY, T_KW_CHORD,
    T_STRING_LIT, T_CHAR_LIT, T_IDENT, T_NUMBER,
    T_LBRACK, T_RBRACK, T_COMMA, T_SEP,
    T_LBRACE, T_RBRACE,
};

static union { int num; char chr; char *str; } yylval_v;

static char  *sb = NULL;
static size_t sb_len = 0, sb_cap = 0;
static void sb_reset(void) {
    if (sb_cap == 0) { sb_cap = 64; sb = malloc(sb_cap); }
    sb_len = 0; sb[0] = '\0';
}
static void sb_push(char c) {
    if (sb_len + 1 >= sb_cap) { sb_cap *= 2; sb = realloc(sb, sb_cap); }
    sb[sb_len++] = c; sb[sb_len] = '\0';
}
%}

%%

"#".*                    { /* comentario a fin de línea */ }

"type"                   { return T_KW_TYPE;  }
"tap"                    { return T_KW_TAP;   }
"delay"                  { return T_KW_DELAY; }
"chord"                  { return T_KW_CHORD; }

"["                      { return T_LBRACK; }
"]"                      { return T_RBRACK; }
"{"                      { return T_LBRACE; }
"}"                      { return T_RBRACE; }
","                      { return T_COMMA;  }
";"                      { return T_SEP;    }

\"                       { sb_reset(); BEGIN(IN_STRING); }
<IN_STRING>\\n           { sb_push('\n'); }
<IN_STRING>\\t           { sb_push('\t'); }
<IN_STRING>\\r           { sb_push('\r'); }
<IN_STRING>\\\"          { sb_push('"');  }
<IN_STRING>\\\\          { sb_push('\\'); }
<IN_STRING>\n            { fprintf(stderr, "lex: unterminated string at line %d\n", yylineno); BEGIN(INITIAL); }
<IN_STRING>\"            { BEGIN(INITIAL); yylval_v.str = strdup(sb); return T_STRING_LIT; }
<IN_STRING>.             { sb_push(yytext[0]); }

\'                       { sb_reset(); BEGIN(IN_CHAR); }
<IN_CHAR>\\n             { sb_push('\n'); }
<IN_CHAR>\\t             { sb_push('\t'); }
<IN_CHAR>\\r             { sb_push('\r'); }
<IN_CHAR>\\\'            { sb_push('\''); }
<IN_CHAR>\\\\            { sb_push('\\'); }
<IN_CHAR>\n              { fprintf(stderr, "lex: unterminated char at line %d\n", yylineno); BEGIN(INITIAL); }
<IN_CHAR>\'              { BEGIN(INITIAL); yylval_v.chr = sb_len == 1 ? sb[0] : '?'; return T_CHAR_LIT; }
<IN_CHAR>.               { sb_push(yytext[0]); }

[0-9]+                   { yylval_v.num = atoi(yytext); return T_NUMBER; }
[a-z_][a-z0-9_]*         { yylval_v.str = strdup(yytext); return T_IDENT; }

[ \t]+                   { /* whitespace horizontal */ }
\r?\n                    { return T_SEP; }

.                        { fprintf(stderr, "lex: unexpected '%c' at line %d\n", *yytext, yylineno); }

%%

int main(int argc, char **argv) {
    if (argc > 1) {
        FILE *f = fopen(argv[1], "r");
        if (!f) { perror(argv[1]); return 1; }
        yyin = f;
    }
    int tok;
    while ((tok = yylex()) != 0) {
        switch (tok) {
            case T_KW_TYPE:    puts("KW_TYPE");   break;
            case T_KW_TAP:     puts("KW_TAP");    break;
            case T_KW_DELAY:   puts("KW_DELAY");  break;
            case T_KW_CHORD:   puts("KW_CHORD");  break;
            case T_STRING_LIT: printf("STRING_LIT(\"%s\")\n", yylval_v.str); free(yylval_v.str); break;
            case T_CHAR_LIT:   printf("CHAR_LIT('%c')\n", yylval_v.chr); break;
            case T_IDENT:      printf("IDENT(%s)\n", yylval_v.str); free(yylval_v.str); break;
            case T_NUMBER:     printf("NUMBER(%d)\n", yylval_v.num); break;
            case T_LBRACK:     puts("LBRACK");    break;
            case T_RBRACK:     puts("RBRACK");    break;
            case T_LBRACE:     puts("LBRACE");    break;
            case T_RBRACE:     puts("RBRACE");    break;
            case T_COMMA:      puts("COMMA");     break;
            case T_SEP:        puts("SEP");       break;
            default:           printf("??? (%d)\n", tok);
        }
    }
    return 0;
}
