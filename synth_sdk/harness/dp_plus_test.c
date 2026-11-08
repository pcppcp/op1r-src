/* drum+ voice features on the host, no firmware data: synthetic FIR (one centre tap) and a constant sample, so the
 * output is proportional to the gain and positions show the pitch.
 *   cc -O1 -I.. dp_plus_test.c ../drumplus/dp_plus.c ../drumplus/dp_voice.c -lm -o build/dp_plus_test */
#include "../drumplus/dp_plus.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define NS 200000
static int16_t fir_buf[64 + 16 * 64 + 64], *fir = fir_buf + 64;
static int16_t data[8 + NS + 64];
static int32_t inc[DP_BLOCK]; static int16_t vel[DP_BLOCK];
static dp_pad pad;
static dp_pre c;

static void setup(int16_t playmode, int16_t reverse)
{
    memset(fir_buf, 0, sizeof fir_buf);
    for (int p = 0; p < 64; p++) fir[7 * 64 - p] = 16384;       /* every phase: the sample at tap 7 */
    for (int i = 0; i < 8 + NS; i++) data[i] = (int16_t)(i < 8 ? 0 : 10000);
    for (int n = 0; n < DP_BLOCK; n++) { inc[n] = 1 << 27; vel[n] = 127 << 8; }
    /* region: sample 1000 .. 150000 of the buffer (hi / lo words of start / end), stock bounds ~ x 1.0093 */
    int32_t st = (1000 * 2048) / 1.0093, en = (150000 * 2048) / 1.0093;
    pad = (dp_pad){ 0, (int16_t)(st >> 16), (int16_t)(en >> 16), playmode, reverse, (int16_t)((st & 0xFFFF) >> 1),
                    (int16_t)((en & 0xFFFF) >> 1), 8192 };
}
static int peak(const int16_t *o) { int m = 0; for (int n = 0; n < DP_BLOCK; n++) m = abs(o[n]) > m ? abs(o[n]) : m; return m; }

int main(void)
{
    setvbuf(stdout, 0, _IONBF, 0);
    int16_t o1[DP_BLOCK], o2[DP_BLOCK];
    dp_voice a, b; dp_xvoice s; dp_extra x;
    /* 1. all extras off = the stock voice, bit for bit (forward one-shot, reverse loop) */
    for (int rv = 0; rv < 2; rv++) {
        setup(rv ? 16384 : 8192, rv ? 16384 : 8192); memset(&x, 0, sizeof x); memset(&s, 0, sizeof s);
        dp_stock_note_on(&a, &pad); (dp_plus_prepare(&x, &c), dp_plus_note_on(&b, &s, &pad, &x, &c, 127, 22050));
        for (int k = 0; k < 400; k++) {
            dp_stock_render(&a, o1, &pad, inc, vel, data, fir);
            dp_plus_render(&b, &s, o2, &pad, &x, &c, inc, vel, data, fir, 22050);
            assert(!memcmp(o1, o2, sizeof o1) && a.pos == b.pos && a.state == b.state);
        }
    }
    printf("neutral extras: identical to the stock voice (400 blocks, forward / reverse loop)\n");
    /* 2. fine tune +50 cents: position advances 2^(50/1200) faster */
    setup(8192, 8192); memset(&x, 0, sizeof x); x.fine = 50;
    (dp_plus_prepare(&x, &c), dp_plus_note_on(&b, &s, &pad, &x, &c, 127, 22050)); int32_t p0 = b.pos;
    for (int k = 0; k < 10; k++) dp_plus_render(&b, &s, o2, &pad, &x, &c, inc, vel, data, fir, 22050);
    double ratio = (double)(b.pos - p0) / (10.0 * DP_BLOCK * 1024), want = pow(2, 50 / 1200.0);
    printf("fine +50 cents: speed %.5f (want %.5f)\n", ratio, want); assert(fabs(ratio / want - 1) < 0.002);
    /* 3. pitch envelope +12 st: starts at double speed, back to 1x after the glide time */
    memset(&x, 0, sizeof x); x.pitch_env = 12; x.pitch_time = 60;
    (dp_plus_prepare(&x, &c), dp_plus_note_on(&b, &s, &pad, &x, &c, 127, 22050)); p0 = b.pos;
    dp_plus_render(&b, &s, o2, &pad, &x, &c, inc, vel, data, fir, 22050);
    double first = (double)(b.pos - p0) / (DP_BLOCK * 1024);
    uint32_t tg = DP_VOICE_X * dp_time_samples(60);                  /* voice samples (88.2 kHz) */
    for (uint32_t k = 0; k < tg / DP_BLOCK + 2; k++) dp_plus_render(&b, &s, o2, &pad, &x, &c, inc, vel, data, fir, 22050);
    p0 = b.pos; dp_plus_render(&b, &s, o2, &pad, &x, &c, inc, vel, data, fir, 22050);
    double later = (double)(b.pos - p0) / (DP_BLOCK * 1024);
    printf("pitch env +12: first block %.3fx, after %u samples %.4fx\n", first, tg, later);
    assert(first > 1.9 && first <= 2.0 && fabs(later - 1) < 1e-9);
    /* 4. envelope: attack ramps up over t_att, linear decay ends the voice after t_dec */
    memset(&x, 0, sizeof x); x.env_on = 1; x.attack = 30; x.decay = 40;
    (dp_plus_prepare(&x, &c), dp_plus_note_on(&b, &s, &pad, &x, &c, 127, 22050));
    uint32_t ta = DP_VOICE_X * dp_time_samples(30), td = DP_VOICE_X * dp_time_samples(40), n = 0, off_at = 0; int pk_att = 0, pk_full = 0;
    while (b.state != DP_OFF && n < 400000) {
        dp_plus_render(&b, &s, o2, &pad, &x, &c, inc, vel, data, fir, 22050);
        if (n == 0) pk_att = peak(o2);
        if (n / DP_BLOCK == ta / DP_BLOCK + 1) pk_full = peak(o2);
        n += DP_BLOCK; if (b.state == DP_OFF) off_at = n;
    }
    printf("envelope: first block %d, after attack (%u) %d, voice off after %u samples (attack + decay %u)\n",
           pk_att, ta, pk_full, off_at, ta + td);
    assert(pk_att < pk_full / 20 && off_at >= ta + td && off_at <= ta + td + 2 * DP_BLOCK);
    /* 5. gate + release: key-up starts the release, then the voice is off */
    setup(0, 8192); memset(&x, 0, sizeof x); x.env_on = 1; x.release = 50;
    (dp_plus_prepare(&x, &c), dp_plus_note_on(&b, &s, &pad, &x, &c, 127, 22050));
    for (int k = 0; k < 20; k++) dp_plus_render(&b, &s, o2, &pad, &x, &c, inc, vel, data, fir, 22050);
    int held = peak(o2); dp_plus_note_off(&b, &s, &pad, &x, &c); n = 0;
    while (b.state != DP_OFF && n < 400000) { dp_plus_render(&b, &s, o2, &pad, &x, &c, inc, vel, data, fir, 22050); n += DP_BLOCK; }
    printf("release: held %d, off %u samples after key-up (release %u)\n", held, n, DP_VOICE_X * dp_time_samples(50));
    assert(held > 0 && n >= DP_VOICE_X * dp_time_samples(50) && n <= DP_VOICE_X * dp_time_samples(50) + DP_BLOCK);
    /* 6. retrig: 4 hits of 1/16 at 120 bpm (22050 samples / beat): restarts every 5512 samples, -2 dB each */
    setup(8192, 8192); memset(&x, 0, sizeof x); x.retrig = 4; x.retrig_div = 2;
    (dp_plus_prepare(&x, &c), dp_plus_note_on(&b, &s, &pad, &x, &c, 127, 22050));
    int restarts = 0, lev[4] = { 0 }, hit = 0, measure = 1; int32_t last = b.pos; n = 0;
    while (n < 40000) {
        dp_plus_render(&b, &s, o2, &pad, &x, &c, inc, vel, data, fir, 22050);
        if (measure && hit < 4) { lev[hit] = peak(o2); measure = 0; }      /* the block after a restart: one hit only */
        if (b.pos < last) { restarts++; hit++; measure = 1; }
        last = b.pos; n += DP_BLOCK;
    }
    printf("retrig: %d restarts, levels %d %d %d %d (ratios %.3f %.3f %.3f, want 0.794)\n", restarts, lev[0], lev[1],
           lev[2], lev[3], (double)lev[1] / lev[0], (double)lev[2] / lev[1], (double)lev[3] / lev[2]);
    assert(restarts == 3);
    for (int i = 1; i < 4; i++) assert(fabs((double)lev[i] / lev[i - 1] - 0.794) < 0.02);
    /* 7. filter: sine at f (one buffer sample per output sample), steady level vs the unfiltered level */
    double lev_lp[3], lev_hp[3], lev_bp[3]; static const double fq[3] = { 100, 1000, 10000 };
    for (int type = 1; type <= 3; type++)
        for (int fi = 0; fi < 3; fi++) {
            setup(8192, 8192);
            for (int i = 0; i < NS; i++) data[8 + i] = (int16_t)(12000 * sin(2 * M_PI * fq[fi] * i / 88200.0));   /* one buffer sample per 88.2 kHz output sample */
            for (int k = 0; k < DP_BLOCK; k++) inc[k] = 1 << 28;
            double lv[2];
            for (int pass = 0; pass < 2; pass++) {                  /* 0: filter off (drive 1 = reference), 1: on */
                memset(&x, 0, sizeof x); x.fine = 0; x.filter = pass ? type : 0; x.cutoff = 71; x.drive = 0;
                if (!pass) x.env_on = 1;                            /* active, but neutral: reference level */
                (dp_plus_prepare(&x, &c), dp_plus_note_on(&b, &s, &pad, &x, &c, 127, 22050));
                double e2 = 0; int cnt = 0;
                for (int k = 0; k < 60; k++) {
                    dp_plus_render(&b, &s, o2, &pad, &x, &c, inc, vel, data, fir, 22050);
                    if (k >= 30) for (int m = 0; m < DP_BLOCK; m++) { e2 += (double)o2[m] * o2[m]; cnt++; }
                }
                lv[pass] = sqrt(e2 / cnt);
            }
            double db = 20 * log10(lv[1] / lv[0]);
            (type == 1 ? lev_lp : type == 2 ? lev_hp : lev_bp)[fi] = db;
        }
    printf("filter @ step 71 (~1 kHz), res 0: LP %.1f / %.1f / %.1f dB, HP %.1f / %.1f / %.1f dB, BP %.1f / %.1f / %.1f dB"
           " (100 Hz / 1 kHz / 10 kHz)\n", lev_lp[0], lev_lp[1], lev_lp[2], lev_hp[0], lev_hp[1], lev_hp[2],
           lev_bp[0], lev_bp[1], lev_bp[2]);
    assert(fabs(lev_lp[0]) < 0.5 && lev_lp[1] < -4.5 && lev_lp[1] > -8 && lev_lp[2] < -35);
    assert(fabs(lev_hp[2]) < 0.5 && lev_hp[1] < -4.5 && lev_hp[1] > -8 && lev_hp[0] < -35);
    assert(lev_bp[1] > lev_bp[0] + 12 && lev_bp[1] > lev_bp[2] + 12);        /* Q = 0.5: ~14 dB a decade away */
    /* 8. maximum resonance on a loud, changing signal: stays bounded */
    setup(16384, 8192);
    for (int i = 0; i < NS; i++) data[8 + i] = (int16_t)(30000 * sin((double)i * i * 1e-7));
    memset(&x, 0, sizeof x); x.filter = 1; x.cutoff = 100; x.resonance = 127;
    (dp_plus_prepare(&x, &c), dp_plus_note_on(&b, &s, &pad, &x, &c, 127, 22050));
    int big = 0;
    for (int k = 0; k < 2000; k++) { dp_plus_render(&b, &s, o2, &pad, &x, &c, inc, vel, data, fir, 22050); big = peak(o2) > big ? peak(o2) : big; }
    printf("resonance 127, 2000 blocks: peak %d, integrators %d / %d\n", big, s.ic1, s.ic2);
    assert(abs(s.ic1) < (1 << 28) && abs(s.ic2) < (1 << 28));
    /* 9. drive: louder, but never above the soft clip's 2/3 of full scale */
    setup(8192, 8192);
    for (int i = 0; i < NS; i++) data[8 + i] = (int16_t)(12000 * sin(2 * M_PI * 200 * i / 44100.0));
    int dpk[2];
    for (int dr = 0; dr < 2; dr++) {
        memset(&x, 0, sizeof x); x.drive = dr ? 127 : 0; x.env_on = !dr;
        (dp_plus_prepare(&x, &c), dp_plus_note_on(&b, &s, &pad, &x, &c, 127, 22050));
        dpk[dr] = 0;
        for (int k = 0; k < 20; k++) { dp_plus_render(&b, &s, o2, &pad, &x, &c, inc, vel, data, fir, 22050); dpk[dr] = peak(o2) > dpk[dr] ? peak(o2) : dpk[dr]; }
    }
    printf("drive: peak %d -> %d (ceiling %d)\n", dpk[0], dpk[1], 2 * 32767 / 3 + 1);
    assert(dpk[1] > dpk[0] && dpk[1] <= 2 * 32767 / 3 + 1);
    /* 10. choke group 1: pad 1 fades out pad 0 within ~2 ms; mono pad 2 fades its own previous hit; pad 3 untouched */
    {
        setup(8192, 8192);
        static dp_extra px[4]; static dp_pre pc[4]; dp_voice vv[3]; dp_xvoice xs[3]; uint8_t vp[3] = { 0, 2, 3 };
        memset(px, 0, sizeof px); memset(xs, 0, sizeof xs);
        px[0].choke = px[1].choke = 1; px[2].mono = 1;
        for (int i = 0; i < 4; i++) dp_plus_prepare(&px[i], &pc[i]);
        for (int i = 0; i < 3; i++) dp_plus_note_on(&vv[i], &xs[i], &pad, &px[vp[i]], &pc[vp[i]], 127, 22050);
        dp_voice *vptr[3] = { &vv[0], &vv[1], &vv[2] };
        int f1 = dp_plus_choke(vptr, xs, vp, 3, 1, px), f2 = dp_plus_choke(vptr, xs, vp, 3, 2, px);
        int blocks = 0;
        while (vv[0].state != DP_OFF && blocks < 100) {
            for (int i = 0; i < 3; i++) dp_plus_render(&vv[i], &xs[i], o2, &pad, &px[vp[i]], &pc[vp[i]], inc, vel, data, fir, 22050);
            blocks++;
        }
        dp_plus_render(&vv[2], &xs[2], o2, &pad, &px[3], &pc[3], inc, vel, data, fir, 22050);
        printf("choke: %d + %d voices fading, off after %d blocks (%d samples); untouched pad: state %d, peak %d\n",
               f1, f2, blocks, blocks * DP_BLOCK, vv[2].state, peak(o2));
        assert(f1 == 1 && f2 == 1 && vv[0].state == DP_OFF && vv[1].state == DP_OFF && blocks * DP_BLOCK <= 256);   /* ~2 ms at 88.2 kHz */
        assert(vv[2].state != DP_OFF && peak(o2) > 0);
        /* the stolen (fading) voice starts clean on its next note */
        dp_plus_note_on(&vv[0], &xs[0], &pad, &px[3], &pc[3], 127, 22050);
        assert(!xs[0].fading && !xs[0].hits_left);
    }
    /* 11. velocity -> pitch: +12 st at 127, 0 at 64, -12 at 1 */
    setup(8192, 8192); memset(&x, 0, sizeof x); x.vel_pitch = 12; dp_plus_prepare(&x, &c);
    double sp[3]; static const int vv3[3] = { 1, 64, 127 };
    for (int i = 0; i < 3; i++) {
        dp_plus_note_on(&b, &s, &pad, &x, &c, vv3[i], 22050); p0 = b.pos;
        dp_plus_render(&b, &s, o2, &pad, &x, &c, inc, vel, data, fir, 22050);
        sp[i] = (double)(b.pos - p0) / (DP_BLOCK * 1024);
    }
    printf("velocity -> pitch: speed %.4f / %.4f / %.4f at velocity 1 / 64 / 127\n", sp[0], sp[1], sp[2]);
    assert(fabs(sp[0] - 0.5) < 0.01 && fabs(sp[1] - 1) < 1e-9 && fabs(sp[2] - 2) < 0.01);
    /* 12. velocity -> start: velocity 0 starts vel_start * 32 samples in, 127 at the start */
    memset(&x, 0, sizeof x); x.vel_start = 100; dp_plus_prepare(&x, &c);
    dp_plus_note_on(&a, &s, &pad, &x, &c, 127, 22050); dp_plus_note_on(&b, &s, &pad, &x, &c, 0, 22050);
    printf("velocity -> start: offset %d samples at velocity 0\n", (b.pos - a.pos) >> 11);
    assert(((b.pos - a.pos) >> 11) == 3200);
    /* 13. velocity -> cutoff: a 1 kHz sine through LP at step 71: soft hits darker */
    setup(8192, 8192);
    for (int i = 0; i < NS; i++) data[8 + i] = (int16_t)(12000 * sin(2 * M_PI * 3000 * i / 44100.0));
    for (int k = 0; k < DP_BLOCK; k++) inc[k] = 1 << 28;
    memset(&x, 0, sizeof x); x.filter = 1; x.cutoff = 71; x.vel_cutoff = 40; dp_plus_prepare(&x, &c);
    double r2[2];
    for (int i = 0; i < 2; i++) {
        dp_plus_note_on(&b, &s, &pad, &x, &c, i ? 127 : 1, 22050);
        for (int k = 0; k < 2; k++) for (int m = 0; m < DP_BLOCK; m++) vel[m] = 127 << 8;
        double e2 = 0; int cnt = 0;
        for (int k = 0; k < 40; k++) {
            dp_plus_render(&b, &s, o2, &pad, &x, &c, inc, vel, data, fir, 22050);
            if (k >= 20) for (int m = 0; m < DP_BLOCK; m++) { e2 += (double)o2[m] * o2[m]; cnt++; }
        }
        r2[i] = sqrt(e2 / cnt);
    }
    printf("velocity -> cutoff: 3 kHz level %.0f (velocity 1) vs %.0f (velocity 127)\n", r2[0], r2[1]);
    assert(r2[1] > 4 * r2[0]);
    /* 14. humanize: start delays within 0 .. human * 4 samples, spread; level never louder */
    memset(&x, 0, sizeof x); x.human = 127; dp_plus_prepare(&x, &c);
    dp_plus_note_on(&a, &s, &pad, &x, &c, 127, 22050);
    int mn = 1 << 30, mx = -1, gmin = 32767, gmax = 0; int32_t base = dp_bound(pad.start_hi, pad.start_lo);
    for (int i = 0; i < 2000; i++) {
        dp_plus_note_on(&b, &s, &pad, &x, &c, 127, 22050);
        int d = (b.pos - base) >> 11; mn = d < mn ? d : mn; mx = d > mx ? d : mx;
        gmin = s.hit_gain < gmin ? s.hit_gain : gmin; gmax = s.hit_gain > gmax ? s.hit_gain : gmax;
    }
    printf("humanize 127: delay %d .. %d samples, hit gain %d .. %d\n", mn, mx, gmin, gmax);
    assert(mn >= 0 && mn < 20 && mx <= 508 && mx > 480 && gmax <= 32767 && gmin >= 32767 - 2540 && gmin < 32000);
    /* 15. saved form: every field clamped into its range, read back the same, offsets distinct */
    {
        dp_extra e; memset(&e, 0, sizeof e);
        assert(dp_extra_set(&e, 6, 99) == 48 && e.pitch_env == 48 && dp_extra_get(&e, 6) == 48);
        assert(dp_extra_set(&e, 18, -500) == -127 && dp_extra_get(&e, 18) == -127);
        assert(dp_extra_set(&e, 9, 1) == 0 && dp_extra_set(&e, 9, 5) == 5 && e.retrig == 5);
        assert(dp_extra_set(&e, 99, 5) == 0 && dp_extra_get(&e, -1) == 0);
        memset(&e, 0, sizeof e);
        int stored[DP_NFIELDS];                                  /* distinct offsets: no field overwrites another */
        for (int i = 0; i < DP_NFIELDS; i++) stored[i] = dp_extra_set(&e, i, -3 - i);
        for (int i = 0; i < DP_NFIELDS; i++) stored[i] = dp_extra_set(&e, i, i + 2) ;
        for (int i = 0; i < DP_NFIELDS; i++) assert(dp_extra_get(&e, i) == stored[i] && stored[i] > 0);
        printf("saved form: %d fields, clamped and read back\n", DP_NFIELDS);
    }
    /* 16. fast path (pre -> stock render -> post) == C path, bit for bit, in several configurations */
    {
        static const struct { int pm, rv, env, exp, pe, fine, flt, res, drv, rt, off_at, choke_at; } cf[] = {
            { 8192, 8192, 1, 0, 12, 20, 1, 90, 60, 0, -1, -1 },     /* one-shot: lin env, pitch env, LP, drive */
            { 16384, 8192, 1, 1, -24, -50, 2, 40, 0, 0, -1, -1 },   /* loop: exp env, HP */
            { 16384, 16384, 0, 0, 0, 7, 3, 127, 127, 0, -1, -1 },   /* reverse loop: BP max reso, max drive */
            { 0, 8192, 1, 1, 5, 0, 1, 20, 0, 0, 40, -1 },           /* gate: release after key-up */
            { 8192, 8192, 1, 0, 0, 0, 0, 0, 30, 6, -1, -1 },        /* retrig 6 at 1/16 (C path in those blocks) */
            { 16384, 8192, 0, 0, 3, 0, 1, 50, 0, 0, -1, 25 },       /* choke fade mid-way */
            { 8192, 8192, 1, 0, 48, 50, 0, 0, 0, 4, -1, -1 },       /* pitch up 8x clamp + retrig */
        };
        for (unsigned k = 0; k < sizeof cf / sizeof cf[0]; k++) {
            setup((int16_t)cf[k].pm, (int16_t)cf[k].rv);
            for (int i = 0; i < NS; i++) data[8 + i] = (int16_t)(15000 * sin(i * 0.031) * (0.5 + 0.5 * sin(i * 0.0007)));
            memset(&x, 0, sizeof x);
            x.env_on = (uint8_t)cf[k].env; x.attack = 10; x.decay = 70; x.release = 40; x.curve_exp = (uint8_t)cf[k].exp;
            x.pitch_env = (int8_t)cf[k].pe; x.pitch_time = 50; x.fine = (int8_t)cf[k].fine; x.filter = (uint8_t)cf[k].flt;
            x.cutoff = 60; x.resonance = (uint8_t)cf[k].res; x.drive = (uint8_t)cf[k].drv;
            x.retrig = (uint8_t)cf[k].rt; x.retrig_div = 2; x.vel_pitch = 3;
            dp_plus_prepare(&x, &c);
            dp_voice va, vb; dp_xvoice sa, sb; memset(&sa, 0, sizeof sa); memset(&sb, 0, sizeof sb);
            dp_plus_note_on(&va, &sa, &pad, &x, &c, 100, 22050); dp_plus_note_on(&vb, &sb, &pad, &x, &c, 100, 22050);
            sb = sa;                                                  /* same random humanize state (none here) */
            int fast = 0, slow = 0;
            for (int blk = 0; blk < 300; blk++) {
                if (blk == cf[k].off_at) { dp_plus_note_off(&va, &sa, &pad, &x, &c); dp_plus_note_off(&vb, &sb, &pad, &x, &c); }
                if (blk == cf[k].choke_at) { sa.fading = sb.fading = 1; sa.fade = sb.fade = 32767; }
                int32_t ib[DP_BLOCK]; int16_t vb2[DP_BLOCK];
                for (int n = 0; n < DP_BLOCK; n++) { ib[n] = inc[n] + n * 4096; vb2[n] = (int16_t)(vel[n] - n * 7); }  /* vary */
                int32_t ia[DP_BLOCK]; int16_t va2[DP_BLOCK];
                memcpy(ia, ib, sizeof ia); memcpy(va2, vb2, sizeof va2);
                dp_plus_render(&va, &sa, o1, &pad, &x, &c, ia, va2, data, fir, 22050);
                if (dp_plus_fast_pre(&vb, &sb, &x, &c, ib, vb2)) {
                    dp_stock_render(&vb, o2, &pad, ib, vb2, data, fir);
                    dp_plus_fast_post(&vb, &sb, &x, &c, o2); fast++;
                } else { dp_plus_render(&vb, &sb, o2, &pad, &x, &c, ib, vb2, data, fir, 22050); slow++; }
                if (memcmp(o1, o2, sizeof o1) || va.pos != vb.pos || va.state != vb.state) {
                    int d = 0; while (o1[d] == o2[d]) d++;
                    printf("fast path differs: config %u block %d sample %d (%d vs %d), pos %d/%d state %d/%d\n",
                           k, blk, d, o1[d], o2[d], va.pos, vb.pos, va.state, vb.state);
                    return 1;
                }
                if (va.state == DP_OFF) break;
            }
            printf("fast path == C path: config %u (%d fast, %d C-path blocks)\n", k, fast, slow);
        }
    }
    printf("OK: drum+ voice features\n");
    return 0;
}
