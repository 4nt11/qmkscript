// vm_internal.h -- macros y helpers privados de la implementación de vm.c.
// NO forma parte del vendor drop -- el .h público (vm.h) se queda limpio.
// Convención: solo lo incluye vm.c. Firmwares que copien vm.{h,c} NO
// necesitan este fichero -- toca copiarlo si añaden traducciones nuevas
// del dispatch, pero nada más.
//
// Requiere en scope las siguientes variables locales del dispatch:
//   pc    : size_t         (program counter)
//   prog  : vm_program_t*  (para prog->len)
//   ops   : vm_ops_t*      (tabla de callbacks, puede ser NULL)
//   ctx   : void*          (opaco, forwarded a cada callback)
//   stack : uint32_t[]     (value stack)
//   sp    : uint8_t        (stack pointer, índice del próximo slot libre)
//   vars  : uint32_t[]     (variables globales)
#pragma once
#include "vm.h"

// ============================================================
// bounds check / callback dispatch
// ============================================================

// Verifica que quedan >=n bytes tras pc; return VM_ERR_TRUNCATED si no.
// Es macro (no static inline) precisamente por el return prematuro.
#define NEED(n) do { if (pc + (n) > prog->len) return VM_ERR_TRUNCATED; } while(0)

// Llama al callback si está enrollado. NULL == no-op silencioso
// (dry-run, testing, firmware que aún no cablea todo).
#define CALL(fn, ...) do { if (ops && ops->fn) ops->fn(__VA_ARGS__); } while(0)

// ============================================================
// stack machine primitives (C.1)
// ============================================================

// Push un uint32_t al stack. Overflow -> return VM_ERR_STACK_OVERFLOW.
#define PUSH(v) do { \
    if (sp >= VM_STACK_SIZE) return VM_ERR_STACK_OVERFLOW; \
    stack[sp++] = (uint32_t)(v); \
} while(0)

// Pop un uint32_t del stack a `dst`. Underflow -> return VM_ERR_STACK_UNDERFLOW.
#define POP(dst) do { \
    if (sp == 0) return VM_ERR_STACK_UNDERFLOW; \
    (dst) = stack[--sp]; \
} while(0)

// Pop dos operandos (a, b) para binary op. b es TOS (empujado último), a es TOS-1.
// Convención: `a OP b` (para SUB: a - b, para DIV: a / b, para LT: a < b, etc.)
#define POP2(a, b) do { POP(b); POP(a); } while(0)
