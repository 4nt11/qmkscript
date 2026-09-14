// emit_vial.h -- backend #2: AST -> macro Vial en formato JSON.
// Vial acepta macros como listas de acciones ["tap", "kc"], ["delay", ms], ["text", "..."].
// Este backend permite iterar SIN recompilar el firmware: pegas el JSON en la GUI de Vial
// (o lo empujas via su CLI) y ya esta.
#pragma once
#include <stdio.h>
#include "ast.h"

void emit_vial(FILE *out, const ast_t *node);
