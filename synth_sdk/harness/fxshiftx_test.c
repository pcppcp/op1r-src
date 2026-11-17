/* Host test of screens/fxshiftx.h: registration and defaults, the values follow their FX (another FX: its
 * defaults), steps per click, fine turns, limits.   cc fxshiftx_test.c */
#include <stdio.h>
#include "../screens/fxshiftx.h"
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static const int A, B;
int main(void)
{
    fxx_register(&A, 0, 6, 16384); fxx_register(&A, 2, 1, 1000); fxx_register(&B, 3, 8, 0);
    fxx_register(&A, 8, 6, 0); fxx_register(&A, 1, 0, 0);
    CHECK(fxx_n(&A, 0) == 6 && fxx_n(&A, 2) == 1 && !fxx_n(&A, 1) && !fxx_n(&A, 3) && fxx_n(&B, 3) == 8, "registration");
    fxx_words_t w = { { 0 }, -1 };
    const int16_t *v = fxx_get(&w, &A, 5);
    CHECK(v[0] == 16384 && v[2] == 1000 && w.owner == 5, "defaults for a fresh pick");
    int32_t acc = 0;
    CHECK(fxx_turn(&acc, &w.v[0], 1024, 6) && (w.v[0] * 6 >> 15) == 4, "a click: next step (%d)", w.v[0] * 6 >> 15);
    CHECK(!fxx_turn(&acc, &w.v[0], 512, 6), "half a click");
    for (int i = 0; i < 20; i++) fxx_turn(&acc, &w.v[0], -1024, 6);
    CHECK((w.v[0] * 6 >> 15) == 0, "stops at step 0");
    CHECK(fxx_turn(&acc, &w.v[2], 400, 1) && w.v[2] == 1100, "fine: delta / 4 (%d)", w.v[2]);
    for (int i = 0; i < 400; i++) fxx_turn(&acc, &w.v[2], 1024, 1);
    CHECK(w.v[2] == 32767 && !fxx_turn(&acc, &w.v[2], 1024, 1), "fine stops at the top");
    v = fxx_get(&w, &A, 5); CHECK(v[2] == 32767, "the same FX keeps its values");
    v = fxx_get(&w, &B, 7); CHECK(w.owner == 7 && v[0] == 0 && v[2] == 0, "another FX: its own defaults");
    { int16_t o = w.owner; v = fxx_get(&w, 0, 9); CHECK(v[0] == 0 && v[3] == 0 && w.owner == o, "unregistered FX: zeros, slot untouched"); }
    { static const int C; fxx_register(&C, 5, 1, 777);
      CHECK(fxx_layers(&C) == 2 && fxx_layers(&A) == 1 && fxx_n(&C, 5) == 1 && !fxx_n(&C, 8), "a second layer");
      fxx_words_t y = { { 0 }, -1 }; const int16_t *u = fxx_get(&y, &C, 3); CHECK(u[5] == 777, "layer 2 default"); }
    printf(fails ? "fxshiftx: %d failed\n" : "fxshiftx: ok\n", fails);
    return fails != 0;
}
