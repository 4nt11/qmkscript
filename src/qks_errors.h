// qks_errors.h -- errores tipados del compilador.
//
// Filosofía: LangSec-aligned. Errores como VALORES enumerados, no como
// strings ad-hoc. El formato humano del mensaje ("qmkscript: line N:
// [E_XXX] ...") lo genera qks_err_report() en qks_errors.c, garantizando
// consistencia y machine-readability para los tests.
//
// Estilo: data (enum + tabla de nombres) en .h; code (la función que
// formatea y sale) en qks_errors.c. Ver [[anti-c-style]].
//
// Roadmap: la opción "C" del design (errores como valores estructurados
// que fluyen hasta main() sin exit temprano) queda para cuando (a)
// queramos acumular varios errores por compilación o (b) integremos el
// compilador en un IDE con structured error reporting.
#pragma once
#include <stddef.h>

typedef enum {
    QKS_ERR_OOM,                   // out of memory (raro, buffer dinámico)
    QKS_ERR_KEY_UNKNOWN,           // "tap zzz" / "chord [ctrl, zzz]"
    QKS_ERR_DELAY_OUT_OF_RANGE,    // delay N con N > 65535 o < 0
    QKS_ERR_STRING_TOO_LONG,       // type "..." con len > 65535 (imposible práctico)
    QKS_ERR_VAR_UNDECLARED,        // referencia a var no declarada
    QKS_ERR_VAR_REDECLARED,        // var x = 1; var x = 2 (nombre reusado)
    QKS_ERR_VAR_LIMIT_EXCEEDED,    // más de 32 vars (VM_VARS_SIZE)
    QKS_ERR_JUMP_TOO_FAR,          // body de if/while > 32767 bytes (max i16 offset)
    QKS_ERR_DUPLICATE_CASE,        // `case N` con N que ya apareció en el mismo switch
    QKS_ERR_SWITCH_TOO_LARGE,      // switch con > 65535 cases (max u16 n_cases)
    QKS_ERR_KEYCODE_UNKNOWN,       // KC_XYZ no existe en la tabla generada
    QKS_ERR_BIND_DUPLICATE,        // bind() aparece más de una vez en el mismo .qks
    QKS_ERR_BIND_INVALID,          // bind(chord [...]) con chord multi-elem (solo mods+1key)
} qks_err_t;

// Nombres cortos para el prefijo "[E_XXX]" del mensaje. Los tests grepean
// por estos códigos -- estables aunque el texto libre del mensaje cambie.
static const char *const qks_err_short_names[] = {
    [QKS_ERR_OOM]                  = "E_OOM",
    [QKS_ERR_KEY_UNKNOWN]          = "E_KEY_UNKNOWN",
    [QKS_ERR_DELAY_OUT_OF_RANGE]   = "E_DELAY_OOR",
    [QKS_ERR_STRING_TOO_LONG]      = "E_STRING_TOO_LONG",
    [QKS_ERR_VAR_UNDECLARED]       = "E_VAR_UNDECLARED",
    [QKS_ERR_VAR_REDECLARED]       = "E_VAR_REDECLARED",
    [QKS_ERR_VAR_LIMIT_EXCEEDED]   = "E_VAR_LIMIT_EXCEEDED",
    [QKS_ERR_JUMP_TOO_FAR]         = "E_JUMP_TOO_FAR",
    [QKS_ERR_DUPLICATE_CASE]       = "E_DUPLICATE_CASE",
    [QKS_ERR_SWITCH_TOO_LARGE]     = "E_SWITCH_TOO_LARGE",
    [QKS_ERR_KEYCODE_UNKNOWN]      = "E_KEYCODE_UNKNOWN",
    [QKS_ERR_BIND_DUPLICATE]       = "E_BIND_DUPLICATE",
    [QKS_ERR_BIND_INVALID]         = "E_BIND_INVALID",
};

// Devuelve el código corto ("E_VAR_UNDECLARED", etc.) o "E_UNKNOWN" para
// valores fuera del enum (defensivo).
static inline const char *qks_err_short(qks_err_t k) {
    size_t idx = (size_t)k;
    if (idx < sizeof(qks_err_short_names) / sizeof(qks_err_short_names[0])
        && qks_err_short_names[idx] != NULL) {
        return qks_err_short_names[idx];
    }
    return "E_UNKNOWN";
}

// Formatea "qmkscript: line N: [E_XXX] <mensaje del fmt>\n" a stderr y sale(1).
// `line = 0` omite la línea (para errores sin contexto, tipo OOM).
_Noreturn void qks_err_report(qks_err_t kind, int line, const char *fmt, ...);
