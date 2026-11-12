#pragma GCC optimize ("no-tree-loop-distribute-patterns") /* no implicit libc calls */
/* perc drum kit (plugin.json "kind": "drum", docs/43): a drum type next to drum, dbox and drum+. A kit is 24 slots, one
 * per key; a slot holds one whole PO-32 patch (side a, side b and the morph between them). The pages show and edit that
 * patch itself (no offsets on top of it): what a knob reads is what the sound has.
 *
 *   slots     a fresh kit is the 24 library presets (PERC_KIT, gen_tables.py); a slot remembers where its sound came
 *             from (a preset, or a transfer), so it can go back to it
 *   transfer  a PO-32 / Microtonic transfer recorded into the kit's sample (12 s) is decoded on the page; its new sounds
 *             are previewed on the keys from the selected one forward and written on a press (one undo)
 *   pages     T1 tone (oscillator, bend), T2 noise (filter, envelope), T4 mix (mix, drive, EQ, level, morph); turn, Shift
 *             + turn, and encoder presses for the modes, as drum+. T3 is the stock FX page.
 *   save      "op1r": {"engine": "perc", "b": "<base64>"}: packed bytes, a preset slot is 4 bytes, an edited or
 *             transferred one adds its 21 (+ 21) words. About 2.8 KB at worst (a big patch froze R246 on save).
 *
 * Voices: the 12 drum voices of the DrumSampler; the stock voice fields (+8: pos, state, blocks) are kept as the stock
 * note_on / note_off / render do, so the firmware's voice allocation works as for a sampled kit. At most PD_SYNTH voices
 * synthesise at once (the oldest fades out). GPL-3.0-or-later with the OP1R firmware permission. */
#include "../../op1_synth.h"
#include "../percsyn/perc_dsp.h"
#include "../percsyn/perc_tables.h"
#include "../percsyn/perc_xfer.h"

#define PD_SLOTS  8
#define PD_PADS   24
#define PD_VOICES 12
#define PD_SYNTH  6                                           /* voices synthesising at once (CPU) */
#define PD_POOL   PERC_XFER_LIST                              /* sounds kept from the kit's recording */
#define PD_TONIC  255                                         /* origin: a transferred sound */

#ifndef OP1_HOST
#define VOICE_OBJ  ((void *const *)0x01AAAD30)                /* the 12 voice objects */
#define VOICE_PAD  ((const int32_t *)0xFF801334)              /* pad of voice v */
#define VOICE_VEL  ((const int16_t *)0xFF8012EC)              /* velocity << 8 of voice v */
#define DRUM_SLOT  (*(const volatile int32_t *)0x01AAAC10)    /* the playing drum slot */
#define SCREEN_TABLE    ((void **)0x01DF4E54)
#define CURRENT_SCREEN  (*(volatile int *)0x018B65C8)
#define DRUM_MODE       (*(volatile int *)0x01DE4C40 == 1)
#define DRUM_SLOT_CUR   (*(volatile int *)0xFF8016D8 & 7)
#define LAST_PAD        (*(volatile int *)0xFF801044)
#define SHIFT_HELD      (*(volatile uint8_t *)0x01DD7F78)
#define KEY_DISPATCH    ((void (*)(int, int))0x019B6C74)      /* the stock key dispatch (dispatcher.c) */
#define MODE_ID         (*(volatile int *)0xFF907D58)         /* ModeSwitcher: 0 synth, 1 drum, 2 tape, 3 mixer */
#define DRUM_TO_PATCH   ((void (*)(void *))0x019FFC50)
#define DRUM_PATCH(s)   ((void *)(0x01DDD9A8 + (s) * 0x38))
#define DRUM_SAMPLER(s) (((uint32_t **)0x01CC10CC)[s])        /* DrumSampler of slot s: [0] = SampleBuffer */
#define TYPE_JOB    ((void (*)(void *))0x0193F86C)
#define SAVE_SLOT   ((void (*)(void *))0x01940574)
#define UI_SLOT     ((void (*)(int))0x019C2BA0)
#define UI_SIGNAL   ((void (*)(void))0x019C2F04)
#define JV_KEY      ((void *(*)(void *, const char *))0x019BFEB4)
#define JV_ASSIGN   ((void *(*)(void *, const void *))0x019BDDC0)
#define JV_CGET     ((const void *(*)(const void *, const char *))0x019BEB70)
#define JV_FROM_STR ((void *(*)(void *, const char *))0x019BDC4C)
#define JV_DTOR     ((void (*)(void *, int))0x019BDD18)
#define JV_REMOVE   ((void (*)(void *, void *, const char *))0x019BEE88)
void dpu_sync_auto(void);                                     /* core (dispatcher.c): pick the pages again */
#endif
enum { JV_STRING = 4, JV_OBJECT = 7 };
typedef struct { int32_t pos, state; uint32_t blocks; } stock_voice;   /* +8 of a voice object */
enum { ST_OFF = 0, ST_HELD = 1, ST_RELEASED = 2 };

/* ---- the kit: 24 slots ---- */
enum { PK_A = 1, PK_B = 2, PK_EDITED = 4 };                   /* saved flags: side a / b stored; changed since loaded */
typedef struct { uint16_t a[21], b[21]; int16_t morph; uint8_t origin, flags; } pd_key;
static pd_key kit[PD_SLOTS][PD_PADS];
static uint32_t kit_sig[PD_SLOTS];                            /* the recording whose sounds this kit has seen */
static uint8_t kit_old[PD_SLOTS];                             /* a v1 kit: saved without that signature */
static volatile uint8_t claimed[PD_SLOTS];

static void key_preset(pd_key *k, int i)
{
    if ((unsigned)i >= PERC_KIT_COUNT) i = 0;
    for (int j = 0; j < 21; j++) { k->a[j] = PERC_KIT[i][j]; k->b[j] = PERC_KIT_R[i][j]; }
    k->morph = 0; k->origin = (uint8_t)i; k->flags = 0;
}
static void key_default(pd_key *k, int pad) { key_preset(k, pad); }
static int16_t clamp15(int32_t v) { return (int16_t)(v < 0 ? 0 : v > 32767 ? 32767 : v); }
static uint16_t clampw(int32_t x) { return (uint16_t)(x < 0 ? 0 : x > 65535 ? 65535 : x); }
/* the side the knobs show and edit: the one the morph is nearer to */
static int key_side(const pd_key *k) { return k->morph >= 16384; }
/* the patch a hit plays: a + (b - a) x morph, per word (libpo32's morph model; modes switch at their thresholds).
 * morph 0 plays side a as is. */
static const uint16_t *key_patch(const pd_key *k, uint16_t *tmp)
{
    int32_t m = k->morph; if (m <= 0) return k->a;
    m += m >> 14;                                             /* 32767 -> 32768: side b exactly */
    for (int i = 0; i < 21; i++) tmp[i] = (uint16_t)(k->a[i] + (((int32_t)k->b[i] - k->a[i]) * m >> 15));
    return tmp;
}

/* ---- the kit's recording: its sounds in the order they arrived, and the ones waiting for a place ---- */
static uint16_t pool[PD_SLOTS][PD_POOL][2][21];              /* [a, b]; b = a when not sent */
static uint32_t pool_h[PD_SLOTS][PD_POOL];
static uint8_t pool_n[PD_SLOTS], pool_seen[PD_SLOTS];        /* seen: decoded since the kit was loaded */
static struct { int8_t slot; uint8_t n, anchor, idx[PD_POOL]; } pend = { -1, 0, 0, { 0 } };

static uint32_t patch_hash(const uint16_t p[2][21])
{
    uint32_t h = 2166136261u;
    for (int s = 0; s < 2; s++) for (int i = 0; i < 21; i++) { h ^= p[s][i]; h *= 16777619u; }
    return h ? h : 1;
}
/* a finished decode of the slot's sample (signature sig): its sounds become the pool; the ones the pool did not have
 * wait for a place from `anchor` on. The first decode of a recording the saved kit already knows is quiet (a boot, a kit
 * load). Returns 1 if the kit's saved state changed (its signature). */
static int pool_take(int slot, const perc_xfer_t *x, uint32_t sig, int anchor)
{
    int quiet = !pool_seen[slot] && (sig == kit_sig[slot] || kit_old[slot]), changed = sig != kit_sig[slot];
    pool_seen[slot] = 1; kit_old[slot] = 0; kit_sig[slot] = sig;
    uint32_t oldh[PD_POOL]; int oldn = pool_n[slot], n = 0, fresh = 0;
    for (int i = 0; i < oldn; i++) oldh[i] = pool_h[slot][i];
    if (pend.slot == slot) pend.slot = -1;
    for (int e = 0; e < x->list_n && n < PD_POOL; e++) {
        if (x->list_right[e] == 2) continue;                  /* its side a never came */
        uint32_t h = patch_hash(x->list[e]);
        int dup = 0, known = 0;
        for (int i = 0; i < n; i++) dup |= pool_h[slot][i] == h;   /* (the first n are already this decode's) */
        if (dup) continue;
        for (int i = 0; i < oldn; i++) known |= oldh[i] == h;
        for (int s = 0; s < 2; s++) for (int j = 0; j < 21; j++) pool[slot][n][s][j] = x->list[e][s][j];
        pool_h[slot][n] = h;
        if (!known && !quiet) pend.idx[fresh++] = (uint8_t)n;
        n++;
    }
    pool_n[slot] = (uint8_t)n;
    if (fresh) { pend.n = (uint8_t)fresh; pend.anchor = (uint8_t)((unsigned)anchor < PD_PADS ? anchor : 0); pend.slot = (int8_t)slot; }
    return changed;
}
static int pend_fit(void) { int room = PD_PADS - pend.anchor; return pend.n < room ? pend.n : room; }
static void key_tonic(pd_key *k, int slot, int i)
{
    for (int j = 0; j < 21; j++) { k->a[j] = pool[slot][i][0][j]; k->b[j] = pool[slot][i][1][j]; }
    k->morph = 0; k->origin = PD_TONIC; k->flags = 0;
}
/* what a hit on the pad plays now: its slot, or the waiting sound previewed there */
static const uint16_t *hit_patch(int slot, int pad, uint16_t *tmp)
{
    if (pend.slot == slot && pad >= pend.anchor && pad < pend.anchor + pend_fit()) return pool[slot][pend.idx[pad - pend.anchor]][0];
    return key_patch(&kit[slot][pad], tmp);
}

/* one undo: the kit as it was before the last place / home / browse (Shift + red press swaps back and forth) */
static pd_key undo_kit[PD_PADS];
static int8_t undo_slot = -1;
static void undo_save(int slot) { for (int i = 0; i < PD_PADS; i++) undo_kit[i] = kit[slot][i]; undo_slot = (int8_t)slot; }
static int undo_swap(int slot)
{
    if (undo_slot != slot) return 0;
    for (int i = 0; i < PD_PADS; i++) { pd_key t = kit[slot][i]; kit[slot][i] = undo_kit[i]; undo_kit[i] = t; }
    return 1;
}

/* ---- voices (audio interrupt) ---- */
static perc_voice_t pv[PD_VOICES];
static volatile uint8_t pv_on[PD_VOICES], pv_fade[PD_VOICES], pv_pending[PD_VOICES];
static uint8_t pv_slot[PD_VOICES], pv_pad[PD_VOICES], pv_vel[PD_VOICES];
static uint32_t pv_t[PD_VOICES], pv_clock;
volatile uint32_t pd_hit_seq; volatile int32_t pd_hit_vel;

#ifndef OP1_HOST
static int voice_index(void *self) { for (int i = 0; i < PD_VOICES; i++) if (VOICE_OBJ[i] == self) return i; return -1; }
static stock_voice *model(void *self) { return (stock_voice *)((uint8_t *)self + 8); }

static void pd_note_on(void *self, const void *pad)
{
    (void)pad;
    int v = voice_index(self), slot = DRUM_SLOT & 7;
    stock_voice *m = model(self);
    m->state = ST_HELD; m->blocks = 0; m->pos = 0;
    if (v < 0) return;
    int p = VOICE_PAD[v]; if ((unsigned)p >= PD_PADS) p = 0;
    int vel = VOICE_VEL[v] >> 8;
    pv_slot[v] = (uint8_t)slot; pv_pad[v] = (uint8_t)p; pv_vel[v] = (uint8_t)vel;
    pv_fade[v] = 0; pv_t[v] = ++pv_clock;
    pv_pending[v] = 1;                                        /* set up in render: one context builds the voice */
    pv_on[v] = 1;                                             /* (after pending: render never sees on without it) */
    int n = 0, oldest = -1;                                   /* at most PD_SYNTH: the oldest other fades out */
    for (int i = 0; i < PD_VOICES; i++) if (pv_on[i] && !pv_fade[i]) { n++; if (i != v && (oldest < 0 || pv_t[i] < pv_t[oldest])) oldest = i; }
    if (n > PD_SYNTH && oldest >= 0) pv_fade[oldest] = 1;
    pd_hit_vel = vel * 258; pd_hit_seq++;
}
static void pd_note_off(void *self, const void *pad) { (void)pad; stock_voice *m = model(self); if (m->state == ST_HELD) m->state = ST_RELEASED; }

static void pd_render(void *self, int16_t *out, const void *pad)
{
    (void)pad;
    int v = voice_index(self), j;
    stock_voice *m = model(self);
    m->blocks++;
    if (v < 0 || !pv_on[v]) { for (j = 0; j < 128; j++) out[j] = 0; m->state = ST_OFF; return; }
    if (pv_pending[v]) {
        pv_pending[v] = 0;
        static const perc_macro_t none;                       /* the patch as it is: nothing on top */
        static uint16_t mp[21];                               /* audio context only */
        perc_prepare(&pv[v].p, hit_patch(pv_slot[v], pv_pad[v], mp), &none, pv_vel[v]);
        perc_trigger(&pv[v]);
        pv[v].prev = 0;
    }
    int16_t half[64];
    int alive = perc_render(&pv[v], half, 64, 0);              /* pitch: the key's own (the stock pitch word is not used) */
    int32_t prev = pv[v].prev;
    for (j = 0; j < 64; j++) { int32_t c = half[j]; out[2 * j] = (int16_t)((prev + c) >> 1); out[2 * j + 1] = (int16_t)c; prev = c; }
    pv[v].prev = prev;
    if (pv_fade[v]) {                                          /* stolen: one block down to silence */
        for (j = 0; j < 128; j++) out[j] = (int16_t)(out[j] * (127 - j) >> 7);
        alive = 0;
    }
    if (!alive) { pv_on[v] = 0; pv_fade[v] = 0; m->state = ST_OFF; }
}
#endif

/* ---- persistence: packed bytes, base64 in op1r.b (one JSON string: small and quick to serialize) ----
 * v2: version(2), signature(u32 LE), then per key: origin(u8), flags(u8), morph(u16 LE), side a (21 u16 LE) if PK_A,
 * side b if PK_B. A side that is not stored is the origin preset's (a transferred sound: b = a).
 * v1 (before the pages edited the patch): sound(u8), flags(u8), 7 knob words, the patch for a transferred sound; read
 * with its knob offsets worked into the patch words. */
#define PD_VER 2
#define PD_RAW_MAX (5 + PD_PADS * (4 + 84))
static uint8_t pd_raw[PD_RAW_MAX];
static char pd_b64[((PD_RAW_MAX + 2) / 3) * 4 + 1];

static uint8_t *put_u16(uint8_t *p, int v) { *p++ = (uint8_t)v; *p++ = (uint8_t)(v >> 8); return p; }
static int get_u16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static int words_differ(const uint16_t *x, const uint16_t *y) { for (int j = 0; j < 21; j++) if (x[j] != y[j]) return 1; return 0; }

static unsigned pack_kit(int slot)
{
    uint8_t *p = pd_raw;
    *p++ = PD_VER;
    p = put_u16(p, (int)(kit_sig[slot] & 0xffff)); p = put_u16(p, (int)(kit_sig[slot] >> 16));
    for (int i = 0; i < PD_PADS; i++) {
        const pd_key *k = &kit[slot][i];
        int preset = k->origin < PERC_KIT_COUNT, f = k->flags & PK_EDITED;
        if (!preset || words_differ(k->a, PERC_KIT[k->origin])) f |= PK_A;
        if (words_differ(k->b, preset ? PERC_KIT_R[k->origin] : k->a)) f |= PK_B;
        if (preset && !(f & (PK_A | PK_B))) f = 0;            /* the preset as it is */
        *p++ = preset ? k->origin : PD_TONIC; *p++ = (uint8_t)f;
        p = put_u16(p, k->morph);
        if (f & PK_A) for (int j = 0; j < 21; j++) p = put_u16(p, k->a[j]);
        if (f & PK_B) for (int j = 0; j < 21; j++) p = put_u16(p, k->b[j]);
    }
    return (unsigned)(p - pd_raw);
}
static void add_word(uint16_t *w, int32_t d) { *w = clampw((int32_t)*w + d); }
/* a v1 key's knob offsets (pitch, length, punch, tone: centre 16384; drive from 0; level: centre 16384) into the
 * patch: exact for pitch, length, tone and drive, close for level, and the bend depth only for punch */
static void bake_v1(uint16_t *w, const int32_t *kn)
{
    int32_t dp = kn[0] - 16384, dl = kn[1] - 16384, du = kn[2] - 16384, dt = kn[3] - 16384, dv = kn[5] - 16384;
    add_word(&w[P_OSCFREQ], dp * 52608 / 65536);                              /* 8 octaves over the knob, 6576 words each */
    add_word(&w[P_OSCDCY], dl * 87137 / 65536); add_word(&w[P_NENVDCY], dl * 87137 / 65536);
    add_word(&w[P_NFILFRQ], dt * 78913 / 65536); add_word(&w[P_EQFREQ], dt * 78913 / 65536);
    add_word(&w[P_DIST], kn[4] * 2);
    add_word(&w[P_LEVEL], dv * 51150 / 65536);
    if (du) w[P_MODAMT] = clampw(32768 + (int32_t)(((int64_t)((int32_t)w[P_MODAMT] - 32768) * (int32_t)perc_pow2(du * 4, 16)) >> 16));
}
static const uint8_t v1_home[PERC_FACTORY_COUNT] = { 0, 2, 5, 4, 14, 8, 10, 3, 11, 12 };   /* a v1 built-in's preset */
static void unpack_v1(int slot, const uint8_t *p, const uint8_t *end)
{
    for (int i = 0; i < PD_PADS; i++) {
        if (p + 16 > end) break;
        pd_key *k = &kit[slot][i];
        int sound = *p++, flags = *p++, edited = 0;
        int32_t kn[7];
        for (int j = 0; j < 7; j++) { kn[j] = clamp15(get_u16(p)); p += 2; }
        if (sound >= PERC_FACTORY_COUNT) {
            if (p + 42 > end) break;
            for (int j = 0; j < 21; j++) { k->a[j] = k->b[j] = (uint16_t)get_u16(p); p += 2; }
            if (flags & 1) { if (p + 42 > end) break; for (int j = 0; j < 21; j++) { k->b[j] = (uint16_t)get_u16(p); p += 2; } }
            k->origin = PD_TONIC;
        } else {
            for (int j = 0; j < 21; j++) { k->a[j] = PERC_FACTORY[sound][j]; k->b[j] = PERC_FACTORY_R[sound][j]; }
            k->origin = v1_home[sound];
        }
        for (int j = 0; j < 6; j++) edited |= kn[j] != (j == 4 ? 0 : 16384);
        if (edited) { bake_v1(k->a, kn); bake_v1(k->b, kn); }
        k->morph = (int16_t)kn[6]; k->flags = edited ? PK_EDITED : 0;
    }
}
static void unpack_kit(int slot, const uint8_t *o, unsigned len)
{
    if (len < 1) return;
    const uint8_t *p = o + 1, *end = o + len;
    if (o[0] == 1) { unpack_v1(slot, p, end); kit_old[slot] = 1; return; }
    if (o[0] != PD_VER || len < 5) return;
    kit_sig[slot] = (uint32_t)get_u16(p) | ((uint32_t)get_u16(p + 2) << 16); p += 4;
    for (int i = 0; i < PD_PADS; i++) {
        if (p + 4 > end) break;
        pd_key k;
        int origin = *p++, f = *p++;
        if (origin < PERC_KIT_COUNT) key_preset(&k, origin); else { key_preset(&k, i); k.origin = PD_TONIC; }
        k.morph = clamp15(get_u16(p)); p += 2;
        if (f & PK_A) { if (p + 42 > end) break; for (int j = 0; j < 21; j++) { k.a[j] = (uint16_t)get_u16(p); p += 2; } }
        if (k.origin == PD_TONIC) for (int j = 0; j < 21; j++) k.b[j] = k.a[j];
        if (f & PK_B) { if (p + 42 > end) break; for (int j = 0; j < 21; j++) { k.b[j] = (uint16_t)get_u16(p); p += 2; } }
        k.flags = (uint8_t)(f & PK_EDITED);
        kit[slot][i] = k;
    }
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void b64_enc(const uint8_t *in, unsigned n)
{
    unsigned o = 0;
    for (unsigned i = 0; i < n; i += 3) {
        unsigned b0 = in[i], b1 = i + 1 < n ? in[i + 1] : 0, b2 = i + 2 < n ? in[i + 2] : 0;
        pd_b64[o++] = B64[b0 >> 2];
        pd_b64[o++] = B64[((b0 & 3) << 4) | (b1 >> 4)];
        pd_b64[o++] = i + 1 < n ? B64[((b1 & 15) << 2) | (b2 >> 6)] : '=';
        pd_b64[o++] = i + 2 < n ? B64[b2 & 63] : '=';
    }
    pd_b64[o] = 0;
}
static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    return c == '+' ? 62 : c == '/' ? 63 : -1;
}
static unsigned b64_dec(const char *in, uint8_t *out, unsigned cap)
{
    unsigned o = 0; int q[4], qn = 0;
    for (const char *t = in; *t; t++) {
        int v = b64_val(*t);
        if (v < 0) continue;
        q[qn++] = v;
        if (qn == 4) {
            if (o < cap) out[o++] = (uint8_t)((q[0] << 2) | (q[1] >> 4));
            if (o < cap) out[o++] = (uint8_t)((q[1] << 4) | (q[2] >> 2));
            if (o < cap) out[o++] = (uint8_t)((q[2] << 6) | q[3]);
            qn = 0;
        }
    }
    if (qn >= 2 && o < cap) out[o++] = (uint8_t)((q[0] << 2) | (q[1] >> 4));
    if (qn >= 3 && o < cap) out[o++] = (uint8_t)((q[1] << 4) | (q[2] >> 2));
    return o;
}
/* a kit from its saved string (0: a fresh kit) */
static void kit_load(int slot, const char *b64)
{
    for (int p = 0; p < PD_PADS; p++) key_default(&kit[slot][p], p);
    kit_sig[slot] = 0; kit_old[slot] = 0; pool_seen[slot] = 0;
    if (pend.slot == slot) pend.slot = -1;
    if (undo_slot == slot) undo_slot = -1;
    if (b64) { unsigned n = b64_dec(b64, pd_raw, PD_RAW_MAX); unpack_kit(slot, pd_raw, n); }
}

/* ================= the pages (logic and drawing; the host tests build this part too) ================= */
enum { PG_TONE, PG_NOISE, PG_MIX, PG_N };
enum { L_NONE = -1, L_SOUND = 30, L_MORPH = 31 };            /* a lane: a patch word (0..20), or one of these */
static const int8_t ui_lane[PG_N][2][4] = {
    { { P_OSCFREQ, P_OSCATK, P_OSCDCY, P_MODAMT }, { L_SOUND, P_OSCVEL, P_MODVEL, P_MODRATE } },
    { { P_NFILFRQ, P_NENVATK, P_NENVDCY, P_NFILQ }, { L_NONE, P_NVEL, L_NONE, L_NONE } },
    { { P_MIX, P_DIST, P_EQFREQ, P_EQGAIN }, { L_MORPH, L_NONE, L_NONE, P_LEVEL } },
};
static const char *const ui_label[PG_N][2][4] = {
    { { "pitch", "attack", "decay", "bend" }, { "sound", "velocity", "bend vel", "bend rate" } },
    { { "filter", "attack", "decay", "reso" }, { "", "velocity", "", "" } },
    { { "mix", "drive", "eq", "eq gain" }, { "morph", "", "", "level" } },
};
static const int8_t ui_mode[PG_N][4] = {                      /* the mode an encoder press steps */
    { P_OSCWAVE, L_NONE, L_NONE, P_MODMODE }, { P_NFILMOD, L_NONE, P_NENVMOD, L_NONE }, { L_NONE, L_NONE, L_NONE, L_NONE },
};
static const char *const ui_mode_name[21][3] = {
    [P_OSCWAVE] = { "sine", "tri", "saw" }, [P_MODMODE] = { "decay", "sine", "noise" },
    [P_NFILMOD] = { "lp", "bp", "hp" }, [P_NENVMOD] = { "exp", "lin", "mod" },
};
static int w_mode(uint16_t w) { return w < 21845 ? 0 : w < 43691 ? 1 : 2; }

/* the sound browser (Shift + blue): "mine" (the slot as it was when the browsing began), the presets, the kit's
 * transferred sounds; turning loads, coming back to "mine" restores */
static struct { int8_t slot, pad; uint8_t valid; int16_t pos; pd_key stash; } br = { -1, -1, 0, 0, { { 0 }, { 0 }, 0, 0, 0 } };
static uint8_t note_kind, note_from, note_n; static uint16_t note_age;   /* the last action, shown for a while */
enum { NOTE_PLACED = 1, NOTE_UNDO, NOTE_HOME, NOTE_SKIP };
static void note(int kind, int from, int n) { note_kind = (uint8_t)kind; note_from = (uint8_t)from; note_n = (uint8_t)n; note_age = 0; }

static void browse(int slot, int pad, int st)
{
    pd_key *k = &kit[slot][pad];
    if (!(br.valid && br.slot == slot && br.pad == pad)) {
        undo_save(slot);
        br.stash = *k; br.slot = (int8_t)slot; br.pad = (int8_t)pad; br.valid = 1;
        br.pos = (int16_t)(k->origin < PERC_KIT_COUNT && !(k->flags & PK_EDITED) ? k->origin + 1 : 0);
    }
    int n = 1 + PERC_KIT_COUNT + pool_n[slot];
    br.pos = (int16_t)(((br.pos + st) % n + n) % n);
    if (br.pos == 0) *k = br.stash;
    else if (br.pos <= PERC_KIT_COUNT) key_preset(k, br.pos - 1);
    else key_tonic(k, slot, br.pos - 1 - PERC_KIT_COUNT);
}
/* an encoder turn of st clicks. Returns 1 if the kit changed. */
static int ui_turn(int slot, int pad, int page, int shift, int knob, int st)
{
    pd_key *k = &kit[slot][pad];
    if (pend.slot == slot) {                                  /* a transfer waits: blue moves where it goes */
        if (knob == 0) { int a = pend.anchor + st; pend.anchor = (uint8_t)(a < 0 ? 0 : a >= PD_PADS ? PD_PADS - 1 : a); }
        return 0;
    }
    int l = ui_lane[page][shift ? 1 : 0][knob & 3];
    if (l == L_NONE) return 0;
    if (l == L_SOUND) { browse(slot, pad, st); return 1; }
    br.valid = 0;
    if (l == L_MORPH) { int16_t m = clamp15(k->morph + st * 1024); if (m == k->morph) return 0; k->morph = m; return 1; }
    uint16_t *w = key_side(k) ? k->b : k->a;
    uint16_t nv = clampw((int32_t)w[l] + st * (l == P_OSCFREQ ? 548 : 1024));   /* pitch: a semitone a click */
    if (nv == w[l]) return 0;
    w[l] = nv; k->flags |= PK_EDITED;
    return 1;
}
static int place_pending(int slot)
{
    int fit = pend_fit(), from = pend.anchor;
    undo_save(slot);
    for (int i = 0; i < fit; i++) key_tonic(&kit[slot][from + i], slot, pend.idx[i]);
    pend.slot = -1; br.valid = 0;
    note(NOTE_PLACED, from, fit);
    return 1;
}
/* an encoder press. Returns 1 if the kit changed. */
static int ui_press(int slot, int pad, int page, int shift, int knob)
{
    pd_key *k = &kit[slot][pad];
    if (pend.slot == slot) {                                  /* blue: place the waiting sounds, red: leave them */
        if (knob == 0) return place_pending(slot);
        if (knob == 3) { pend.slot = -1; note(NOTE_SKIP, 0, 0); }
        return 0;
    }
    if (shift) {
        if (knob == 0) {                                      /* home: the slot's own preset again, else the key's */
            undo_save(slot);
            key_preset(k, k->origin < PERC_KIT_COUNT && (k->flags & PK_EDITED) ? k->origin : pad);
            br.valid = 0; note(NOTE_HOME, pad, 1);
            return 1;
        }
        if (knob == 3 && undo_swap(slot)) { br.valid = 0; note(NOTE_UNDO, 0, 0); return 1; }
        return 0;
    }
    int l = ui_mode[page][knob & 3];
    if (l == L_NONE) return 0;
    uint16_t *w = key_side(k) ? k->b : k->a;
    static const uint16_t step[3] = { 32768, 65535, 0 };
    w[l] = step[w_mode(w[l])]; k->flags |= PK_EDITED; br.valid = 0;
    return 1;
}

/* ---- values in their units ---- */
static char *put(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }
static char *num(char *p, int32_t x) { char t[10]; int n = 0; if (x < 0) { *p++ = '-'; x = -x; } do t[n++] = (char)('0' + x % 10); while ((x /= 10)); while (n) *p++ = t[--n]; *p = 0; return p; }
static char *dec1(char *p, int32_t x10) { if (x10 < 0) { *p++ = '-'; x10 = -x10; } p = num(p, x10 / 10); *p++ = '.'; return num(p, x10 % 10); }
static int32_t q16mul(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 16); }
static int32_t xq(uint16_t p) { return (int32_t)p + (p >> 15); }                       /* 0..65536 */
static uint32_t w_hz10(uint16_t w) { return perc_pow2(PERC_L2_20 + q16mul(xq(w), PERC_L2_1000) + PERC_L2_10, 0); }   /* 20 x 1000^x Hz */
static uint32_t w_dcy10(uint16_t w) { return perc_pow2(q16mul(xq(w), PERC_L2_1000) + 2 * PERC_L2_10, 0); }           /* 10 x 1000^x ms */
static uint32_t w_atk10(uint16_t w)                                                    /* x 10^((12x - 7) / 5) s */
{
    int32_t x = xq(w); if (!x) return 0;
    return perc_pow2(perc_log2((uint32_t)x) - (16 << 16) + PERC_L2_0P0398 + q16mul(q16mul(x, 157286), PERC_L2_10) + PERC_L2_1000 + PERC_L2_10, 0);
}
static int32_t w_semis10(const uint16_t *w)                                            /* b |b| x 96 (decay) or 48 */
{
    int32_t b = 2 * xq(w[P_MODAMT]) - 65536;
    return (int32_t)(((int64_t)b * (b < 0 ? -b : b) * (w_mode(w[P_MODMODE]) == 0 ? 960 : 480)) >> 32);
}
/* the bend's rate: the time of the decay (ms x 10; 0 = it holds), or minus the sine / noise frequency (Hz x 10) */
static int32_t w_rate10(const uint16_t *w)
{
    int32_t x = xq(w[P_MODRATE]), m = w_mode(w[P_MODMODE]);
    if (x <= 0) return 0;
    if (m == 0) return (int32_t)((int64_t)(67502 - x) * 10000 / (3 * x));
    int32_t l2 = perc_log2((uint32_t)x) - (16 << 16) + (m == 1 ? PERC_L2_2000 + q16mul(q16mul(x - 65536, 229376), PERC_L2_10)
                                                              : PERC_L2_20000 + q16mul(4 * (x - 65536), PERC_L2_10));
    return -(int32_t)perc_pow2(l2 + PERC_L2_10, 0);
}
static int32_t w_level_db10(uint16_t w)                                                /* 60x - 49 - 1/x dB */
{
    int32_t x = xq(w);
    return (int32_t)((((int64_t)60 * x - 49 * 65536 - ((int64_t)1 << 32) / x) * 10) >> 16);
}
static char *fmt_hz(char *p, uint32_t hz10)
{
    if (hz10 < 1000) return put(dec1(p, (int32_t)hz10), "hz");
    if (hz10 < 100000) return put(num(p, (int32_t)(hz10 / 10)), "hz");
    return put(dec1(p, (int32_t)(hz10 / 1000)), "k");
}
static char *fmt_ms(char *p, uint32_t ms10)
{
    if (!ms10) return put(p, "0ms");
    if (ms10 < 100) return put(dec1(p, (int32_t)ms10), "ms");
    if (ms10 < 10000) return put(num(p, (int32_t)(ms10 / 10)), "ms");
    return put(dec1(p, (int32_t)(ms10 / 1000)), "s");
}
static const char *const NOTE_NAME[12] = { "c", "c#", "d", "d#", "e", "f", "f#", "g", "g#", "a", "a#", "b" };
static char *fmt_key(char *p, int pad) { p = put(p, NOTE_NAME[(pad + 5) % 12]); return num(p, 1 + (pad + 5) / 12); }   /* key 1 = f1 */

static void fmt_lane(const pd_key *k, int slot, int pad, int l, char *v)
{
    const uint16_t *w = key_side(k) ? k->b : k->a;
    int32_t x = l >= 0 && l < 21 ? xq(w[l]) : 0;
    v[0] = 0;
    switch (l) {
    case L_SOUND:
        if (k->origin < PERC_KIT_COUNT) put(v, PERC_KIT_NAME[k->origin]);
        else if (br.valid && br.slot == slot && br.pad == pad && br.pos > PERC_KIT_COUNT) num(put(v, "tonic "), br.pos - PERC_KIT_COUNT);
        else put(v, "tonic");
        break;
    case L_MORPH: put(num(put(v, key_side(k) ? "b " : "a "), (k->morph * 100 + 16383) >> 15), "%"); break;
    case P_OSCFREQ: case P_NFILFRQ: case P_EQFREQ: fmt_hz(v, w_hz10(w[l])); break;
    case P_OSCDCY: case P_NENVDCY: fmt_ms(v, w_dcy10(w[l])); break;
    case P_OSCATK: case P_NENVATK: fmt_ms(v, w_atk10(w[l])); break;
    case P_MODAMT: { int32_t s = w_semis10(w); char *p = v; if (s > 0) *p++ = '+'; put(dec1(p, s), "st"); break; }
    case P_MODRATE: { int32_t r = w_rate10(w); if (r > 0) fmt_ms(v, (uint32_t)r); else if (r < 0) fmt_hz(v, (uint32_t)-r); else put(v, w_mode(w[P_MODMODE]) ? "0hz" : "hold"); break; }
    case P_NFILQ: { uint32_t q10 = perc_pow2(q16mul(5 * x, PERC_L2_10), 0); if (q10 < 1000) dec1(v, (int32_t)q10); else num(v, (int32_t)(q10 / 10)); break; }
    case P_MIX: { int n = (x * 100 + 32768) >> 16; num(put(num(v, 100 - n), "/"), n); break; }                /* tone / noise */
    case P_DIST: put(num(v, (x * 100 + 32768) >> 16), "%"); break;
    case P_EQGAIN: { int32_t d = ((x - 32768) * 800) / 65536; char *p = v; if (d > 0) *p++ = '+'; put(dec1(p, d), "db"); break; }
    case P_LEVEL: if (x < 7) put(v, "-inf"); else { int32_t d = w_level_db10(w[l]); char *p = v; if (d > 0) *p++ = '+'; put(dec1(p, d), "db"); } break;
    case P_OSCVEL: case P_NVEL: case P_MODVEL: put(num(v, (x * 200 + 32768) >> 16), "%"); break;
    default: break;
    }
}
/* the page's words: the top line (name, info), and per lane its label, value and press state */
static void ui_texts(int slot, int pad, int page, int shift, int reading, char *name, char *info,
                     char lab[4][12], char val[4][12], char prs[4][8])
{
    const pd_key *k = &kit[slot][pad];
    for (int i = 0; i < 4; i++) lab[i][0] = val[i][0] = prs[i][0] = 0;
    info[0] = 0;
    if (pend.slot == slot) {                                  /* "4 new > c#2-e2" */
        int fit = pend_fit();
        char *p = put(num(name, pend.n), " new > "); p = fmt_key(p, pend.anchor);
        if (fit > 1) { *p++ = '-'; fmt_key(p, pend.anchor + fit - 1); }
        if (fit < pend.n) put(num(put(num(info, fit), " of "), pend.n), " fit");
        put(lab[0], "to key"); fmt_key(val[0], pend.anchor); put(prs[0], "place"); put(prs[3], "skip");
        return;
    }
    { char *p = put(fmt_key(name, pad), " "); fmt_lane(k, slot, pad, L_SOUND, p); while (*p) p++; if (k->flags & PK_EDITED) put(p, "*"); }
    if (reading) put(info, "reading..");
    else if (note_kind && note_age < 150) {
        if (note_kind == NOTE_PLACED) { char *p = put(info, "placed "); p = fmt_key(p, note_from); if (note_n > 1) { *p++ = '-'; fmt_key(p, note_from + note_n - 1); } }
        else put(info, note_kind == NOTE_UNDO ? "undo" : note_kind == NOTE_HOME ? "factory" : "skipped");
    } else if (k->morph > 0 || page == PG_MIX) put(info, key_side(k) ? "side b" : "side a");
    for (int i = 0; i < 4; i++) {
        int l = ui_lane[page][shift ? 1 : 0][i];
        put(lab[i], ui_label[page][shift ? 1 : 0][i]);
        if (l != L_NONE) fmt_lane(k, slot, pad, l, val[i]);
        const uint16_t *w = key_side(k) ? k->b : k->a;
        if (shift) put(prs[i], i == 0 ? "factory" : i == 3 && undo_slot == slot ? "undo" : "");
        else if (ui_mode[page][i] != L_NONE) put(prs[i], ui_mode_name[ui_mode[page][i]][w_mode(w[ui_mode[page][i]])]);
    }
}

/* ---- drawing (GUI thread): the sound's own curves in the colours of the knobs that shape them. The LCD is larger
 * than the bezel shows: nothing sits in the corners. ---- */
#define Q(v) ((int32_t)(v) * 1024)
#define TS 12
#define CV_TOP 46                                             /* the curves' area */
#define CV_BASE 90
#define CV_X0 24
#define CV_X1 296
static const uint8_t c_blue[4] = { 0x4D, 0x9E, 0xFF, 0xFF }, c_green[4] = { 0x00, 0xED, 0x95, 0xFF },
                     c_white[4] = { 0xFF, 0xFF, 0xFF, 0xFF }, c_red[4] = { 0xFF, 0x3A, 0x5D, 0xFF },
                     c_grey[4] = { 0x6F, 0x71, 0x73, 0xFF }, c_dark[4] = { 0x38, 0x3A, 0x40, 0xFF };
static const uint8_t *const col4[4] = { c_blue, c_green, c_white, c_red };
static void *rr;
static void ln(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)   /* Q10 */
{
    if (x0 == x1 && y0 == y1) return;
    op1_pt a = { x0, y0 }, b = { x1, y1 };
    op1_raster_line(rr, &a, &b, w, c);
}
static int32_t clampq(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static uint32_t isqrt32(uint32_t v) { uint32_t r = 0, b = 1u << 30; while (b > v) b >>= 2; while (b) { if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; } return r; }
/* time (ms x 10) -> 0..256 across the area: square root of the time over 1.5 s, so 10 ms and 1 s both show */
static int32_t t_u(uint32_t ms10) { return ms10 >= 15000 ? 256 : (int32_t)isqrt32(ms10 * 65536u / 15000u); }
static int32_t u_x(int32_t u, int x0, int x1) { return Q(x0) + (Q(x1 - x0) * u >> 8); }
static uint32_t u_t(int32_t u) { return (uint32_t)(15000 * u * u) >> 16; }
/* 2^(-log2(1000) t / tau), Q10: a fall of 1000x over tau */
static int32_t fall(uint32_t t10, uint32_t tau10)
{
    if (!tau10) return 0;
    int64_t e = (int64_t)PERC_L2_1000 * t10 / tau10;
    return e > (20 << 16) ? 0 : (int32_t)perc_pow2(-(int32_t)e, 10);
}
/* an envelope over x0..x1: the attack (colour ca), then the decay (cd): a fall of 1000x, or a straight line */
static void draw_env(int x0, int x1, uint32_t atk10, uint32_t dcy10, int lin, int32_t w, const uint8_t *ca, const uint8_t *cd)
{
    int32_t ua = t_u(atk10), xa = u_x(ua, x0, x1), top = Q(CV_TOP), base = Q(CV_BASE);
    if (xa < Q(x0) + 0x400) xa = Q(x0) + 0x400;              /* attack 0: an edge */
    ln(Q(x0), base, xa, top, w, ca);
    if (lin) { ln(xa, top, u_x(t_u(atk10 + dcy10), x0, x1), base, w, cd); return; }
    int32_t px = xa, py = top;
    for (int i = 1; i <= 16; i++) {
        int32_t u = ua + (256 - ua) * i / 16, x = u_x(u, x0, x1);
        uint32_t t = u_t(u);
        int32_t y = base - (int32_t)(((int64_t)(base - top) * fall(t > atk10 ? t - atk10 : 0, dcy10)) >> 10);
        ln(px, py, x, y, w, cd);
        px = x; py = y;
    }
}
/* a filter's response between x0 and x1 around the level y0: mode 0 lp, 1 bp, 2 hp; the peak grows with q */
static void draw_filter(int32_t x0, int32_t x1, int mode, uint16_t f, uint16_t q)
{
    int32_t fc = x0 + (int32_t)(((int64_t)(x1 - x0) * f) >> 16), y0 = Q(70), lo = Q(CV_BASE), pk = y0 - (Q(22) * q >> 16);
    int32_t a = clampq(fc - Q(mode == 0 ? 6 : 14), x0, x1), e = clampq(fc + Q(mode == 2 ? 6 : 14), x0, x1);
    if (mode == 0) { ln(x0, y0, a, y0, 0x600, c_blue); ln(a, y0, fc, pk, 0x600, c_red); ln(fc, pk, e, lo, 0x600, c_blue); }
    else if (mode == 2) { ln(a, lo, fc, pk, 0x600, c_blue); ln(fc, pk, e, y0, 0x600, c_red); ln(e, y0, x1, y0, 0x600, c_blue); }
    else { ln(a, lo, fc, pk, 0x600, c_blue); ln(fc, pk, e, lo, 0x600, c_red); }
}
static void draw_tone(const uint16_t *w, int32_t lw)
{
    draw_env(CV_X0, CV_X1, w_atk10(w[P_OSCATK]), w_dcy10(w[P_OSCDCY]), 0, lw, c_green, c_white);
    /* the pitch on a 20 Hz .. 20 kHz scale (blue), the bend on top of it (red) */
    int32_t top = Q(CV_TOP), base = Q(CV_BASE), span = base - top;
    int32_t yp = base - (int32_t)(((int64_t)span * w[P_OSCFREQ]) >> 16);
    int32_t dy = (int32_t)(((int64_t)span * w_semis10(w) * 548 / 10) >> 16);          /* 548 words a semitone */
    int32_t rate = w_rate10(w), mode = w_mode(w[P_MODMODE]);
    ln(Q(CV_X0), yp, Q(CV_X1), yp, 0x300, c_blue);
    if (!dy) return;
    int32_t px = Q(CV_X0), py = clampq(yp - dy, top - Q(4), base);
    for (int i = 1; i <= 16; i++) {
        int32_t u = 256 * i / 16, x = u_x(u, CV_X0, CV_X1), y;
        if (mode == 0) y = yp - (rate ? (int32_t)(((int64_t)dy * fall(u_t(u), (uint32_t)rate)) >> 10) : dy);
        else y = yp + ((i & 1) ? dy : -dy) * (mode == 2 ? 1 + (i * 7 % 3) : 3) / 3;   /* sine: even swings; noise: uneven */
        y = clampq(y, top - Q(4), base);
        ln(px, py, x, y, 0x500, c_red);
        px = x; py = y;
    }
}
static void draw_noise(const uint16_t *w, int32_t lw)
{
    int em = w_mode(w[P_NENVMOD]);
    uint32_t a = w_atk10(w[P_NENVATK]), d = w_dcy10(w[P_NENVDCY]);
    if (em == 1) { a = a * 2 / 3; d = d * 2 / 3; }           /* linear: times x 2/3 */
    draw_env(CV_X0, 188, a, d, em == 1, lw, c_green, c_white);
    if (em == 2) for (int i = 1; i <= 3; i++) ln(Q(CV_X0 + 5 * i), Q(CV_TOP), Q(CV_X0 + 5 * i), Q(CV_TOP + 14), 0x400, c_white);   /* mod: the claps */
    draw_filter(Q(204), Q(CV_X1), w_mode(w[P_NFILMOD]), w[P_NFILFRQ], w[P_NFILQ]);
}
static void draw_mix(const pd_key *k, const uint16_t *w, int shift)
{
    /* tone / noise balance: two bars */
    int32_t m = w[P_MIX];
    ln(Q(24), Q(58), Q(104), Q(58), 0x1400, c_dark); ln(Q(24), Q(74), Q(104), Q(74), 0x1400, c_dark);
    ln(Q(24), Q(58), Q(24) + (int32_t)(((int64_t)Q(80) * (65535 - m)) >> 16), Q(58), 0x1400, c_blue);
    ln(Q(24), Q(74), Q(24) + (int32_t)(((int64_t)Q(80) * m) >> 16), Q(74), 0x1400, c_blue);
    /* drive: the shaper's curve, in -> out */
    int32_t g = 1024 + (w[P_DIST] * 12 >> 6), px = 0, py = 0;                         /* 1 .. 13, Q10 */
    for (int i = 0; i <= 12; i++) {
        int32_t in = (i - 6) * 1024 / 6, v = (int32_t)(((int64_t)in * g) >> 10), av = v < 0 ? -v : v;
        int32_t out = (int32_t)((int64_t)v * 1024 / (1024 + av)), full = (int32_t)((int64_t)g * 1024 / (1024 + g));
        int32_t x = Q(150) + (in * 26), y = Q(68) - (int32_t)((int64_t)out * Q(20) / full);
        if (i) ln(px, py, x, y, 0x600, c_green);
        px = x; py = y;
    }
    /* EQ: the line (white) and its bell (red) */
    int32_t x0 = Q(194), x1 = Q(274), fc = x0 + (int32_t)(((int64_t)(x1 - x0) * w[P_EQFREQ]) >> 16);
    int32_t h = ((int32_t)w[P_EQGAIN] - 32768) * 22 / 32, a = clampq(fc - Q(10), x0, x1), e = clampq(fc + Q(10), x0, x1);
    ln(x0, Q(68), a, Q(68), 0x500, c_white); ln(e, Q(68), x1, Q(68), 0x500, c_white);
    ln(a, Q(68), fc, Q(68) - h, 0x500, c_red); ln(fc, Q(68) - h, e, Q(68), 0x500, c_red);
    /* level */
    ln(Q(288), Q(CV_BASE), Q(288), Q(CV_TOP), 0x1000, c_dark);
    ln(Q(288), Q(CV_BASE), Q(288), Q(CV_BASE) - (int32_t)(((int64_t)Q(CV_BASE - CV_TOP) * w[P_LEVEL]) >> 16), 0x1000, c_red);
    /* morph: a .. b */
    if (shift || k->morph > 0) {
        int32_t mx = Q(120) + (int32_t)(((int64_t)Q(80) * k->morph) >> 15);
        ln(Q(120), Q(42), Q(200), Q(42), 0x300, c_grey); ln(mx, Q(39), mx, Q(45), 0x800, c_blue);
    }
}
/* the kit: 24 keys as a small keyboard (sharps raised): a preset grey, a transferred sound blue, the selected key
 * white under-lined, waiting sounds blinking green where they would go, placed ones green for a moment */
static void draw_strip(int slot, int pad, uint32_t frame, int hit)
{
    static const uint8_t sharp[12] = { 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0 };
    int pf = pend.slot == slot ? pend_fit() : 0;
    for (int p = 0; p < PD_PADS; p++) {
        const pd_key *k = &kit[slot][p];
        int32_t x = Q(64 + 8 * p) + 0xE00, y0 = Q(sharp[(p + 5) % 12] ? 10 : 14);
        const uint8_t *c = k->origin == PD_TONIC ? c_blue : c_grey;
        if (pf && p >= pend.anchor && p < pend.anchor + pf) c = (frame & 16) ? c_green : c_dark;
        else if (note_kind == NOTE_PLACED && note_age < 60 && p >= note_from && p < note_from + note_n) c = c_green;
        else if (p == pad && hit) c = c_white;
        ln(x, y0, x, y0 + Q(8), 0x1800, c);
        if (p == pad) ln(x - Q(3), Q(25), x + Q(3), Q(25), 0x600, c_white);
    }
}

enum { TX_NAME, TX_INFO, TX_LAB, TX_VAL = 6, TX_PRS = 10, TX_N = 14 };
#ifndef OP1_TEXT_SIZE
#define OP1_TEXT_SIZE 0xA4
#endif
static uint32_t tx[TX_N][OP1_TEXT_SIZE / 4];
static char tx_shown[TX_N][28];
static uint8_t tx_made;
static void txt(int i, const char *s)
{
    static const int32_t ident[9] = { 1024, 0, 0, 0, 1024, 0, 0, 0, 1024 };
    if (!s[0]) return;
    int same = 1;
    for (int j = 0; j < 28; j++) { if (tx_shown[i][j] != s[j]) { same = 0; break; } if (!s[j]) break; }
    if (!same) { op1_text_set(tx[i], s); int j = 0; for (; j < 27 && s[j]; j++) tx_shown[i][j] = s[j]; tx_shown[i][j] = 0; }
    op1_text_draw(tx[i], rr, ident);
}
static uint32_t ui_frame, ui_seq, ui_age = 1000;
static void ui_draw(void *raster, int slot, int pad, int page, int shift, int reading)
{
    rr = raster;
    if (!tx_made) {
        op1_text_ctor(tx[TX_NAME], Q(24), 28, TS, c_white);
        op1_text_ctor(tx[TX_INFO], Q(176), 28, TS, c_grey);
        for (int i = 0; i < 4; i++) {
            op1_text_ctor(tx[TX_LAB + i], Q(8 + 78 * i), 112, TS, col4[i]);
            op1_text_ctor(tx[TX_VAL + i], Q(8 + 78 * i), 126, TS, c_white);
            op1_text_ctor(tx[TX_PRS + i], Q(8 + 78 * i), 94, TS, col4[i]);
        }
        tx_made = 1;
    }
    ui_frame++;
    if (note_age < 1000) note_age++;
    if (pd_hit_seq != ui_seq) { ui_seq = pd_hit_seq; ui_age = 0; } else if (ui_age < 1000) ui_age++;
    const pd_key *k = &kit[slot][pad];
    const uint16_t *w = key_side(k) ? k->b : k->a;
    int32_t lw = ui_age < 5 ? 0xA00 : 0x600;                 /* the envelope flashes with a hit */
    draw_strip(slot, pad, ui_frame, ui_age < 5);
    if (pend.slot == slot) {                                  /* waiting sounds: the first one's shape */
        const uint16_t *pw = pool[slot][pend.idx[0]][0];
        draw_env(CV_X0, 150, w_atk10(pw[P_OSCATK]), w_dcy10(pw[P_OSCDCY]), 0, lw, c_green, c_white);
        draw_env(170, CV_X1, w_atk10(pw[P_NENVATK]), w_dcy10(pw[P_NENVDCY]), w_mode(pw[P_NENVMOD]) == 1, lw, c_green, c_white);
    } else if (page == PG_TONE) draw_tone(w, lw);
    else if (page == PG_NOISE) draw_noise(w, lw);
    else draw_mix(k, w, shift);
    for (int i = 0; i < 4; i++) ln(Q(8 + 78 * i), Q(108), Q(78 + 78 * i), Q(108), 0x800, col4[i]);
    static char name[28], info[28], lab[4][12], val[4][12], prs[4][8];
    ui_texts(slot, pad, page, shift, reading, name, info, lab, val, prs);
    txt(TX_NAME, name); txt(TX_INFO, info);
    for (int i = 0; i < 4; i++) { txt(TX_LAB + i, lab[i]); txt(TX_VAL + i, val[i]); txt(TX_PRS + i, prs[i]); }
}

#ifndef OP1_HOST
/* ================= firmware glue ================= */
/* ---- the kit's transfer: decoded from the slot's sample (GUI thread, bounded per frame) ---- */
static perc_xfer_t xf_dec;
static int xf_slot = -1;
static uint32_t xf_pos, xf_len, xf_sig[PD_SLOTS], xf_now;
static int xf_anchor;                                         /* the key selected when the recording was found */
static const int16_t *xf_data;
static volatile uint32_t dirty_keys[PD_SLOTS];               /* changed since kit_write */
static int pd_pad(void) { int p = LAST_PAD; return p >= 0 && p < PD_PADS ? p : 0; }
static const uint8_t *kit_sample(int slot) { uint32_t *o = DRUM_SAMPLER(slot & 7); return o && o[0] ? (const uint8_t *)o[0] : 0; }
static uint32_t kit_length(const uint8_t *sb)             /* the recording's length from its peaks (as granular) */
{
    const int16_t *pk = (const int16_t *)(sb + 4);
    for (int i = OP1_PEAK_BUCKETS - 1; i >= 0; i--) if (pk[i] > 96) return (uint32_t)(i + 1) * 256u;
    return 0;
}
static void xfer_step(int slot)
{
    const uint8_t *sb = kit_sample(slot);
    if (!sb) return;
    if (xf_slot < 0) {
        const int16_t *pk = (const int16_t *)(sb + 4);
        uint32_t h = 2166136261u;
        for (int i = 0; i < OP1_PEAK_BUCKETS; i++) { h ^= (uint16_t)pk[i]; h *= 16777619u; }
        if (!h) h = 1;
        if (h == xf_sig[slot]) return;
        xf_sig[slot] = h; xf_now = h;                        /* the recording's content: saved with the kit */
        xf_len = kit_length(sb); xf_anchor = pd_pad();
        if (!xf_len) { perc_xfer_begin(&xf_dec); if (pool_take(slot, &xf_dec, h, 0)) dirty_keys[slot] |= 1; br.valid = 0; return; }   /* empty: no sounds */
        perc_xfer_begin(&xf_dec); xf_slot = slot; xf_pos = 0;
        xf_data = *(const int16_t *const *)sb + 8;           /* after the 8 guard samples */
        return;
    }
    if (xf_slot != slot) { xf_sig[xf_slot] = 0; xf_slot = -1; return; }
    uint32_t n = xf_len - xf_pos; if (n > 2048) n = 2048;   /* small chunks: the page stays smooth while decoding */
    perc_xfer_push(&xf_dec, xf_data + xf_pos, n); xf_pos += n;
    if (xf_pos >= xf_len) {                                  /* the whole sample: all transfers in it */
        if (pool_take(slot, &xf_dec, xf_now, xf_anchor)) dirty_keys[slot] |= 1;   /* written with the next key or turn */
        br.valid = 0;
        xf_slot = -1;
    }
}

static uint8_t jv_type(const void *v) { return ((const uint8_t *)v)[8]; }
static int jv_is_str(const void *v, const char *s)
{
    if (!v || jv_type(v) != JV_STRING) return 0;
    const char *p = *(const char *const *)v;
    if (!p) return 0;
    while (*s && *p == *s) p++, s++;
    return *p == *s;
}
static int ours(const void *op1r) { return op1r && jv_type(op1r) == JV_OBJECT && jv_is_str(JV_CGET(op1r, "engine"), "perc"); }

static void pd_slot_loaded(int slot, void *op1r)
{
    if ((unsigned)slot >= PD_SLOTS) return;
    if (!ours(op1r)) { claimed[slot] = 0; return; }
    const void *b = JV_CGET(op1r, "b");
    kit_load(slot, b && jv_type(b) == JV_STRING ? *(const char *const *)b : 0);   /* (kits older than "b": fresh) */
    claimed[slot] = 1;
}
static void kit_write(int slot)
{
    void *json = (uint8_t *)DRUM_PATCH(slot) + 4;
    void *op = JV_KEY(json, "op1r");
    if (!ours(op)) return;
    b64_enc(pd_raw, pack_kit(slot));
    uint32_t tmp[4];
    JV_FROM_STR(tmp, pd_b64); JV_ASSIGN(JV_KEY(op, "b"), tmp); JV_DTOR(tmp, 2);
}
/* from the key thread only (knob turns, key events via pd_sync; the stock pad writes run there too), never waits:
 * while a patch job runs the keys stay dirty for the next turn or key */
static void pd_flush(void)
{
    for (int slot = 0; slot < PD_SLOTS; slot++) {
        if (!dirty_keys[slot]) continue;
        if (!op1_patch_idle()) return;                        /* a save / type job runs: on the next turn or key */
        dirty_keys[slot] = 0;
        if (!claimed[slot]) continue;
        kit_write(slot);
        DRUM_TO_PATCH(DRUM_PATCH(slot));                      /* marks the patch changed: the delayed save writes it */
    }
}

/* type job: our row -> claim the sampled kit (fresh keys); another selection -> let go, then the stock job */
static void pd_type_job(void *job)
{
    int slot = *(int32_t *)((uint8_t *)job + 4), sel = *(int32_t *)((uint8_t *)job + 8);
    if ((unsigned)slot >= PD_SLOTS) { TYPE_JOB(job); return; }
    void *json = (uint8_t *)DRUM_PATCH(slot) + 4;
    uint32_t tmp[4];
    if (sel < 2) {
        JV_REMOVE(json, tmp, "op1r"); JV_DTOR(tmp, 2);
        claimed[slot] = 0;
        dpu_sync_auto();
        TYPE_JOB(job);
        return;
    }
    if (!jv_is_str(JV_KEY(json, "type"), "drum")) return;     /* dbox: not a sampled kit */
    void *op = JV_KEY(json, "op1r");                          /* null -> object on the first key */
    JV_FROM_STR(tmp, "perc"); JV_ASSIGN(JV_KEY(op, "engine"), tmp); JV_DTOR(tmp, 2);
    pd_slot_loaded(slot, op);                                 /* its keys (a perc kit from before keeps them; else fresh) */
    kit_write(slot);                                          /* inside the job: no patch job can overlap */
    dirty_keys[slot] = 0;
    struct { uint32_t vt; uint8_t drum, pad[3]; int32_t slot; } save = { 0, 1, { 0, 0, 0 }, slot };
    SAVE_SLOT(&save);                                         /* saved before the UI wakes (as sampler+, docs/66) */
    UI_SLOT(slot);
    UI_SIGNAL();
}
static int pd_is_plus(int slot) { return (unsigned)slot < PD_SLOTS && claimed[slot]; }

/* ---- the pages: two screen objects, for T1 (0x10: tone, and mix after T4) and T2 (0x17: noise) ---- */
#define SCREEN_CTOR     ((void *(*)(void *, int, const char *))0x01A3BAB0)
#define SVG_LOAD        ((void *(*)(void *))0x01A503D8)
#define ITER_SCREEN_VT  ((const uint32_t *)0x019103FC)
static uint32_t pd_obj[2][32], pd_vt[10];
static void *pd_stock[2];
static uint8_t pd_ready, pd_mix, pd_t4_down, pd_editor;
static int32_t pd_acc[4];
static int pd_slot_ours(void) { return DRUM_MODE && claimed[DRUM_SLOT_CUR]; }
static void *pd_screen_for(int id)
{
    if (!pd_ready || (id != 0x10 && id != 0x17)) return 0;
    if (pd_slot_ours()) return pd_obj[id == 0x17];
    return pd_stock[id == 0x17];
}
static void pd_sync(int editor_open_on_10)
{
    pd_flush();                                               /* (inside a type job: not idle, so later) */
    pd_editor = (uint8_t)editor_open_on_10;
    if (!pd_ready) return;
    if (!editor_open_on_10) SCREEN_TABLE[0x10] = pd_screen_for(0x10);
    SCREEN_TABLE[0x17] = pd_screen_for(0x17);
}
/* the page shown: PG_TONE / PG_NOISE / PG_MIX, or -1 */
static int pd_page(void)
{
    if (!pd_ready || !pd_slot_ours()) return -1;
    int id = CURRENT_SCREEN;
    if (id == 0x10 && SCREEN_TABLE[0x10] == (void *)pd_obj[0]) return pd_mix ? PG_MIX : PG_TONE;
    if (id == 0x17 && SCREEN_TABLE[0x17] == (void *)pd_obj[1]) return PG_NOISE;
    return -1;
}
static void pd_changed(int slot, int pad) { dirty_keys[slot] |= 1u << pad; pd_flush(); }
static int pd_knob(int knob, int delta)
{
    int page = pd_page();
    if (page < 0 || knob < 0 || knob > 3) return 0;
    int32_t *acc = &pd_acc[knob];
    if ((*acc > 0 && delta < 0) || (*acc < 0 && delta > 0)) *acc = 0;
    *acc += delta;
    int st = *acc / 1024;                                     /* a step per hardware click (delta 1024) */
    if (!st) return 1;
    *acc -= st * 1024;
    int slot = DRUM_SLOT_CUR, pad = pd_pad();
    if (ui_turn(slot, pad, page, SHIFT_HELD, knob, st)) pd_changed(slot, pad);
    return 1;
}
/* keys: the encoder presses on our pages; T4 anywhere in drum mode on a perc kit (from T1, T2, the FX page, or the LFO
 * page the mode came back to: the LFO does nothing for a perc kit) shows the mix page on the T1 screen, T1 the tone page
 * again. Shift + T-keys and T3 stay stock. */
static int pd_keyev(int code, int pressed)
{
    if (code == 0x09) {
        if (!pressed) { int r = pd_t4_down; pd_t4_down = 0; return r; }
        int page = pd_page();
        if (SHIFT_HELD || pd_editor || !pd_ready || !pd_slot_ours() || (page < 0 && MODE_ID != 1)) return 0;
        pd_mix = 1; pd_t4_down = 1;
        if (page != PG_TONE && page != PG_MIX) {              /* to the T1 screen, as a stock T1 press */
            KEY_DISPATCH(0x06, 1); KEY_DISPATCH(0x06, 0);
            SCREEN_TABLE[0x10] = pd_screen_for(0x10);
        }
        return 1;
    }
    if (code == 0x06 && pressed && !SHIFT_HELD) { pd_mix = 0; return 0; }
    if (code < 0x3B || code > 0x3E) return 0;
    int page = pd_page();
    if (page < 0) return 0;
    if (pressed) {
        int slot = DRUM_SLOT_CUR, pad = pd_pad();
        if (ui_press(slot, pad, page, SHIFT_HELD, code - 0x3B)) { dirty_keys[slot] |= 1u << pad; pd_flush(); }
    }
    return 1;
}

static void pd_init(void *scr) { (void)scr; }
static void pd_update(void *scr, int dt)
{
    int slot = DRUM_SLOT_CUR;
    xfer_step(slot);
    op1_screen_draw(scr, dt);
    ui_draw(op1_raster(), slot, pd_pad(), scr == (void *)pd_obj[1] ? PG_NOISE : pd_mix ? PG_MIX : PG_TONE, SHIFT_HELD, xf_slot >= 0);
}
static void pd_ui_build(void)
{
    for (int i = 0; i < 10; i++) pd_vt[i] = ITER_SCREEN_VT[i];    /* is_visible: id == current */
    pd_vt[3 * 2 + 1] = (uint32_t)pd_update;
    pd_vt[4 * 2 + 1] = (uint32_t)pd_init;
    for (int i = 0; i < 2; i++) {
        int id = i ? 0x17 : 0x10;
        void *saved = SCREEN_TABLE[id];
        SCREEN_CTOR(pd_obj[i], id, "usbout.svg");
        SCREEN_TABLE[id] = saved;
        pd_obj[i][3] = (uint32_t)pd_vt;
    }
}
static void pd_ui_load(void)
{
    for (int i = 0; i < 2; i++) pd_obj[i][0] = (uint32_t)SVG_LOAD((void *)pd_obj[i][2]);
    if (!pd_obj[0][0] || !pd_obj[1][0]) return;
    pd_stock[0] = SCREEN_TABLE[0x10]; pd_stock[1] = SCREEN_TABLE[0x17];
    pd_ready = pd_stock[0] && pd_stock[1];
}

const op1_drum_t PERCDRUM = {
    pd_render, pd_note_on, pd_note_off, pd_slot_loaded, pd_is_plus, pd_type_job,
    pd_ui_build, pd_ui_load, pd_screen_for, pd_sync, pd_knob, pd_keyev };
#endif
