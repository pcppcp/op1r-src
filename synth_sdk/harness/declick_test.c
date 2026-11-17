/* Host test of dsp/declick.h: a switch adds the jump back and it decays to 0 within a few ms; the same effect again
 * changes nothing; after a gap the dry sample is the reference; an empty-pointer block fades into the dry bus.
 *   cc declick_test.c */
#include <stdio.h>
#include "../dsp/declick.h"
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void fill(int16_t *x, int v) { for (int i = 0; i < 64; i++) x[i] = (int16_t)v; }
int main(void)
{
    dc_state_t s = { 0 }; int16_t x[64];
    fill(x, 20000); dc_run(&s, 1, x, 64, 0, 10);
    CHECK(x[0] == 20000 && x[63] == 20000, "first effect untouched");
    fill(x, -20000); dc_run(&s, 2, x, 64, 0, 11);
    CHECK(x[0] == 20000 && s.switches == 1, "switch: starts at the old output (%d)", x[0]);
    for (int i = 1; i < 64; i++) CHECK(x[i] <= x[i - 1], "monotonic at %d", i);
    int b = 12; do { fill(x, -20000); dc_run(&s, 2, x, 64, 0, (uint32_t)b++); } while (s.off && b < 40);
    CHECK(!s.off && x[63] == -20000 && b < 20, "decayed after %d blocks", b - 12);
    fill(x, 5000); dc_run(&s, 2, x, 64, 0, (uint32_t)b++);
    CHECK(x[0] == 5000 && s.switches == 1, "same effect: no offset");
    fill(x, 9000); dc_run(&s, 3, x, 64, 1000, (uint32_t)b + 5);
    CHECK(x[0] == 1000, "after a gap: the dry sample is the reference (%d)", x[0]);
    dc_state_t g = { 0 }; fill(x, 10000); dc_run(&g, 1, x, 64, 0, 50);
    fill(x, 0); CHECK(dc_gap(&g, x, 64, 51) && x[0] == 10000 && x[63] < 10000 && x[63] > 0, "gap: fades into the dry bus");
    fill(x, 0); CHECK(dc_gap(&g, x, 64, 52), "second dry block goes on");
    fill(x, 0); CHECK(!dc_gap(&g, x, 64, 60), "no gap handling after a pause");
    printf(fails ? "declick: %d failed\n" : "declick: ok\n", fails);
    return fails != 0;
}
