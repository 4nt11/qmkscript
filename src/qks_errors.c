// qks_errors.c -- implementación de qks_err_report.
// Formato canonical: "qmkscript: line N: [E_XXX] <mensaje>\n"
// Uso variádico para que cada callsite escriba su mensaje contextual.
#include "qks_errors.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

_Noreturn void qks_err_report(qks_err_t kind, int line, const char *fmt, ...) {
    if (line > 0) {
        fprintf(stderr, "qmkscript: line %d: [%s] ", line, qks_err_short(kind));
    } else {
        fprintf(stderr, "qmkscript: [%s] ", qks_err_short(kind));
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}
