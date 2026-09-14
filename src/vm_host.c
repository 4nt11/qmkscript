// vm_host.c -- policy del host: tabla vm_ops_t que IMPRIME cada evento
// en vez de tecleárselo al SO. Espejo de lo que hará el firmware AN360,
// solo que allá los callbacks son send_string/tap_code/... de QMK.
//
// ctx == FILE* out. Cada callback lo cast'ea y escribe una línea.
#include <stdio.h>
#include <ctype.h>
#include "vm_host.h"

static void h_send_string(const uint8_t *bytes, uint16_t len, void *ctx) {
    FILE *out = (FILE *)ctx;
    fprintf(out, "TYPE   \"");
    for (uint16_t i = 0; i < len; i++)
        fputc(isprint(bytes[i]) ? bytes[i] : '.', out);
    fprintf(out, "\"  (%u bytes)\n", len);
}
static void h_tap_code(uint8_t kc, void *ctx) {
    fprintf((FILE *)ctx, "TAP    kc=0x%02x\n", kc);
}
static void h_wait_ms(uint16_t ms, void *ctx) {
    fprintf((FILE *)ctx, "DELAY  %u ms\n", ms);
}
static void h_register_mods(uint8_t mods, void *ctx) {
    fprintf((FILE *)ctx, "MOD+   mods=0x%02x\n", mods);
}
static void h_unregister_mods(uint8_t mods, void *ctx) {
    fprintf((FILE *)ctx, "MOD-   mods=0x%02x\n", mods);
}

const vm_ops_t vm_host_ops = {
    .send_string     = h_send_string,
    .tap_code        = h_tap_code,
    .wait_ms         = h_wait_ms,
    .register_mods   = h_register_mods,
    .unregister_mods = h_unregister_mods,
};
