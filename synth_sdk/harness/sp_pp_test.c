/* sampler+ ping-pong on the device path: samplerplus/sp_fw.c itself (pingpong, stock_at), with the bit-exact C model
 * of the stock render (sp_stock_render) as STOCK_RENDER. A held note must keep turning inside the loop when the head
 * is already past the loop end (loop end knob turned below it, a late velocity / random start) and when the loop is
 * shorter than one block's travel; a normal loop must follow the C model (sp_plus_render).
 * cc -O1 -DOP1_HOST -I.. sp_pp_test.c ../samplerplus/sp_plus.c ../samplerplus/sp_voice.c -lm */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../samplerplus/sp_plus.h"

static int32_t host_inc[SP_BLOCK];
static const int32_t RATE = 0x1511F7C;
static int16_t *g_data, fir_buf[64 + 16 * 64 + 64], *g_fir = fir_buf + 64;
static void host_render(void *vo, int16_t *out, const int16_t *k)
{
    sp_stock_render((sp_voice *)((uint8_t *)vo + 8), out, k, host_inc, RATE, g_data, g_fir);
}
#define BUF_INC      host_inc
#define STOCK_RENDER host_render
#ifndef SP_FW_SRC
#define SP_FW_SRC "../samplerplus/sp_fw.c"
#endif
#include SP_FW_SRC
/* the rest of sp_fw.c links against the core and the pages: never called here */
int host_slot; int32_t op1_db_last_err;
int sp_adsr_stock(void *a, int16_t *e, const int16_t *w, const int16_t *v) { (void)a; (void)e; (void)w; (void)v; return 0; }
void spu_ui_build(void) {}
void spu_ui_load(void) {}
void spu_sync(int e) { (void)e; }
void *spu_screen_for(int id) { (void)id; return 0; }
int spu_knob(int k, int d) { (void)k; (void)d; return 0; }
int spu_key(int c, int p) { (void)c; (void)p; return 0; }

typedef struct { long silent, outside, turns, mturns; } pp_stats;

/* hold one note for `blocks` blocks; at block `at` the loop end word becomes `le2` (-1: never). turns: blocks that
 * end in the other direction, mturns the same for the C model (its step differs by up to 2 units: positions drift) */
static pp_stats hold(int16_t *k, int semis, int vel_start, int xfade, int blocks, int at, int le2)
{
    sp_extra x; sp_extra_default(&x);
    x.loop_mode = 1; x.xfade = (uint8_t)xfade; x.vel_start = (uint8_t)vel_start;
    sp_pre c; sp_plus_prepare(&x, &c);
    sp_xvoice xs; memset(&xs, 0, sizeof xs);
    uint32_t vo[4] = { 0 };
    sp_plus_note_on(model(vo), &xs, k, &x, &c);
    sp_plus_velocity(model(vo), &xs, k, &x, 0);
    sp_voice mv = *model(vo); sp_xvoice ms = xs;                   /* the C model, same note */
    int32_t inc = (int32_t)(SP_INC_C4 * pow(2, semis / 12.0)), minc[SP_BLOCK];
    pp_stats r = { 0, 0, 0, 0 };
    int16_t out[SP_BLOCK], mout[SP_BLOCK];
    for (int b = 0; b < blocks; b++) {
        if (b == at) k[2] = (int16_t)le2;
        int32_t ls = sp_bound(k[1], k[5]), le = sp_bound(k[2], k[6]);
        for (int i = 0; i < SP_BLOCK; i++) host_inc[i] = minc[i] = inc;
        int d0 = xs.dir;
        pingpong(vo, out, k, &x, &xs, RATE);
        r.turns += xs.dir != d0;
        int pk = 0;
        for (int i = 0; i < SP_BLOCK; i++) if (abs(out[i]) > pk) pk = abs(out[i]);
        r.silent += !pk;
        int32_t p = model(vo)->pos;
        if (b > at + 1) r.outside += p < ls - (1 << 12) || p > le + (1 << 12);   /* settled: inside the loop */
        int md = ms.dir;
        sp_plus_render(&mv, &ms, mout, k, &x, &c, minc, RATE, g_data, g_fir);
        r.mturns += ms.dir != md;
    }
    return r;
}

int main(void)
{
    g_data = calloc(264600 + 64, 2);
    for (int i = 0; i < 264600; i++) g_data[8 + i] = (int16_t)(8000 * sin(i * 0.05));
    for (int j = -64; j < 16 * 64 + 64; j++) { int d = abs(j - 7 * 64); g_fir[j] = (int16_t)(d < 64 ? (64 - d) * 511 : 0); }
    pp_stats r;

    /* the head moves ~9.25 samples a block at 0 st (the dumped rate); a loop word is ~8.15 samples */
    /* 1. a normal loop (~1000 samples): turns at both ends, as the model */
    int16_t a[8] = { 4000, 4000, 4125, 32767, 12000, 0, 0, 32767 };
    r = hold(a, 0, 0, 0, 2000, -1, -1);
    printf("normal loop: %ld turns (model %ld), %ld silent, %ld outside\n", r.turns, r.mturns, r.silent, r.outside);
    assert(r.turns >= 10 && labs(r.turns - r.mturns) <= 1 && !r.silent && !r.outside);
    int16_t a2[8] = { 4000, 4000, 4125, 32767, 12000, 0, 0, 32767 };   /* with a turn crossfade */
    r = hold(a2, 0, 0, 40, 2000, -1, -1);
    assert(r.turns >= 10 && labs(r.turns - r.mturns) <= 1 && !r.silent && !r.outside);

    /* 2. the loop end turned below the head: at block 300 the head is near word 4340, the loop end goes to 4100 */
    int16_t b[8] = { 4000, 4000, 6000, 32767, 12000, 0, 0, 32767 };
    r = hold(b, 0, 0, 0, 2000, 300, 4100);
    printf("loop end below the head: %ld turns (model %ld), %ld silent, %ld outside\n", r.turns, r.mturns, r.silent, r.outside);
    assert(r.turns >= 10 && labs(r.turns - r.mturns) <= 1 && !r.silent && !r.outside);

    /* 3. velocity start: velocity 0 starts 8128 samples in, past a ~2400-sample loop */
    int16_t c[8] = { 4000, 4000, 4300, 32767, 12000, 0, 0, 32767 };
    r = hold(c, 0, 127, 0, 2000, -1, -1);
    printf("start past the loop end: %ld turns (model %ld), %ld silent, %ld outside\n", r.turns, r.mturns, r.silent, r.outside);
    assert(r.turns >= 4 && labs(r.turns - r.mturns) <= 1 && !r.silent && !r.outside);

    /* 4. a ~97-sample loop at +48 semitones (~148 samples a block): more than one turn per block */
    int16_t d[8] = { 4000, 4000, 4012, 32767, 12000, 0, 0, 32767 };
    r = hold(d, 48, 0, 0, 2000, -1, -1);
    printf("short loop, +48 st: %ld silent, %ld outside\n", r.silent, r.outside);
    assert(!r.silent && !r.outside);

    printf("sp_pp_test: ok\n");
    return 0;
}
