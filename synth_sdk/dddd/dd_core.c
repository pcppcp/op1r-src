/* dddd core (docs/53), see dd_core.h. All state shared (paraphonic); per sample: 4 sources, the loop (up to 4 taps),
 * one SVF, fold / clip / crush, the stutter gate, DC blocker; per block: taps, filter coefficient, limiter gain. */
#include "dd_core.h"

static const uint32_t per_c[12] = { 2761712, 2606709, 2460405, 2322314, 2191972, 2068946, 1952825, 1843221,
                                    1739769, 1642124, 1549958, 1462966 };   /* samples per cycle at DD_SR (Q8), midi 0 .. 11 */
static const int16_t inv_n[DD_TAPS + 1] = { 0, 32767, 16384, 10923, 8192 };

static inline int32_t clampi(int32_t x, int32_t lo, int32_t hi) { return x < lo ? lo : x > hi ? hi : x; }
static inline uint32_t xr(dd_t *d) { uint32_t x = d->rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return d->rng = x; }

uint32_t dd_period(int midi)
{
    midi = clampi(midi, 0, 127);
    return per_c[midi % 12] >> (midi / 12);
}

void dd_init(dd_t *d, uint32_t seed)
{
    uint8_t *p = (uint8_t *)d;
    for (unsigned i = 0; i < sizeof *d; i++) p[i] = 0;
    d->rng = seed ? seed : 0x2545F491u;
    d->chaos_x = 12345; d->gain = 32767; d->gate_g = 32767;
    d->ntaps = 1; d->tap[0] = 337; d->inc = 12740000u;     /* C4 at DD_SR */
}

void dd_reset_loop(dd_t *d)
{
    for (int i = 0; i < DD_BUF; i++) d->buf[i] = 0;
    d->lp = d->bp = d->fbk = d->dcx = d->dcy = 0;
}

void dd_set_notes(dd_t *d, const uint8_t *notes, int n)
{
    if (n <= 0) return;
    if (n > DD_TAPS) { notes += n - DD_TAPS; n = DD_TAPS; }   /* the newest keys */
    for (int i = 0; i < n; i++) {
        uint32_t t = dd_period(notes[i]) >> 8;
        while (t >= DD_BUF / 2) t >>= 1;                       /* low notes: an octave up into the buffer */
        d->tap[i] = (int32_t)(t < 2 ? 2 : t);
    }
    d->ntaps = n;
    uint32_t per = dd_period(notes[n - 1]);                     /* oscillators: the newest key */
    d->inc = (uint32_t)(((uint64_t)1 << 40) / (per ? per : 1));
}

void dd_set_incs(dd_t *d, const uint32_t *inc, int n)
{
    if (n <= 0) return;
    if (n > DD_TAPS) { inc += n - DD_TAPS; n = DD_TAPS; }
    for (int i = 0; i < n; i++) {
        uint32_t t = inc[i] ? (uint32_t)(((uint64_t)1 << 32) / inc[i]) : DD_BUF / 2;   /* per key change (block rate) */
        while (t >= DD_BUF / 2) t >>= 1;
        d->tap[i] = (int32_t)(t < 2 ? 2 : t);
    }
    d->ntaps = n;
    d->inc = inc[n - 1];
}

void dd_render(dd_t *d, const dd_par *p, int16_t *out, int n)
{
    /* ---- per block ---- */
    const int32_t hands = clampi(p->hands, 0, 32767), destroy = clampi(p->destroy, 0, 32767);
    int32_t tap[DD_TAPS];
    int32_t spread = (clampi(p->spread, 0, 32767) * (d->h_spread >> 1)) >> 15;   /* -0.5 .. 0.5 of the loop, Q15 */
    for (int k = 0; k < d->ntaps; k++) tap[k] = clampi(d->tap[k] + ((d->tap[k] * spread) >> 15), 2, DD_BUF - 2);
    const int32_t inv = inv_n[d->ntaps];
    const int32_t fbg = (clampi(p->feedback, 0, 32767) * 5) >> 2;                  /* 0 .. 1.25: past 0.8 it screams */
    int32_t cut = 12000 + ((p->env * 20000) >> 15) + d->h_cut;   /* open enough at the attack: no muffled start */
    cut = clampi(cut, 300, 32767);
    /* SVF: f = 2 sin(pi fc / fs), damping q. The cutoff curve was made for 44.1 kHz (f0); at 88.2 kHz the same cutoffs
     * need f = 2 sin(asin(f0 / 2) / 2) = (f0 / 2) (1 + f0^2 / 32), within 0.3 % (as kennel 3.2) */
    const int32_t f0 = 300 + ((((cut * cut) >> 15) * 29000) >> 15), q = 20000;
    const int32_t f = (f0 >> 1) + (((f0 >> 1) * (((f0 * f0) >> 15) >> 5)) >> 15);
    const int32_t fm = clampi(p->filter, 0, 32767);
    const int32_t src = clampi(p->source + d->h_src, 0, 32767) * 3;              /* 0 .. 3 sources, Q15 */
    const int seg = src >> 15 > 2 ? 2 : src >> 15, sfrac = seg == 2 && src >= 3 * 32767 ? 32767 : src & 32767;
    const int32_t drive = 16384 + (clampi(p->vel, 0, 32767) >> 1);              /* velocity: 0.5 .. 1.0 into destroy */
    int32_t d1 = clampi(destroy * 3, 0, 32767), d2 = clampi(destroy * 3 - 32767, 0, 32767),
            d3 = clampi(destroy * 3 - 65534, 0, 32767);
    const int32_t g1 = 4096 + ((d1 * 3) >> 3), g2 = 4096 + ((d2 * 7) >> 3);     /* Q12: fold 1 .. 4, clip 1 .. 8 */
    const int32_t bits = 16 - ((d3 * 13) >> 15);                                  /* crush: 16 .. 3 bits */
    const int32_t mask = (int32_t)(0xFFFFFFFFu << (16 - bits));
    const int32_t hold_n = 1 + ((clampi(p->crush, 0, 32767) * 31) >> 15);          /* 1/N of 44.1 kHz */
    const int32_t hold_len = hold_n == 1 ? 1 : 2 * hold_n;                          /* in 88.2 kHz samples */
    const int32_t interval = 2 * DD_SR >> ((hands * 7) >> 15);                    /* the hands: 2 s .. 15 ms */
    const int stutter = hands > 26000;
    const uint32_t inc = d->inc;
    int32_t peak = 0, pre[256];                                 /* (n <= 256) the block before the limiter */

    for (int i = 0; i < n; i++) {
        uint32_t r = xr(d);
        /* the hands: jump the cutoff, the loop spread and the source mix */
        if (--d->hands_n <= 0) {
            d->hands_n = interval;
            d->jumps++;
            d->h_cut = (int32_t)(((int32_t)(r >> 16) - 32768) * hands >> 15);   /* +-hands */
            d->h_spread = (int32_t)((xr(d) >> 16) & 0xFFFF) - 32768;
            d->h_src = (int32_t)(((int32_t)((xr(d) >> 17) & 0x7FFF) - 16384) * hands >> 16);
        }
        if (stutter && --d->gate_n <= 0) {                     /* stutter: random cuts */
            uint32_t g = xr(d);
            d->gate_n = 400 + (int32_t)(g % 3600u);            /* (per cut, not per sample) */
            d->gate = (g >> 30) == 0;                          /* a quarter of the time: silent */
        }
        int32_t gt = stutter && d->gate ? 0 : 32767;
        d->gate_g += gt > d->gate_g ? 512 : gt < d->gate_g ? -512 : 0;
        d->gate_g = clampi(d->gate_g, 0, 32767);

        /* sources */
        int32_t s[4];
        s[0] = (int16_t)(r >> 16);                             /* white */
        if ((r & 0xFFF) < 20) d->crackle = (int16_t)(xr(d) >> 16);   /* crackle: sparse impulses (~430 / s) */
        d->crackle -= (d->crackle + (d->crackle > 0 ? 63 : 0)) >> 6;   /* rounded up so a tail reaches 0 */
        s[1] = d->crackle;
        uint32_t cph = d->chaos_ph + inc;
        int step = cph < d->chaos_ph;                          /* wrapped: a step at the key's pitch */
        d->chaos_ph = cph;
        if (step) {                                            /* the logistic map */
            int32_t x = d->chaos_x;
            x = (((x * (32768 - x)) >> 15) * (15400 + (int32_t)(r & 1023))) >> 12;   /* r = 3.76 .. 4.0 */
            d->chaos_x = clampi(x, 1, 32767);
        }
        s[2] = d->chaos_x * 2 - 32768;
        d->ph[1] += inc >> 1;                                  /* B modulates A; C is the metal partner */
        d->ph[0] += (int32_t)d->ph[1] < 0 ? inc + (inc >> 1) : inc - (inc >> 2);
        d->ph[2] += inc + (inc >> 1) - (inc >> 4);
        s[3] = (int32_t)(d->ph[0] ^ d->ph[2] ^ (d->ph[1] << 1)) < 0 ? 24000 : -24000;   /* ring of three squares */
        int32_t x = s[seg] + (((s[seg + 1] - s[seg]) * sfrac) >> 15);

        /* the loop: one tap per held key, clipped inside */
        int32_t acc = 0;
        for (int k = 0; k < d->ntaps; k++) acc += d->buf[(d->wp - (uint32_t)tap[k]) & (DD_BUF - 1)];
        acc = (acc * inv) >> 15;
        int32_t y = clampi(x + ((acc * fbg) >> 15), -32767, 32767);
        d->buf[d->wp & (DD_BUF - 1)] = (int16_t)y;
        d->wp++;

        /* filter: low-pass -> band-pass -> high-pass */
        int32_t hp = clampi(y - d->lp - ((q * d->bp) >> 15), -65535, 65535);   /* products stay below 2^31 */
        d->bp = clampi(d->bp + ((f * hp) >> 15), -65536, 65535);
        d->lp = clampi(d->lp + ((f * d->bp) >> 15), -65536, 65535);
        y = fm < 16384 ? d->lp + (((d->bp - d->lp) * fm) >> 14) : d->bp + (((hp - d->bp) * (fm - 16384)) >> 14);
        y = clampi(y, -32767, 32767);

        /* destroy: fold, clip, crush */
        y = (y * drive) >> 15;
        y = (y * g1) >> 12;                                    /* fold: two reflections at most (|y| < 4 x) */
        for (int k = 0; k < 3; k++) { if (y > 32767) y = 65534 - y; else if (y < -32767) y = -65534 - y; }
        y = clampi((y * g2) >> 12, -32767, 32767);
        y &= mask;
        if (--d->crush_n <= 0) { d->crush_n = hold_len; d->hold = y; }
        y = d->hold;

        /* stutter, DC blocker, limiter */
        y = (y * d->gate_g) >> 15;
        int32_t o = y - d->dcx + ((d->dcy * 32686) >> 15);    /* 0.9975 (35 Hz at DD_SR) */
        d->dcx = y; d->dcy = clampi(o, -65536, 65535);
        int32_t a = d->dcy < 0 ? -d->dcy : d->dcy;
        if (a > peak) peak = a;
        pre[i] = d->dcy;
    }
    /* limiter on this block (no lag: a new note starts at full level): the gain ramps over the block from the last
     * block's to the one that brings this peak to ~16000; one division per block */
    int32_t tgt = peak > 16000 ? (int32_t)(((int64_t)16000 * 32767) / peak) : 32767;
    int32_t g = d->gain, step = (tgt - g) / (n > 0 ? n : 1);
    int32_t x1 = d->dc2x, y1 = d->dc2y;
    for (int i = 0; i < n; i++) {                              /* + a second DC blocker after the gain (the clipped */
        g += step;                                             /* loop's rails are lopsided) */
        int32_t x = (pre[i] * g) >> 15;
        y1 = clampi(x - x1 + ((y1 * 32686) >> 15), -65536, 65535); x1 = x;
        out[i] = (int16_t)clampi(y1, -16384, 16384);           /* half scale */
    }
    d->dc2x = x1; d->dc2y = y1;
    d->gain = tgt;
}
