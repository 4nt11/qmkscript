// Backend bytecode: AST -> blob binario ejecutable por la VM (ver vm.h).
// Dos modos:
//   emit_bytecode      -> escribe header + code + HALT en `out` (binario, pipe a xxd o > out.bin)
//   emit_bytecode_dump -> disassembly humano-legible (`0004  01 05 00 68 6f 6c 61  STR len=5 "hola"`)
#pragma once
#include <stdio.h>
#include "ast.h"

void emit_bytecode     (FILE *out, const ast_t *root);
void emit_bytecode_dump(FILE *out, const ast_t *root);
