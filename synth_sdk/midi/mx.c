/* MIDI parameter CCs (docs/60): absolute CCs on the params channel set sound / FX / LFO / mixer values like turning
 * the knob (the page's range table, the slot's live words, the stock patch setter), so they are saved with the sound.
 *   12-15 / 16-18 synth engine 1-4 / page 2 (plugin engines: words 5-7)   20-23 synth envelope
 *   24-27 / 28-31 synth LFO 1-4 / Shift words 4-7                          40-43 / 44-46 synth FX 1-4 / page 2
 *   33-36 drum engine 1-4 of the target pad   37 target pad 0..23          48-51 / 52-54 drum FX 1-4 / page 2
 *   56-59 / 60-63 drum LFO 1-4 / Shift 4-7    78-81 drum envelope          70-73 / 74-76 master FX 1-4 / page 2
 *   102-105 track volume   106-109 pan   110-113 mute   114-117 aux FX send   118 tape input level
 * Sound targets: the CC (any thread) stores the value and queues key code MX_KEY; the KeyHandler thread
 * (key_event_hook -> mx_apply) writes the word and calls the setter, as the controllers' on_knob do:
 *   synth (slot 0xFF8016D4, patch 0x01DDD7E8 + s*0x38): engine 0xFF8013C4 + s*16, range 0x01DE5890 + engine*0x40 +
 *     k*8 {min, max, scale, mode}, set_knobs 0x01A00D14; envelope 0x01B60EF0 + s*16, range 0x01DE5710 + k*6, 0x01A00C98;
 *     FX 0xFF801444, type 0xFF801734[s], range 0x01DE5C50 + type*0x30 + k*6, set_fx_params 0x019FF664 (if 0x0196727C);
 *     LFO 0xFF8014C4, type 0xFF801384[s], range 0x01DE5740 + type*0x30 + k*6, set_lfo_params 0x019FF734 (if 0x0196726C)
 *   drum (slot 0xFF8016D8, patch 0x01DDD9A8 + d*0x38): pad words 0x01AAADB4 + d*0x180 + pad*16 (type 0xFF801704[d],
 *     range as the synth engine, type 9: 0x01DE5AD0 + k*8), setter 0x019FFC50; envelope 0xFF801544, range 0x01DE56C8,
 *     0x01A0005C; FX 0xFF8015C4 (type 0xFF801754, if 0x0196725C); LFO 0xFF801644 (type 0xFF8013A4, if 0x0196724C)
 *   master FX 0xFF8016C4 (type 0xFF801090, on [0x01AAAC40]), saved by the master controller 0x01AAD608's saveable
 *     (+8) through 0x0193EF3C, as its knobs do
 * Skipped: linked start / end knobs (synth range mode 2 / 3; drum pad knobs 2 / 3 of sample kits), LFO knobs whose range step is 0 (not used by the type), the
 * quad LFO (packed words), LFO type 5 knob 4 / type 6 knob 1 (coarse wrap), page 2 of stock engines / FX, targets
 * that are off. Mixer / tape values are written straight into the live words (any thread, docs/11 §9). */
#include "../op1_synth.h"
#include "mx.h"

static const struct { uint8_t g, n, cc, cc2; } grp[MX_NG] = {   /* knobs 0-3 at cc.., 4.. at cc2.. (0: cc + k) */
    { MX_SE, 7, 12, 16 }, { MX_SV, 4, 20, 0 }, { MX_SL, 8, 24, 0 }, { MX_SF, 7, 40, 44 }, { MX_DE, 4, 33, 0 },
    { MX_DP, 1, 37, 0 }, { MX_DF, 7, 48, 52 }, { MX_DL, 8, 56, 0 }, { MX_DV, 4, 78, 0 }, { MX_MF, 7, 70, 74 },
    { MX_VOL, 4, 102, 0 }, { MX_PAN, 4, 106, 0 }, { MX_MUTE, 4, 110, 0 }, { MX_AUX, 4, 114, 0 }, { MX_TAPE, 1, 118, 0 } };

int mx_group(int id, int *knob)
{
    for (int g = 0; g < MX_NG; id -= grp[g].n, g++)
        if (id < grp[g].n) { if (knob) *knob = id; return id < 0 ? -1 : g; }
    return -1;
}
int mx_cc_of(int id)
{
    int k, g = mx_group(id, &k);
    if (g < 0) return -1;
    return k < 4 || !grp[g].cc2 ? grp[g].cc + k : grp[g].cc2 + k - 4;
}
int mx_index(int cc)
{
    static int8_t map[128]; static uint8_t init;
    if (!init) {
        for (int i = 0; i < 128; i++) map[i] = -1;
        for (int id = 0; id < MX_N; id++) map[mx_cc_of(id)] = (int8_t)id;
        init = 1;
    }
    return cc >= 0 && cc < 128 ? map[cc] : -1;
}
int mx_scale(const int16_t *rg, int v)                     /* 0..127 over [min, max] */
{
    int lo = rg[0], hi = rg[1];
    if (hi <= lo) return lo;
    return lo + (hi - lo) * (v & 127) / 127;
}
int mx_unscale(const int16_t *rg, int w)
{
    int lo = rg[0], hi = rg[1];
    if (hi <= lo) return 0;
    int v = ((w - lo) * 127 + (hi - lo) / 2) / (hi - lo);
    return v < 0 ? 0 : v > 127 ? 127 : v;
}

#ifdef OP1_HOST
int mx_cc(int cc, int v) { (void)v; return mx_index(cc) >= 0; }
void mx_apply(void) {}
int mx_read(int id) { (void)id; return -1; }
#else
extern int mx_engine_plugin(int n), mx_fx_plugin(int n);  /* dispatcher.c: plugin with that order loaded */
extern uint8_t fr_send[4];
extern void fr_set_send(int t, int v);                     /* screens/fxroute.c */
static uint32_t irq_off(void) { uint32_t m; __asm__ volatile ("cli %0;" : "=d"(m)); return m; }
static void irq_on(uint32_t m) { __asm__ volatile ("sti %0;" : : "d"(m)); }

/* ---- mixer / tape: live words, any thread (docs/11 §9) ---- */
#define MIX_RANGE(i)  ((const volatile int16_t *)0xFF8018CC + (i) * 3)   /* vol 0..3, pan 4..7: {min, max, step} */
#define TAPE_RANGE    ((const volatile int16_t *)0x01DE56F8 + 3 * 3)
#define VOL(t)        ((volatile int16_t *)0xFF801928 + (t))
#define PAN(t)        ((volatile int16_t *)0xFF801930 + (t))
#define MUTE(t)       ((volatile uint8_t *)0xFF8010A0 + (t))
#define TAPE_IN       (*(volatile int16_t *)0x01B4EF6E)
static void mix_set(int g, int t, int v)
{
    if (g == MX_VOL) { const volatile int16_t *r = MIX_RANGE(t); if (r[1] > r[0]) *VOL(t) = (int16_t)(r[0] + (r[1] - r[0]) * v / 127); }
    else if (g == MX_PAN) {
        const volatile int16_t *r = MIX_RANGE(4 + t);
        int p = (v - 64) * 0x4000 / 63;                     /* stock pan range -0x4000..0x4000, 64 = centre */
        if (r[1] > r[0]) *PAN(t) = (int16_t)(p < r[0] ? r[0] : p > r[1] ? r[1] : p);
    } else if (g == MX_MUTE) { *MUTE(t) = v >= 64; ((void (*)(void))0x01967ED0)(); }   /* Core::update_mutes */
    else { const volatile int16_t *r = TAPE_RANGE; if (r[1] > r[0]) TAPE_IN = (int16_t)(r[0] + (r[1] - r[0]) * v / 127); }
}
static int mix_get(int g, int t)
{
    if (g == MX_VOL) { const volatile int16_t *r = MIX_RANGE(t); int16_t rg[2] = { r[0], r[1] }; return mx_unscale(rg, *VOL(t)); }
    if (g == MX_PAN) { int v = *PAN(t) * 63 / 0x4000 + 64; return v < 0 ? 0 : v > 127 ? 127 : v; }
    if (g == MX_MUTE) return *MUTE(t) ? 127 : 0;
    const volatile int16_t *r = TAPE_RANGE; int16_t rg[2] = { r[0], r[1] }; return mx_unscale(rg, TAPE_IN);
}

/* ---- sound targets ---- */
#define SSLOT       (*(volatile int *)0xFF8016D4 & 7)
#define DSLOT       (*(volatile int *)0xFF8016D8 & 7)
#define SPATCH(s)   ((void *)(0x01DDD7E8 + (s) * 0x38))
#define DPATCH(d)   ((void *)(0x01DDD9A8 + (d) * 0x38))
#define LAST_PAD    (*(volatile int *)0xFF801044)
typedef void (*setter)(void *, int16_t *);
static int8_t dpad = -1;                                   /* CC 37: target pad, -1 = the last played pad */

typedef struct { int16_t *w; int k; const int16_t *rg; setter set; void *patch; int page2, save_master; } tgt_t;

static int lfo_quad(const int16_t *w) { int m = (uint16_t)w[7] >> 8; return m >= 0xA5 && m <= 0xAF; }
static int pad_now(void) { int p = dpad >= 0 ? dpad : LAST_PAD; return p >= 0 && p < 24 ? p : 0; }

/* where target id lives now; 0 if it cannot be set / read at the moment */
static int locate(int id, tgt_t *t)
{
    int k, g = mx_group(id, &k), s = SSLOT, d = DSLOT, typ;
    t->k = k; t->page2 = 0; t->save_master = 0; t->rg = 0;
    switch (g) {
    case MX_SE: {
        t->w = (int16_t *)(0xFF8013C4 + s * 16); t->set = (setter)0x01A00D14; t->patch = SPATCH(s);
        if (k >= 4) { if (!mx_engine_plugin(t->w[4] - OP1_ENGINE_MAGIC)) return 0; t->k = k + 1; t->page2 = 2; return 1; }
        typ = ((volatile int *)0xFF8016E4)[s];
        if (typ < 0 || typ > 31) return 0;
        t->rg = (const int16_t *)(0x01DE5890 + typ * 0x40 + k * 8);
        return t->rg[3] == 0 || t->rg[3] == 1;               /* linked sample ranges (mode 2 / 3): stock only */
    }
    case MX_SV: t->w = (int16_t *)(0x01B60EF0 + s * 16); t->rg = (const int16_t *)(0x01DE5710 + k * 6);
                t->set = (setter)0x01A00C98; t->patch = SPATCH(s); return 1;
    case MX_DV: t->w = (int16_t *)(0xFF801544 + d * 16); t->rg = (const int16_t *)(0x01DE56C8 + k * 6);
                t->set = (setter)0x01A0005C; t->patch = DPATCH(d); return 1;
    case MX_SF: case MX_DF: case MX_MF: {
        int i = g == MX_SF ? s : d;
        if (g == MX_MF) { if (!*(volatile uint8_t *)0x01AAAC40) return 0; t->w = (int16_t *)0xFF8016C4; typ = *(volatile int *)0xFF801090;
                          t->set = 0; t->patch = 0; t->save_master = 1; }
        else {
            if (!((int (*)(int))(g == MX_SF ? 0x0196727C : 0x0196725C))(i)) return 0;   /* FX off */
            t->w = (int16_t *)((g == MX_SF ? 0xFF801444 : 0xFF8015C4) + i * 16);
            typ = ((volatile int *)(g == MX_SF ? 0xFF801734 : 0xFF801754))[i];
            t->set = (setter)0x019FF664; t->patch = g == MX_SF ? SPATCH(i) : DPATCH(i);
        }
        if (k >= 4) { if (!mx_fx_plugin(t->w[4] - OP1_ENGINE_MAGIC)) return 0; t->k = k + 1; t->page2 = 1; return 1; }
        if (typ < 0 || typ > 15) return 0;
        t->rg = (const int16_t *)(0x01DE5C50 + typ * 0x30 + k * 6);
        return 1;
    }
    case MX_SL: case MX_DL: {
        int i = g == MX_SL ? s : d;
        if (!((int (*)(int))(g == MX_SL ? 0x0196726C : 0x0196724C))(i)) return 0;       /* LFO off */
        t->w = (int16_t *)((g == MX_SL ? 0xFF8014C4 : 0xFF801644) + i * 16);
        typ = ((volatile int *)(g == MX_SL ? 0xFF801384 : 0xFF8013A4))[i];
        if (typ < 0 || typ > 15 || (typ == 4 && lfo_quad(t->w)) || (typ == 5 && k == 3) || (typ == 6 && k == 0)) return 0;
        t->rg = (const int16_t *)(0x01DE5740 + typ * 0x30 + k * 6);
        t->set = (setter)0x019FF734; t->patch = g == MX_SL ? SPATCH(i) : DPATCH(i);
        return t->rg[2] != 0;                                 /* step 0: the type does not use this knob */
    }
    case MX_DE: {
        typ = ((volatile int *)0xFF801704)[d];
        if (typ < 0 || typ > 31) return 0;
        t->w = (int16_t *)(0x01AAADB4 + d * 0x180 + pad_now() * 16);
        t->rg = (const int16_t *)(typ == 9 ? 0x01DE5AD0 + k * 8 : 0x01DE5890 + typ * 0x40 + k * 8);
        t->set = (setter)0x019FFC50; t->patch = DPATCH(d);
        if (typ != 9 && (k == 1 || k == 2)) return 0;         /* sample start / end: Ranges::knob_range_fx always links
                                                                 * knobs 1 / 2 (+ fine words 5 / 6), whatever the mode */
        return t->rg[3] == 0 || t->rg[3] == 1;
    }
    default: return 0;
    }
}

static void apply(int id, int v)
{
    int k, g = mx_group(id, &k);
    if (g == MX_DP) { dpad = (int8_t)(v % 24); return; }
    if (g == MX_AUX) { fr_set_send(k, v * 100 / 127); return; }
    tgt_t t;
    if (!locate(id, &t)) return;
    if (t.page2) t.w[t.k] = (int16_t)(t.page2 == 2 ? 1 + v * 32766 / 127 : v * 32767 / 127);   /* engine page words: 0 = unset */
    else { if (t.rg[1] <= t.rg[0]) return; t.w[t.k] = (int16_t)mx_scale(t.rg, v); }
    if (t.set) t.set(t.patch, t.w);
    if (t.save_master) ((void (*)(void *))0x0193EF3C)((void *)0x01AAD610);   /* master controller's saveable */
}

int mx_read(int id)
{
    int k, g = mx_group(id, &k);
    if (g < 0) return -1;
    if (g >= MX_VOL && g != MX_AUX) return mix_get(g, k);
    if (g == MX_AUX) return fr_send[k] * 127 / 100;
    if (g == MX_DP) return pad_now();
    tgt_t t;
    if (!locate(id, &t)) return -1;
    if (t.page2) { int w = t.w[t.k]; int v = t.page2 == 2 ? (w - 1) * 127 / 32766 : w * 127 / 32767; return v < 0 ? 0 : v > 127 ? 127 : v; }
    if (t.rg[1] <= t.rg[0]) return -1;
    return mx_unscale(t.rg, t.w[t.k]);
}

static volatile uint8_t val[MX_N];
static volatile uint32_t pend[(MX_N + 31) / 32];
static volatile uint8_t queued;

int mx_cc(int cc, int v)
{
    int id = mx_index(cc);
    if (id < 0) return 0;
    int k, g = mx_group(id, &k);
    if (g >= MX_VOL && g != MX_AUX) { mix_set(g, k, v & 127); return 1; }   /* live words: right here */
    uint32_t m = irq_off();
    val[id] = (uint8_t)(v & 127); pend[id >> 5] |= 1u << (id & 31);
    int push = !queued;
    if (push) {                                           /* one pending MX_KEY event at a time (as kb_push in ms_fw.c) */
        volatile uint32_t *rd = (volatile uint32_t *)0x01DD5244, *wr = (volatile uint32_t *)0x01DD5248;
        uint32_t w = *wr, n = (w + 1) & 0xFF;
        if (n != *rd && !*(volatile uint8_t *)0x01DD524D) { ((volatile uint16_t *)0x01DD5044)[w] = (uint16_t)(MX_KEY | MX_KEY_DOWN << 8); *wr = n; queued = 1; }
        else push = 0;
    }
    irq_on(m);
    if (push) ((void (*)(void))0x019C2F04)();             /* wake KeyHandler */
    return 1;
}

void mx_apply(void)                                       /* KeyHandler thread: MX_KEY from the queue */
{
    uint32_t p[(MX_N + 31) / 32]; uint8_t v[MX_N];
    uint32_t m = irq_off();
    for (int i = 0; i < (MX_N + 31) / 32; i++) { p[i] = pend[i]; pend[i] = 0; }
    for (int i = 0; i < MX_N; i++) v[i] = val[i];
    queued = 0;
    irq_on(m);
    for (int id = 0; id < MX_N; id++) if (p[id >> 5] >> (id & 31) & 1) apply(id, v[id]);
}
#endif
