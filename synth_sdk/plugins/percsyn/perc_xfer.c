#pragma GCC optimize ("no-tree-loop-distribute-patterns") /* no implicit libc calls */
/* perc: PO-32 transfer reader. Record a PO-32 / Microtonic transfer into the slot's sample (input on a perc slot, or
 * load one with the sample browser); the main loop decodes it and its patches become the po-32 types (Shift + blue,
 * after the built-in sounds; perc_xbank.c) or the drum kit's keys (percdrum). A synth slot's sample holds 6 s (the
 * preamble and about 13 patch packets); a drum kit's 12 s holds a whole kit (16 instruments, both sides).
 *
 * The DPSK demodulator and the framing (whitening, CRC state, packet layout, final tail) follow libpo32
 * (MIT, Copyright (c) 2026 Eric Lewis; po32.c, po_wire_internal.h); this is an integer port.
 * GPL-3.0-or-later with the OP1R firmware permission. */
#include "op1_plugin.h"
#include "perc.h"
#include "perc_xfer.h"

/* ---- framing (libpo32's wire format) ---- */
static inline uint16_t crc_mix(uint16_t s, uint8_t x)
{
    uint16_t m = x;
    return (uint16_t)((((uint32_t)s << 8) | m) ^ ((uint32_t)m << 12) ^ ((uint32_t)m << 5));
}
static inline uint16_t crc_update(uint16_t s, uint8_t raw)
{
    uint8_t x = (uint8_t)(raw ^ (s >> 8));
    x ^= (uint8_t)(x >> 4);
    return crc_mix(s, x);
}
static inline uint8_t unwhiten(uint16_t s, uint8_t c) { return (uint8_t)(c - (s & 0xffu)); }

static int payload_ok(uint16_t tag, unsigned len)
{
    switch (tag) {
    case 0x37B2: return len == 43;                          /* patch */
    case 0xD022: return len == 211;                         /* pattern */
    case 0x505A: return len >= 35;                          /* state */
    case 0xA354: return len == 2;                           /* knob */
    case 0x4AB4: case 0xB892: case 0x496F: return len == 1; /* reset, erase, pattern slot */
    }
    return 0;
}

#define SYNC_LO 0x55555555u                                 /* the preamble's last 8 bytes, LSB first */
#define SYNC_HI 0xCF9D1914u

static void desync(perc_xfer_t *x) { x->synced = 0; x->work_len = 0; }

static void on_byte(perc_xfer_t *x, uint8_t b)
{
    if (x->work_len >= sizeof(x->work)) { desync(x); return; }
    x->work[x->work_len++] = b;
    if (unwhiten(x->crc, x->work[0]) == 0xC3) {             /* final tail: the transfer is complete */
        if (x->work_len >= 5) { x->done = 1; desync(x); }
        return;
    }
    if (x->work_len < 3) return;
    uint16_t s = x->crc;
    uint8_t lo = unwhiten(s, x->work[0]); s = crc_update(s, lo);
    uint8_t hi = unwhiten(s, x->work[1]); s = crc_update(s, hi);
    uint8_t n = unwhiten(s, x->work[2]);
    uint16_t tag = (uint16_t)(lo | (hi << 8));
    if (!payload_ok(tag, n)) { desync(x); return; }
    unsigned total = 5u + n;
    if (x->work_len < total) return;
    /* the whole packet: header, payload and the state trailer */
    s = x->crc;
    uint8_t raw[260];
    for (unsigned i = 0; i < 3u + n; i++) { raw[i] = unwhiten(s, x->work[i]); s = crc_update(s, raw[i]); }
    uint16_t trailer = s;
    uint8_t t0 = unwhiten(s, x->work[3 + n]); s = crc_update(s, t0);
    uint8_t t1 = unwhiten(s, x->work[4 + n]); s = crc_update(s, t1);
    if (t0 != (uint8_t)trailer || t1 != (uint8_t)(trailer >> 8)) { desync(x); return; }
    x->crc = s; x->work_len = 0; x->packets++;
    if (tag == 0x37B2) {                                    /* patch: command 0x1N left / 0x2N right, 21 x u16 LE */
        const uint8_t *p = raw + 3;
        unsigned side = (unsigned)(p[0] >> 4), inst = p[0] & 15u;
        if (side == 1 || side == 2) {
            uint16_t *dst = x->patch[inst][side - 1];
            for (int i = 0; i < 21; i++) dst[i] = (uint16_t)(p[1 + 2 * i] | (p[2 + 2 * i] << 8));
            x->have[inst] |= (uint8_t)side;
            /* list_right: 0 side a only, 1 both, 2 side b only so far (it came first) */
            int e = -1;
            for (int i = x->list_n - 1; i >= 0 && e < 0; i--)
                if (x->list_inst[i] == inst && x->list_ses[i] == x->session && x->list_right[i] == (side == 1 ? 2 : 0)) e = i;
            if (e >= 0) x->list_right[e] = 1;
            else if (x->list_n < PERC_XFER_LIST) {
                e = x->list_n++;
                x->list_inst[e] = (uint8_t)inst; x->list_ses[e] = x->session; x->list_right[e] = side == 1 ? 0 : 2;
                if (side == 1) for (int i = 0; i < 21; i++) x->list[e][1][i] = dst[i];   /* until side b comes: b = a */
            }
            if (e >= 0) for (int i = 0; i < 21; i++) x->list[e][side - 1][i] = dst[i];
        }
    }
}

/* ---- DPSK demodulator: 1201.92 baud, carrier 3 cycles per symbol, 44.1 kHz ---- */
#define SYM_INC  117057150u                                  /* 1201.923 / 44100 x 2^32 */
#define CAR_INC  351171450u                                  /* 3 x SYM_INC */

void perc_xfer_begin(perc_xfer_t *x)
{
    unsigned char *p = (unsigned char *)x;
    for (unsigned i = 0; i < sizeof(*x); i++) p[i] = 0;
    x->sym = ((int64_t)1 << 32) - SYM_INC;                  /* the first sample ends a symbol, as libpo32 */
}

static void boundary(perc_xfer_t *x)
{
    if (x->started) {
        int64_t dot = (int64_t)x->ai * x->pi + (int64_t)x->aq * x->pq;
        uint32_t bit = dot >= 0;
        if ((x->pi > 0) != (x->ai > 0)) {                    /* timing recovery on an I sign change, gain 0.01 */
            int64_t a = x->ai < 0 ? -(int64_t)x->ai : x->ai, b = x->pi < 0 ? -(int64_t)x->pi : x->pi;
            if (a + b > 0) x->sym += ((a - b) * ((int64_t)1 << 32) / (a + b)) / 100;
        }
        if (!x->synced) {
            x->win_lo = (x->win_lo >> 1) | (x->win_hi << 31);
            x->win_hi = (x->win_hi >> 1) | (bit << 31);
            if (x->win_lo == SYNC_LO && x->win_hi == SYNC_HI) {   /* re-syncs for a further transfer in one recording */
                x->synced = 1; x->session++; x->crc = 0x1D0F; x->cur = 0; x->bits = 0; x->work_len = 0;
            }
        } else {
            x->cur |= (uint8_t)(bit << x->bits);
            if (++x->bits == 8) { uint8_t b = x->cur; x->cur = 0; x->bits = 0; on_byte(x, b); }
        }
    } else x->started = 1;
    x->pi = x->ai; x->pq = x->aq; x->ai = 0; x->aq = 0;
}

void perc_xfer_push(perc_xfer_t *x, const int16_t *s, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {                           /* the whole recording: several transfers accumulate */
        x->car += CAR_INC;
        int32_t sn = OP1_SINE_Q[(x->car >> 19) & 8191], cs = OP1_SINE_Q[((x->car >> 19) + 2048) & 8191];
        x->ai += (s[i] * sn) >> 8;
        x->aq += (s[i] * cs) >> 8;
        x->sym += SYM_INC;
        if (x->sym >= ((int64_t)1 << 32)) { x->sym -= (int64_t)1 << 32; boundary(x); }
    }
}
