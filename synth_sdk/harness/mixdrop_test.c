/* Host test of tape/mixdrop.c: cc -DOP1_HOST mixdrop_test.c ../tape/mixdrop.c -o build/mixdrop_test */
#include <stdint.h>
#include <stdio.h>
void tape_mix4(int16_t *d, const int16_t *const t[4], int32_t n);
int main(void)
{
    static int16_t a[6] = { 100, -100, 30000, -30000, 0, 32767 }, b[6] = { 1, 2, 3, 4, 5, 6 },
                   c[6] = { 0, 0, 10000, -10000, 0, 1 }, e[6] = { -100, 100, 0, 0, 7, 0 };
    const int16_t *t[4] = { a, b, c, e };
    static const int16_t want[6] = { 1, 2, 32767, -32768, 12, 32767 };   /* sums, saturated */
    tape_mix4(a, t, 6);                                                  /* in place, as in the firmware */
    int ok = 1;
    for (int i = 0; i < 6; i++) ok &= a[i] == want[i];
    printf("mixdrop: %s\n", ok ? "OK" : "FAIL");
    return !ok;
}
