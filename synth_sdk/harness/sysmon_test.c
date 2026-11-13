/* Host test of screens/sysmon.c: fake counters / heaps / disk / MIDI, checks the percent maths and the text, and
 * (arg "render") prints one JSON frame per state for render_monitor.py.  cc -DOP1_HOST -I.. sysmon_test.c */
#include "../op1_synth.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
void l1_begin(void) {} void l1_end(void) {}                 /* l1pool.c: harness/l1pool_test.c */
#include "../screens/sysmon.c"

uint32_t sm_host_cycles, sm_host_idle_lo, sm_host_midi_fill[2], sm_host_stage[12];
int plug_loaded = 17, plug_refused, plug_last_err, plug_skipped;
int32_t op1_db_last_err;
static int split[SMC_N];                                     /* % of each block per counter (audio split) */
static uint32_t heap_sz[2], heap_used[2], dk_total, dk_free;
int sm_host_heap(int i, uint32_t *size, uint32_t *used) { *size = heap_sz[i]; *used = heap_used[i]; return 1; }
int sm_host_disk(uint32_t *total, uint32_t *f) { *total = dk_total; *f = dk_free; return dk_total != 0; }

static struct { int32_t x0, y0, x1, y1, w; uint8_t c[4]; } L[256]; static int nl;
typedef struct { int32_t x, y; uint8_t c[4]; char s[48]; } txt_t;
static txt_t *drawn[32]; static int nt;
void host_raster_line(void *r, const void *p0, const void *p1, int32_t w, const uint8_t *c)
{ (void)r; const op1_pt *a = p0, *b = p1; L[nl].x0 = a->x; L[nl].y0 = a->y; L[nl].x1 = b->x; L[nl].y1 = b->y; L[nl].w = w; memcpy(L[nl].c, c, 4); nl++; }
void *host_text_ctor(void *o, int32_t x, int32_t y, int32_t sz, const uint8_t *c)
{ txt_t *t = o; (void)sz; assert(sizeof(txt_t) <= OP1_TEXT_SIZE); t->x = x; t->y = y; memcpy(t->c, c, 4); t->s[0] = 0; return o; }
void host_text_set(void *o, const char *s) { snprintf(((txt_t *)o)->s, 48, "%s", s); }
void host_text_draw(void *o, void *r, const int32_t *m) { (void)r; (void)m; drawn[nt++] = o; }
void *host_raster(void) { return (void *)1; }
void host_screen_draw(void *s, int dt) { (void)s; (void)dt; }

/* run frames with a given load: per frame `wall` cycles, idle share, audio busy share (per block of 1/11 frame) */
static void run(int frames, uint32_t wall, int idle_pct, int aud_pct, int aud_peak_pct)
{
    for (int f = 0; f < frames; f++) {
        for (int b = 0; b < 8; b++) {                        /* 8 audio blocks per frame, as sm_audio_cb counts */
            uint32_t per = wall / 8, busy = per * (b == 3 ? aud_peak_pct : aud_pct) / 100;
            uint32_t p = sm_host_cycles - aud_last_t0;      /* block starts on a fixed grid */
            aud_last_t0 = sm_host_cycles; aud_busy += busy;
            for (int i = 0; i < SMC_N; i++) sm_cy[i] += per * split[i] / 100;
            static const int stg[12] = { 18, 0, 0, 0, 3, 0, 0, 1, 0, 6, 0, 7 };   /* -> o1 */     /* % per stage timer record */
            for (int i = 0; i < 12; i++) sm_host_stage[i] += per * stg[i] / 100;
            if (busy > aud_max) { aud_max = busy; aud_max_period = p; }
            sm_host_cycles += per;
        }
        sm_host_idle_lo += wall * idle_pct / 100;
        nl = nt = 0;
        sm_update(0, 1);
    }
}

static void dump(const char *label)
{
    printf("{\"label\": \"%s\", \"lines\": [", label);
    for (int i = 0; i < nl; i++)
        printf("%s[%d,%d,%d,%d,%d,[%d,%d,%d,%d]]", i ? "," : "", L[i].x0, L[i].y0, L[i].x1, L[i].y1, L[i].w,
               L[i].c[0], L[i].c[1], L[i].c[2], L[i].c[3]);
    printf("], \"text\": [");
    for (int i = 0; i < nt; i++)
        printf("%s[%d,%d,[%d,%d,%d],\"%s\"]", i ? "," : "", drawn[i]->x, drawn[i]->y, drawn[i]->c[0], drawn[i]->c[1],
               drawn[i]->c[2], drawn[i]->s);
    printf("]}\n");
}

static const char *val(int row) { return ((txt_t *)txt_mem[2 * row + 1])->s; }

void in_clock(uint32_t t) { (void)t; } void in_end(void) {}   /* input engines (input/in_host.c): none on the host */
volatile uint32_t op1r_boot_ms[2] = { 1140, 420 };
volatile uint32_t tm_cyc;                               /* tape/tm.c (free tape engine, page 5) */
const int32_t *volatile pc_in_b, *volatile pc_in_c;                 /* midi/pclk.c */
volatile uint32_t l1rd[4], l1rt[4], os_selftest[4], eq_selftest[4];                 /* boottime.c: L1 diagnostic, runtime patches, output stage self-test (0: off) */
const unsigned l1_pokes_n = 9;
volatile uint32_t bt_split[8] = { 400, 520, 90, 1200, 640, 410, 330, 120 };          /* boot timers (plugload.c / dispatcher.c) */
int main(int argc, char **argv)
{
    int render = argc > 1 && !strcmp(argv[1], "render");
    heap_sz[0] = heap_sz[1] = 16u << 20; heap_used[0] = 9u << 20; heap_used[1] = 4u << 20;
    dk_total = 488u << 20; dk_free = 317u << 20;
    sm_init(0);
    run(64, 4000000, 82, 12, 15);                            /* idle */
    assert(!strcmp(val(SM_CPU), "18%")); assert(!strcmp(val(SM_DSP), "12%"));
    assert(!strcmp(val(SM_RAM), "13.0/32mb")); assert(!strcmp(val(SM_DISK), "171/488mb"));
    if (render) dump("idle"); else printf("idle: cpu %s dsp %s ram %s disk %s midi %s\n", val(0), val(1), val(2), val(3), val(4));
    heap_used[0] = 10u << 20; heap_used[1] = 5u << 20; sm_host_midi_fill[0] = 30; sm_host_midi_fill[1] = 80;
    split[SMC_SYNTH] = 6; split[SMC_SAMPLER] = 14; split[SMC_DRUM] = 5; split[SMC_ENV] = 3; split[SMC_FX] = 15;
    run(64, 4000000, 36, 55, 71);                            /* busy */
    assert(!strcmp(val(SM_CPU), "64%")); assert(pk_pct[SM_DSP] >= 70);
    if (render) dump("busy (6 voices, 3 FX, MIDI clock): audio split, page 1/4"); else printf("busy: cpu %s dsp %s pk %d midi %s\n", val(0), val(1), pk_pct[SM_DSP], val(4));
    { char want[48]; snprintf(want, sizeof want, "1/11 inst 25 env 3 fx 15 rest 14");
      if (strcmp(((txt_t *)txt_mem[2 * SM_ROWS + 2])->s, want)) { printf("got [%s]\n", ((txt_t *)txt_mem[2 * SM_ROWS + 2])->s); return 1; } }
    for (int p = 1; p < PG_N; p++) {                         /* BLUE turn: the other helper metrics */
        sm_turn(1024); run(8, 4000000, 36, 55, 71);
        static const char *const lab[PG_N] = { "", "page 2/11: voices", "page 3/11: plugins", "page 4/11: system", "page 5/11: stock stages", "page 6/11: other records", "page 7/11: boot stages", "page 8/11: op1r boot share", "page 9/11: db split", "page 10/11: snd split", "page 11/11: performance monitor" };
        if (render) dump(lab[p]); else printf("page %d: %s\n", p + 1, ((txt_t *)txt_mem[2 * SM_ROWS + 2])->s);
        if (p == PG_STAGES && strcmp(((txt_t *)txt_mem[2 * SM_ROWS + 2])->s, "5/11 blk 18 eq 6 out 7 dec 3 tp 1 fr 0")) fprintf(stderr, "stages: [%s]\n", ((txt_t *)txt_mem[2 * SM_ROWS + 2])->s);
        if (p == PG_RAW) assert(!strcmp(((txt_t *)txt_mem[2 * SM_ROWS + 2])->s, "6/11 1:0 3:0 5:0 6:0 8:0 10:0 o1"));
        if (p == PG_STAGES) assert(!strcmp(((txt_t *)txt_mem[2 * SM_ROWS + 2])->s, "5/11 blk 18 eq 6 out 7 dec 3 tp 1 fr 0"));
        if (p == PG_BOOT) assert(!strcmp(((txt_t *)txt_mem[2 * SM_ROWS + 2])->s, "7/11 boot 0.0 mnt 0.0 db 0.0 snd 0.0"));
        if (p == PG_BOOTD) assert(!strcmp(((txt_t *)txt_mem[2 * SM_ROWS + 2])->s, "9/11 open 0.4 clips 0.5 fat 0.1"));
        if (p == PG_BOOTS) assert(!strcmp(((txt_t *)txt_mem[2 * SM_ROWS + 2])->s, "10/11 pre 1.2 0.6 slot 0.4 0.3 smp 0.1"));
        if (p == PG_PF) assert(!strcmp(((txt_t *)txt_mem[2 * SM_ROWS + 2])->s, "11/11 pf off"));   /* host: no monitor */
        if (p == PG_BOOTR) assert(!strcmp(((txt_t *)txt_mem[2 * SM_ROWS + 2])->s, "8/11 scr 0.0 op1r plg 1.1 scr 0.4"));
    }
    sm_turn(1024); assert(sm_page == PG_AUDIO);              /* wraps */
    memset(split, 0, sizeof split);
    heap_used[0] = 15u << 20; heap_used[1] = 13u << 20; dk_free = 29u << 20; sm_host_midi_fill[0] = 410; sm_host_midi_fill[1] = 1023;
    run(256, 4000000, 3, 93, 100);                           /* overload */
    assert(!strcmp(val(SM_CPU), "97%")); assert(pk_pct[SM_DSP] == 100);
    if (render) dump("overload, MIDI out ring full"); else printf("overload: cpu %s dsp %s midi %s disk %s\n", val(0), val(1), val(4), val(3));
    /* counter wrap: 32-bit CYCLES overflow between samples must not break the ratio */
    sm_host_cycles = 0xFFFFFFFFu - 1000000; sm_host_idle_lo = 0xFFFFFFF0u;
    run(16, 4000000, 50, 10, 10);
    assert(!strcmp(val(SM_CPU), "50%"));
    if (!render) printf("ok\n");
    return 0;
}
