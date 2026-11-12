/* Resource monitor page (docs/18): clock screen (Shift+help) + T1. Shows CPU (total), AUDIO (audio callback),
 * RAM (both heaps), DISK (yaffs) and MIDI ring fill. Geometry/colours: sysmon_geom.h (display/gen_monitor_svg.py).
 *
 * Data sources (filled by hooks in dispatcher.c / hook.S):
 *   sm_audio_cb      wraps the audio DMA callback 0xFFA04804 (ISR): busy cycles per block
 *   sm_idle_run      wraps kTE_Idle::run: remembers the idle thread object -> VDK run-cycle stats
 * Everything else is read in the GUI thread, a few times per second (frame counter, no clock needed: all loads
 * are ratios of CYCLES).
 */
#include "../op1_synth.h"
#include "sysmon_geom.h"

#ifdef OP1_HOST
extern uint32_t sm_host_cycles, sm_host_idle_lo;            /* harness: fake counters / sources */
extern int sm_host_heap(int i, uint32_t *size, uint32_t *used);
extern int sm_host_disk(uint32_t *total, uint32_t *free_);
extern uint32_t sm_host_midi_fill[2];
#define READ_CYCLES()            (sm_host_cycles)
extern uint32_t sm_host_stage[12];
#define STAGE_SUM(i)             (sm_host_stage[i])
#define IDLE_TOTAL_LO(stats)     (sm_host_idle_lo)
#define AUDIO_CB(a, b, c)        ((void)0)
#else
static inline uint32_t READ_CYCLES(void) { uint32_t c; __asm__ volatile ("%0 = CYCLES;" : "=d"(c)); return c; }
#define IDLE_TOTAL_LO(stats)     (((volatile uint32_t *)(stats))[0x20 / 4])   /* VDK stats: total run cycles (lo) */
#define AUDIO_CB(a, b, c)        (((void (*)(void *, void *, void *))0xFFA04804)((a), (b), (c)))
#define STAGE_SUM(i)             (((volatile uint32_t *)(0x01BA2618 + (i) * 0x2C))[0x20 / 4])   /* stock stage timer: cycle sum (lo) */
#define HEAP_LOOKUP(i)           (((uint32_t *(*)(int))0x01A9F2E2)(i))
#define LOCK_SCHED()             (((void (*)(void))0x01A9F76C)())
#define UNLOCK_SCHED()           (((void (*)(void))0x01A9F78E)())
#define YAFFS_FREE(p)            (((int32_t (*)(const char *))0x01A9B1C0)(p))
#define YAFFS_TOTAL(p)           (((int32_t (*)(const char *))0x01A9B284)(p))
#define MIDI_RX_WR               (*(volatile uint32_t *)0x01AB1510)
#define MIDI_RX_RD               (*(volatile uint32_t *)0x01AB1514)
#define MIDI_TX_WR               (*(volatile uint32_t *)0x01AB2B44)
#define MIDI_TX_RD               (*(volatile uint32_t *)0x01AB1B3C)
#endif

/* ---------------- sources ---------------- */
static volatile uint32_t aud_busy, aud_max, aud_last_t0, aud_max_period;   /* ISR side, reset by the GUI */
void sm_audio_cb(void *a, void *b, void *c)                  /* replaces the callback pointer (audio::init) */
{
    uint32_t t0 = READ_CYCLES();
    extern void l1_begin(void), l1_end(void);
    extern void in_clock(uint32_t), in_end(void);
    in_clock(t0);                                            /* input engines: the block rate (input/in_host.c) */
    { extern const int32_t *volatile pc_in_b, *volatile pc_in_c; pc_in_b = (const int32_t *)b; pc_in_c = (const int32_t *)c; }   /* pulse sync: codec input (midi/pclk.c) */
    l1_begin();                                              /* L1 pool: leases move only inside the callback */
    AUDIO_CB(a, b, c);
    l1_end();
    in_end();
    uint32_t d = READ_CYCLES() - t0, p = t0 - aud_last_t0;
    aud_last_t0 = t0;
    aud_busy += d;
    if (d > aud_max) { aud_max = d; aud_max_period = p; }
}

static void *volatile idle_obj;                              /* kTE_Idle thread object (VDK thread) */
#ifndef OP1_HOST
void sm_idle_run(void *self)                                 /* kTE_Idle vtable run slot; never returns */
{
    idle_obj = self;
    ((void (*)(void *))0x01A5E240)(self);
}
#endif
static void *idle_stats(void)                                /* VDK stats block (heap pointer at thread+0x34) */
{
#ifdef OP1_HOST
    return (void *)1;
#else
    uint32_t s = idle_obj ? ((volatile uint32_t *)idle_obj)[0x34 / 4] : 0;
    return (s >= 0x100 && s < 0x04000000 && !(s & 3)) ? (void *)s : 0;   /* SDRAM heap pointer, else unknown */
#endif
}

/* optional profile line: paulstretch phase cycles (shelved/paulstretch/paulstretch_dsp.c), weak: absent = no line */
extern volatile uint32_t ps_prof[7] __attribute__((weak));
static uint32_t prof_last[7];
static int32_t prof_pct[7];
/* audio split (op1_synth.h SMC_*): cycles counted around the hooked calls, as % of the time (averages) */
volatile uint32_t sm_cy[SMC_N];
static uint32_t cy_last[SMC_N];
static int32_t cy_pct[SMC_N];
/* the bottom line: helper metrics, BLUE turn steps through them (sm_turn) */
enum { PG_AUDIO, PG_VOICES, PG_PLUGINS, PG_SYSTEM, PG_STAGES, PG_RAW, PG_BOOT, PG_BOOTR, PG_BOOTD, PG_BOOTS, PG_N };
/* stock audio stage timers (docs/32): 12 records x 0x2C at 0x01BA2618, run by Core_render_block; mapping checked
 * against the emulator profile: 0 whole block, 4 decimator + DC blocker, 7 tape, 9 master EQ, 11 output stage */
#define ST_N 12
static uint32_t st_last[ST_N];
static int32_t fr_pct, frt_pct, frl_pct; static uint32_t fr_slow, fr_c0, fr_c3;                                     /* free tape engine, % (tape/tm.c tm_cyc) */
static int32_t st_pct[ST_N];                                /* per record, % of the time */
static int sm_page;
static int32_t sm_acc;
void sm_turn(int delta)
{
    sm_acc += delta;
    int st = sm_acc / 1024;
    if (!st) return;
    sm_acc -= st * 1024;
    sm_page = ((sm_page + st) % PG_N + PG_N) % PG_N;
}
extern int plug_loaded, plug_refused, plug_last_err, plug_skipped;

/* ---------------- state ---------------- */
#define PEAK_DECAY 2                                        /* peak falls by 2 % per sample (~4 samples/s) */
static int32_t v_pct[SM_ROWS], pk_pct[SM_ROWS], midi_pct[2], midi_pk[2];
static uint32_t ram_used, ram_size, disk_used, disk_size;
static uint32_t last_wall, last_idle, frame;
static int have_cpu;

static int32_t pct(uint32_t a, uint32_t b)                   /* 100 * a / b without overflow / division by 0 */
{
    if (!b) return 0;
    while (a > 0x7FFFFFFFu / 100) { a >>= 1; b >>= 1; }
    if (!b) return 100;
    int32_t r = (int32_t)(a * 100 / b);
    return r > 100 ? 100 : r;
}
static void peak(int32_t *pk, int32_t v) { *pk = v > *pk - PEAK_DECAY ? v : *pk - PEAK_DECAY; }

static void sample_fast(void)                                /* CPU, AUDIO, MIDI: every few frames */
{
    uint32_t now = READ_CYCLES(), wall = now - last_wall;
    void *st = idle_stats();
    if (st) {
        uint32_t idle = IDLE_TOTAL_LO(st), di = idle - last_idle;
        if (have_cpu) { v_pct[SM_CPU] = 100 - pct(di, wall); peak(&pk_pct[SM_CPU], v_pct[SM_CPU]); }
        last_idle = idle; have_cpu = 1;
    }
    if (ps_prof)                                             /* gather fft mags synth bitrev ifft out */
        for (int i = 0; i < 7; i++) { uint32_t c = ps_prof[i]; prof_pct[i] = pct(c - prof_last[i], wall); prof_last[i] = c; }
    for (int i = 0; i < SMC_N; i++) { uint32_t c = sm_cy[i]; cy_pct[i] = pct(c - cy_last[i], wall); cy_last[i] = c; }
    for (int i = 0; i < ST_N; i++) { uint32_t c = STAGE_SUM(i); st_pct[i] = pct(c - st_last[i], wall); st_last[i] = c; }
    { extern volatile uint32_t tm_cyc; static uint32_t l; const uint32_t c = tm_cyc; fr_pct = pct(c - l, wall); l = c; }   /* free tape engine */
    { extern volatile uint32_t tm_slow; static uint32_t l; fr_slow = tm_slow - l; l = tm_slow; }
    { extern volatile uint32_t tm_cyc_t[4], tm_blocks; static uint32_t l0, l3, lb;
      const uint32_t b = tm_blocks - lb, c0 = tm_cyc_t[0] - l0, c3 = tm_cyc_t[3] - l3;
      fr_c0 = b ? c0 / b : 0; fr_c3 = b ? c3 / b : 0; lb = tm_blocks; l0 = tm_cyc_t[0]; l3 = tm_cyc_t[3]; }
    { extern volatile uint32_t tm_cyc_tracks; static uint32_t l; const uint32_t c = tm_cyc_tracks; frt_pct = pct(c - l, wall); l = c; }
    { extern volatile uint32_t tm_cyc_loop; static uint32_t l; const uint32_t c = tm_cyc_loop; frl_pct = pct(c - l, wall); l = c; }
    uint32_t busy = aud_busy, mx = aud_max, mp = aud_max_period;
    aud_busy = 0; aud_max = 0;
    v_pct[SM_DSP] = pct(busy, wall);
    peak(&pk_pct[SM_DSP], mp ? pct(mx, mp) : v_pct[SM_DSP]);
    last_wall = now;
#ifdef OP1_HOST
    uint32_t fill[2] = { sm_host_midi_fill[0], sm_host_midi_fill[1] };
#else
    uint32_t fill[2] = { (MIDI_RX_WR - MIDI_RX_RD) & 0x3FF, (MIDI_TX_WR - MIDI_TX_RD) & 0x3FF };   /* 1024 entries */
#endif
    for (int i = 0; i < 2; i++) { midi_pct[i] = pct(fill[i], 1024); peak(&midi_pk[i], midi_pct[i]); }
}

static void sample_ram(void)
{
    ram_used = ram_size = 0;
    for (int i = 0; i < 2; i++) {
#ifdef OP1_HOST
        uint32_t sz, used;
        if (!sm_host_heap(i, &sz, &used)) continue;
#else
        LOCK_SCHED();                                        /* the lock malloc/free use */
        uint32_t *h = HEAP_LOOKUP(i);
        if (!h) { UNLOCK_SCHED(); continue; }
        uint32_t sz = h[1], used = h[3] - h[0], fr = 0;      /* desc: base, size, ?, bump; free list at +0x18 */
        const uint32_t *b = (const uint32_t *)h[0x18 / 4];
        for (int n = 0; b && n < 8192; n++) { fr += b[0]; b = (const uint32_t *)b[2]; }   /* {size, heap, next} */
        UNLOCK_SCHED();
        used = used > fr ? used - fr : 0;
#endif
        ram_size += sz; ram_used += used;
    }
    v_pct[SM_RAM] = pct(ram_used, ram_size);
}

static void sample_disk(void)
{
#ifdef OP1_HOST
    uint32_t total, fr;
    if (!sm_host_disk(&total, &fr)) return;
#else
    /* shortcut: no check for USB disk mode (flag not found yet); upgrade if the page ever runs during disk mode */
    int32_t t = YAFFS_TOTAL("/yaffs2"), f = YAFFS_FREE("/yaffs2");
    if (t <= 0 || f < 0) return;
    uint32_t total = (uint32_t)t, fr = (uint32_t)f;
#endif
    disk_size = total; disk_used = total > fr ? total - fr : 0;
    v_pct[SM_DISK] = pct(disk_used, disk_size);
}

/* ---------------- text ---------------- */
static char *put_u(char *p, uint32_t v)                     /* decimal, no libc */
{
    char t[10]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = t[--n];
    return p;
}
static char *put_s(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *put_sec(char *p, uint32_t ms)                    /* "9.1": seconds with one decimal */
{ ms = (ms + 50) / 100; p = put_u(p, ms / 10); *p++ = '.'; *p++ = (char)('0' + ms % 10); return p; }
static char *put_mb(char *p, uint32_t bytes, int dec)       /* firmware font: no uppercase glyphs (they render as symbols); "13.1" (dec) or "171" MB, 1 MB = 2^20 */
{
    uint32_t t = (bytes >> 10) * 10 / 1024;                  /* tenths of MB */
    p = put_u(p, t / 10);
    if (dec) { *p++ = '.'; *p++ = (char)('0' + t % 10); }
    return p;
}
static void value_text(int row, char *o)
{
    char *p = o;
    switch (row) {
    case SM_CPU:  if (have_cpu) { p = put_u(p, (uint32_t)v_pct[row]); *p++ = '%'; } else p = put_s(p, "--"); break;
    case SM_DSP:  p = put_u(p, (uint32_t)v_pct[row]); *p++ = '%'; break;
    case SM_RAM:  p = put_mb(p, ram_used, 1); *p++ = '/'; p = put_mb(p, ram_size, 0); *p++ = 'm'; *p++ = 'b'; break;
    case SM_DISK: p = put_mb(p, disk_used, 0); *p++ = '/'; p = put_mb(p, disk_size, 0); *p++ = 'm'; *p++ = 'b'; break;
    case SM_MIDI: p = put_u(p, (uint32_t)midi_pk[0]); *p++ = '/'; p = put_u(p, (uint32_t)midi_pk[1]); *p++ = '%'; break;
    }
    *p = 0;
}

/* ---------------- screen ---------------- */
#define LABEL_X   8
#define VALUE_X   (SM_BX1 + 6)
#define TEXT_SIZE 12
static uint32_t txt_mem[2 * SM_ROWS + 3][OP1_TEXT_SIZE / 4];    /* label + value per row, "in" / "out", profile */
static int txt_ok;
static const int32_t ident[9] = { 1024, 0, 0, 0, 1024, 0, 0, 0, 1024 };
static const uint8_t c_label[4] = { 0xAE, 0xB1, 0xDC, 0xFF }, c_value[4] = { 0xFF, 0xFF, 0xFF, 0xFF },
                     c_dim[4] = { 0x6F, 0x71, 0x73, 0xFF }, c_red[4] = { 0xFF, 0x3A, 0x5D, 0xFF },
                     c_white[4] = { 0xFF, 0xFF, 0xFF, 0xFF };

static void sm_init(void *scr)
{
    (void)scr;
    for (int r = 0; r < SM_ROWS; r++) {
        op1_text_ctor(txt_mem[2 * r], OP1_Q10(LABEL_X), sm_row_y[r], TEXT_SIZE, c_label);
        op1_text_set(txt_mem[2 * r], sm_row_label[r]);
        op1_text_ctor(txt_mem[2 * r + 1], OP1_Q10(VALUE_X), sm_row_y[r], TEXT_SIZE, c_value);
    }
    op1_text_ctor(txt_mem[2 * SM_ROWS], OP1_Q10(SM_BX0 + 2), sm_row_y[SM_MIDI] + 14, TEXT_SIZE, c_dim);
    op1_text_set(txt_mem[2 * SM_ROWS], "in");
    op1_text_ctor(txt_mem[2 * SM_ROWS + 1], OP1_Q10(SM_MID + 6), sm_row_y[SM_MIDI] + 14, TEXT_SIZE, c_dim);
    op1_text_set(txt_mem[2 * SM_ROWS + 1], "out");
    op1_text_ctor(txt_mem[2 * SM_ROWS + 2], OP1_Q10(LABEL_X), 136, TEXT_SIZE, c_dim);
    txt_ok = 1;
    sample_ram(); sample_disk();
}

static void bar(void *r, int x0, int x1, int y, int32_t v, int32_t pk, const uint8_t *rgb, int show_pk)
{
    uint8_t c[4] = { rgb[0], rgb[1], rgb[2], 0xFF };
    int32_t w = (x1 - x0 - 2) * v / 100;                    /* one thick line per bar */
    if (w >= 1) {
        op1_pt a = { OP1_Q10(x0 + 1), OP1_Q10(y) + OP1_Q10(SM_BH) / 2 }, b = { OP1_Q10(x0 + 1 + w), a.y };
        op1_raster_line(r, &a, &b, OP1_Q10(SM_BH - 2), c);
    }
    if (show_pk) {                                           /* peak marker: white, red at the top */
        int32_t px = OP1_Q10(x0 + 1) + OP1_Q10(x1 - x0 - 2) * pk / 100;
        op1_pt a = { px, OP1_Q10(y - 2) }, b = { px, OP1_Q10(y + SM_BH + 2) };
        op1_raster_line(r, &a, &b, OP1_Q10(1.5), pk >= 98 ? c_red : c_white);
    }
}

/* audio row: one bar split into instruments (custom synths + sampler + drum voices, green), synth envelopes (blue),
 * custom FX (white), the rest of the audio callback (stock engines / FX, mixer, tape: grey); the peak as before */
static void audio_bar(void *r, int y)
{
    static const uint8_t seg_rgb[4][3] = { { 0x00, 0xED, 0x95 }, { 0x4D, 0x9E, 0xFF }, { 0xFF, 0xFF, 0xFF }, { 0x6F, 0x71, 0x73 } };
    int32_t inst = cy_pct[SMC_SYNTH] + cy_pct[SMC_SAMPLER] + cy_pct[SMC_DRUM], env = cy_pct[SMC_ENV], fx = cy_pct[SMC_FX];
    int32_t tot = v_pct[SM_DSP], rest = tot - inst - env - fx, seg[4] = { inst, env, fx, rest < 0 ? 0 : rest };
    int32_t span = SM_BX1 - SM_BX0 - 2, x = SM_BX0 + 1, acc = 0;
    for (int i = 0; i < 4; i++) {
        int32_t x1 = SM_BX0 + 1 + span * (acc + seg[i]) / 100;    /* cumulative: no rounding gaps */
        if (x1 > SM_BX1 - 1) x1 = SM_BX1 - 1;
        acc += seg[i];
        if (x1 > x) {
            uint8_t c[4] = { seg_rgb[i][0], seg_rgb[i][1], seg_rgb[i][2], 0xFF };
            op1_pt a = { OP1_Q10(x), OP1_Q10(y) + OP1_Q10(SM_BH) / 2 }, b = { OP1_Q10(x1), a.y };
            op1_raster_line(r, &a, &b, OP1_Q10(SM_BH - 2), c);
        }
        x = x1;
    }
    bar(r, SM_BX0, SM_BX1, y, 0, pk_pct[SM_DSP], sm_row_rgb[SM_DSP], 1);   /* peak marker only */
}

static void bottom_text(void *r, const char *t)            /* the shared bottom line: rebuilt only on change */
{
    static char last[48];
    int k = 0;
    while (t[k] && t[k] == last[k]) k++;
    if (t[k] != last[k]) { op1_text_set(txt_mem[2 * SM_ROWS + 2], t); for (k = 0; (last[k] = t[k]) != 0; k++) ; }
    op1_text_draw(txt_mem[2 * SM_ROWS + 2], r, ident);
}

static void sm_update(void *scr, int dt)
{
    if ((frame & 7) == 0) sample_fast();
    if ((frame & 31) == 16) sample_ram();
    if ((frame & 255) == 128) sample_disk();
    frame++;
    /* regions (docs/75): per row the bar, its peak marker and the value; the bottom line. Static: frames, ticks, labels */
    static const op1_rect_t dyn[SM_ROWS + 1] = {
        { SM_BX0 - 2, 14 - 4, 319, 14 + 16 }, { SM_BX0 - 2, 38 - 4, 319, 38 + 16 }, { SM_BX0 - 2, 62 - 4, 319, 62 + 16 },
        { SM_BX0 - 2, 86 - 4, 319, 86 + 16 }, { SM_BX0 - 2, 110 - 4, 319, 110 + 16 }, { 0, 132, 319, 152 } };
    _Static_assert(SM_ROWS == 5, "regions per row");
    op1_screen_regions(scr, dyn, SM_ROWS + 1);
    void *r = op1_raster();
    if (op1_screen_static(scr)) {
        op1_screen_draw(scr, dt);
        if (txt_ok) {
            for (int row = 0; row < SM_ROWS; row++) op1_text_draw(txt_mem[2 * row], r, ident);
            op1_text_draw(txt_mem[2 * SM_ROWS], r, ident);
            op1_text_draw(txt_mem[2 * SM_ROWS + 1], r, ident);
        }
        op1_screen_static_done(scr);
    }
    for (int row = 0; row < SM_ROWS; row++) {
        int y = sm_row_y[row];
        if (row == SM_MIDI) {
            bar(r, SM_BX0, SM_MID - 4, y, midi_pct[0], midi_pk[0], sm_row_rgb[row], 1);
            bar(r, SM_MID + 4, SM_BX1, y, midi_pct[1], midi_pk[1], sm_row_rgb[row], 1);
        } else if (row == SM_DSP)
            audio_bar(r, y);
        else
            bar(r, SM_BX0, SM_BX1, y, v_pct[row], pk_pct[row], sm_row_rgb[row], row <= SM_DSP);
    }
    if (!txt_ok) return;
    static char last[SM_ROWS][24];
    for (int row = 0; row < SM_ROWS; row++) {
        char s[24]; int k = 0;
        value_text(row, s);
        while (s[k] && s[k] == last[row][k]) k++;
        if (s[k] != last[row][k]) {                          /* text_set rebuilds the glyph lists: only on change */
            op1_text_set(txt_mem[2 * row + 1], s);
            for (k = 0; (last[row][k] = s[k]) != 0; k++) ;
        }
        op1_text_draw(txt_mem[2 * row + 1], r, ident);
    }
    extern int32_t op1_db_last_err __attribute__((weak));
    char t[48], *q = t;
    q = put_u(q, (uint32_t)sm_page + 1); *q++ = '/'; q = put_u(q, PG_N); *q++ = ' ';
    if (ps_prof) {                                           /* "paul 41 f12 m5 s9 b1 i12 o2" (% of the audio time) */
        static const char tag[7] = { 'g', 'f', 'm', 's', 'b', 'i', 'o' };
        int32_t sum = 0;
        for (int i = 0; i < 7; i++) sum += prof_pct[i];
        q = put_u(put_s(t, "paul "), (uint32_t)sum);
        for (int i = 0; i < 7; i++) { *q++ = ' '; *q++ = tag[i]; q = put_u(q, (uint32_t)prof_pct[i]); }
    } else if (sm_page == PG_AUDIO) {                        /* "1/6 inst 12 env 3 fx 20 rest 8" (% of the time) */
        int32_t inst = cy_pct[SMC_SYNTH] + cy_pct[SMC_SAMPLER] + cy_pct[SMC_DRUM];
        int32_t rest = v_pct[SM_DSP] - inst - cy_pct[SMC_ENV] - cy_pct[SMC_FX];
        q = put_u(put_s(q, "inst "), (uint32_t)inst); q = put_u(put_s(q, " env "), (uint32_t)cy_pct[SMC_ENV]);
        q = put_u(put_s(q, " fx "), (uint32_t)cy_pct[SMC_FX]); q = put_u(put_s(q, " rest "), (uint32_t)(rest < 0 ? 0 : rest));
    } else if (sm_page == PG_VOICES) {                       /* "2/6 synth 5 sampler 9 drum 3" */
        q = put_u(put_s(q, "synth "), (uint32_t)cy_pct[SMC_SYNTH]); q = put_u(put_s(q, " sampler "), (uint32_t)cy_pct[SMC_SAMPLER]);
        q = put_u(put_s(q, " drum "), (uint32_t)cy_pct[SMC_DRUM]);
    } else if (sm_page == PG_PLUGINS) {                      /* "3/6 plugins 17 refused 0" (+ skipped, last error) */
        q = put_u(put_s(q, "plugins "), (uint32_t)plug_loaded); q = put_u(put_s(q, " refused "), (uint32_t)plug_refused);
        if (plug_skipped) q = put_u(put_s(q, " skip "), (uint32_t)plug_skipped);
        if (plug_last_err) q = put_u(put_s(q, " err "), (uint32_t)plug_last_err);
    } else if (sm_page == PG_STAGES) {                       /* "5/6 blk 18 eq 6 out 7 dec 3 tp 0" (stock timers) */
        /* shortcut: eq / out / dec left out while the free tape engine is measured (docs/82); put back after */
        q = put_u(put_s(q, "blk "), (uint32_t)st_pct[0]);
        q = put_u(put_s(q, " tp "), (uint32_t)st_pct[7]);
        q = put_u(put_s(q, " fr "), (uint32_t)fr_pct); q = put_u(put_s(q, "/"), (uint32_t)frt_pct); q = put_u(put_s(q, "/"), (uint32_t)frl_pct);
        q = put_u(put_s(q, " c"), fr_c0); q = put_u(put_s(q, "/"), fr_c3);   /* cycles per block: track 1 / track 4 */
    } else if (sm_page == PG_BOOT || sm_page == PG_BOOTR) {
        /* boot (docs/79): the firmware's own stage timers (OP_Main 0x019EFEEC: 25 x ms at 0x01AB3030; +0x10 mount +
         * display, +0x18 db / clips / FAT with op1r's plugin loader, +0x28 sounds, +0x50 screens with op1r's screens),
         * "7/8 boot 9.1 mnt 4.2 db 1.2 snd 2.0"; page 8: the screens and op1r's own share, "8/8 scr 1.5 op1r plg 1.1 scr 0.4" */
        extern volatile uint32_t op1r_boot_ms[2];
        static const uint8_t slot[4] = { 0x10, 0x18, 0x28, 0x50 };
        static const char *const tag[4] = { " mnt ", " db ", " snd ", " scr " };
#ifdef OP1_HOST
        static const int32_t bt[25];
#else
        const volatile int32_t *bt = (const volatile int32_t *)0x01AB3030;
#endif
        if (sm_page == PG_BOOT) {                            /* "7/8 boot 9.1 mnt 4.2 db 1.2 snd 2.0" */
            uint32_t tot = 0;
            for (int i = 0; i < 25; i++) tot += bt[i] > 0 ? (uint32_t)bt[i] : 0u;
            q = put_sec(put_s(q, "boot "), tot);
            for (int i = 0; i < 3; i++) q = put_sec(put_s(q, tag[i]), (uint32_t)(bt[slot[i] / 4] > 0 ? bt[slot[i] / 4] : 0));
        } else {                                             /* "8/8 scr 1.5 op1r plg 1.1 scr 0.4" */
            q = put_sec(put_s(q, "scr "), (uint32_t)(bt[slot[3] / 4] > 0 ? bt[slot[3] / 4] : 0));
            q = put_sec(put_s(q, " op1r plg "), op1r_boot_ms[0]);
            q = put_sec(put_s(q, " scr "), op1r_boot_ms[1]);
        }
    } else if (sm_page == PG_BOOTD || sm_page == PG_BOOTS) {
        /* the db and snd boot stages split (docs/79, boottime.c): "9/10 open 0.4 clips 0.5 fat 0.1",
         * "10/10 pre 1.2 0.6 slot 0.4 0.3 smp 0.1" (synth / drum) */
        extern volatile uint32_t bt_split[8];
        extern volatile uint32_t l1rd[4], l1rt[4];
        static const char hx[] = "0123456789abcdef";
        extern volatile uint32_t os_check[8] __attribute__((weak));   /* dsp/outstage.c check builds (docs/81) */
        extern volatile uint32_t eq_check[8] __attribute__((weak));   /* dsp/eq.c check builds */
        extern volatile uint32_t os_selftest[4];
        extern volatile uint32_t eq_selftest[4];
        if (sm_page == PG_BOOTD && eq_selftest[3] && eq_selftest[1]) {   /* L1 master EQ self-test failed: "9/10 eqt 48 3" */
            q = put_u(put_s(q, "eqt "), eq_selftest[0]); q = put_u(put_s(q, " "), eq_selftest[1]);
        } else if (sm_page == PG_BOOTD && os_selftest[3] && os_selftest[1]) {   /* L1 output stage self-test failed: "9/10 ost 64 3" */
            q = put_u(put_s(q, "ost "), os_selftest[0]); q = put_u(put_s(q, " "), os_selftest[1]);
        } else
        if (sm_page == PG_BOOTD && &eq_check && eq_check[0]) {   /* "9/10 eq 91274 0 14191 60000" */
            q = put_u(put_s(q, "eq "), eq_check[0]); q = put_u(put_s(q, " "), eq_check[1]);
            q = put_u(put_s(q, " "), eq_check[6]); q = put_u(put_s(q, " "), eq_check[7]);
        } else if (sm_page == PG_BOOTD && &os_check && os_check[0]) {   /* "9/10 os 91274 0 12866 13732": blocks, differing, cycles */
            q = put_u(put_s(q, "os "), os_check[0]); q = put_u(put_s(q, " "), os_check[1]);
            q = put_u(put_s(q, " "), os_check[6]); q = put_u(put_s(q, " "), os_check[7]);
        } else if (sm_page == PG_BOOTD && l1rt[3] && l1rt[1]) {     /* L1 patches at boot: shown only if one failed, "9/10 l1p 8/9 1" */
            extern const unsigned l1_pokes_n;
            q = put_u(put_s(q, "l1p "), l1rt[0]); *q++ = '/'; q = put_u(q, l1_pokes_n);
            q = put_u(put_s(q, " "), l1rt[1]);
        } else if (sm_page == PG_BOOTD && l1rd[0]) {         /* diagnostic OP1R_L1READ: "9/10 l1 3c076007 1ad735c6" */
            q = put_s(q, "l1");
            for (int k = 1; k < 3; k++) { *q++ = ' '; for (int s = 28; s >= 0; s -= 4) *q++ = hx[(l1rd[k] >> s) & 15]; }
#ifdef SPK_DIAG
        } else if (sm_page == PG_BOOTD) {                    /* OP1R_SPKDIAG=1: "9/10 e8 0 e9 1 1d 0 a6 2a" (speaker amp state) */
            static const char hx2[] = "0123456789abcdef";
            const uint8_t v[4] = { *(volatile uint8_t *)0xFF9004E8, *(volatile uint8_t *)0xFF9004E9,
                                   *(volatile uint8_t *)0x018B621D, *(volatile uint8_t *)0x01BAA6E9 };
            static const char *const nm[4] = { "e8 ", " e9 ", " 1d ", " a6 " };
            for (int k = 0; k < 4; k++) { q = put_s(q, nm[k]); *q++ = hx2[v[k] >> 4]; *q++ = hx2[v[k] & 15]; }
#endif
#ifdef TAPE_IO
        } else if (sm_page == PG_BOOTD) {                    /* OP1R_TAPEIO=1: "9/10 r 12.3 40.1 w 20.2 60.3 n 37" (ms per 64 KB) */
            extern volatile uint32_t ts_io[6];
            for (int w = 0; w < 2; w++) {
                const uint32_t n = ts_io[3 * w], avg = n ? ts_io[3 * w + 1] / n : 0, mx = ts_io[3 * w + 2];
                q = put_s(q, w ? " w " : "r ");
                q = put_u(q, avg / 1000); *q++ = '.'; q = put_u(q, avg / 100 % 10);
                q = put_u(put_s(q, " "), mx / 1000); *q++ = '.'; q = put_u(q, mx / 100 % 10);
            }
            q = put_u(put_s(q, " n "), ts_io[0]);
#endif
        } else if (sm_page == PG_BOOTD) {
            q = put_sec(put_s(q, "open "), bt_split[0]); q = put_sec(put_s(q, " clips "), bt_split[1]);
            q = put_sec(put_s(q, " fat "), bt_split[2]);
        } else {
            q = put_sec(put_s(q, "pre "), bt_split[3]); q = put_sec(put_s(q, " "), bt_split[4]);
            q = put_sec(put_s(q, " slot "), bt_split[5]); q = put_sec(put_s(q, " "), bt_split[6]);
            q = put_sec(put_s(q, " smp "), bt_split[7]);
        }
    } else if (sm_page == PG_RAW) {                          /* "6/6 1:0 3:0 5:0 6:0 8:0 10:0 o12": other records, o = blk - all */
        static const uint8_t rec[6] = { 1, 3, 5, 6, 8, 10 };
        int32_t oth = st_pct[0];
        for (int i = 1; i < ST_N; i++) oth -= st_pct[i];
        for (int i = 0; i < 6; i++) { if (i) *q++ = ' '; q = put_u(q, rec[i]); *q++ = ':'; q = put_u(q, (uint32_t)st_pct[rec[i]]); }
        q = put_u(put_s(q, " o"), (uint32_t)(oth < 0 ? 0 : oth));
    } else {                                                 /* "4/6 db ok" / "db err 7" */
        q = put_s(q, "db ");
        if (&op1_db_last_err && op1_db_last_err) q = put_u(put_s(q, "err "), (uint32_t)op1_db_last_err); else q = put_s(q, "ok");
    }
    *q = 0;
    bottom_text(r, t);
}

const op1_screen_t SCREEN_sysmon = { sm_init, sm_update };
