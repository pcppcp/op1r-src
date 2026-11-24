/* Host test of engplus/ep.c (docs/90): fm encoder presses flip knobs only on the fm page (not with Shift, releases
 * taken), flipped turns edit words 5..8 and save, zero words keep the stock render bit for bit, the stages act and
 * stay bounded, the overlay draws only flipped knobs inside the free columns and ends off the page. */
#include "../op1_synth.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int ep_key(int code, int pressed);
int ep_knob(int knob, int delta);
void ep_fm_render(void *self, int16_t *out, const int16_t *knobs);
void ep_digital_render(void *self, int16_t *out, const int16_t *knobs);
static int dims; void host_raster_dim(void *r) { (void)r; dims++; }
int host_cur_screen, host_synth_mode = 1, host_shift, host_slot, host_saved;
int16_t host_knobs[8][8];
static uint32_t ph;
void host_fm_render(void *self, int16_t *out, const int16_t *k)     /* a loud square wave, as fm at full level */
{
    (void)self; (void)k;
    for (int i = 0; i < OP1_BLOCK; i++, ph++) out[i] = (ph / 50) & 1 ? 30000 : -30000;
}
static const op1_tool_t *ov;
void op1_overlay(const op1_tool_t *t) { ov = t; }
static int lines, texts; static int32_t xmin = 1 << 30, xmax, ymax;
void host_raster_line(void *r, const void *p0, const void *p1, int32_t w, const uint8_t *c)
{
    (void)r; (void)w; (void)c; const op1_pt *a = p0, *b = p1; lines++;
    int32_t lo = a->x < b->x ? a->x : b->x, hi = a->x < b->x ? b->x : a->x;
    if (lo < xmin) xmin = lo; if (hi > xmax) xmax = hi; if (a->y > ymax) ymax = a->y;
}
void *host_text_ctor(void *o, int32_t x, int32_t y, int32_t s, const uint8_t *c)
{ (void)s; (void)c; assert(y + 12 < 140); assert(x < OP1_Q10(60) || x >= OP1_Q10(260)); return o; }
void host_text_set(void *t, const char *s) { (void)t; for (; *s; s++) assert(*s >= 'a' && *s <= 'z'); }
void host_text_draw(void *t, void *r, const int32_t *m) { (void)t; (void)r; (void)m; texts++; }
void *host_raster(void) { return 0; }

int main(void)
{
    static const int16_t d[4] = { 15000, 0, 100, 1500 };   /* fm's stock words 4..7 = all stages off */
    for (int i = 0; i < 4; i++) host_knobs[0][4 + i] = d[i];
    /* Shift down on the fm page starts the overlay; the key stays stock (never swallowed) */
    host_cur_screen = 2; host_shift = 1; assert(!ep_key(0x26, 1) && !ov);
    host_cur_screen = 0; assert(!ep_key(0x26, 1) && ov);
    assert(!ep_key(0x3B, 1) && !ep_key(0x3B, 0));           /* encoder presses: stock */
    /* Shift + turn edits word 4 + k (drive here) and saves; without Shift the stock knob */
    assert(ep_knob(0, 1024 * 10) && (host_knobs[0][4] ^ 15000) > 3000 && host_saved == 1);
    host_shift = 0; assert(!ep_knob(1, 1024)); host_shift = 1;
    for (int i = 0; i < 200; i++) ep_knob(0, 1024);
    assert((host_knobs[0][4] ^ 15000) == 32767);
    for (int i = 0; i < 200; i++) ep_knob(0, -1024);
    assert(host_knobs[0][4] == 15000);                         /* back to the stock word */
    /* render: all four 0 = the stock block; stages bounded (int16) and acting */
    int16_t a[OP1_BLOCK], b[OP1_BLOCK], k[8] = { 0, 0, 0, 0, 15000, 0, 100, 1500 };   /* a stock fm sound */
    ph = 0; host_fm_render(0, a, k); ph = 0; ep_fm_render((void *)1, b, k);
    assert(!memcmp(a, b, sizeof a));
    k[5] = 30000; k[6] = 32767 ^ 100; k[7] = 20000 ^ 1500;
    for (int n = 0; n < 200; n++) { ep_fm_render((void *)2, b, k); }
    int diff = 0; ph = 0; host_fm_render(0, a, k);
    for (int i = 0; i < OP1_BLOCK; i++) diff |= a[i] != b[i];
    assert(diff);
    /* overlay: all four labels + bars, inside the free columns; ends on Shift release or off the page */
    host_knobs[0][4] = 16384 ^ 15000;
    assert(ov->draw(0) == 1 && texts == 4 && lines >= 4 && xmin >= OP1_Q10(8) && ymax < OP1_Q10(140));
    host_shift = 0; assert(ov->draw(0) == 0);
    host_shift = 1; host_cur_screen = 2; assert(ov->draw(0) == 0);
    /* digital (screen 1): its stock words are 0, so a value is the word itself; zero words = stock bytes */
    host_cur_screen = 1; host_shift = 1; host_knobs[0][5] = 0;
    assert(ep_knob(1, 1024 * 10) && host_knobs[0][5] > 3000);
    int16_t z[8] = { 0 }; ph = 0; host_fm_render(0, a, z); ph = 0; ep_digital_render((void *)3, b, z);
    assert(!memcmp(a, b, sizeof a));
    assert(ov->draw(0) == 1 && dims == 0);                   /* digital: free columns, not dimmed */
    host_cur_screen = 3; assert(ov->draw(0) == 1 && dims == 1);   /* string: dimmed under the labels */
    host_cur_screen = 2; assert(!ep_knob(1, 1024) && ov->draw(0) == 0);   /* drwave (2): stock */
    printf("engplus: ok\n");
    return 0;
}
