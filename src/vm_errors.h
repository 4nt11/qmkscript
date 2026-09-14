// vm_errors.h -- traducción vm_result_t -> string humano.
//
// SOLO host-side. El firmware NO incluye este header: ahorra ~200 B de
// flash con strings que nunca usaría (los códigos numéricos de error se
// exponen por qmk console si hace falta debug).
//
// Convención de estilo: data (tablas, mensajes) en headers; code (lógica)
// en .c. Aquí la lookup vive como static inline porque es trivialmente
// una lectura del array.
#pragma once
#include "vm.h"
#include <stddef.h>

// Tabla de nombres humanos, indexada por vm_result_t. Slots vacíos se
// tratan como (código desconocido) por vm_err_str.
static const char *const vm_error_names[] = {
    [VM_OK]                    = "OK",
    [VM_ERR_TOO_SHORT]         = "fichero < 12 bytes (no cabe ni el header)",
    [VM_ERR_BAD_MAGIC]         = "magic no es 'QKSC'",
    [VM_ERR_BAD_VERSION]       = "versión de bytecode incompatible",
    [VM_ERR_LEN_MISMATCH]      = "code_len != file_size - 12",
    [VM_ERR_NO_HALT]           = "el código no termina en HALT",
    [VM_ERR_CODE_TOO_LONG]     = "code_len excede VM_MAX_CODE_LEN",
    [VM_ERR_JMP_MISALIGNED]    = "JMP/JMPZ aterriza en el medio de otra instrucción (weird-machine)",
    [VM_ERR_TRUNCATED]         = "opcode con operando cortado por final de code",
    [VM_ERR_UNKNOWN_OP]        = "opcode desconocido",
    [VM_ERR_STACK_OVERFLOW]    = "stack overflow (>32 slots simultáneos)",
    [VM_ERR_STACK_UNDERFLOW]   = "stack underflow (pop sin push -- bytecode malformado)",
    [VM_ERR_VAR_OOB]           = "índice de variable fuera de rango (>= 32)",
    [VM_ERR_JMP_OOB]           = "target de jump cae fuera del código",
    [VM_ERR_DIV_ZERO]          = "división por cero",
    [VM_ERR_LAYOUT_OOB]        = "layout index fuera de rango (LAYOUT_DEFAULT..LAYOUT_ES)",
};

// Devuelve el nombre del error, o "(código desconocido)" para valores
// fuera de la tabla o slots vacíos.
static inline const char *vm_err_str(vm_result_t r) {
    size_t idx = (size_t)r;
    if (idx < sizeof(vm_error_names) / sizeof(vm_error_names[0])
        && vm_error_names[idx] != NULL) {
        return vm_error_names[idx];
    }
    return "(código desconocido)";
}
