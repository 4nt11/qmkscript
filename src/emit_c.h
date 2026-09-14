// emit_c.h -- backend #1: AST -> bloque C que se pega en un keymap QMK.
// Emite un cuerpo de funcion que llama SEND_STRING/SS_TAP/SS_DELAY.
#pragma once
#include <stdio.h>
#include "ast.h"

// Escribe el bloque C en 'out'. No incluye main ni firmas -- solo el cuerpo,
// para que puedas pegarlo dentro de tu process_record_user o macro custom.
void emit_c(FILE *out, const ast_t *node);
