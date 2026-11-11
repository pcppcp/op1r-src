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
#else
#define X          ((volatile uint8_t *)0xFF802960)              /* R246's transport (docs/29) */
#define LOOP_S     (*(volatile int32_t *)0x01B4EF34)
#define LOOP_E     (*(volatile int32_t *)0x01B4EF38)
#define LOOP_ON    (*(volatile uint8_t *)0xFF8029C5)             /* loop on (docs/26; 0x018C17F4 is not it) */
#define OUT_L      ((volatile int16_t *)0xFF800D34)
#define OUT_R      ((volatile int16_t *)0xFF800DB4)
#define MALLOC     ((void *(*)(unsigned))0x01A9F348)             /* heap 0 */
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
static int32_t last_pos = -1;
static uint32_t blk;
static uint8_t rec_seen;
volatile int8_t tm_scrub;                                        /* -1 / +1: < / > held on our screen (tfscreen.c) */
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

/* scrubbing: every head at scrub_v (no loops), the tracks mixed as in play (R246's player is stopped or winding) */
static void tm_scrub_block(void)
{
    int32_t accL[64], accR[64];
    for (int i = 0; i < 64; i++) accL[i] = accR[i] = 0;
    const int64_t step = (int64_t)scrub_v << 16;
    for (int t = 0; t < 4; t++) {
        int64_t h = head[t];
        const int16_t gl = GAIN_L(t), gr = GAIN_R(t);
        const int16_t *cache = 0; int32_t cache_c = -1;
        for (int i = 0; i < 64; i++) {
            if (h < 0) h = 0;
            const int32_t n = (int32_t)(h >> 32), f = (int32_t)((uint64_t)h >> 20 & 0xFFF);
            const int32_t s0 = tm_sample(t, n, &cache, &cache_c), s1 = tm_sample(t, n + 1, &cache, &cache_c);
            const int32_t s = s0 + ((s1 - s0) * f >> 12);        /* linear: scrubbing only */
            accL[i] += s * gl >> 16; accR[i] += s * gr >> 16;     /* -6 dB, as winding is quieter */
            h += step;
        }
        head[t] = h < 0 ? 0 : h;
        const int32_t c = (int32_t)(h >> 32) >> 15, d = step < 0 ? -1 : 1;
        for (int k = 1; k <= 3; k++) tm_chunk(t, c + k * d, 1);
    }
    for (int i = 0; i < 64; i++) {
        OUT_L[i] = (int16_t)(accL[i] > 32767 ? 32767 : accL[i] < -32768 ? -32768 : accL[i]);
        OUT_R[i] = (int16_t)(accR[i] > 32767 ? 32767 : accR[i] < -32768 ? -32768 : accR[i]);
    }
}

void tm_block(void)                                              /* audio ISR, after tape_play_mix */
{
    blk++;
    if (!tm_on || !slot[0].data) return;                         /* memory comes from tm_service (main loop) */
    const int32_t mode = MODE, pos = TPOS >> 6;
    if (mode == 1) rec_seen = 1;                                 /* recording (R246's machine) changes chunks we may hold */
    {                                                            /* scrubbing: spin up / down to +-8x like stock winding */
        const int32_t target = tm_scrub * (8 << 16), dv = 0x2000;      /* 1/8x per block: ~90 ms to 8x */
        scrub_v = scrub_v < target ? (target - scrub_v < dv ? target : scrub_v + dv) : (scrub_v - target < dv ? target : scrub_v - dv);
    }
    if (scrub_v) { tm_scrub_block(); last_pos = -1; return; }
    if (mode != 2) { tm_resync(pos); last_pos = -1; return; }    /* playing only; stopped / winding: the heads follow the transport */
    if (rec_seen) {                                              /* first play after a recording: reload every chunk */
        rec_seen = 0;
        for (int i = 0; i < TM_SLOTS; i++) if (slot[i].t >= 0) { slot[i].state = 1; slot[i].gen++; }
    }
    const int32_t v = TSPEED;
    int32_t d = last_pos < 0 ? 0x7FFFFFFF : pos - last_pos;
    if (d != 0x7FFFFFFF && LOOP_ON && LOOP_E > LOOP_S) {         /* the transport's own loop wrap is no locate */
        const int32_t L = LOOP_E - LOOP_S;
        d %= L; if (d > L / 2) d -= L; else if (d < -L / 2) d += L;
    }
    if (d > 1024 || d < -1024) tm_resync(pos);                   /* (re)start, locate */
    last_pos = pos;
    int32_t accL[64], accR[64];
    for (int i = 0; i < 64; i++) accL[i] = accR[i] = 0;
    for (int t = 0; t < 4; t++) {
        const int64_t step = ((int64_t)v * tm_tr[t].speed) >> 11;       /* (1x = 2^27) x Q16 -> Q32 */
        int64_t h = head[t];
        int64_t lo = 0, hi = (int64_t)(TM_CHUNKS * CHUNK) << 32;
        if (tm_tr[t].lon && tm_tr[t].lout > tm_tr[t].lin) {
            lo = (int64_t)tm_tr[t].lin << 32; hi = (int64_t)tm_tr[t].lout << 32;
            if (h < lo || h >= hi) { int64_t r = (h - lo) % (hi - lo); if (r < 0) r += hi - lo; h = lo + r; }   /* loop just set: fold in */
        }
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
            accL[i] += s * gl >> 15; accR[i] += s * gr >> 15;
            h += step;
            if (h >= hi) h -= hi - lo; else if (h < lo) h += hi - lo;   /* the stored head stays inside its loop */
        }
        head[t] = h;
        const int32_t c = (int32_t)(h >> 32) >> 15, d = step < 0 ? -1 : 1;   /* prefetch ahead, and the loop start */
        for (int k = 1; k <= 3; k++) tm_chunk(t, c + k * d, 1);
        if (hi - lo < ((int64_t)(TM_CHUNKS * CHUNK) << 32)) tm_chunk(t, (int32_t)((d > 0 ? lo : hi - 1) >> 32) >> 15, 1);
    }
    for (int i = 0; i < 64; i++) {
        OUT_L[i] = (int16_t)(accL[i] > 32767 ? 32767 : accL[i] < -32768 ? -32768 : accL[i]);
        OUT_R[i] = (int16_t)(accR[i] > 32767 ? 32767 : accR[i] < -32768 ? -32768 : accR[i]);
    }
}

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
