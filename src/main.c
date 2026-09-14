// main.c -- pipeline completo: .qks -> lex+parse -> AST -> emit_c/vial/dump/bytecode.
// Modo bonus --push: compila a bytecode y lo pipea a `qks-push` en un solo comando.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <getopt.h>
#include "ast.h"
#include "emit_c.h"
#include "emit_vial.h"
#include "emit_bytecode.h"

// Los generan flex / bison; los declaramos aqui para no depender de sus .h.
extern FILE *yyin;
extern int   yyparse(ast_t **root);

static void usage(FILE *out, const char *prog) {
    fprintf(out,
        "usage: %s [--emit=MODE | --push] [input.qks]\n"
        "  --emit=dump      pretty-print the AST (default)\n"
        "  --emit=c         emit a QMK SEND_STRING block\n"
        "  --emit=vial      emit a Vial macro as JSON\n"
        "  --emit=bytecode  emit VM bytecode (binary, pipe to xxd or > out.bin)\n"
        "  --emit=bcdump    emit VM bytecode disassembly (human-readable)\n"
        "  --push           compile bytecode and push to the AN360 via qks-push\n"
        "  -h, --help       this help\n"
        "  input omitted -> read stdin\n",
        prog);
}

// Encuentra qks-push: primero en el mismo directorio de este binario
// (típicamente build/), fallback al PATH del shell.
static void resolve_qks_push(char *out, size_t out_sz) {
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n > 0) {
        self[n] = '\0';
        char *slash = strrchr(self, '/');
        if (slash) {
            *(slash + 1) = '\0';
            int w = snprintf(out, out_sz, "%sqks-push", self);
            if (w > 0 && (size_t)w < out_sz && access(out, X_OK) == 0) return;
        }
    }
    // fallback: que el shell resuelva
    snprintf(out, out_sz, "qks-push");
}

// Modo --push: pipe bytecode a `qks-push -`. popen se encarga del fork+exec+pipe.
static int emit_and_push(const ast_t *root) {
    char qks_push[PATH_MAX];
    resolve_qks_push(qks_push, sizeof(qks_push));

    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "%s -", qks_push);

    FILE *p = popen(cmd, "w");
    if (!p) { perror("qmkscript --push: popen"); return 1; }

    emit_bytecode(p, root);       // escribe binario al pipe

    int status = pclose(p);
    if (status == -1) { perror("qmkscript --push: pclose"); return 1; }
    if (status != 0) {
        fprintf(stderr, "qmkscript --push: qks-push falló (status=%d)\n", status);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *mode = "dump";
    int push_mode    = 0;
    static const struct option opts[] = {
        {"emit", required_argument, 0, 'e'},
        {"push", no_argument,       0, 'p'},
        {"help", no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "e:ph", opts, NULL)) != -1) {
        switch (c) {
            case 'e': mode = optarg; break;
            case 'p': push_mode = 1; break;
            case 'h': usage(stdout, argv[0]); return 0;
            default:  usage(stderr, argv[0]); return 2;
        }
    }

    // Argumento posicional: fichero de entrada. Si no hay, se queda con stdin.
    if (optind < argc) {
        yyin = fopen(argv[optind], "r");
        if (!yyin) { perror(argv[optind]); return 1; }
    }

    ast_t *root = NULL;
    if (yyparse(&root) != 0) return 1;   // yyerror ya imprimio el detalle

    if (push_mode) return emit_and_push(root);

    if      (!strcmp(mode, "dump")) ast_dump(root, 0);
    else if (!strcmp(mode, "c"))    emit_c(stdout, root);
    else if (!strcmp(mode, "vial")) emit_vial(stdout, root);
    else if (!strcmp(mode, "bytecode")) emit_bytecode     (stdout, root);
    else if (!strcmp(mode, "bcdump"))   emit_bytecode_dump(stdout, root);
    else {
        fprintf(stderr, "qmkscript: unknown --emit=%s\n", mode);
        usage(stderr, argv[0]);
        return 2;
    }
    return 0;
}
