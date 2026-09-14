// qks-push -- envía un .bin de qmkscript al AN360 por raw-HID.
// Paso 3.2 del plan: solo transporte. El firmware imprime cada frame por
// `qmk console`; aún no hay storage ni intérprete on-device.
//
// Wire format v2 (32 bytes por frame, ver keyboards/an360/qks_hid.c):
//   [0]     magic 0xE0
//   [1]     cmd (0x01 = WRITE, 0x02 = FLUSH, 0x03 = DUMP, 0x04 = RESET)
//   [2]     seq
//   [3]     payload_len (0..27)
//   [4]     slot_idx (0..QKS_STORAGE_SLOTS-1)
//   [5..31] payload
//
// Respuesta (32 bytes, VIA/Vial hace raw_hid_send automático):
//   [0..2]  echo (magic, cmd, seq)
//   [3]     status (0 = OK)
//
// Discovery: busca por VID/PID + usage_page 0xFF60 usage 0x61
// (convención QMK para raw-HID, discrimina de keyboard/mouse/console).
#include <hidapi/hidapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <getopt.h>

#define AN360_VID           0x1322
#define AN360_PID           0x5772
#define QMK_RAW_USAGE_PAGE  0xFF60
#define QMK_RAW_USAGE       0x0061

#define FRAME_SIZE      32
#define PAYLOAD_MAX     27   // FRAME_SIZE - 5 bytes de header (v2: incluye slot)
#define SLOT_MAX        8    // debe matchear QKS_STORAGE_SLOTS en el firmware
#define QKS_HID_MAGIC   0xE0
#define QKS_CMD_WRITE   0x01
#define QKS_CMD_FLUSH   0x02
#define QKS_CMD_DUMP    0x03   // firmware imprime el slot por qmk console
#define QKS_CMD_RESET   0x04   // firmware descarta el buffer sin commit

// Slurp entero de un FILE* a buffer malloc'd.
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
        if (n == 0) break;
    }
    if (ferror(f)) { free(buf); return NULL; }
    *out_len = len;
    return buf;
}

// Busca el raw-HID interface del kb entre todas las HID que expone.
// El match por (usage_page, usage) es lo que distingue el raw de keyboard/mouse.
static hid_device *open_an360_raw(void) {
    struct hid_device_info *devs = hid_enumerate(AN360_VID, AN360_PID);
    if (!devs) {
        fprintf(stderr, "qks-push: no encuentro AN360 (%04x:%04x). ¿Conectado?\n",
                AN360_VID, AN360_PID);
        return NULL;
    }
    hid_device *h = NULL;
    for (struct hid_device_info *d = devs; d; d = d->next) {
        if (d->usage_page == QMK_RAW_USAGE_PAGE && d->usage == QMK_RAW_USAGE) {
            h = hid_open_path(d->path);
            if (h) {
                fprintf(stderr, "qks-push: abierto %s (usage_page=0x%04x)\n",
                        d->path, d->usage_page);
                break;
            }
            fprintf(stderr, "qks-push: hid_open_path(%s) falló: %ls\n",
                    d->path, hid_error(NULL));
        }
    }
    hid_free_enumeration(devs);
    if (!h) fprintf(stderr,
        "qks-push: el AN360 enumera pero no hay raw-HID (usage_page=0x%04x).\n"
        "          ¿Compilaste con RAW_ENABLE=yes?\n", QMK_RAW_USAGE_PAGE);
    return h;
}

// Envía UN frame y lee el ACK. Devuelve 0 en éxito, ≠0 en error.
// buf[0] = report ID (0x00 para raw-HID sin numered reports, típico de QMK).
static int send_frame(hid_device *h, uint8_t cmd, uint8_t seq, uint8_t slot,
                      const uint8_t *payload, uint8_t payload_len) {
    uint8_t out[1 + FRAME_SIZE] = {0};
    out[0] = 0x00;                    // report ID
    out[1] = QKS_HID_MAGIC;
    out[2] = cmd;
    out[3] = seq;
    out[4] = payload_len;
    out[5] = slot;
    if (payload_len) memcpy(out + 6, payload, payload_len);

    int wn = hid_write(h, out, sizeof out);
    if (wn < 0) {
        fprintf(stderr, "qks-push: hid_write seq=%u falló: %ls\n",
                seq, hid_error(h));
        return 1;
    }

    uint8_t in[FRAME_SIZE] = {0};
    int rn = hid_read_timeout(h, in, sizeof in, 500);   // 500 ms
    if (rn <= 0) {
        fprintf(stderr, "qks-push: sin ACK para seq=%u (timeout)\n", seq);
        return 2;
    }
    if (in[0] != QKS_HID_MAGIC || in[1] != cmd || in[2] != seq) {
        fprintf(stderr, "qks-push: ACK inesperado seq=%u (got %02x %02x %02x)\n",
                seq, in[0], in[1], in[2]);
        return 3;
    }
    if (in[3] != 0) {
        fprintf(stderr, "qks-push: firmware devolvió status=%u para seq=%u\n",
                in[3], seq);
        return 4;
    }
    return 0;
}

static void usage(FILE *out, const char *prog) {
    fprintf(out,
        "usage: %s [--slot N] <file.bin | - | --dump | --reset>\n"
        "  <file.bin>   Envía el .bin al AN360 (%04x:%04x): WRITEs + FLUSH.\n"
        "  -            Lee stdin (`qmkscript --emit=bytecode x.qks | %s -`).\n"
        "  --slot N     Escribe/dumpea slot N (default 0). Rango 0..%d.\n"
        "  --dump       Pide al firmware que imprima el slot por qmk console.\n"
        "  --reset      Descarta buffer del firmware sin commit.\n",
        prog, AN360_VID, AN360_PID, prog, SLOT_MAX - 1);
}

// Envía UN frame de comando (cero payload). Usado por --dump y --reset.
static int send_command_only(hid_device *h, uint8_t cmd, uint8_t slot) {
    return send_frame(h, cmd, 0, slot, NULL, 0);
}

int main(int argc, char **argv) {
    uint8_t slot = 0;
    // Parse --slot N si viene primero. Ponytail: dos flags, cero getopt.
    if (argc >= 3 && !strcmp(argv[1], "--slot")) {
        int v = atoi(argv[2]);
        if (v < 0 || v >= SLOT_MAX) {
            fprintf(stderr, "qks-push: slot %d fuera de rango [0..%d]\n",
                    v, SLOT_MAX - 1);
            return 2;
        }
        slot = (uint8_t)v;
        argv += 2; argc -= 2;
    }
    if (argc != 2) { usage(stderr, argv[0]); return 2; }
    if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        usage(stdout, argv[0]); return 0;
    }

    if (!strcmp(argv[1], "--dump") || !strcmp(argv[1], "--reset")) {
        uint8_t cmd = !strcmp(argv[1], "--dump") ? QKS_CMD_DUMP : QKS_CMD_RESET;
        if (hid_init() != 0) { fprintf(stderr, "hid_init falló\n"); return 1; }
        hid_device *h = open_an360_raw();
        if (!h) { hid_exit(); return 1; }
        int r = send_command_only(h, cmd, slot);
        if (r == 0) fprintf(stderr, "qks-push: %s slot=%u OK (mira `qmk console`)\n",
                            argv[1], slot);
        hid_close(h); hid_exit();
        return r == 0 ? 0 : 1;
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
    if (!blob) { fprintf(stderr, "qks-push: no pude leer\n"); return 1; }

    if (hid_init() != 0) {
        fprintf(stderr, "qks-push: hid_init falló\n"); free(blob); return 1;
    }
    hid_device *h = open_an360_raw();
    if (!h) { hid_exit(); free(blob); return 1; }

    size_t   off        = 0;
    uint8_t  seq        = 0;
    uint32_t frame_no   = 0;
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > PAYLOAD_MAX) chunk = PAYLOAD_MAX;
        if (send_frame(h, QKS_CMD_WRITE, seq, slot, blob + off, (uint8_t)chunk) != 0)
            goto fail;
        off += chunk;
        seq++;
        frame_no++;
    }
    if (send_frame(h, QKS_CMD_FLUSH, seq, slot, NULL, 0) != 0) goto fail;

    fprintf(stderr, "qks-push: OK -- slot=%u, %zu bytes en %u frames + FLUSH\n",
            slot, len, frame_no);
    hid_close(h); hid_exit(); free(blob);
    return 0;

fail:
    hid_close(h); hid_exit(); free(blob);
    return 1;
}
