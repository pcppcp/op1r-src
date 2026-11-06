/* Host test for midi/mx.c: the params CC map (docs/60) and range scaling.   cc -DOP1_HOST -I.. mx_test.c ../midi/mx.c */
#include <assert.h>
#include <stdio.h>
#include "../midi/mx.h"
static void group_at(int cc, int g, int k) { int kk, id = mx_index(cc); assert(id >= 0); assert(mx_group(id, &kk) == g && kk == k); assert(mx_cc_of(id) == cc); }
int main(void)
{
    /* the agreed layout */
    for (int k = 0; k < 4; k++) { group_at(12 + k, MX_SE, k); group_at(20 + k, MX_SV, k); group_at(33 + k, MX_DE, k);
                                  group_at(40 + k, MX_SF, k); group_at(48 + k, MX_DF, k); group_at(70 + k, MX_MF, k);
                                  group_at(78 + k, MX_DV, k); group_at(102 + k, MX_VOL, k); group_at(106 + k, MX_PAN, k);
                                  group_at(110 + k, MX_MUTE, k); group_at(114 + k, MX_AUX, k); }
    for (int k = 0; k < 3; k++) { group_at(16 + k, MX_SE, 4 + k); group_at(44 + k, MX_SF, 4 + k); group_at(52 + k, MX_DF, 4 + k);
                                  group_at(74 + k, MX_MF, 4 + k); }
    for (int k = 0; k < 8; k++) { group_at(24 + k, MX_SL, k); group_at(56 + k, MX_DL, k); }
    group_at(37, MX_DP, 0); group_at(118, MX_TAPE, 0);
    /* every target has its own CC, none of the avoided ones */
    int n = 0;
    for (int cc = 0; cc < 128; cc++) if (mx_index(cc) >= 0) n++;
    assert(n == MX_N);
    static const int avoid[] = { 0, 1, 6, 7, 10, 11, 32, 38, 64, 65, 66, 67, 68, 69, 91, 93, 96, 97, 98, 99, 100, 101,
                                 120, 121, 122, 123, 124, 125, 126, 127 };
    for (unsigned i = 0; i < sizeof avoid / sizeof avoid[0]; i++) assert(mx_index(avoid[i]) == -1);
    for (int id = 0; id < MX_N; id++) assert(mx_index(mx_cc_of(id)) == id);
    assert(mx_group(-1, 0) == -1 && mx_group(MX_N, 0) == -1);
    /* scaling and its inverse */
    const int16_t full[3] = { 0, 32767, 0 }, sym[3] = { -16384, 16384, 0 }, empty[3] = { 0, 0, 0 };
    assert(mx_scale(full, 0) == 0 && mx_scale(full, 127) == 32767 && mx_scale(full, 64) == 16512);
    assert(mx_scale(sym, 0) == -16384 && mx_scale(sym, 127) == 16384);
    assert(mx_scale(empty, 100) == 0);                       /* unfilled table: min, no write past it */
    for (int v = 0; v < 128; v++) {
        int x = mx_scale(sym, v); assert(x >= -16384 && x <= 16384);
        assert(mx_unscale(sym, x) == v && mx_unscale(full, mx_scale(full, v)) == v);   /* round trip (feedback) */
    }
    printf("mx_test: ok\n");
    return 0;
}
