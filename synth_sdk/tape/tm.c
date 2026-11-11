/* Free tape machine, v1 (docs/82): the four tracks play with their own speed, loop length, offset and direction.
 * R246's tape player keeps running (transport, position, loop markers, UI); while free is selected and the tape plays,
 * tm_block (audio ISR, from fr_snap right after tape_play_mix) overwrites its output L / R 0xFF800D34 / DB4 with ours.
 * Data: our own single-track chunk cache (TM_SLOTS x 64 KB from heap 0, only while free is on), filled by tm_service
 * from op1r's tape_service (main loop): a chunk R246's clip cache holds is copied from there (fresh recordings),
 * else read from tape_c.raw (R246's layout: track t, chunk c at TRACK_BASE[t] + c * 0x10000).
 * Heads: Q32.32 samples. Each track has its own loop (in / out in samples, on / off: s1 / s2 / s3 on the screen,
 * screens/tfscreen.c); without it the head runs free from the transport position. Each block every head moves by the
 * transport speed x the track's speed; a locate or a (re)start resyncs them to the transport.
 * Mix: 4-point Hermite interpolation, R246's per-block track gains (tm_g, from its mix loops), saturating sums.
 * shortcut: no output filter / start-stop ramp of R246's player yet; upgrade if start / stop clicks on the device. */
#include <stdint.h>

#define TM_SLOTS   32
#define TM_CHUNKS  0x1E5                                         /* chunks per track (R246) */
#define CHUNK      0x8000                                        /* samples */

typedef struct { int32_t speed; int32_t lin, lout; uint8_t lon, lset; } tm_track;   /* lset: in set (marker shown) */   /* speed Q16 (negative = reverse); own loop in / out (samples), on */
volatile tm_track tm_tr[4] = { { 0x10000, 0, 0, 0, 0 }, { 0x10000, 0, 0, 0, 0 }, { 0x10000, 0, 0, 0, 0 }, { 0x10000, 0, 0, 0, 0 } };
#ifdef TAPE_FREE
volatile uint8_t tm_on = 1;                                      /* test builds (OP1R_TAPEFREE=1): on */
#else
volatile uint8_t tm_on;                                          /* the free machine selected */
#endif

#ifdef OP1_HOST
extern uint8_t host_tm_xport[0x80]; extern int32_t host_tm_loop[2]; extern uint8_t host_tm_loopon;
extern int16_t host_tm_out[2][64]; extern void *host_tm_malloc(unsigned);
extern int host_tm_read(int t, int chunk, int16_t *dst);         /* fills 64 KB, returns 1 */
#define X          host_tm_xport
#define LOOP_S     host_tm_loop[0]
#define LOOP_E     host_tm_loop[1]
#define LOOP_ON    host_tm_loopon
#define OUT_L      host_tm_out[0]
#define OUT_R      host_tm_out[1]
#define MALLOC     host_tm_malloc
static int16_t host_bus[64];
#define BUS        host_bus
#else
#define X          ((volatile uint8_t *)0xFF802960)              /* R246's transport (docs/29) */
#define LOOP_S     (*(volatile int32_t *)0x01B4EF34)
#define LOOP_E     (*(volatile int32_t *)0x01B4EF38)
#define LOOP_ON    (*(volatile uint8_t *)0xFF8029C5)             /* loop on (docs/26; 0x018C17F4 is not it) */
#define OUT_L      ((volatile int16_t *)0xFF800D34)
#define OUT_R      ((volatile int16_t *)0xFF800DB4)
#define MALLOC     ((void *(*)(unsigned))0x01A9F348)             /* heap 0 */
#define BUS        ((const volatile int16_t *)0xFF800BB4)        /* the instrument bus R246 records */
#define Y_LSEEK    ((int (*)(int, int, int))0x01A9A0E4)
#define Y_READ     ((int (*)(int, void *, unsigned))0x01A99C90)
#define TAPE_FD    (*(volatile int *)0x01B4EF44)
#define TRACK_BASE ((volatile int *)0x018C1810)
#define CLIP_IDX   (*(uint8_t **volatile *)0x01B4EF48)           /* chunk -> R246 clip entry */
#endif
#define MODE       (*(volatile int32_t *)(X + 0x00))             /* 0 stop, 1 rec, 2 play, 3 wind */
#define TSPEED     (*(volatile int32_t *)(X + 0x08))             /* 1x = 0x08000000 (emulator: 64 samples per block); winding 0x40000000 = 8x */
#define TPOS       (*(volatile int32_t *)(X + 0x14))             /* samples << 6 */
/* R246's per-block track gains (a stack array in tape_play_mix: L[4], R[4]), copied by op1r's mix-loop hooks (hook.S
 * mix_hook_fwd / rev); X + 0x38 / 0x40 are only inputs to that computation (docs/82) */
volatile int16_t tm_g[8] = { 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767 };
#define GAIN_L(t)  (tm_g[t])
#define GAIN_R(t)  (tm_g[4 + (t)])

typedef struct { int16_t *data; volatile int16_t t, chunk; volatile uint8_t state; volatile uint8_t gen; uint32_t used; } tm_slot;
static tm_slot slot[TM_SLOTS];                                   /* state 0 free, 1 wanted, 2 loaded */
static int8_t map[4][TM_CHUNKS];                                 /* chunk -> slot + 1 (0 none) */
static int64_t head[4];
static uint32_t blk;
static uint8_t rec_seen;
volatile int8_t tm_scrub;                                        /* -1 / +1: < / > held on our screen (tfscreen.c) */
volatile uint8_t tm_scrub_track;                                 /* the active track: only it scrubs (and sounds) */
static int32_t scrub_v;                                          /* current scrub speed, Q16 (ramps to +-8x) */
int tm_scrubbing(void) { return scrub_v != 0; }
volatile uint32_t tm_miss, tm_loads;

static int tm_alloc(void)
{
    if (slot[0].data) return 1;
    int16_t *m = (int16_t *)MALLOC(TM_SLOTS * CHUNK * 2);
    if (!m) return 0;
    for (int i = 0; i < TM_SLOTS; i++) { slot[i].data = m + i * CHUNK; slot[i].t = -1; slot[i].state = 0; }
    return 1;
}

/* the slot of (t, chunk), requesting it if missing; the data only if loaded */
static const int16_t *tm_chunk(int t, int c, int want)
{
    if (c < 0 || c >= TM_CHUNKS) return 0;
    int s = map[t][c] - 1;
    if (s >= 0) { slot[s].used = blk; return slot[s].state == 2 ? slot[s].data : 0; }
    if (!want) return 0;
    int v = -1; uint32_t age = 0;                               /* LRU slot not used this block */
    for (int i = 0; i < TM_SLOTS; i++) {
        if (slot[i].t < 0) { v = i; break; }
        if (blk - slot[i].used > age && slot[i].used != blk) { age = blk - slot[i].used; v = i; }
    }
    if (v < 0) return 0;
    if (slot[v].t >= 0) map[slot[v].t][slot[v].chunk] = 0;
    slot[v].state = 1; slot[v].gen++;
    slot[v].t = (int16_t)t; slot[v].chunk = (int16_t)c; slot[v].used = blk;
    map[t][c] = (int8_t)(v + 1);
    return 0;
}

static inline int16_t tm_sample(int t, int32_t i, const int16_t **cache, int32_t *cache_c)
{
    const int32_t c = i >> 15;
    if (c != *cache_c) { *cache_c = c; *cache = tm_chunk(t, c, 1); }
    if (!*cache) { tm_miss++; return 0; }
    return (*cache)[i & (CHUNK - 1)];
}

static void tm_resync(int32_t pos)
{
    for (int t = 0; t < 4; t++) {
        int32_t h = pos;
        const volatile tm_track *tr = &tm_tr[t];
        if (tr->lon && tr->lout > tr->lin && (pos < tr->lin || pos >= tr->lout)) {
            const int32_t len = tr->lout - tr->lin;
            int32_t r = (pos - tr->lin) % len; if (r < 0) r += len;
            h = tr->lin + r;
        }
        head[t] = (int64_t)h << 32;
    }
}

static int32_t xlast = -1;                                       /* transport position last block (jump detection) */
volatile int16_t tm_in_peak;                                     /* the recorded signal (the instrument bus) for the meter */
void tm_set_head(int t, int32_t pos) { head[t & 3] = (int64_t)(pos < 0 ? 0 : pos) << 32; }   /* GUI: bar jumps */

static int64_t rec_h0[4], rec_step[4], rec_lo[4], rec_hi[4];    /* this block's head path per track (for recording) */
static void tm_track_block(int t, int64_t step, int loops, int gain_shift, int32_t *accL, int32_t *accR)
{
    int64_t h = head[t];
    int64_t lo = 0, hi = (int64_t)(TM_CHUNKS * CHUNK) << 32;
    if (loops && tm_tr[t].lon && tm_tr[t].lout > tm_tr[t].lin) {
        lo = (int64_t)tm_tr[t].lin << 32; hi = (int64_t)tm_tr[t].lout << 32;
        if (h < lo || h >= hi) { int64_t r = (h - lo) % (hi - lo); if (r < 0) r += hi - lo; h = lo + r; }   /* loop just set: fold in */
    }
    rec_h0[t] = h; rec_step[t] = step; rec_lo[t] = lo; rec_hi[t] = hi;
    const int16_t gl = GAIN_L(t), gr = GAIN_R(t);
    const int16_t *cache = 0; int32_t cache_c = -1;
    for (int i = 0; i < 64; i++) {
        if (h >= hi) h -= hi - lo; else if (h < lo) h += hi - lo;
        const int32_t n = (int32_t)(h >> 32), f = (int32_t)((uint64_t)h >> 20 & 0xFFF);   /* fraction Q12 */
        const int32_t sm = tm_sample(t, n - 1, &cache, &cache_c), s0 = tm_sample(t, n, &cache, &cache_c),
                      s1 = tm_sample(t, n + 1, &cache, &cache_c), s2 = tm_sample(t, n + 2, &cache, &cache_c);
        /* 4-point Hermite (Catmull-Rom): coefficients up to ~2^18, x Q12 stays in 32 bits */
        const int32_t c1 = (s1 - sm) >> 1, c2 = sm - (5 * s0 >> 1) + 2 * s1 - (s2 >> 1),
                      c3 = ((s2 - sm) >> 1) + (3 * (s0 - s1) >> 1);
        int32_t s = ((((c3 * f >> 12) + c2) * f >> 12) + c1) * f >> 12;
        s += s0; s = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
        accL[i] += s * gl >> gain_shift; accR[i] += s * gr >> gain_shift;
        h += step;
        if (h >= hi) h -= hi - lo; else if (h < lo) h += hi - lo;   /* the stored head stays inside its loop */
    }
    if (h < 0) h = 0;
    head[t] = h;
    const int32_t c = (int32_t)(h >> 32) >> 15, d = step < 0 ? -1 : 1;   /* prefetch ahead, and the loop start */
    for (int k = 1; k <= 3; k++) tm_chunk(t, c + k * d, 1);
    if (hi - lo < ((int64_t)(TM_CHUNKS * CHUNK) << 32)) tm_chunk(t, (int32_t)((d > 0 ? lo : hi - 1) >> 32) >> 15, 1);
}

void tm_block(void)                                              /* audio ISR, after tape_play_mix */
{
    blk++;
    {                                                            /* the meter: the bus R246 records (last block's) */
        int32_t pk = tm_in_peak * 31 >> 5;
        const volatile int16_t *b = BUS;
        for (int i = 0; i < 64; i++) { int32_t v = b[i] < 0 ? -b[i] : b[i]; if (v > pk) pk = v; }
        tm_in_peak = (int16_t)(pk > 32767 ? 32767 : pk);
    }
    if (!tm_on || !slot[0].data) return;                         /* memory comes from tm_service (main loop) */
    const int32_t mode = MODE, pos = TPOS >> 6, v = TSPEED;
    if (mode == 1) rec_seen = 1;                                 /* recording (R246's machine) changes chunks we may hold */
    {                                                            /* the transport jumped (locate, marker): heads resync */
        int32_t d = xlast < 0 ? 0x7FFFFFFF : pos - (xlast + ((mode == 1 || mode == 2) ? v >> 21 : 0));
        if (d != 0x7FFFFFFF && LOOP_ON && LOOP_E > LOOP_S) {     /* the transport's own loop wrap is no jump */
            const int32_t L = LOOP_E - LOOP_S;
            d %= L; if (d > L / 2) d -= L; else if (d < -L / 2) d += L;
        }
        if (d > 1024 || d < -1024) tm_resync(pos);
        xlast = pos;
    }
    {                                                            /* scrubbing: spin up / down to +-8x like stock winding */
        const int32_t target = tm_scrub * (8 << 16), dv = 0x2000;
        scrub_v = scrub_v < target ? (target - scrub_v < dv ? target : scrub_v + dv) : (scrub_v - target < dv ? target : scrub_v - dv);
    }
    const int playing = mode == 1 || mode == 2, sc = tm_scrub_track & 3;
    if (!playing && !scrub_v) return;                            /* stopped: R246's (silent) output stays */
    if (playing && rec_seen && mode == 2) {                      /* first play after a recording: reload every chunk */
        rec_seen = 0;
        for (int i = 0; i < TM_SLOTS; i++) if (slot[i].t >= 0) { slot[i].state = 1; slot[i].gen++; }
    }
    int32_t accL[64], accR[64];
    for (int i = 0; i < 64; i++) accL[i] = accR[i] = 0;
    for (int t = 0; t < 4; t++) {
        if (scrub_v && t == sc) tm_track_block(t, (int64_t)scrub_v << 16, 0, 16, accL, accR);   /* -6 dB, no loops */
        else if (playing) tm_track_block(t, ((int64_t)v * tm_tr[t].speed) >> 11, 1, 15, accL, accR);   /* (1x = 2^27) x Q16 -> Q32 */
    }
    for (int i = 0; i < 64; i++) {
        OUT_L[i] = (int16_t)(accL[i] > 32767 ? 32767 : accL[i] < -32768 ? -32768 : accL[i]);
        OUT_R[i] = (int16_t)(accR[i] > 32767 ? 32767 : accR[i] < -32768 ? -32768 : accR[i]);
    }
}

/* ---- recording (FREE): the active track at its own head / speed / loop (docs/82) ----
 * In place of R246's tape_record_block (both CALLs -> hook.S l1_tramp_rec -> tm_rec_entry): the bus x the record level
 * onto the tape positions the head crossed this block (as played, tm_track_block), linear between the bus samples,
 * overdub sat16(old + new), into R246's clip cache entry of the chunk (dirty + track bit: tape_c::service saves it)
 * and our read slot. A chunk not in R246's cache is queued and that part skipped (as R246 does). The take: range
 * reset at record start (0x01A54C1C), widened per block (0x01A54BB4): R246 closes it (Clips) on stop, onto its
 * selected track (= ours: tfscreen.c sets it). */
#ifndef OP1_HOST
#define REC_LEVEL  (*(volatile int16_t *)0x01B4EF6E)
#define SEL_TRACK  (*(volatile int32_t *)0xFF802964)
#define QUEUE_CLIP ((void (*)(int))0x01A54CD4)
#define RANGE_ADD  ((void (*)(int32_t))0x01A54BB4)
#define RANGE_RST  ((void (*)(void))0x01A54C1C)
#define REC_STOCK  ((void (*)(void *))0xFFA06334)
volatile uint32_t tm_rec_written, tm_rec_missed;
static uint8_t rec_on;
static int16_t rec_fade;
static void tm_record(const volatile int16_t *bus)
{
    const int t = SEL_TRACK & 3;
    if (MODE != 1) { rec_on = 0; return; }
    if (!rec_on) { rec_on = 1; rec_fade = 0; RANGE_RST(); }
    const int64_t step = rec_step[t], lo = rec_lo[t], hi = rec_hi[t];
    if (!step) return;
    const int32_t lvl = REC_LEVEL < 0 ? 0 : REC_LEVEL;
    int32_t in[65];
    for (int i = 0; i < 64; i++) {                               /* level, fade-in over the first block */
        int32_t s = bus[i] * lvl >> 15;
        if (rec_fade < 0x7FFF) { s = s * rec_fade >> 15; rec_fade = (int16_t)(rec_fade + 512 > 0x7FFF ? 0x7FFF : rec_fade + 512); }
        in[i] = s;
    }
    in[64] = in[63];
    const int64_t ast = step < 0 ? -step : step;
    const uint32_t inv = (uint32_t)((((uint64_t)1 << 47) / (uint64_t)ast));   /* 2^15 / |step| in samples, Q32 -> frac Q15 */
    int64_t h = rec_h0[t];
    int32_t first = -1, last = -1;
    const uint8_t *e = 0; int32_t ec = -1; int16_t *slot_d = 0;
    for (int i = 0; i < 64; i++) {
        const int64_t h1 = h + step;
        int32_t n = step > 0 ? (int32_t)((h + 0xFFFFFFFFLL) >> 32) : (int32_t)(h >> 32), nend = (int32_t)(h1 >> 32);
        for (int g = 0; g < 8; g++) {                            /* the integer positions crossed (at most 8: up to 4x + slack) */
            if (step > 0 ? ((int64_t)n << 32) >= h1 : ((int64_t)n << 32) <= h1) break;
            int32_t pn = n;
            int64_t q = (int64_t)pn << 32;
            if (q >= hi) pn -= (int32_t)((hi - lo) >> 32); else if (q < lo) pn += (int32_t)((hi - lo) >> 32);
            const uint32_t fr = (uint32_t)(((uint64_t)(step > 0 ? ((int64_t)n << 32) - h : h - ((int64_t)n << 32)) >> 17) * inv >> 15) & 0x7FFF;
            const int32_t v = in[i] + ((in[i + 1] - in[i]) * (int32_t)fr >> 15);
            const int32_t c = pn >> 15;
            if (c != ec) {                                       /* R246's clip cache entry of this chunk, our slot */
                ec = c; e = CLIP_IDX ? CLIP_IDX[c] : 0;
                if (!(e && e[0] && *(const int16_t *)(e + 6) == c)) { e = 0; QUEUE_CLIP(c); }
                const int s = c >= 0 && c < TM_CHUNKS ? map[t][c] - 1 : -1;
                slot_d = s >= 0 && slot[s].state == 2 ? slot[s].data : 0;
            }
            if (e) {
                int16_t *d = (int16_t *)(*(uint8_t *const *)(e + 0x10) + t * 0x10000) + (pn & (CHUNK - 1));
                int32_t o = *d + v; o = o > 32767 ? 32767 : o < -32768 ? -32768 : o;
                *d = (int16_t)o;
                if (slot_d) slot_d[pn & (CHUNK - 1)] = (int16_t)o;
                ((volatile uint8_t *)e)[2] = 1; *(volatile uint16_t *)(e + 4) |= (uint16_t)(1u << t);
                if (first < 0) first = pn;
                last = pn;
                tm_rec_written++;
            } else tm_rec_missed++;
            n += step > 0 ? 1 : -1;
        }
        (void)nend;
        h = h1;
        if (h >= hi) h -= hi - lo; else if (h < lo) h += hi - lo;
    }
    if (first >= 0) { RANGE_ADD(first); RANGE_ADD(last); }
    { const int32_t c = (int32_t)(h >> 32) >> 15, d = step < 0 ? -1 : 1; QUEUE_CLIP(c); QUEUE_CLIP(c + d); }   /* ahead */
}
void tm_rec_entry(void *bus)                                     /* audio ISR, in place of tape_record_block (hook.S) */
{
    if (!tm_on || !slot[0].data) { REC_STOCK(bus); return; }
    tm_record((const volatile int16_t *)bus);
}
#endif

int32_t tm_head_pos(int t) { return (int32_t)(head[t] >> 32); }   /* GUI: head in samples */
int tm_span(int t, int32_t *lo, int32_t *hi)                     /* GUI: the track's own loop (1 if on) */
{
    if (!(tm_tr[t].lon && tm_tr[t].lout > tm_tr[t].lin)) return 0;
    *lo = tm_tr[t].lin; *hi = tm_tr[t].lout;
    return 1;
}

int tm_service(void)                                             /* main loop (tape_service): one chunk per call */
{
    if (!tm_on) return 0;
    if (!tm_alloc()) return 0;
    for (int i = 0; i < TM_SLOTS; i++) {
        tm_slot *s = &slot[i];
        if (s->state != 1) continue;
        const uint8_t gen = s->gen; const int t = s->t, c = s->chunk;
#ifdef OP1_HOST
        host_tm_read(t, c, s->data);
#else
        const uint8_t *e = CLIP_IDX ? CLIP_IDX[c] : 0;           /* R246's clip cache holds it (fresh recordings) */
        if (e && e[0] && *(const int16_t *)(e + 6) == c) {
            const int16_t *src = (const int16_t *)(*(uint8_t *const *)(e + 0x10) + t * 0x10000);
            for (int k = 0; k < CHUNK; k++) s->data[k] = src[k];
        } else {
            Y_LSEEK(TAPE_FD, TRACK_BASE[t] + c * 0x10000, 0);
            Y_READ(TAPE_FD, s->data, 0x10000);
        }
#endif
        if (s->gen == gen) s->state = 2;                         /* reassigned meanwhile: stays wanted */
        tm_loads++;
        return 1;
    }
    return 0;
}
