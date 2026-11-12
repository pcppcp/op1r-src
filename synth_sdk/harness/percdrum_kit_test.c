/* perc drum kit on the host: save format (v2 sizes, round trip, v1 kits), the transfer pool (new sounds, preview, place
 * forward, undo), the sound browser, edits and their readouts; "render" prints frames for harness/render_percdrum.py.
 *   cc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -DOP1_HOST -I. -Iharness -Iplugins/percsyn \
 *      harness/percdrum_kit_test.c plugins/percsyn/perc_dsp.c -lm -o harness/build/percdrum_kit_test */
#include "../plugins/percdrum/perc_drum.c"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24]; int16_t host_voice_vel[6];

static struct { int32_t x0, y0, x1, y1, w; uint8_t c[4]; } L[4096]; static int nl;
void host_raster_line(void *r, const void *p0, const void *p1, int32_t w, const uint8_t *c)
{ (void)r; const op1_pt *a = p0, *b = p1; if (nl < 4096) { L[nl].x0 = a->x; L[nl].y0 = a->y; L[nl].x1 = b->x; L[nl].y1 = b->y; L[nl].w = w; memcpy(L[nl].c, c, 4); nl++; } }
void *host_raster(void) { return (void *)1; }
typedef struct { int32_t x, y; uint8_t c[4]; char s[32]; } htxt;
static htxt tstore[32]; static void *tkeys[32]; static int ntk;
static htxt *T(void *o) { for (int i = 0; i < ntk; i++) if (tkeys[i] == o) return &tstore[i]; tkeys[ntk] = o; return &tstore[ntk++]; }
void *host_text_ctor(void *o, int32_t x, int32_t y, int32_t sz, const uint8_t *c)
{ (void)sz; htxt *t = T(o); t->x = x; t->y = y; memcpy(t->c, c, 4); t->s[0] = 0; return o; }
void host_text_set(void *o, const char *s) { snprintf(T(o)->s, 32, "%s", s); }
static htxt *drawn[32]; static int nt;
void host_text_draw(void *o, void *r, const int32_t *m) { (void)r; (void)m; drawn[nt++] = T(o); }

static void frame(const char *title, int slot, int pad, int page, int shift, int reading)
{
    nl = nt = 0;
    ui_draw((void *)1, slot, pad, page, shift, reading);
    printf("{\"title\":\"%s\",\"lines\":[", title);
    for (int i = 0; i < nl; i++) printf("%s[%d,%d,%d,%d,%d,[%d,%d,%d]]", i ? "," : "", L[i].x0, L[i].y0, L[i].x1, L[i].y1, L[i].w, L[i].c[0], L[i].c[1], L[i].c[2]);
    printf("],\"text\":[");
    for (int i = 0; i < nt; i++) printf("%s{\"x\":%d,\"y\":%d,\"c\":[%d,%d,%d],\"s\":\"%s\"}", i ? "," : "", drawn[i]->x, drawn[i]->y, drawn[i]->c[0], drawn[i]->c[1], drawn[i]->c[2], drawn[i]->s);
    printf("]}\n");
}

/* a v1 blob as the per-key build wrote it */
static unsigned v1_pack(uint8_t *o, const int sound[24], int16_t kn[24][7], uint16_t pa[24][21], uint16_t pb[24][21], const int sided[24])
{
    uint8_t *p = o; *p++ = 1;
    for (int i = 0; i < 24; i++) {
        *p++ = (uint8_t)sound[i]; *p++ = (uint8_t)sided[i];
        for (int j = 0; j < 7; j++) p = put_u16(p, kn[i][j]);
        if (sound[i] >= PERC_FACTORY_COUNT) { for (int j = 0; j < 21; j++) p = put_u16(p, pa[i][j]); if (sided[i]) for (int j = 0; j < 21; j++) p = put_u16(p, pb[i][j]); }
    }
    return (unsigned)(p - o);
}
/* the old voice: patch + knob offsets (perc_drum.c before the pages edited the patch) */
static void old_macros(const int16_t *kn, perc_macro_t *m)
{
    m->pitch = (kn[0] - 16384) * 8; m->length = (kn[1] - 16384) * 13 + ((kn[1] - 16384) >> 2);
    m->punch = (kn[2] - 16384) * 4; m->tone = (kn[3] - 16384) * 12; m->dist = kn[4] * 2; m->level_db = (kn[5] - 16384) * 48;
}
static double render_diff(const uint16_t *pa, const perc_macro_t *ma, const uint16_t *pb, const perc_macro_t *mb, double *peak)
{
    static perc_voice_t va, vb; static int16_t a[44100], b[44100];
    perc_prepare(&va.p, pa, ma, 100); perc_trigger(&va); perc_prepare(&vb.p, pb, mb, 100); perc_trigger(&vb);
    for (int i = 0; i < 44100; i += 64) { perc_render(&va, a + i, 64, 0); perc_render(&vb, b + i, 64, 0); if (i + 128 > 44100) break; }
    double e = 0, s = 0, pk = 0;
    for (int i = 0; i < 44032; i++) { e += (double)(a[i] - b[i]) * (a[i] - b[i]); s += (double)a[i] * a[i]; if (fabs((double)a[i]) > pk) pk = fabs((double)a[i]); }
    if (peak) *peak = pk;
    return s > 0 ? sqrt(e / s) : 0;
}
static void texts(int slot, int pad, int page, int shift, char lab[4][12], char val[4][12], char prs[4][8], char *name, char *info)
{ ui_texts(slot, pad, page, shift, 0, name, info, lab, val, prs); }

int main(int argc, char **argv)
{
    for (int i = 0; i < 8192; i++) host_sine[i] = (int16_t)lrint(32767.0 * sin(2 * M_PI * i / 8192.0));
    srand(11);
    char name[28], info[28], lab[4][12], val[4][12], prs[4][8];

    /* ---- save: sizes and round trip ---- */
    kit_load(0, 0);
    unsigned n = pack_kit(0); b64_enc(pd_raw, n);
    printf("fresh kit: %u bytes -> %zu base64 chars\n", n, strlen(pd_b64));
    assert(n == 5 + 24 * 4);
    for (int p = 0; p < 24; p++) { pd_key *k = &kit[0][p]; k->origin = PD_TONIC; k->morph = (int16_t)(rand() & 32767); k->flags = PK_EDITED; for (int j = 0; j < 21; j++) { k->a[j] = (uint16_t)rand(); k->b[j] = (uint16_t)rand(); } }
    kit_sig[0] = 0xDEADBEEF;
    n = pack_kit(0); b64_enc(pd_raw, n);
    printf("worst case (24 transferred, both sides): %u bytes -> %zu base64 chars (v1 was 3204)\n", n, strlen(pd_b64));
    assert(n == PD_RAW_MAX && strlen(pd_b64) < 3000);
    for (int p = 0; p < 24; p++) {                           /* a mixed kit */
        pd_key *k = &kit[0][p];
        if (p % 4 == 0) key_preset(k, p);
        else if (p % 4 == 1) { key_preset(k, (p * 5) % 24); k->a[P_DIST] = 30000; k->flags = PK_EDITED; k->morph = 5000; }
        else if (p % 4 == 2) { key_preset(k, p); k->b[P_OSCFREQ] = 1234; k->flags = PK_EDITED; }
        else if (p % 8 == 3) for (int j = 0; j < 21; j++) k->b[j] = k->a[j];   /* a one-sided transfer */
    }
    static pd_key saved[24]; memcpy(saved, kit[0], sizeof saved);
    n = pack_kit(0); b64_enc(pd_raw, n);
    static char str[4096]; strcpy(str, pd_b64);
    kit_load(0, str);
    int bad = 0;
    for (int p = 0; p < 24; p++) { pd_key *a = &saved[p], *b = &kit[0][p]; bad += a->origin != b->origin || a->morph != b->morph || a->flags != b->flags || memcmp(a->a, b->a, 42) || memcmp(a->b, b->b, 42); }
    printf("mixed kit: %u bytes, round trip %s\n", n, bad ? "FAIL" : "ok"); assert(!bad && kit_sig[0] == 0xDEADBEEF);
    for (unsigned cut = 0; cut < n; cut += 7) { kit_load(1, 0); unpack_kit(1, pd_raw, cut); }    /* truncated: no overrun (ASan) */

    /* ---- v1 kits: the knob offsets go into the patch; the sound stays ---- */
    {
        static int sound[24], sided[24]; static int16_t kn[24][7]; static uint16_t pa[24][21], pb[24][21];
        for (int i = 0; i < 24; i++) {
            sound[i] = i < 20 ? i % 10 : 10 + i; sided[i] = i == 22;
            for (int j = 0; j < 21; j++) { pa[i][j] = PERC_FACTORY[i % 10][j]; pb[i][j] = PERC_FACTORY_R[i % 10][j]; }
            kn[i][0] = kn[i][1] = kn[i][2] = kn[i][3] = 16384; kn[i][4] = 0; kn[i][5] = 16384; kn[i][6] = 0;
        }
        kn[1][0] = 16384 + 5 * 683; kn[2][1] = 16384 - 6 * 1024; kn[3][3] = 16384 + 4096; kn[4][4] = 8192; kn[5][5] = 16384 - 4096;
        kn[6][2] = 16384 + 8192; kn[7][0] = 16384 - 7 * 683; kn[7][1] = 16384 + 4096; kn[7][4] = 4096; kn[21][0] = 16384 + 3 * 683; kn[22][6] = 9000;
        static uint8_t blob[4096]; unsigned bn = v1_pack(blob, sound, kn, pa, pb, sided);
        kit_load(2, 0); unpack_kit(2, blob, bn);
        static const char *what[24] = { [0] = "untouched", [1] = "pitch +5 st", [2] = "length down", [3] = "tone up", [4] = "drive 25%", [5] = "level -3 dB", [6] = "punch +50% (approximate)", [7] = "pitch, length, drive", [21] = "transferred, pitch +3" };
        static const perc_macro_t none;
        for (int i = 0; i < 24; i++) if (what[i]) {
            perc_macro_t m; old_macros(kn[i], &m);
            double pk, d = render_diff(pa[i], &m, kit[2][i].a, &none, &pk);
            printf("  v1 key %2d %-26s old vs converted: %5.1f %% rms difference (peak %5.0f)\n", i + 1, what[i], d * 100, pk);
            if (i != 6) assert(d < (i == 5 ? 0.06 : 0.02));
        }
        assert(kit[2][0].origin == 0 && !kit[2][0].flags && kit[2][1].origin == 2 && (kit[2][1].flags & PK_EDITED));
        assert(kit[2][21].origin == PD_TONIC && kit[2][22].morph == 9000);
    }

    /* ---- readouts against the formulas ---- */
    kit_load(0, 0);
    {
        pd_key *k = &kit[0][0];
        for (int t = 0; t < 2000; t++) {
            for (int j = 0; j < 21; j++) k->a[j] = (uint16_t)rand();
            double x;
            x = k->a[P_OSCFREQ] / 65535.0; double hz = 20 * pow(1000, x), g = w_hz10(k->a[P_OSCFREQ]) / 10.0; assert(fabs(g - hz) < hz * 0.004 + 0.11);
            x = k->a[P_OSCDCY] / 65535.0; double ms = 10 * pow(1000, x); g = w_dcy10(k->a[P_OSCDCY]) / 10.0; assert(fabs(g - ms) < ms * 0.004 + 0.11);
            x = k->a[P_OSCATK] / 65535.0; ms = 1000 * x * pow(10, (12 * x - 7) / 5); g = w_atk10(k->a[P_OSCATK]) / 10.0; assert(fabs(g - ms) < ms * 0.004 + 0.11);
            x = k->a[P_LEVEL] / 65535.0; if (x > 0.02) { double db = 60 * x - 49 - 1 / x; g = w_level_db10(k->a[P_LEVEL]) / 10.0; assert(fabs(g - db) < 0.11); }
            x = k->a[P_MODAMT] / 65535.0; { double b = 2 * x - 1, st = b * fabs(b) * (w_mode(k->a[P_MODMODE]) ? 48 : 96); assert(fabs(w_semis10(k->a) / 10.0 - st) < 0.11); }
            x = k->a[P_MODRATE] / 65535.0; if (x > 0.01) { int m = w_mode(k->a[P_MODMODE]); double want = m == 0 ? (1.03 - x) / (3 * x) * 1000 : m == 1 ? -2000 * x * pow(10, 3.5 * (x - 1)) : -20000 * x * pow(10, 4 * (x - 1));
                g = w_rate10(k->a) / 10.0; assert(fabs(g - want) < fabs(want) * 0.005 + 0.11); }
        }
        key_preset(k, 0);                                    /* the kick, as its preset was written: 52 Hz, 420 ms, +30 st, 18 % drive */
        texts(0, 0, PG_TONE, 0, lab, val, prs, name, info);
        printf("kick tone page: %s | %s %s | %s %s | %s %s | %s %s | %s, %s\n", name, lab[0], val[0], lab[1], val[1], lab[2], val[2], lab[3], val[3], prs[0], prs[3]);
        assert(!strcmp(name, "f1 kick") && (!strcmp(val[0], "52.0hz") || !strcmp(val[0], "51.9hz")) && !strcmp(val[1], "0ms") && !strcmp(val[2], "420ms") && !strcmp(prs[0], "sine") && !strcmp(prs[3], "decay"));
        texts(0, 0, PG_MIX, 0, lab, val, prs, name, info);
        printf("kick mix page:  %s %s | %s %s | %s %s | %s %s\n", lab[0], val[0], lab[1], val[1], lab[2], val[2], lab[3], val[3]);
        assert(!strcmp(val[1], "18%") && !strcmp(val[0], "82/18") && !strcmp(val[3], "0.0db"));
        /* the drive is the sound's: down means less */
        assert(ui_turn(0, 0, PG_MIX, 0, 1, -5)); texts(0, 0, PG_MIX, 0, lab, val, prs, name, info); assert(!strcmp(val[1], "10%"));
        texts(0, 0, PG_TONE, 0, lab, val, prs, name, info); assert(!strcmp(name, "f1 kick*"));
        assert(ui_turn(0, 0, PG_TONE, 0, 0, 12)); assert(fabs(w_hz10(k->a[P_OSCFREQ]) / 10.0 - 104) < 0.6);   /* 12 clicks: an octave */
        assert(ui_press(0, 0, PG_TONE, 0, 0)); texts(0, 0, PG_TONE, 0, lab, val, prs, name, info); assert(!strcmp(prs[0], "tri"));
        /* morph past the middle: the knobs show and edit side b; side a stays */
        pd_key before = *k; for (int i = 0; i < 20; i++) ui_turn(0, 0, PG_MIX, 1, 0, 1);
        assert(key_side(k)); ui_turn(0, 0, PG_MIX, 0, 1, 3); assert(!memcmp(before.a, k->a, 42) && k->b[P_DIST] == before.b[P_DIST] + 3072);
        /* home: the preset again; undo: the edits again */
        pd_key edited = *k; assert(ui_press(0, 0, PG_TONE, 1, 0)); assert(!kit[0][0].flags && !memcmp(k->a, PERC_KIT[0], 42));
        assert(ui_press(0, 0, PG_TONE, 1, 3)); assert(!memcmp(k, &edited, sizeof edited));
    }

    /* ---- transfer: new sounds wait from the selected key, play there, are placed on a press ---- */
    {
        static perc_xfer_t x; memset(&x, 0, sizeof x);
        kit_load(0, 0);
        for (int e = 0; e < 4; e++) { x.list_n++; for (int j = 0; j < 21; j++) { x.list[e][0][j] = (uint16_t)(1000 * (e + 1) + j); x.list[e][1][j] = (uint16_t)(e == 1 ? 9 : 1000 * (e + 1) + j); } }
        assert(pool_take(0, &x, 111, 8) == 1);              /* keys 1-8 are in use: the selected key is c#2 (pad 8) */
        assert(pend.slot == 0 && pend.n == 4 && pend.anchor == 8 && pool_n[0] == 4);
        texts(0, 8, PG_TONE, 0, lab, val, prs, name, info); printf("waiting: \"%s\" %s %s [%s] [%s]\n", name, lab[0], val[0], prs[0], prs[3]);
        assert(!strcmp(name, "4 new > c#2-e2") && !strcmp(prs[0], "place"));
        uint16_t tmp[21];
        assert(hit_patch(0, 9, tmp)[0] == 2000 && hit_patch(0, 7, tmp) == kit[0][7].a && kit[0][9].origin == 9);   /* previewed, not written */
        ui_turn(0, 8, PG_TONE, 0, 0, 14); assert(pend.anchor == 22 && pend_fit() == 2);                          /* near the top: 2 of 4 fit */
        texts(0, 8, PG_TONE, 0, lab, val, prs, name, info); assert(!strcmp(info, "2 of 4 fit")); printf("near the top: \"%s\" (%s)\n", name, info);
        ui_turn(0, 8, PG_TONE, 0, 0, -14);
        assert(ui_press(0, 8, PG_TONE, 0, 0));               /* blue press: placed on c#2 .. e2 */
        for (int e = 0; e < 4; e++) assert(kit[0][8 + e].origin == PD_TONIC && kit[0][8 + e].a[0] == 1000 * (e + 1));
        assert(kit[0][9].b[0] == 9 && kit[0][8].b[0] == 1000 && kit[0][7].origin == 7 && kit[0][12].origin == 12 && pend.slot < 0);
        texts(0, 8, PG_TONE, 0, lab, val, prs, name, info); assert(!strcmp(info, "placed c#2-e2") && !strcmp(name, "c#2 tonic"));
        assert(ui_press(0, 8, PG_TONE, 1, 3)); for (int e = 0; e < 4; e++) assert(kit[0][8 + e].origin == 8 + e);   /* undo */
        assert(ui_press(0, 8, PG_TONE, 1, 3)); assert(kit[0][8].origin == PD_TONIC);                               /* and back */
        /* the same recording again (a boot): nothing waits; one more sound recorded: only that one waits */
        n = pack_kit(0); b64_enc(pd_raw, n); strcpy(str, pd_b64); kit_load(0, str);
        assert(pool_take(0, &x, 111, 0) == 0 && pend.slot < 0);
        x.list_n = 5; for (int j = 0; j < 21; j++) x.list[4][0][j] = x.list[4][1][j] = (uint16_t)(7000 + j);
        assert(pool_take(0, &x, 222, 14) == 1 && pend.n == 1 && pend.idx[0] == 4 && pend.anchor == 14);
        texts(0, 14, PG_TONE, 0, lab, val, prs, name, info); assert(!strcmp(name, "1 new > g2"));
        ui_press(0, 14, PG_TONE, 0, 3); assert(pend.slot < 0 && kit[0][14].origin == 14);                        /* red press: left out */
        /* a v1 kit has no signature: its first decode is quiet too; a recording with no transfer empties the pool */
        { uint8_t v1[1 + 24 * 16]; memset(v1, 0, sizeof v1); v1[0] = 1; for (int i = 0; i < 24; i++) { uint8_t *q = v1 + 1 + 16 * i; for (int j = 0; j < 4; j++) put_u16(q + 2 + 2 * j, 16384); put_u16(q + 12, 16384); }
          kit_load(3, 0); unpack_kit(3, v1, sizeof v1); assert(kit_old[3] && kit[3][5].origin == 0 && !kit[3][5].flags);
          assert(pool_take(3, &x, 333, 0) == 1 && pend.slot < 0 && pool_n[3] == 5);
          static perc_xfer_t none; assert(pool_take(3, &none, 444, 0) == 1 && pool_n[3] == 0); }
        /* the browser: mine, the presets, the kit's transferred sounds; back to mine restores the edit */
        key_preset(&kit[0][3], 3); ui_turn(0, 3, PG_MIX, 0, 1, 4); pd_key mine = kit[0][3];
        ui_turn(0, 3, PG_TONE, 1, 0, 1); assert(kit[0][3].origin == 0 && !kit[0][3].flags);
        ui_turn(0, 3, PG_TONE, 1, 0, 24); assert(kit[0][3].origin == PD_TONIC && kit[0][3].a[0] == 1000);
        texts(0, 3, PG_TONE, 1, lab, val, prs, name, info); assert(!strcmp(val[0], "tonic 1") && !strcmp(lab[0], "sound"));
        ui_turn(0, 3, PG_TONE, 1, 0, -25); assert(!memcmp(&kit[0][3], &mine, sizeof mine));
    }
    if (argc > 1 && !strcmp(argv[1], "render")) {
        static perc_xfer_t x; memset(&x, 0, sizeof x);
        kit_load(0, 0); pool_n[0] = 0; note_kind = 0;
        for (int p = 8; p < 12; p++) { kit[0][p].origin = PD_TONIC; }
        frame("t1 tone (kick)", 0, 0, PG_TONE, 0, 0); frame("t1 + shift", 0, 0, PG_TONE, 1, 0);
        frame("t2 noise (snare)", 0, 2, PG_NOISE, 0, 0); frame("t2 + shift", 0, 2, PG_NOISE, 1, 0);
        kit[0][10].a[P_EQGAIN] = 45000; kit[0][10].a[P_DIST] = 26000; kit[0][10].morph = 9000;
        frame("t4 mix (cymbal)", 0, 10, PG_MIX, 0, 0); frame("t4 + shift", 0, 10, PG_MIX, 1, 0);
        frame("t1 hi-hat (sine bend)", 0, 5, PG_TONE, 0, 0); frame("t2 clap (mod envelope)", 0, 4, PG_NOISE, 0, 0);
        for (int e = 0; e < 4; e++) { x.list_n++; for (int j = 0; j < 21; j++) x.list[e][0][j] = x.list[e][1][j] = PERC_FACTORY[e + 4][j]; }
        pool_take(0, &x, 5, 13); ui_frame = 16;
        frame("transfer waiting (4 new from f#2)", 0, 13, PG_TONE, 0, 0);
        ui_press(0, 13, PG_TONE, 0, 0); frame("placed", 0, 13, PG_TONE, 0, 0);
        return 0;
    }
    printf("OK: perc kit (save, v1 kits, readouts, edits, transfer place / undo, browser)\n");
    return 0;
}
