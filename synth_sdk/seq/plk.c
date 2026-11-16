/* Parameter-lock recording on the sequencer pages (docs/87): per mode (synth / drum) one clip of up to 14 lanes, the
 * current slot's engine and FX knob words (0..3 page 1, 5..7 page 2), recorded over a length (1/4 .. 16 bars) while
 * the sequencer plays and played back in a loop, linear between the recorded points.
 *
 * Clock: the core's player proxy (dispatcher.c seq_player_get) calls plk_start (player reset: the clip restarts),
 * plk_tick (1/96 beat) and plk_stop on the sequencer thread. The sequencer clock runs all the time (emulator: also
 * without PLAY), so the clip loops all the time; synced recording starts at the clip's next start, free at once. Words: written live (as dots' locks, docs/17); a word's
 * value before the first write (its base) comes back at stop. Recording: each tick the declared words are compared with
 * the last value seen or written here, so a change is a turn (any page, MIDI CC), never our own playback. A lane is
 * replaced only if it changed during the pass (overdub per lane). A lane plays only while the slot has the engine /
 * FX it was recorded with. Host: harness/plk_test.c (PLK_HOST words). */
#include "../op1_synth.h"
#include "plk.h"

#ifdef OP1_HOST
int16_t plk_host_words[2][2][8];                                 /* [mode][group 0 engine, 1 fx][word] */
int32_t plk_host_type[2][2];                                     /* engine / fx id per mode and group */
uint16_t plk_host_mask[2][2];                                    /* declared word masks */
static int16_t *word_ptr(int mode, int g, int w) { return &plk_host_words[mode][g][w]; }
static int32_t type_id(int mode, int g) { return plk_host_type[mode][g]; }
static uint16_t lane_mask(int mode, int g) { return plk_host_mask[mode][g]; }
static uint32_t irq_off(void) { return 0; }
static void irq_on(uint32_t m) { (void)m; }
#else
extern uint16_t op1_engine_plock[], op1_fx_plock[];               /* plugload.c: 0x100 declared | word mask */
extern int mx_engine_plugin(int n), mx_fx_plugin(int n);         /* dispatcher.c: the plugin with that number, or 0 */
#define SLOT(m)      (*(volatile int *)((m) ? 0xFF8016D8 : 0xFF8016D4) & 7)
static int16_t *words(int m, int g)
{
    const int s = SLOT(m);
    if (g == 0) return m ? 0 : (int16_t *)(0xFF8013C4 + s * 16);  /* engine words: synth mode only */
    return (int16_t *)((m ? 0xFF8015C4 : 0xFF801444) + s * 16);
}
static int16_t *word_ptr(int m, int g, int w) { int16_t *p = words(m, g); return p ? p + w : 0; }
static int32_t type_id(int m, int g)                              /* the engine / FX type and the plugin number on it */
{
    const int s = SLOT(m);
    const int16_t *p = words(m, g);
    if (!p) return -1;
    const int t = g == 0 ? ((volatile int *)0xFF8016E4)[s] : ((volatile int *)(m ? 0xFF801754 : 0xFF801734))[s];
    return t << 16 | (uint16_t)(p[4] - OP1_ENGINE_MAGIC);
}
static uint16_t lane_mask(int m, int g)                           /* declared words, else page 1 (engines: clamping ones) */
{
    const int16_t *p = words(m, g);
    if (!p) return 0;
    const int n = p[4] - OP1_ENGINE_MAGIC;
    const int pl = g == 0 ? mx_engine_plugin(n) : mx_fx_plugin(n);
    if (pl) { const uint16_t d = g == 0 ? op1_engine_plock[pl] : op1_fx_plock[pl]; if (d & 0x100) return d & 0xEF; }
    if (g == 1) return 0x0F;
    const int eng = ((volatile int *)0xFF8016E4)[SLOT(m)];
    if (eng < 0 || eng > 31) return 0;
    uint16_t k = 0;                                               /* range mode 0 (clamp) only: no linked / wrapping knobs */
    for (int i = 0; i < 4; i++) if (((const int16_t *)(0x01DE5890 + eng * 0x40 + i * 8))[3] == 0) k |= (uint16_t)(1 << i);
    return k;
}
static uint32_t irq_off(void) { uint32_t m; __asm__ volatile ("cli %0;" : "=d"(m) : : "memory"); return m; }
static void irq_on(uint32_t m) { __asm__ volatile ("sti %0;" : : "d"(m) : "memory"); }
#endif

static const uint8_t lane_word[7] = { 0, 1, 2, 3, 5, 6, 7 };
const uint16_t plk_len_ticks[PLK_NLEN] = { 96, 192, 384, 768, 1536, 3072, 6144 };   /* 1/4 .. 16 bars of 4 beats */
plk_clip_t plk_clip[2] = { { .len_i = 3, .sync = 1 }, { .len_i = 3, .sync = 1 } };   /* 2 bars, synced */
plk_run_t plk_run[2];

int plk_lane_group(int l) { return l / 7; }
int plk_lane_word(int l) { return lane_word[l % 7]; }
uint16_t plk_lanes(int m)                                         /* lanes available now (bit l) */
{
    return (uint16_t)(((lane_mask(m, 0) & 0x0F) | ((lane_mask(m, 0) >> 1) & 0x70))
                    | (((lane_mask(m, 1) & 0x0F) | ((lane_mask(m, 1) >> 1) & 0x70)) << 7));
}
static int32_t len_ticks(int m) { return plk_len_ticks[plk_clip[m].len_i < PLK_NLEN ? plk_clip[m].len_i : 2]; }

/* the lane's value at tick t: linear between its events (cursor: the event at or before t, moved forward) */
static int lane_value(const plk_lane_t *ln, int32_t t, uint16_t *cur)
{
    if (!ln->n) return ln->start;
    int i = *cur < ln->n && ln->ev[*cur].t <= t ? *cur : 0;
    while (i + 1 < ln->n && ln->ev[i + 1].t <= t) i++;
    *cur = (uint16_t)i;
    if (ln->ev[i].t > t) return ln->start;
    if (i + 1 >= ln->n) return ln->ev[i].v;
    const int32_t t0 = ln->ev[i].t, t1 = ln->ev[i + 1].t, v0 = ln->ev[i].v, v1 = ln->ev[i + 1].v;
    if (t1 - t0 <= 1) return v0;                                   /* (tick-rate steps play as recorded) */
    return (int)(v0 + (v1 - v0) * (t - t0) / (t1 - t0));
}

static void put(int m, int l, int v)                              /* write a lane's word (base kept at the first write) */
{
    plk_run_t *r = &plk_run[m];
    int16_t *w = word_ptr(m, plk_lane_group(l), plk_lane_word(l));
    if (!w) return;
    if (!(r->based >> l & 1)) { r->base[l] = *w; r->based |= (uint16_t)(1 << l); }
    *w = (int16_t)v; r->last[l] = (int16_t)v;
}
static void revert(int m)                                         /* the bases back (stop, CUT, length / slot change) */
{
    plk_run_t *r = &plk_run[m];
    for (int l = 0; l < PLK_LANES; l++)
        if (r->based >> l & 1) { int16_t *w = word_ptr(m, plk_lane_group(l), plk_lane_word(l)); if (w) *w = r->base[l]; }
    r->based = 0;
}

static void rec_begin(int m)
{
    plk_run_t *r = &plk_run[m];
    r->state = PLK_REC; r->pos = 0; r->seen = 0;
    const uint16_t lanes = plk_lanes(m);
    for (int l = 0; l < PLK_LANES; l++) if (lanes >> l & 1) {
        int16_t *w = word_ptr(m, plk_lane_group(l), plk_lane_word(l));
        r->last[l] = w ? *w : 0;
    }
}

void plk_halt(int on)                                             /* STOP: the clips stop, bases back; PLAY: from the start */
{
    for (int m = 0; m < 2; m++) {
        const uint32_t k = irq_off();
        plk_run[m].halted = (uint8_t)on;
        if (on) plk_stop(m); else plk_run[m].pos = 0;
        irq_on(k);
    }
}
void plk_start(int m)                                             /* the sequence's first step (player reset) */
{
    plk_run_t *r = &plk_run[m & 1];
    r->running = 1;
    if (plk_clip[m & 1].sync) r->pos = 0;
    if (r->state == PLK_ARMED && plk_clip[m & 1].sync) rec_begin(m & 1);
}
void plk_stop(int m)                                              /* transport stop: bases back, a recording ends */
{
    plk_run_t *r = &plk_run[m & 1];
    r->running = 0;
    if (r->state == PLK_REC) r->state = PLK_IDLE;
    revert(m & 1);
}
void plk_rec(int m)                                               /* REC on the PLK page: arm / start / stop */
{
    plk_run[m & 1].halted = 0;                                    /* (after STOP: REC starts the clip again) */
    if (!plk_run[m & 1].running) plk_start(m);
    plk_run_t *r = &plk_run[m & 1];
    const uint32_t k = irq_off();
    if (r->state == PLK_IDLE) {
        if (plk_clip[m & 1].sync) r->state = PLK_ARMED;           /* waits for the first step (plk_start) */
        else rec_begin(m & 1);                                    /* free: now */
    } else { if (r->state == PLK_REC) plk_changed(); r->state = PLK_IDLE; }   /* armed: disarmed; recording: ends here */
    irq_on(k);
}
void plk_cut(int m, int lane)                                     /* lane < 0: all */
{
    const uint32_t k = irq_off();
    revert(m & 1);
    for (int l = 0; l < PLK_LANES; l++) if (lane < 0 || l == lane) plk_clip[m & 1].lane[l].n = 0, plk_clip[m & 1].lane[l].type = 0;
    irq_on(k);
    plk_changed();
}
void plk_set_len(int m, int len_i)
{
    const uint32_t k = irq_off();
    plk_clip[m & 1].len_i = (uint8_t)(len_i < 0 ? 0 : len_i >= PLK_NLEN ? PLK_NLEN - 1 : len_i);
    if (plk_run[m & 1].pos >= len_ticks(m & 1)) plk_run[m & 1].pos = 0;
    irq_on(k);
}

static void wrap(int m)                                           /* the clip is back at its start */
{
    plk_run_t *r = &plk_run[m];
    if (r->state == PLK_REC) { r->state = PLK_IDLE; plk_changed(); }   /* one pass: the clip loops from here */
    else if (r->state == PLK_ARMED) rec_begin(m);                 /* synced: at the clip's start */
}
void plk_tick(int m)                                              /* sequencer thread, 1/96 beat while it plays */
{
    m &= 1;
    plk_run_t *r = &plk_run[m];
    plk_clip_t *c = &plk_clip[m];
    if (r->halted) return;                                        /* STOP: until PLAY / REC */
    if (!r->running) plk_start(m);                                /* the first tick after a stop: the start */
    const int32_t len = len_ticks(m), t = r->pos;
    const int32_t sp = len / (PLK_EV / 2) > 1 ? len / (PLK_EV / 2) : 1;   /* event spacing: a pass fits half the events */
    int any = r->state == PLK_REC;
    for (int l = 0; l < PLK_LANES && !any; l++) any = c->lane[l].n != 0;
    const uint16_t lanes = any ? plk_lanes(m) : 0;                /* (nothing recorded, not recording: just the clock) */
    for (int l = 0; l < PLK_LANES; l++) {
        if (!(lanes >> l & 1)) continue;
        plk_lane_t *ln = &c->lane[l];
        if (!ln->n && r->state != PLK_REC) continue;
        int16_t *w = word_ptr(m, plk_lane_group(l), plk_lane_word(l));
        if (!w) continue;
        if (r->state == PLK_REC && *w != r->last[l]) {             /* a turn: recorded (the lane's first one replaces it) */
            if (!(r->seen >> l & 1)) {
                r->seen |= (uint16_t)(1 << l);
                ln->n = 0; ln->start = r->last[l]; ln->type = type_id(m, plk_lane_group(l));
                ln->ev[ln->n].t = 0; ln->ev[ln->n].v = r->last[l]; ln->n++;
            }
            if (ln->n >= 2 && t - ln->ev[ln->n - 1].t < sp) {   /* close to the last point: it moves (the clip's events
                                                                  * last a whole pass, also at 16 bars) */
                ln->ev[ln->n - 1].v = *w; r->last[l] = *w;
                continue;
            }
            if (ln->n && ln->ev[ln->n - 1].t + sp < t && ln->n < PLK_EV) {   /* after a pause: the held value up to here
                                                                  * (else playback would glide over the whole pause) */
                ln->ev[ln->n].t = (uint16_t)(t - 1); ln->ev[ln->n].v = r->last[l]; ln->n++;
            }
            if (ln->n < PLK_EV) { ln->ev[ln->n].t = (uint16_t)t; ln->ev[ln->n].v = *w; ln->n++; }
            r->last[l] = *w;
            continue;
        }
        if (r->state == PLK_REC && (r->seen >> l & 1)) continue;   /* being recorded: not played */
        if (!ln->n || ln->type != type_id(m, plk_lane_group(l))) continue;
        const int v = lane_value(ln, t, &r->cur[l]);
        if (v != *w || !(r->based >> l & 1)) put(m, l, v);
    }
    r->pos = t + 1 >= len ? 0 : t + 1;
    if (r->pos == 0) wrap(m);
}
volatile uint8_t plk_tracks[2];
void plk_set_tracks(int m, int n) { plk_tracks[m & 1] = (uint8_t)(n < 0 ? 0 : n > 16 ? 16 : n); }
void plk_track_restart(int m, int track)                          /* the clip follows that track: back to its start */
{                                                                 /* track | PLK_FIRST: its first step since started */
    m &= 1;
    plk_run_t *r = &plk_run[m];
    const int k = plk_clip[m].trk, t = (track & 0xFF) + 1;
    if (r->halted || !(k == t || (k == PLK_START + t && (track & PLK_FIRST)))) return;
    if (r->pos) { r->pos = 0; wrap(m); }                          /* (at 0 already: the clip's own wrap did it) */
}

/* ---- persistence (DB table plk, saved by the firmware's background saver like dots / euclid) ----
 * Row id mode * 16 + lane (0..13): hex type (8) start (4) n (4), then n x t (4) v (4); id mode * 16 + 15: len_i, sync. */
volatile uint8_t plk_save_req;
void plk_changed(void) { plk_save_req = 1; }
static const char hexd[] = "0123456789abcdef";
static char *hex(char *o, uint32_t v, int n) { while (n--) *o++ = hexd[(v >> (4 * n)) & 15]; return o; }
static int unhex(const char *p, int n, uint32_t *v)
{
    uint32_t x = 0;
    for (int i = 0; i < n; i++) {
        const char c = p[i];
        const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0) return 0;
        x = x << 4 | (uint32_t)d;
    }
    *v = x; return 1;
}
int plk_serialize(int id, char *o)                                /* -> chars written (no 0) */
{
    char *s0 = o;
    const int m = id >> 4 & 1, l = id & 15;
    if (l == 15) return (int)(hex(hex(hex(o, plk_clip[m].len_i, 2), plk_clip[m].sync, 2), plk_clip[m].trk, 2) - s0);
    const plk_lane_t *ln = &plk_clip[m].lane[l];
    o = hex(o, (uint32_t)ln->type, 8); o = hex(o, (uint16_t)ln->start, 4); o = hex(o, ln->n, 4);
    for (int i = 0; i < ln->n; i++) { o = hex(o, ln->ev[i].t, 4); o = hex(o, (uint16_t)ln->ev[i].v, 4); }
    return (int)(o - s0);
}
int plk_parse(int id, const char *p, int size)                    /* 1 = taken; anything malformed leaves it empty */
{
    const int m = id >> 4 & 1, l = id & 15;
    uint32_t a, b, n;
    if (l == 15) {
        uint32_t k = 0;                                           /* (4 chars: before the track lock) */
        if ((size != 4 && size != 6) || !unhex(p, 2, &a) || !unhex(p + 2, 2, &b) || a >= PLK_NLEN
            || (size == 6 && (!unhex(p + 4, 2, &k) || (k > 16 && (k <= PLK_START || k > PLK_START + 16))))) return 0;
        plk_clip[m].len_i = (uint8_t)a; plk_clip[m].sync = b != 0; plk_clip[m].trk = (uint8_t)k; return 1;
    }
    if (l >= PLK_LANES || size < 16 || !unhex(p, 8, &a) || !unhex(p + 8, 4, &b) || !unhex(p + 12, 4, &n)
        || n > PLK_EV || size != 16 + 8 * (int)n) return 0;
    plk_lane_t *ln = &plk_clip[m].lane[l];
    for (uint32_t i = 0, prev = 0; i < n; i++) {
        uint32_t t, v;
        if (!unhex(p + 16 + 8 * i, 4, &t) || !unhex(p + 20 + 8 * i, 4, &v) || t >= 6144 || t < prev) { ln->n = 0; return 0; }
        ln->ev[i].t = (uint16_t)t; ln->ev[i].v = (int16_t)v; prev = t;
    }
    ln->type = (int32_t)a; ln->start = (int16_t)b; ln->n = (uint16_t)n;
    return 1;
}

/* ======================= page (T2 on a sequencer page; dispatcher.c routes keys, turns and the screen) ========= */
#ifndef OP1_HOST
volatile uint8_t plk_view;                                        /* the PLK page is shown instead of the sequencer's */
static uint8_t ui_sel[2];                                         /* selected lane */
static int32_t ui_acc[4];
static void range(int m, int l, int *lo, int *hi)                 /* the lane's word range (page 2: the plugin's 0..32767) */
{
    *lo = 0; *hi = 32767;
    const int w = plk_lane_word(l);
    if (w >= 4) return;
    const int s = SLOT(m);
    const int16_t *rg;
    if (plk_lane_group(l) == 0) {
        const int eng = ((volatile int *)0xFF8016E4)[s];
        if (eng < 0 || eng > 31) return;
        rg = (const int16_t *)(0x01DE5890 + eng * 0x40 + w * 8);
    } else {
        const int t = ((volatile int *)(m ? 0xFF801754 : 0xFF801734))[s];
        if (t < 0 || t > 7) return;
        rg = (const int16_t *)(0x01DE5C50 + t * 0x30 + w * 6);
    }
    if (rg[1] > rg[0]) { *lo = rg[0]; *hi = rg[1]; }
}
static int sel_lane(int m)                                        /* the selection moved onto an available lane */
{
    const uint16_t a = plk_lanes(m);
    if (!a) return -1;
    while (!(a >> ui_sel[m] & 1)) ui_sel[m] = (uint8_t)((ui_sel[m] + 1) % PLK_LANES);
    return ui_sel[m];
}
int plk_page_key(int m, int code, int pressed, int knob, int shift)   /* 1 = handled; knob: encoder press 0..3 */
{
    if (knob == 3) { if (pressed) { plk_clip[m].sync ^= 1; plk_changed(); } return 1; }   /* red press: sync / free */
    if (code == 0x21) { if (pressed) plk_rec(m); return 1; }      /* REC */
    if (code == 0x0C) { if (pressed) plk_cut(m, shift ? -1 : sel_lane(m)); return 1; }   /* CUT lane, Shift+CUT all */
    return 0;
}
void plk_page_turn(int m, int knob, int delta)
{
    if (knob == 2) {                                              /* white: the track the clip restarts with, or none */
        const int n = 2 * plk_tracks[m] + 1, d = delta > 0 ? 1 : -1;   /* off, t1..tN (every loop), s1..sN (start) */
        if ((ui_acc[2] > 0) != (delta > 0)) ui_acc[2] = 0;
        ui_acc[2] += delta;
        if (ui_acc[2] / 1024 == 0) return;
        ui_acc[2] = 0;
        int i = plk_clip[m].trk > PLK_START ? plk_clip[m].trk - PLK_START + plk_tracks[m] : plk_clip[m].trk;
        i = (i % n + d + n) % n;
        plk_clip[m].trk = (uint8_t)(i > plk_tracks[m] ? PLK_START + i - plk_tracks[m] : i);
        plk_changed();
        return;
    }
    if (knob == 3) {                                              /* red: the selected word (recorded like a knob turn) */
        const int l = sel_lane(m);
        int16_t *w = l >= 0 ? word_ptr(m, plk_lane_group(l), plk_lane_word(l)) : 0;
        if (!w) return;
        int lo, hi; range(m, l, &lo, &hi);
        int v = *w + (int)((int64_t)(hi - lo) * delta / 65536);
        if (v == *w) v += delta > 0 ? 1 : -1;
        *w = (int16_t)(v < lo ? lo : v > hi ? hi : v);
        return;
    }
    if ((ui_acc[knob] > 0) != (delta > 0)) ui_acc[knob] = 0;
    ui_acc[knob] += delta;
    const int st = ui_acc[knob] / 1024;                           /* a detent */
    if (!st) return;
    ui_acc[knob] -= st * 1024;
    if (knob == 0) {                                              /* blue: lane */
        const uint16_t a = plk_lanes(m);
        if (!a) return;
        int l = ui_sel[m];
        for (int n = st > 0 ? st : -st; n; n--)
            do l = (l + (st > 0 ? 1 : PLK_LANES - 1)) % PLK_LANES; while (!(a >> l & 1));
        ui_sel[m] = (uint8_t)l;
    } else { plk_set_len(m, plk_clip[m].len_i + st); plk_changed(); }   /* green: length */
}

#define BG_REGISTER ((void (*)(void *))0x0193F010)
#define BG_DIRTY    ((void (*)(void *))0x0193EF3C)
#define DB_HANDLE   ((void *(*)(void))0x0196DD44)
#define DB_EXEC     ((void (*)(void *, const char *))0x0196F950)
#define DB_GET      ((void (*)(void *, void *, const char *))0x0196FBD0)
typedef struct { uint32_t alloc; union { char buf[16]; char *ptr; } u; uint32_t size, cap; uint32_t pad; } dstr;
static const char CREATE[] = "CREATE TABLE IF NOT EXISTS plk (id INTEGER PRIMARY KEY, data TEXT NOT NULL)";
static char sql[64 + 16 + 8 * PLK_EV];
typedef char sql_fits[sizeof sql < 0x2000 - 64 ? 1 : -1];        /* the firmware's SQL format buffer: 8 KB */
static void bg_nop(void *self) { (void)self; }
static int row_used(int id) { return (id & 15) == 15 || (id & 15) < PLK_LANES; }
static void bg_save(void *self)                                   /* saver thread */
{
    (void)self;
    void *db = DB_HANDLE();
    DB_EXEC(db, CREATE);
    for (int id = 0; id < 32; id++) {
        if (!row_used(id)) continue;
        static const char head[] = "INSERT OR REPLACE INTO plk VALUES (";
        char *o = sql;
        for (const char *h = head; *h; h++) *o++ = *h;
        if (id >= 10) *o++ = (char)('0' + id / 10);
        *o++ = (char)('0' + id % 10); *o++ = ','; *o++ = '\'';
        o += plk_serialize(id, o);                                /* (a lane recorded meanwhile: a torn row fails the parse) */
        *o++ = '\''; *o++ = ')'; *o = 0;
        op1_db_write(db, sql);
    }
}
static const uint32_t bg_vt[6] = { 0, 0, 0, (uint32_t)bg_nop, 0, (uint32_t)bg_save };
static struct { uint8_t dirty, saving, pad[2]; const uint32_t *vt; uint32_t more[2]; } bg;
static uint8_t bg_ready;
static void load(void)                                            /* GUI thread, once */
{
    void *db = DB_HANDLE();
    DB_EXEC(db, CREATE);
    for (int id = 0; id < 32; id++) {
        if (!row_used(id)) continue;
        static char q[] = "SELECT data FROM plk WHERE id=00";
        q[sizeof q - 3] = (char)('0' + id / 10); q[sizeof q - 2] = (char)('0' + id % 10);
        dstr s = { 0 };
        DB_GET(&s, db, q);
        if (s.size) plk_parse(id, s.cap > 15 ? s.u.ptr : s.u.buf, (int)s.size);   /* (heap copy, once per boot, not freed) */
    }
    BG_REGISTER(&bg);
    bg.vt = bg_vt;
    bg_ready = 1;
    plk_save_req = 0;
}
void plk_poll(void)
{
    if (!bg_ready && !op1r_safe_boot) load();
    if (plk_save_req && bg_ready) { plk_save_req = 0; BG_DIRTY(&bg); }
}

/* player proxy (dispatcher.c seq_player_get, op1r sequencers): the clock reaches plk, then the sequencer's player.
 * Vtable: {this offset, fn} pairs (PlayerManager calls vt[0x2C](self + (int16)vt[0x28], counter)). */
static void *inner;
#define FWD(i) (*(void (**)(void *, int))((uint8_t *)*(void **)inner + (i) * 4 + 4))((uint8_t *)inner + *(int16_t *)((uint8_t *)*(void **)inner + (i) * 4), a)
static void p_fw1(void *self, int a) { (void)self; FWD(2); }
static void p_down(void *self, int a) { (void)self; FWD(4); }
static void p_up(void *self, int a) { (void)self; FWD(6); }
static void p_reset(void *self, int a) { (void)self; plk_start(*(volatile int *)0x01DE4C40 == 1); FWD(8); }
static void p_tick(void *self, int a) { (void)self; plk_tick(*(volatile int *)0x01DE4C40 == 1); FWD(10); }
static void p_fw6(void *self, int a) { (void)self; FWD(12); }
static void p_stop(void *self, int a) { (void)self; plk_stop(0); plk_stop(1); FWD(14); }
static const uint32_t p_vt[16] = { 0, 0, 0, (uint32_t)p_fw1, 0, (uint32_t)p_down, 0, (uint32_t)p_up,
                                   0, (uint32_t)p_reset, 0, (uint32_t)p_tick, 0, (uint32_t)p_fw6, 0, (uint32_t)p_stop };
static struct { const uint32_t *vt; uint32_t pad[7]; } p_obj = { p_vt, { 0 } };
void *plk_player(void *in) { if (!in) return in; plk_poll(); inner = in; return &p_obj; }

static void ln(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c)
{
    op1_pt a = { x0, y0 }, b = { x1, y1 };
    op1_raster_line(r, &a, &b, w, c);
}
static const uint8_t c_enc[4][4] = { {0x4D,0x9E,0xFF,0xFF}, {0x00,0xED,0x95,0xFF}, {0xFF,0xFF,0xFF,0xFF}, {0xFF,0x3A,0x5D,0xFF} };
static const uint8_t c_grey[4] = { 0x6F,0x71,0x73,0xFF }, c_dim[4] = { 0x40,0x42,0x44,0xFF }, c_lilac[4] = { 0xAE,0xB1,0xDC,0xFF };
#define Q OP1_Q10
#define ROWS 8
#define ROW_H 12
#define Y0 40
#define X0 44
#define X1 296
static void dot(void *r, int32_t x, int32_t y, int32_t h, const uint8_t *c)   /* two crossing bars, half size h */
{
    ln(r, x - h, y, x + h, y, 2 * h, c); ln(r, x, y - h, x, y + h, 2 * h, c);
}
static void marker(void *r, int32_t x, int32_t y, int l)          /* page 1: a dot, page 2: a colon, the knob's colour */
{
    const int w = plk_lane_word(l);
    const uint8_t *c = c_enc[w < 4 ? w : w - 4];
    if (w < 4) dot(r, x, y, 0x700, c);
    else { dot(r, x, y - 0xA00, 0x400, c); dot(r, x, y + 0xA00, 0x400, c); }
}
static char *num(char *p, int v) { if (v >= 10) p = num(p, v / 10); *p++ = (char)('0' + v % 10); *p = 0; return p; }

/* stock font (lowercase; "s" / "x" upper case = the synth / synth FX symbols, op1-design ICONS) */
enum { T_LEN, T_SYNC, T_FREE, T_VAL, T_TRK, T_NONE, T_ROW, T_N = T_ROW + 2 * ROWS };
static uint32_t txt[T_N][OP1_TEXT_SIZE / 4];
static uint8_t txt_ok;
static const int32_t ident[9] = { 1024, 0, 0, 0, 1024, 0, 0, 0, 1024 };
#define TY(yc) ((yc) - 7)                                         /* text top for a row centre */
static void txt_init(void)
{
    /* header: state, length (green), track lock (white), selected lane's mark + value (red turn), sync / free (red press) */
    op1_text_ctor(txt[T_LEN], Q(32), 7, 16, c_enc[1]);
    op1_text_ctor(txt[T_VAL], Q(196), 7, 16, c_enc[3]);
    op1_text_ctor(txt[T_SYNC], Q(268), 9, 12, c_enc[3]); op1_text_set(txt[T_SYNC], "sync");
    op1_text_ctor(txt[T_FREE], Q(268), 9, 12, c_grey);   op1_text_set(txt[T_FREE], "free");
    op1_text_ctor(txt[T_TRK], Q(140), 9, 12, c_enc[2]);              /* white: track lock "t2", or "off" */
    op1_text_ctor(txt[T_NONE], Q(120), 76, 12, c_grey);  op1_text_set(txt[T_NONE], "no lanes");
    for (int row = 0; row < ROWS; row++) {
        const int yc = Y0 + row * ROW_H + ROW_H / 2;
        op1_text_ctor(txt[T_ROW + 2 * row], Q(4), TY(yc), 12, c_lilac);     op1_text_set(txt[T_ROW + 2 * row], "S");
        op1_text_ctor(txt[T_ROW + 2 * row + 1], Q(4), TY(yc), 12, c_lilac); op1_text_set(txt[T_ROW + 2 * row + 1], "X");
    }
    txt_ok = 1;
}
#define TXT(i) op1_text_draw(txt[i], r, ident)

static void draw_static(void *r, int m, int sel, const int *list, int n, int first)
{
    const plk_run_t *ru = &plk_run[m];
    const plk_clip_t *c = &plk_clip[m];
    const int len = len_ticks(m);
    TXT(T_LEN); TXT(c->sync ? T_SYNC : T_FREE); TXT(T_TRK);
    if (sel >= 0) marker(r, Q(186), Q(15), sel);
    ln(r, Q(8), Q(30), Q(312), Q(30), 0x400, c_dim);
    for (int row = 0; row < ROWS && first + row < n; row++) {
        const int i = first + row, l = list[i], g = plk_lane_group(l);
        const int32_t yc = Q(Y0 + row * ROW_H + ROW_H / 2);
        if (i && plk_lane_group(list[i - 1]) != g) ln(r, Q(14), yc - Q(ROW_H / 2), Q(X1), yc - Q(ROW_H / 2), 0x300, c_grey);
        if (!row || plk_lane_group(list[i - 1]) != g) TXT(T_ROW + 2 * row + g);
        marker(r, Q(30), yc, l);
        const plk_lane_t *la = &c->lane[l];
        const int recl = ru->state == PLK_REC && (ru->seen >> l & 1);
        const int w = plk_lane_word(l);
        const uint8_t *col = recl ? c_enc[3] : c_enc[w < 4 ? w : w - 4];
        if (!la->n) ln(r, Q(X0), yc, Q(X1), yc, 0x400, c_dim);
        else {                                                    /* the curve: 48 segments over the clip */
            int lo, hi; range(m, l, &lo, &hi);
            uint16_t cur = 0;
            int32_t px = 0, py = 0;
            const int end = recl ? ru->pos : len;
            for (int k = 0; k <= 48; k++) {
                const int t = k * len / 48;
                if (t > end) break;
                const int v = lane_value(la, t, &cur);
                const int32_t xx = Q(X0) + Q(X1 - X0) * k / 48, yy = yc + Q(4.5) - (int32_t)((int64_t)Q(9) * (v - lo) / (hi - lo));
                if (k) ln(r, px, py, xx, yy, 0x4CC, col);
                px = xx; py = yy;
            }
        }
        if (l == sel)                                             /* brackets */
            for (int s = 0; s < 2; s++) {
                const int32_t xa = s ? Q(X1 + 4) : Q(X0 - 4), d = s ? -Q(3) : Q(3);
                ln(r, xa, yc - Q(5), xa, yc + Q(5), 0x4CC, c_enc[2]);
                ln(r, xa, yc - Q(5), xa + d, yc - Q(5), 0x4CC, c_enc[2]); ln(r, xa, yc + Q(5), xa + d, yc + Q(5), 0x4CC, c_enc[2]);
            }
    }
    if (!n) TXT(T_NONE);
    if (n > ROWS) {                                               /* scroll bar */
        ln(r, Q(314), Q(Y0), Q(314), Q(Y0 + ROWS * ROW_H), 0x800, c_dim);
        ln(r, Q(314), Q(Y0) + Q(ROWS * ROW_H) * first / n, Q(314), Q(Y0) + Q(ROWS * ROW_H) * (first + ROWS) / n, 0x800, c_grey);
    }
}

static void scr_init(void *scr) { (void)scr; }
static void scr_update(void *scr, int dt_ms)
{
    if (!txt_ok) txt_init();
    plk_poll();
    const int m = *(volatile int *)0x01DE4C40 == 1;
    const plk_run_t *ru = &plk_run[m];
    const plk_clip_t *c = &plk_clip[m];
    const uint16_t a = plk_lanes(m);
    const int sel = sel_lane(m), len = len_ticks(m);
    int list[PLK_LANES], n = 0, si = 0;
    for (int l = 0; l < PLK_LANES; l++) if (a >> l & 1) { if (l == sel) si = n; list[n++] = l; }
    int first = si - ROWS / 2 + 1; if (first > n - ROWS) first = n - ROWS; if (first < 0) first = 0;
    /* the static picture (cached, docs/75) changes with: selection, length, sync, lanes, recorded data, the recording */
    uint32_t sig = (uint32_t)sel * 131u + c->len_i * 7u + c->trk * 1009u + c->sync + a * 977u + (uint32_t)m * 31337u + (uint32_t)first * 13u;
    for (int l = 0; l < PLK_LANES; l++) sig = sig * 33u + c->lane[l].n + (uint32_t)c->lane[l].type;
    if (ru->state == PLK_REC) sig += 0x9E3779B9u + (uint32_t)(ru->pos * 48 / len);
    static uint32_t last_sig, last_len;
    if (sig != last_sig) { last_sig = sig; op1_screen_static_dirty(scr); }
    static uint8_t last_trk = 0xFF;
    const int tk = c->trk > PLK_START ? c->trk - PLK_START : c->trk;
    const int trk = tk <= plk_tracks[m] ? c->trk : 0;              /* (another sequencer with fewer tracks: off) */
    if (trk != last_trk) {
        char t[4] = { 'o', 'f', 'f', 0 };
        if (trk) { t[0] = trk > PLK_START ? 's' : 't'; num(t + 1, tk); }   /* t: every loop, s: at the start only */
        last_trk = (uint8_t)trk; op1_text_set(txt[T_TRK], t);
    }
    if (c->len_i != last_len) {
        static const char *const lens[PLK_NLEN] = { "1/4 bar", "1/2 bar", "1 bar", "2 bars", "4 bars", "8 bars", "16 bars" };
        last_len = c->len_i; op1_text_set(txt[T_LEN], lens[c->len_i < PLK_NLEN ? c->len_i : 2]);
    }
    static const op1_rect_t dyn[3] = { { 0, 0, 22, 29 }, { 192, 0, 262, 29 }, { X0 - 1, Y0 - 5, X1 + 1, Y0 + ROWS * ROW_H + 1 } };
    op1_screen_regions(scr, dyn, 3);
    void *r = op1_raster();
    if (op1_screen_static(scr)) {
        op1_screen_draw(scr, dt_ms);
        draw_static(r, m, sel, list, n, first);
        op1_screen_static_done(scr);
    }
    /* dynamic: state, value, playhead */
    if (ru->state == PLK_REC) dot(r, Q(13), Q(15), Q(4), c_enc[3]);
    else if (ru->state == PLK_ARMED) {
        ln(r, Q(9), Q(11), Q(17), Q(11), 0x600, c_enc[3]); ln(r, Q(9), Q(19), Q(17), Q(19), 0x600, c_enc[3]);
        ln(r, Q(9), Q(11), Q(9), Q(19), 0x600, c_enc[3]); ln(r, Q(17), Q(11), Q(17), Q(19), 0x600, c_enc[3]);
    } else if (!ru->halted) for (int i = 0; i < 10; i += 2) ln(r, Q(8 + i), Q(9) + i * 0x266, Q(8 + i), Q(21) - i * 0x266, 0x900, c_enc[2]);   /* play triangle */
    else ln(r, Q(8), Q(15), Q(18), Q(15), Q(10), c_grey);         /* stopped: a square */
    if (sel >= 0) {
        int16_t *w = word_ptr(m, plk_lane_group(sel), plk_lane_word(sel));
        int lo, hi; range(m, sel, &lo, &hi);
        char t[8]; num(t, w ? (int)((int64_t)(*w - lo) * 100 / (hi - lo)) : 0);
        static char last_t[8];
        int k = 0; while (t[k] == last_t[k] && t[k]) k++;
        if (t[k] != last_t[k]) { op1_text_set(txt[T_VAL], t); for (k = 0; k < 8; k++) last_t[k] = t[k]; }
        TXT(T_VAL);
    }
    if (ru->running && !ru->halted && ru->state != PLK_ARMED) {
        const int32_t xh = Q(X0) + (int32_t)((int64_t)Q(X1 - X0) * ru->pos / len);
        ln(r, xh, Q(Y0 - 4), xh, Q(Y0 + ROWS * ROW_H), 0x4CC, ru->state == PLK_REC ? c_enc[3] : c_enc[2]);
    }
}
const op1_screen_t plk_screen = { scr_init, scr_update };
#endif
