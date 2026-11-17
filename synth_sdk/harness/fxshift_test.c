/* Host test of screens/fxshift.h (op1_fx_shift / op1_fx_push_bit): steps per click on words 5 / 6, the 7-bit fields
 * of word 7, push bits, an unregistered FX untouched, a CC keeps the push bits.   cc fxshift_test.c */
#include <stdio.h>
#include "op1_plugin.h"
#undef op1_fx_shift
#undef op1_fx_push_bit
#include "../screens/fxshift.h"
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static const op1_fx_t A, B;
int main(void)
{
    op1_fx_shift(&A, 0, 8); op1_fx_shift(&A, 1, 1); op1_fx_shift(&A, 2, 6); op1_fx_shift(&A, 3, 1); op1_fx_push_bit(&A, 3, 15);
    CHECK(fxs_n(&A, 0) == 8 && fxs_n(&A, 1) == 1 && !fxs_n(&B, 0), "registration");
    int16_t w[8] = { 0 }; int32_t acc[4] = { 0 };
    CHECK(fxs_turn(&acc[0], w, 0, 1024, 8) && (w[5] * 8 >> 15) == 1, "blue: unset -> step 1 after a click (%d)", w[5]);
    CHECK(!fxs_turn(&acc[0], w, 0, 512, 8), "half a click: nothing");
    fxs_turn(&acc[0], w, 0, 512, 8); for (int i = 0; i < 20; i++) fxs_turn(&acc[0], w, 0, 1024, 8);
    CHECK((w[5] * 8 >> 15) == 7, "blue stops at the last step (%d)", w[5] * 8 >> 15);
    fxs_turn(&acc[1], w, 1, 400, 1); CHECK(w[6] == 100, "green fine: delta / 4 (%d)", w[6]);
    for (int i = 0; i < 3; i++) fxs_turn(&acc[2], w, 2, 1024, 6);
    CHECK((w[7] & 127) == 3 && !(w[7] >> 8), "white field 3 (%04x)", (uint16_t)w[7]);
    for (int i = 0; i < 200; i++) fxs_turn(&acc[3], w, 3, 1024, 1);
    CHECK(((w[7] >> 8) & 127) == 127 && (w[7] & 127) == 3, "red field 127, white kept (%04x)", (uint16_t)w[7]);
    CHECK(fxs_push(&A, w, 3, 1) && (w[7] & 0x8000) && ((w[7] >> 8) & 127) == 127, "red push toggles bit 15 (%04x)", (uint16_t)w[7]);
    CHECK(fxs_push(&A, w, 3, 0) && (w[7] & 0x8000), "release taken, nothing changed");
    CHECK(!fxs_push(&A, w, 0, 1) && !fxs_push(&B, w, 3, 1), "other knobs / FX: not ours");
    CHECK((uint16_t)fxs_keep_push_bits(&A, w[7], 0x1234) == (0x1234 | 0x8000), "a CC keeps bit 15");
    CHECK(fxs_keep_push_bits(&B, w[7], 0x1234) == 0x1234, "no push bits: as written");
    for (int i = 0; i < 8; i++) op1_fx_shift((const op1_fx_t *)(&A + 1 + i), 0, 2);   /* fill the table */
    CHECK(!fxs_n(&B, 0), "table full: unregistered stays stock");
    printf(fails ? "fxshift: %d failed\n" : "fxshift: ok\n", fails);
    return fails != 0;
}
