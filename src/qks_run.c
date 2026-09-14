// qks-run -- runtime host para bytecode qmkscript.
// Espejo de lo que ejecutará el firmware AN360. Paso 2.1: solo valida.
//
// Uso:
//   qks-run <fichero.bin>   -- lee, valida header, reporta OK o error.
//   qks-run -               -- lee stdin (útil: `qmkscript --emit=bc x.qks | qks-run -`)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vm_host.h"
#include "vm_errors.h"   // vm_err_str(): traducción código -> mensaje humano

// Slurp entero de un FILE* a un buffer malloc'd. Devuelve NULL en error.
// El caller es dueño del buffer.
static uint8_t *slurp(FILE *f, size_t *out_len) {
    size_t cap = 4096, len = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        if (len == cap) {
            cap *= 2;
            uint8_t *nb = realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        size_t n = fread(buf + len, 1, cap - len, f);
        len += n;
        if (n == 0) break;   // EOF o error; ferror() lo cazamos abajo
    }
    if (ferror(f)) { free(buf); return NULL; }
    *out_len = len;
    return buf;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <file.bin | ->\n", argv[0]);
        return 2;
    }

    FILE *f;
    int close_it = 0;
    if (!strcmp(argv[1], "-")) {
        f = stdin;
    } else {
        f = fopen(argv[1], "rb");
        if (!f) { perror(argv[1]); return 1; }
        close_it = 1;
    }

    size_t len;
    uint8_t *blob = slurp(f, &len);
    if (close_it) fclose(f);
    if (!blob) { fprintf(stderr, "qks-run: no pude leer el fichero\n"); return 1; }

    vm_program_t prog;
    const char *err = NULL;
    vm_result_t r = vm_load_and_validate(blob, len, &prog, &err);
    if (r != VM_OK) {
        fprintf(stderr, "qks-run: bytecode inválido (%d): %s\n", r, err);
        free(blob);
        return 1;
    }

    fprintf(stderr, "-- %zu bytes de código, ejecutando --\n", prog.len);
    vm_result_t xr = vm_exec(&prog, &vm_host_ops, stdout);
    if (xr != VM_OK) {
        fprintf(stderr, "qks-run: fallo en ejecución (%d): %s\n",
                xr, vm_err_str(xr));
        free(blob);
        return 1;
    }

    free(blob);
    return 0;
}
