/* Free tape machine, v1 (docs/82): the four tracks play with their own speed, loop length, offset and direction.
 * R246's tape player keeps running (transport, position, loop markers, UI); while free is selected and the tape plays,
 * tm_block (audio ISR, from fr_snap right after tape_play_mix) overwrites its output L / R 0xFF800D34 / DB4 with ours.
 * Data: our own single-track chunk cache (TM_SLOTS x 64 KB from heap 0, only while free is on), filled by tm_service
 * from op1r's tape_service (main loop): a chunk R246's clip cache holds is copied from there (fresh recordings),
 * else read from tape_c.raw (R246's layout: track t, chunk c at TRACK_BASE[t] + c * 0x10000).
 * Heads: Q32.32 samples. With R246's loop on, track t loops in [Ls + off_t L, + loop_t L) of the global loop
 * [Ls, Le) (L = Le - Ls); without it the heads run free from the transport position. Each block every head moves
 * by the transport speed x the track's speed; a locate or a (re)start resyncs them to the transport.
 * Mix: linear interpolation, R246's per-track gains (L 0xFF802998 + 2t, R 0xFF8029A0 + 2t), saturating sums.
 * shortcut: v1 has no output filter / start-stop ramp of R246's player and linear interpolation only; upgrade
 * (R246's tail filter, a better interpolator) once the engine is confirmed on the device. */
#include <stdint.h>

#define TM_SLOTS   32
#define TM_CHUNKS  0x1E5                                         /* chunks per track (R246) */
#define CHUNK      0x8000                                        /* samples */

typedef struct { int32_t speed; uint16_t loop, off; } tm_track;   /* speed Q16 (negative = reverse); loop / off Q16 of the global loop */
volatile tm_track tm_tr[4] = { { 0x10000, 0xFFFF, 0 }, { 0x10000, 0xFFFF, 0 }, { 0x10000, 0xFFFF, 0 }, { 0x10000, 0xFFFF, 0 } };
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
#define LOOP_ON    (*(volatile uint8_t *)0x018C17F4)
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
#define TSPEED     (*(volatile int32_t *)(X + 0x08))             /* Q2.30, 1x = 0x40000000 */
#define TPOS       (*(volatile int32_t *)(X + 0x14))             /* samples << 6 */
#define GAIN_L(t)  (((volatile int16_t *)(X + 0x38))[t])
#define GAIN_R(t)  (((volatile int16_t *)(X + 0x40))[t])

typedef struct { int16_t *data; volatile int16_t t, chunk; volatile uint8_t state; volatile uint8_t gen; uint32_t used; } tm_slot;
static tm_slot slot[TM_SLOTS];                                   /* state 0 free, 1 wanted, 2 loaded */
static int8_t map[4][TM_CHUNKS];                                 /* chunk -> slot + 1 (0 none) */
static int64_t head[4];
static int32_t last_pos = -1;
static uint32_t blk;
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
        if (LOOP_ON && LOOP_E > LOOP_S) {
            const int32_t L = LOOP_E - LOOP_S, base = LOOP_S + (int32_t)((int64_t)L * tm_tr[t].off >> 16);
            const int32_t len = (int32_t)((int64_t)L * tm_tr[t].loop >> 16) + 1;
            int32_t r = (pos - LOOP_S) % len; if (r < 0) r += len;
            h = base + r;
        }
        head[t] = (int64_t)h << 32;
    }
}

void tm_block(void)                                              /* audio ISR, after tape_play_mix */
{
    blk++;
    if (!tm_on || !slot[0].data) return;                         /* memory comes from tm_service (main loop) */
    const int32_t mode = MODE, pos = TPOS >> 6;
    if (mode != 2) { last_pos = -1; return; }                    /* playing only (v1); else R246's output stays */
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
        const int64_t step = ((int64_t)v * tm_tr[t].speed) >> 14;       /* Q30 x Q16 -> Q32 */
        int64_t h = head[t];
        int64_t lo = 0, hi = (int64_t)(TM_CHUNKS * CHUNK) << 32;
        if (LOOP_ON && LOOP_E > LOOP_S) {
            const int32_t L = LOOP_E - LOOP_S, base = LOOP_S + (int32_t)((int64_t)L * tm_tr[t].off >> 16);
            lo = (int64_t)base << 32; hi = lo + (((int64_t)L * tm_tr[t].loop >> 16) + 1 << 32);
        }
        const int16_t gl = GAIN_L(t), gr = GAIN_R(t);
        const int16_t *cache = 0; int32_t cache_c = -1;
        for (int i = 0; i < 64; i++) {
            if (h >= hi) h -= hi - lo; else if (h < lo) h += hi - lo;
            const int32_t n = (int32_t)(h >> 32), f = (int32_t)((uint64_t)h >> 17 & 0x7FFF);
            const int32_t s0 = tm_sample(t, n, &cache, &cache_c), s1 = tm_sample(t, n + 1, &cache, &cache_c);
            const int32_t s = s0 + ((s1 - s0) * f >> 15);
            accL[i] += s * gl >> 15; accR[i] += s * gr >> 15;
            h += step;
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
