/* Host test + preview frames for screens/sampedit.c: word <-> sample, zero crossings, trim by rotation, undo
 * (bit-exact), finalize, marker limits. "render": JSON frames for harness/render_usbout.py-style drawing. */
#include "../op1_synth.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int16_t host_clip[1 << 20]; int32_t host_clip_len, host_clip_kind;
#include "../screens/sampedit.c"

static struct { int32_t x0, y0, x1, y1, w; uint8_t c[4]; } L[4096]; static int nl;
void host_raster_line(void *r, const void *p0, const void *p1, int32_t w, const uint8_t *c)
{ (void)r; const op1_pt *a = p0, *b = p1; if (nl < 4096) { L[nl].x0 = a->x; L[nl].y0 = a->y; L[nl].x1 = b->x; L[nl].y1 = b->y; L[nl].w = w; memcpy(L[nl].c, c, 4); nl++; } }
void *host_raster(void) { return (void *)1; }
typedef struct { int32_t x, y, align; uint8_t c[4]; char s[32]; } htxt;     /* Text mock beside the object */
static htxt tstore[16]; static void *tkeys[16]; static int ntk;
static htxt *T(void *o) { for (int i = 0; i < ntk; i++) if (tkeys[i] == o) return &tstore[i]; tkeys[ntk] = o; return &tstore[ntk++]; }
void *host_text_ctor(void *o, int32_t x, int32_t y, int32_t sz, const uint8_t *c)
{ (void)sz; htxt *t = T(o); t->x = x; t->y = y; memcpy(t->c, c, 4); t->s[0] = 0; t->align = 0; return o; }
void host_text_set(void *o, const char *s) { snprintf(T(o)->s, 32, "%s", s); T(o)->align = *(int32_t *)((uint8_t *)o + 0x60); }
static htxt *drawn[16]; static int nt;
void host_text_draw(void *o, void *r, const int32_t *m) { (void)r; (void)m; drawn[nt++] = T(o); }

static int16_t buf_all[SE_N_DRUM + 8 + 8], *buf = buf_all + 8, ref[SE_N_DRUM];   /* + 8 after: overrun check */
static void make_sample(void)                                     /* 2.4 s: 4 decaying notes, then silence */
{
    memset(buf_all, 0, sizeof buf_all);
    for (int n = 0; n < 4; n++) {
        double f = 110.0 * (n + 2), t0 = n * 0.55;
        for (int i = 0; i < 44100 / 2; i++) {
            int k = (int)(t0 * 44100) + i;
            buf[k] += (int16_t)(20000 * exp(-i / 6000.0) * sin(2 * M_PI * f * i / 44100) + (rand() % 400 - 200) * exp(-i / 2000.0));
        }
    }
}
static void frame(sampedit *e, const char *label)
{
    nl = 0; nt = 0; se_draw(e, 0);
    printf("{\"label\": \"%s\", \"lines\": [", label);
    for (int i = 0; i < nl; i++) printf("%s[%d,%d,%d,%d,%d,[%d,%d,%d,%d]]", i ? "," : "", L[i].x0, L[i].y0, L[i].x1, L[i].y1, L[i].w, L[i].c[0], L[i].c[1], L[i].c[2], L[i].c[3]);
    printf("], \"text\": [");
    for (int i = 0; i < nt; i++)                                   /* centred: the renderer gets x - width/2 */
        printf("%s[%d,%d,[%d,%d,%d],\"%s\",%d]", i ? "," : "", drawn[i]->x, drawn[i]->y, drawn[i]->c[0], drawn[i]->c[1], drawn[i]->c[2], drawn[i]->s, drawn[i]->align);
    printf("]}\n");
}

int main(int argc, char **argv)
{
    static sampedit e;
    int16_t w[8];
    srand(1); make_sample(); se_texts_init();
    if (argc > 1 && !strcmp(argv[1], "render")) {
        int16_t w0[8] = { 0, 0, 9000, 9000, 12000, 0, 0, 0x2D3F };            /* fresh recording: end at 2.5 s */
        memcpy(w, w0, sizeof w); se_open(&e, buf, w);
        e.view_len = se_w2s(9600); se_move(&e, 0, 0, 0);
        w[0] = (int16_t)se_s2w(24000, 0); w[3] = (int16_t)se_s2w(44100 * 2 + 30000, 0);
        frame(&e, "markers: start 0.54 s, end 2.68 s (view 0..2.6 s)");
        w[1] = (int16_t)se_s2w(30870, 0); w[2] = (int16_t)se_s2w(61740, 0);       /* loop 0.7 .. 1.4 s */
        frame(&e, "loop points (words 1 / 2): grey [ ] at the bottom, display only");
        e.loopsel = 1; frame(&e, "blue press: loop points blue / green (edited), start / end grey"); e.loopsel = 0;
        w[1] = w0[1]; w[2] = w0[2];
        e.snap = 1; e.last = 0; e.view_len = 4096; e.view0 = se_w2s(w[0]) - 2048; se_move(&e, 0, 0, 0);
        frame(&e, "zoomed on start, ZC snap on");
        e.snap = 2; e.view0 = 0; e.view_len = se_w2s(9600); w[0] = 0; se_move(&e, 0, 1, 1);
        frame(&e, "transient snap (tr): start on note 2, ticks = detected hits");
        se_tr_sens(&e, 2); frame(&e, "tr sensitivity 7 (Shift + red): label tr 7"); se_tr_sens(&e, -2);
        e.snap = 0; w[0] = (int16_t)se_s2w(24000, 0); w[3] = (int16_t)se_s2w(44100 * 2 + 30000, 0);
        se_host_shift = 1; frame(&e, "Shift held: alternate labels"); se_host_shift = 0;
        se_attenuate(&e, -12); frame(&e, "red right past 0 dB: +6.0 db (louder, clipped peaks)"); se_attenuate(&e, 12);
        e.mode = 1; e.fd_shape = 2; e.fd_curve = 1; e.fd_pct = 60;
        frame(&e, "fade mode: both, power, 60 % (blue), preview + curve");
        e.fd_shape = 0; e.fd_curve = 2; e.fd_pct = 100; e.view0 = 0; e.view_len = 44100 * 3; se_move(&e, 0, 0, 0);
        frame(&e, "fade mode: in, exp, 100 %");
        e.view0 = 0; e.view_len = se_w2s(9600); se_move(&e, 0, 0, 0);
        e.mode = 0; e.snap = 2; e.view0 = 0; e.view_len = 44100 * 2 + 20000;              /* README screenshot */
        se_transients(&e); w[0] = (int16_t)se_s2w(e.on[1], 0); w[3] = (int16_t)se_s2w(e.on[3] - 2000, 0);
        frame(&e, "readme: selection over note 2 and 3, transient snap");
        e.mode = 0; e.snap = 1;
        e.view0 = 0; e.view_len = se_w2s(9600); se_trim(&e);
        {                                                           /* drum: a 12 s kit, 8 hits */
            static int16_t tab[24 * 8];
            static const int32_t hit[8] = { 1000, 30000, 61000, 95000, 140000, 200000, 260000, 330000 };
            memset(buf_all, 0, sizeof buf_all);
            for (int h = 0; h < 8; h++) for (int i = 0; i < 11025; i++)
                buf[hit[h] + i] = (int16_t)(16000 * exp(-i / 2000.0) * sin(2 * M_PI * (150 + 40 * h) * i / 44100));
            e.drum = 1; e.pt = tab; se_n = SE_N_DRUM;
            for (int p = 0; p < 24; p++) { e.pad = (uint8_t)p; se_pad_set(&e, 0, p * 18000); se_pad_set(&e, 1, p * 18000 + 16000); }
            se_open_drum(&e, buf, tab, 3); e.snap = 0; e.trimmed = 0;
            e.view0 = 0; e.view_len = 360000;
            frame(&e, "drum (drum + scissors): pad 4, ticks = pad starts");
            e.mode = 2; e.ac_method = 0; e.ac_sens = 4; se_ac_compute(&e);
            frame(&e, "drum auto-cut (Shift + T3): hits, 8 pads, preview");
            se_ac_apply(&e); e.mode = 0; e.pad = 2;
            frame(&e, "after ok: pad 3 = hit 3, UNDO = Shift + left");
        }
        frame(&e, "after trim (T1): UNDO = Shift + left");
        return 0;
    }
    /* word <-> sample */
    assert(se_w2s(0) == 0 && se_w2s(32768) == SE_N && se_s2w(SE_N, 0) == 32767);
    for (int s = 0; s < SE_N; s += 997) assert(abs(se_w2s(se_s2w(s, 0)) - s) <= 5);
    /* zero crossings: found within range, exact sign change; silence counts as zero */
    for (int s = 100; s < 44100 * 2; s += 1234) {
        int32_t z = se_zero_cross(buf, s);
        assert(z >= 1 && abs(z - s) <= SE_ZC_RANGE);
        assert(buf[z] == 0 || (buf[z - 1] < 0) != (buf[z] < 0));
    }
    /* trim + undo: bit-exact restore, words back */
    int16_t w0[8] = { 0, 2000, 8000, 9000, 12000, 0, 0, 0x2D3F };
    memcpy(w, w0, sizeof w); se_open(&e, buf, w);
    memcpy(ref, buf, (SE_N * sizeof *buf));
    w[0] = 3000; w[3] = 7000;
    int32_t a = se_w2s(3000), b = se_w2s(7000);
    assert(se_trim(&e));
    assert(w[0] == 0 && w[3] <= se_s2w(b - a, 0) && se_w2s(w[3]) <= b - a && w[1] == 0 && w[2] == w[3]);
    { int32_t pe = se_w2s(w[3]), f = e.tr_fade;                    /* the kept audio is at the front, edges faded */
      assert(f == 132 && buf[0] == 0 && buf[pe - 1] == 0);
      assert(!memcmp(buf + f, ref + a + f, (size_t)(pe - 2 * f) * 2)); }
    assert(!se_trim(&e));                                          /* one level */
    assert(se_undo(&e) && !memcmp(buf, ref, (SE_N * sizeof *buf)) && w[0] == 3000 && w[3] == 7000 && w[1] == 2000);
    /* finalize: kept audio, then silence */
    assert(se_trim(&e)); se_finalize(&e);
    { int32_t f = e.tr_fade; assert(!memcmp(buf + f, ref + a + f, (size_t)(se_w2s(w[3]) - 2 * f) * 2)); }
    for (int i = b - a; i < SE_N; i++) assert(buf[i] == 0);
    /* DC: mean removed in the selection only */
    memcpy(buf, ref, SE_N * sizeof *buf); memcpy(w, w0, sizeof w); se_open(&e, buf, w);
    w[0] = 3000; w[3] = 4000; a = se_w2s(3000); b = se_w2s(4000);
    for (int i = a; i < b; i++) buf[i] = (int16_t)(ref[i] / 2 + 3000);
    assert(se_dc(&e));
    { long long sm = 0; for (int i = a; i < b; i++) sm += buf[i]; assert(llabs(sm / (b - a)) <= 1); }
    assert(buf[a - 1] == ref[a - 1] && buf[b] == ref[b]);
    assert(!se_undo(&e));
    /* marker limits: start < end, within 0..32767 */
    memcpy(w, w0, sizeof w); se_open(&e, ref, w);
    se_move(&e, 0, 100000, 1); assert(w[0] == w[3] - 1);
    se_move(&e, 1, -100000, 1); assert(w[3] == w[0] + 1);
    se_move(&e, 1, 100000, 1); assert(w[3] == 32767);
    se_move(&e, 0, -100000, 1); assert(w[0] == 0);
    /* snap: lands within 4 samples of a crossing; fine steps always move on, crossing by crossing, both ways */
    e.snap = 1; w[0] = 0; se_move(&e, 0, 2500, 1);
    int32_t s0 = se_w2s(w[0]), z = se_zero_cross(ref, s0);
    assert(z >= 0 && abs(z - s0) <= 5);
    for (int k = 0; k < 200; k++) { int16_t o = w[0]; se_move(&e, 0, 1, 1); assert(w[0] > o); }
    for (int k = 0; k < 200; k++) { int16_t o = w[0]; se_move(&e, 0, -1, 1); assert(w[0] < o); }
    e.view_len = 4096; for (int k = 0; k < 50; k++) { int16_t o = w[0]; se_move(&e, 0, 1, 0); assert(w[0] > o); }   /* zoomed */
    /* zero / normalize / move the selection */
    memcpy(buf, ref, (SE_N * sizeof *buf)); memcpy(w, w0, sizeof w); se_open(&e, buf, w);
    w[0] = 3000; w[3] = 4000; a = se_w2s(3000); b = se_w2s(4000);
    assert(se_zero(&e)); for (int i = a; i < b; i++) assert(buf[i] == 0);
    assert(buf[a - 1] == ref[a - 1] && buf[b] == ref[b]);
    memcpy(buf, ref, (SE_N * sizeof *buf)); w[0] = 3000; w[3] = 4000;
    for (int i = a; i < b; i++) buf[i] = (int16_t)(ref[i] / 4);
    assert(se_normalize(&e));
    int pk = 0; for (int i = a; i < b; i++) pk = abs(buf[i]) > pk ? abs(buf[i]) : pk;
    assert(pk >= 32760 && pk <= 32767 && buf[b] == ref[b]);
    assert(!se_normalize(&e));                                     /* already full scale */
    e.snap = 0; e.view_len = SE_N; w[0] = 3000; w[3] = 4000;
    se_shift_sel(&e, 5); assert(w[3] - w[0] == 1000 && w[0] > 3000);
    se_shift_sel(&e, -100000); assert(w[0] == 0 && w[3] == 1000);
    se_shift_sel(&e, 100000); assert(w[3] == 32767 && w[0] == 31767);
    /* blue press: loop mode moves words 1 / 2 (kept ordered, fine words cleared), start / end untouched */
    e.loopsel = 1; w[1] = 5000; w[2] = 6000; w[5] = 77; w[6] = 88;
    w[0] = 4000; w[3] = 8000;
    se_move(&e, 0, -100000, 1); assert(w[1] == 4000 && w[5] == 0);          /* not before start */
    se_move(&e, 0, 100000, 1); assert(w[1] == 5999 && w[0] == 4000 && w[3] == 8000);
    se_move(&e, 1, -100000, 1); assert(w[2] == 6000 && w[6] == 0);
    se_move(&e, 1, 100000, 1); assert(w[2] == 8000);                        /* not after end */
    e.loopsel = 0;
    se_move(&e, 1, -2500, 1); assert(w[3] == 5500 && w[2] == 5500 && w[1] == 5499);   /* end pushes the loop */
    se_move(&e, 0, 100000, 1); assert(w[0] == 5499 && w[1] == 5499 && w[2] == 5500);
    e.loopsel = 0;
    e.snap = 1; w[0] = 3000; w[3] = 4000; se_shift_sel(&e, 1);
    { int32_t s1 = se_w2s(w[0]), z1 = se_zero_cross(buf, s1); assert(w[3] - w[0] == 1000 && w[0] > 3000 && abs(z1 - s1) <= 5); }
    /* transients: the 4 note onsets (0, 0.55, 1.10, 1.65 s), each just before its hit; stepping goes note to note */
    memcpy(buf, ref, SE_N * sizeof *buf); memcpy(w, w0, sizeof w); se_open(&e, buf, w);
    se_transients(&e);
    assert(e.n_on == 4);
    for (int i = 0; i < 4; i++) { int32_t t = (int32_t)(i * 0.55 * 44100); assert(e.on[i] <= t + 8 && e.on[i] >= t - 300); }
    e.snap = 2; w[0] = 0; w[3] = 32767;
    se_move(&e, 0, 1, 1); assert(abs(se_w2s(w[0]) - e.on[1]) <= 5);
    /* transient sensitivity (Shift + red with tr): a soft re-attack (3x above the decayed tail) at 0.5 s is a hit only
     * at high sensitivity; "tr N" = 9 - rise ratio, clamped 1..7 */
    {
        int16_t keep = e.ac_sens;
        for (int i = 0; i < SE_N; i++) {
            double t = i / 44100.0, env = 20000 * exp(-t * 3.2);              /* ~4000 at 0.5 s */
            if (t >= 0.5) env = 12000 * exp(-(t - 0.5) * 3.2);
            buf[i] = (int16_t)(env * sin(2 * M_PI * 440 * t));
        }
        e.ac_sens = 4; e.on_dirty = 1; se_transients(&e); int n4 = e.n_on;
        se_tr_sens(&e, -10); assert(e.ac_sens == 8 && e.on_dirty); se_transients(&e); int n1 = e.n_on;
        se_tr_sens(&e, 10); assert(e.ac_sens == 2); se_transients(&e); int n7 = e.n_on;
        printf("tr sensitivity: hits at 1 / 4 / 7: %d / %d / %d\n", n1, n4, n7);
        assert(n1 == 1 && n4 == 1 && n7 == 2);
        e.ac_sens = keep; memcpy(buf, ref, SE_N * sizeof *buf); e.on_dirty = 1;
    }
    se_move(&e, 0, 1, 1); assert(abs(se_w2s(w[0]) - e.on[2]) <= 5);
    se_move(&e, 0, -1, 1); assert(abs(se_w2s(w[0]) - e.on[1]) <= 5);
    { int16_t o = w[0]; se_move(&e, 0, -1, 1); assert(w[0] < o); }     /* to the first (at 0) */
    se_zero(&e);  assert(e.on_dirty);                                   /* edits mark the list stale */
    /* fade: in / out / both, linear / equal power, length; only the selection */
    memcpy(w, w0, sizeof w); se_open(&e, buf, w);
    for (int i = 0; i < SE_N; i++) buf[i] = 10000;
    w[0] = 3000; w[3] = 4000; a = se_w2s(3000); b = se_w2s(4000);
    e.fd_shape = 0; e.fd_curve = 0; e.fd_pct = 100; assert(se_fade_apply(&e));
    assert(buf[a] == 0 && abs(buf[(a + b) / 2] - 5000) <= 10 && buf[b - 1] >= 9990 && buf[a - 1] == 10000 && buf[b] == 10000);
    for (int i = 0; i < SE_N; i++) buf[i] = 10000;
    e.fd_shape = 1; e.fd_curve = 1; assert(se_fade_apply(&e));     /* out, equal power: -3 dB at the middle */
    assert(buf[b - 1] == 0 && abs(buf[(a + b) / 2] - 7071) <= 30 && buf[a] >= 9990);
    for (int i = 0; i < SE_N; i++) buf[i] = 10000;
    e.fd_shape = 0; e.fd_curve = 2; assert(se_fade_apply(&e));     /* in, exp: 0, -30 dB at the middle, full at the end */
    assert(buf[a] == 0 && abs(buf[(a + b) / 2] - 307) <= 15 && buf[b - 1] >= 9900 && buf[(a + b) / 2 + (b - a) / 4] < 2000);
    for (int i = 0; i < SE_N; i++) buf[i] = 10000;
    e.fd_shape = 2; e.fd_curve = 0; e.fd_pct = 50; assert(se_fade_apply(&e));   /* both, a quarter of each half */
    { int32_t q = (b - a) / 4; assert(buf[a] == 0 && buf[b - 1] == 0 && buf[a + q + 1] == 10000 && buf[(a + b) / 2] == 10000); }
    se_fade_len(&e, -1000); assert(e.fd_pct == 1); se_fade_len(&e, 1000); assert(e.fd_pct == 100);
    memcpy(buf, ref, SE_N * sizeof *buf);
    /* copy / cut / paste through the clipboard */
    memcpy(buf, ref, SE_N * sizeof *buf); memcpy(w, w0, sizeof w); se_open(&e, buf, w);
    w[0] = 3000; w[3] = 4000; a = se_w2s(3000); b = se_w2s(4000);
    assert(se_copy(&e) && host_clip_len == b - a && host_clip_kind == 1 && !memcmp(host_clip, ref + a, (size_t)(b - a) * 2));
    assert(se_cut(&e));                                             /* removed, gap closed, tail zero */
    assert(buf[a - 1] == ref[a - 1] && !memcmp(buf + a, ref + b, (size_t)(SE_N - b) * 2));
    for (int i = SE_N - (b - a); i < SE_N; i++) assert(buf[i] == 0);
    assert(w[3] == w[0] + 1);
    assert(se_paste(&e));                                           /* back in: the original again */
    assert(!memcmp(buf, ref, (size_t)(SE_N - (b - a)) * 2));
    assert(se_w2s(w[3]) <= b && se_w2s(w[3]) >= b - 9);
    host_clip_kind = 3; assert(!se_paste(&e)); host_clip_kind = 1;  /* only tape audio */
    /* reverse: the selection mirrored, the rest untouched, twice = original */
    memcpy(buf, ref, SE_N * sizeof *buf); memcpy(w, w0, sizeof w); se_open(&e, buf, w);
    w[0] = 3000; w[3] = 4000; a = se_w2s(3000); b = se_w2s(4000);
    assert(se_reverse_sel(&e));
    for (int i = a; i < b; i++) assert(buf[i] == ref[a + b - 1 - i]);
    assert(buf[a - 1] == ref[a - 1] && buf[b] == ref[b]);
    assert(se_reverse_sel(&e) && !memcmp(buf, ref, SE_N * sizeof *buf));
    /* attenuation: preview on the gain word (playback ~ word^2), applied on close, word restored */
    memcpy(buf, ref, SE_N * sizeof *buf); memcpy(w, w0, sizeof w); se_open(&e, buf, w);
    for (int i = 0; i < 8; i++) buf[SE_N + i] = 0x5A5A;
    se_attenuate(&e, 12);                                           /* 6 dB */
    { double g = (double)w[7] / w0[7]; assert(fabs(g * g - 0.5012) < 0.003); assert(abs(e.att_amp - 32845) < 40); }
    assert(buf[1000] == ref[1000]);                                 /* not applied yet */
    se_attenuate(&e, -12); assert(e.att == 0 && w[7] == w0[7]);     /* back to 0 dB */
    se_attenuate(&e, 500); assert(e.att == SE_ATT_MAX);
    se_attenuate(&e, -(SE_ATT_MAX - 12));
    assert(se_apply_att(&e) && w[7] == w0[7]);
    for (int i = 0; i < SE_N; i += 101) assert(abs(buf[i] - (int)lround(ref[i] * 0.50119)) <= 2);
    for (int i = 0; i < 8; i++) assert((uint16_t)buf[SE_N + i] == 0x5A5A);   /* nothing written past the audio */
    assert(!se_apply_att(&e));
    /* louder (red right past 0 dB): +6 dB = x2 on close, saturating; preview word x sqrt(2); limit +24 dB */
    memcpy(buf, ref, SE_N * sizeof *buf); memcpy(w, w0, sizeof w); se_open(&e, buf, w);
    buf[100] = 10000; buf[101] = -12000; buf[102] = 30000; buf[103] = -30000;
    se_attenuate(&e, -12); assert(e.att == -12 && abs(e.att_amp - 130762) < 100);
    { double g = (double)w[7] / w0[7]; assert(fabs(g * g - 1.9953) < 0.01); }
    assert(se_apply_att(&e) && w[7] == w0[7]);
    assert(abs(buf[100] - 19953) <= 3 && abs(buf[101] + 23944) <= 3 && buf[102] == 32767 && buf[103] == -32768);
    se_attenuate(&e, -500); assert(e.att == -SE_BOOST_MAX && w[7] <= 32767 && w[7] > w0[7]);
    se_attenuate(&e, 500 + SE_BOOST_MAX); assert(e.att == SE_ATT_MAX);
    e.att = 0; e.att_amp = 65536; w[7] = w0[7];
    /* ---- drum ---- */
    {
        static int16_t tab[24 * 8];
        static const int32_t hit[8] = { 1000, 30000, 61000, 95000, 140000, 200000, 260000, 330000 };
        memset(buf_all, 0, sizeof buf_all);
        for (int h = 0; h < 8; h++)                                  /* decaying hits, 0.25 s each */
            for (int i = 0; i < 11025; i++)
                buf[hit[h] + i] = (int16_t)(16000 * exp(-i / 2000.0) * sin(2 * M_PI * (150 + 40 * h) * i / 44100));
        for (int p = 0; p < 24; p++) { e.pad = (uint8_t)p; e.pt = tab; e.drum = 1; se_n = SE_N_DRUM;
                                       se_pad_set(&e, 0, p * 20000); se_pad_set(&e, 1, p * 20000 + 15000); }
        se_open_drum(&e, buf, tab, 3); e.snap = 0;
        assert(SE_N == SE_N_DRUM && U2S(&e, SE_GET(&e, 0)) == 60000 && U2S(&e, SE_GET(&e, 3)) == 75000);
        for (int32_t x = 0; x < SE_N_DRUM; x += 7919) { se_pad_set(&e, 0, x); assert(se_pad_get(&e, 0) == x); }   /* exact */
        se_pad_set(&e, 0, 60000);
        se_move(&e, 1, 1, 1); assert(se_pad_get(&e, 1) == 75001);       /* fine = 1 sample */
        e.pad = 4; assert(se_pad_get(&e, 0) == 80000);                   /* other pads untouched */
        /* fit: pad around hit 4 with silence on both sides */
        e.pad = 4; se_pad_set(&e, 0, 130000); se_pad_set(&e, 1, 160000);
        assert(se_fit(&e)); { int32_t a = se_pad_get(&e, 0), b = se_pad_get(&e, 1);
          assert(a >= 139990 && a <= 140001 && b > 141000 && b <= 140000 + 11025); }
        assert(!se_fit(&e));                                              /* already tight */
        /* auto-cut, hits: the 8 hits on pads 1..8, ends before the silence, the other pads kept */
        int32_t old16a, old16b; e.pad = 16; old16a = se_pad_get(&e, 0); old16b = se_pad_get(&e, 1);
        e.ac_method = 0; e.ac_sens = 4; se_ac_compute(&e);
        assert(e.ac_n == 8);
        for (int h = 0; h < 8; h++) {
            assert(e.ac_cut[h][0] <= hit[h] + 8 && e.ac_cut[h][0] >= hit[h] - 300);
            assert(e.ac_cut[h][1] > hit[h] + 1000 && e.ac_cut[h][1] <= hit[h] + 11025 + 8);
        }
        int32_t before4 = (e.pad = 4, se_pad_get(&e, 0));
        assert(se_ac_apply(&e));
        e.pad = 2; assert(se_pad_get(&e, 0) == e.ac_cut[2][0] && se_pad_get(&e, 1) == e.ac_cut[2][1]);
        e.pad = 16; assert(se_pad_get(&e, 0) == old16a && se_pad_get(&e, 1) == old16b);
        assert(se_ac_undo(&e)); e.pad = 4; assert(se_pad_get(&e, 0) == before4); assert(!se_ac_undo(&e));
        /* first 24 when there are more: 30 hits */
        memset(buf_all, 0, sizeof buf_all);
        for (int h = 0; h < 30; h++) for (int i = 0; i < 4000; i++)
            buf[5000 + h * 15000 + i] = (int16_t)(12000 * exp(-i / 800.0) * sin(2 * M_PI * 200 * i / 44100));
        e.on_dirty = 1; se_ac_compute(&e);
        assert(e.ac_n == 24 && abs(e.ac_cut[23][0] - (5000 + 23 * 15000)) <= 300);
        /* equal: 24 slices up to the end of the sound */
        e.ac_method = 1; se_ac_compute(&e);
        assert(e.ac_n == 24 && e.ac_cut[0][0] == 0);
        /* equal: blue sets the number of parts (1..24, default 24) */
        assert(e.ac_parts == 24);
        se_ac_parts(&e, -20); assert(e.ac_parts == 4 && e.ac_n == 4 && e.ac_cut[0][0] == 0);
        { int32_t len = e.ac_cut[3][1]; for (int i = 1; i < 4; i++) assert(abs(e.ac_cut[i][0] - len * i / 4) <= 1100 && e.ac_cut[i - 1][1] == e.ac_cut[i][0]); }
        se_ac_parts(&e, -100); assert(e.ac_parts == 1 && e.ac_n == 1 && e.ac_cut[0][0] == 0);
        se_ac_parts(&e, 100); assert(e.ac_parts == 24 && e.ac_n == 24);
        for (int i = 1; i < 24; i++) assert(e.ac_cut[i][0] > e.ac_cut[i - 1][0] && e.ac_cut[i - 1][1] == e.ac_cut[i][0]);
        /* a decaying tail with ripples: still one hit per note (hardware: hits inside one sound) */
        memset(buf_all, 0, sizeof buf_all);
        for (int h = 0; h < 3; h++) for (int i = 0; i < 40000; i++) {
            double env = exp(-i / 9000.0) * (1.0 + 0.6 * sin(2 * M_PI * 9 * i / 44100));   /* tremolo-like ripple */
            buf[10000 + h * 60000 + i] = (int16_t)(14000 * env * sin(2 * M_PI * 220 * i / 44100) + (rand() % 200 - 100));
        }
        e.on_dirty = 1; se_transients(&e); assert(e.n_on == 3);
        /* pad change: the region is brought into view */
        e.pad = 0; se_pad_set(&e, 0, 400000); se_pad_set(&e, 1, 420000);
        e.view0 = 0; e.view_len = 50000; se_show_region(&e);
        assert(e.view0 <= 400000 && e.view0 + e.view_len >= 420000 && e.view_len <= 31000);
        /* sensitivity: right = more hits (lower ratio), clamped */
        e.ac_method = 0; se_ac_sens(&e, 10); assert(e.ac_sens == 2); se_ac_sens(&e, -10); assert(e.ac_sens == 8);
        /* cut zeroes in place, paste overwrites (no shift of the other pads' audio) */
        e.pad = 0; se_pad_set(&e, 0, 5000); se_pad_set(&e, 1, 9000);
        int16_t at20000 = buf[20000];
        assert(se_cut(&e)); for (int i = 5000; i < 9000; i++) assert(buf[i] == 0);
        assert(buf[20000] == at20000);
        assert(se_paste(&e)); assert(buf[5000] == host_clip[0] && buf[20000] == at20000);
        e.drum = 0; se_n = SE_N_SYNTH;
    }
    /* trim, then an edit that moves audio: the kept audio survives closing (paste pushed it right) */
    {
        srand(1); make_sample(); memcpy(ref, buf, SE_N * sizeof *buf);
        memcpy(w, w0, sizeof w); se_open(&e, buf, w);
        w[0] = 3000; w[3] = 7000; a = se_w2s(3000); b = se_w2s(7000);
        assert(se_trim(&e));
        int32_t f = e.tr_fade, len = b - a, n = 10000;
        for (int i = 0; i < n; i++) host_clip[i] = 1234;
        host_clip_len = n; host_clip_kind = 1;
        w[3] = (int16_t)se_s2w(len, 1);                                /* drop at the start: the trim moves right */
        assert(se_paste(&e) && buf[0] == 1234 && buf[n - 1] == 1234);
        se_finalize(&e);                                               /* closing */
        assert(!memcmp(buf + n + f, ref + a + f, (size_t)(len - 2 * f - 8) * 2));
        /* trim, end marker moved, undo: bit-exact (the fades are undone where they were made) */
        memcpy(buf, ref, SE_N * sizeof *buf); memcpy(w, w0, sizeof w); se_open(&e, buf, w);
        w[0] = 3000; w[3] = 7000;
        assert(se_trim(&e));
        se_move(&e, 1, -300, 1);
        assert(se_undo(&e) && !memcmp(buf, ref, SE_N * sizeof *buf) && w[0] == 3000 && w[3] == 7000);
        /* trim, cut, undo: the cut made the trim final, undo cannot rotate the shifted audio */
        memcpy(buf, ref, SE_N * sizeof *buf); memcpy(w, w0, sizeof w); se_open(&e, buf, w);
        w[0] = 3000; w[3] = 7000;
        assert(se_trim(&e));
        int32_t cw = (int16_t)se_s2w(len / 2, 1);
        w[0] = (int16_t)cw; w[3] = (int16_t)se_s2w(len, 1);            /* cut the second half of the kept audio */
        int32_t ca = se_w2s(cw);
        assert(se_cut(&e) && !e.trimmed && !se_undo(&e));
        assert(!memcmp(buf + f, ref + a + f, (size_t)(ca - f) * 2));    /* the first half stays at the front */
        for (int i = len; i < SE_N; i++) assert(buf[i] == 0);          /* the trimmed-away audio is gone */
    }
    /* drum drop: a long clip stops at the next pad (other pads keep their audio) */
    {
        static int16_t tab[24 * 8];
        memset(buf_all, 0, sizeof buf_all);
        for (int i = 0; i < SE_N_DRUM; i++) buf[i] = (int16_t)(i % 1000 + 1);
        memcpy(ref, buf, SE_N_DRUM * sizeof *buf);
        memset(tab, 0, sizeof tab);
        for (int p = 0; p < 24; p++) { e.pad = (uint8_t)p; e.pt = tab; e.drum = 1; se_n = SE_N_DRUM;
                                       se_pad_set(&e, 0, 100000 + p * 10000); se_pad_set(&e, 1, 100000 + p * 10000 + 5000); }
        se_open_drum(&e, buf, tab, 3);                                 /* pad 3: [130000, 135000) */
        for (int i = 0; i < 100000; i++) host_clip[i] = -7;
        host_clip_len = 100000; host_clip_kind = 1;
        assert(se_paste(&e));
        for (int i = 130000; i < 140000; i++) assert(buf[i] == -7);    /* up to pad 4's start */
        assert(!memcmp(buf + 140000, ref + 140000, (size_t)(SE_N_DRUM - 140000) * 2));
        assert(se_pad_get(&e, 1) == 140000);
        e.pad = 4; assert(se_pad_get(&e, 0) == 140000 && se_pad_get(&e, 1) == 145000);
        e.drum = 0; se_n = SE_N_SYNTH;
    }
    /* rotation on odd sizes */
    for (int n = 1; n < 50; n++) for (int k = 0; k <= n; k++) {
        int16_t v[64]; for (int i = 0; i < n; i++) v[i] = (int16_t)i;
        se_rotate(v, n, k); for (int i = 0; i < n; i++) assert(v[i] == (k <= 0 || k >= n ? i : (i + k) % n));
    }
    printf("ok\n");
    return 0;
}
