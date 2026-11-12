/* perc_kit: make PO-32 transfers that fit one perc recording, on the computer.
 *
 *   perc_kit kit.mtpreset out.wav            Microtonic preset: its 8 drums -> one transfer (about 3.5 s; a
 *                                            preset has one side per drum)
 *   perc_kit transfer.wav out_prefix         a PO-32 transfer (44.1 kHz mono WAV, e.g. from afconvert):
 *                                              out_prefix_kit.wav    16 instruments, both morph sides, for a perc
 *                                                                    drum kit (12 s sample)
 *                                              out_prefix_1-8.wav, out_prefix_9-16.wav   left sides, for the perc
 *                                                                    synth engine (6 s sample)
 *
 * Each output has 0.3 s of silence first and 0.2 s after.
 * Play it into the OP-1 (input on a perc slot) or record it into the slot any other way. The names of the drums go
 * to stdout. Uses libpo32 (MIT, Eric Lewis) for the Microtonic text import, the encoder and the modulator:
 *   cc -O2 -I.. -I$LIBPO32/core/include -I$LIBPO32/core/src perc_kit.c $LIBPO32/core/src/po32.c \
 *      $LIBPO32/core/src/po32_patch_import.c -lm -o perc_kit
 * GPL-3.0-or-later with the OP1R firmware permission. */
#include "po32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FS 44100
static uint16_t patch[16][2][21]; static int have[16]; static char name[16][64];   /* have: bit 0 left, bit 1 right */

static int write_transfer(const char *path, int first, int last, int sides, double max_s)
{
    static uint8_t frame[16384]; po32_builder_t b; po32_builder_init(&b, frame, sizeof frame);
    int n = 0;
    for (int i = first; i <= last; i++) if (have[i] & 1)
        for (int sd = 0; sd < 2; sd++) if ((sides >> sd & 1) && (have[i] >> sd & 1)) {
            uint8_t pl[43]; pl[0] = (uint8_t)((sd ? 0x20 : 0x10) | i);
            for (int k = 0; k < 21; k++) { pl[1 + 2 * k] = (uint8_t)patch[i][sd][k]; pl[2 + 2 * k] = (uint8_t)(patch[i][sd][k] >> 8); }
            if (po32_builder_append_packet(&b, PO32_TAG_PATCH, pl, 43, NULL) != PO32_OK) return -1;
            if (!sd) n++;
        }
    if (!n) return 0;
    size_t flen; if (po32_builder_finish(&b, &flen) != PO32_OK) return -1;
    size_t ns = po32_render_sample_count(flen, FS), lead = FS * 3 / 10, tail = FS / 5;
    float *buf = calloc(ns, sizeof(float)); po32_render_dpsk_f32(frame, flen, FS, buf, ns);
    FILE *f = fopen(path, "wb"); if (!f) { free(buf); return -1; }
    int bytes = (int)(lead + ns + tail) * 2;
    int32_t hdr[11] = { 0x46464952, 36 + bytes, 0x45564157, 0x20746d66, 16, 0x00010001, FS, FS * 2, 0x00100002, 0x61746164, bytes };
    fwrite(hdr, 4, 11, f);
    for (size_t i = 0; i < lead + ns + tail; i++) {
        int16_t s = (int16_t)(i >= lead && i < lead + ns ? buf[i - lead] * 26000 : 0); fwrite(&s, 2, 1, f);
    }
    fclose(f); free(buf);
    double total = (lead + ns + tail) / (double)FS;
    fprintf(stderr, "%s: %d sounds, %.2f s of transfer, %.2f s in all%s\n", path, n, ns / (double)FS, total,
            total > max_s ? " -- LONGER than the sample it is for" : "");
    return n;
}

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); *len = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc(*len + 1); fread(b, 1, *len, f); b[*len] = 0; fclose(f); return b;
}

/* a .mtpreset: DrumPatches { 1: { Key = value ... } ... } -> "Key: value" lines -> libpo32's importer */
static int load_mtpreset(const char *path)
{
    size_t len; char *t = slurp(path, &len); if (!t) return -1;
    char *p = strstr(t, "DrumPatches:"); if (!p) { free(t); return -1; }
    int count = 0;
    for (int d = 1; d <= 16; d++) {
        char key[16]; snprintf(key, sizeof key, "\n\t\t%d: {", d);
        char *s = strstr(p, key); if (!s) break;
        s += strlen(key);
        char *e = strstr(s, "\n\t\t}"); if (!e) break;
        static char txt[8192]; size_t o = 0;
        for (char *l = s; l < e && o < sizeof txt - 128; ) {
            char *nl = memchr(l, '\n', (size_t)(e - l)); if (!nl) nl = e;
            char *eq = memchr(l, '=', (size_t)(nl - l));
            if (eq) {
                char *k0 = l; while (k0 < eq && (*k0 == '\t' || *k0 == ' ')) k0++;
                char *k1 = eq; while (k1 > k0 && k1[-1] == ' ') k1--;
                char *v0 = eq + 1; while (v0 < nl && *v0 == ' ') v0++;
                if ((size_t)(k1 - k0) == 4 && !memcmp(k0, "Name", 4)) {
                    size_t m = (size_t)(nl - v0); if (m > 63) m = 63;
                    memcpy(name[d - 1], v0, m); name[d - 1][m] = 0;
                }
                o += (size_t)snprintf(txt + o, sizeof txt - o, "%.*s: %.*s\n", (int)(k1 - k0), k0, (int)(nl - v0), v0);
            }
            l = nl + 1;
        }
        po32_patch_params_t pp;
        if (po32_patch_parse_mtdrum_text(txt, o, &pp) != PO32_OK) { fprintf(stderr, "drum %d: not imported\n", d); continue; }
        const float *f = (const float *)&pp;
        for (int k = 0; k < 21; k++) { float v = f[k] < 0 ? 0 : f[k] > 1 ? 1 : f[k]; patch[d - 1][0][k] = (uint16_t)(v * 65535 + 0.5f); }
        have[d - 1] = 1; count++;
    }
    free(t); return count;
}

/* a recorded / rendered PO-32 transfer (16-bit mono WAV, 44.1 kHz) */
static int got(const po32_packet_t *pk, void *u)
{
    (void)u;
    int side = pk->payload_len == 43 ? pk->payload[0] >> 4 : 0;
    if (pk->tag_code == PO32_TAG_PATCH && (side == 1 || side == 2)) {
        int i = pk->payload[0] & 15;
        for (int k = 0; k < 21; k++) patch[i][side - 1][k] = (uint16_t)(pk->payload[1 + 2 * k] | pk->payload[2 + 2 * k] << 8);
        have[i] |= side; snprintf(name[i], sizeof name[i], "instrument %d", i + 1);
    }
    return 0;
}
static int load_transfer(const char *path)
{
    size_t len; char *t = slurp(path, &len); if (!t || len < 44) return -1;
    size_t p = 12; float *f = 0; size_t n = 0;
    while (p + 8 < len) {
        unsigned cl = (unsigned char)t[p+4] | (unsigned char)t[p+5] << 8 | (unsigned char)t[p+6] << 16 | (unsigned)(unsigned char)t[p+7] << 24;
        if (!memcmp(t + p, "data", 4)) { n = cl / 2; f = malloc(n * sizeof(float)); for (size_t i = 0; i < n; i++) f[i] = (int16_t)((unsigned char)t[p+8+2*i] | (unsigned char)t[p+9+2*i] << 8) / 32768.0f; break; }
        p += 8 + cl + (cl & 1);
    }
    free(t); if (!f) return -1;
    po32_demodulator_t d; po32_demodulator_init(&d, FS);
    po32_demodulator_push(&d, f, n, got, 0); po32_demodulator_flush(&d, got, 0); free(f);
    int c = 0; for (int i = 0; i < 16; i++) c += have[i] & 1; return c;
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: perc_kit kit.mtpreset out.wav | perc_kit transfer.wav out_prefix\n"); return 2; }
    size_t l = strlen(argv[1]);
    if (l > 9 && !strcmp(argv[1] + l - 9, ".mtpreset")) {
        if (load_mtpreset(argv[1]) <= 0) { fprintf(stderr, "%s: no drums imported\n", argv[1]); return 1; }
        if (write_transfer(argv[2], 0, 7, 1, 6.0) <= 0) return 1;
    } else {
        if (load_transfer(argv[1]) <= 0) { fprintf(stderr, "%s: no patches decoded\n", argv[1]); return 1; }
        char a[1024], b[1024], k[1024];
        snprintf(a, sizeof a, "%s_1-8.wav", argv[2]); snprintf(b, sizeof b, "%s_9-16.wav", argv[2]); snprintf(k, sizeof k, "%s_kit.wav", argv[2]);
        if (write_transfer(k, 0, 15, 3, 12.0) < 0 || write_transfer(a, 0, 7, 1, 6.0) < 0 || write_transfer(b, 8, 15, 1, 6.0) < 0) return 1;
    }
    for (int i = 0; i < 16; i++) if (have[i] & 1) printf("po-32 %d  %s%s\n", i + 1, name[i], have[i] & 2 ? "  (morph)" : "");
    return 0;
}
