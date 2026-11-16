/* Host test (docs/86): ELSE-MARIE's fast mix and direct path (tape/tm.c tm_slots_block) against the general per-sample
 * path: 12 slots (1x, pitch up / down, reverse, reverse + pitch, ping-pong, once, panned, a synced tempo ratio, ...),
 * the same output within a few LSB (the fast paths round the combined gain once); and they do most of the work. */
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
uint8_t host_tm_xport[0x80]; int32_t host_tm_loop[2]; uint8_t host_tm_loopon;
int16_t host_tm_out[2][64];
void *host_tm_malloc(unsigned n) { return malloc(n); }
static int16_t val(int t, int32_t n) { return (int16_t)(9000 * sin(n * 2 * M_PI / (97.3 + 13 * t)) + 3000 * sin(n * 0.0137)); }
int host_tm_read(int t, int c, int16_t *dst) { for (int k = 0; k < 0x8000; k++) dst[k] = val(t, c * 0x8000 + k); return 1; }
#include "../tape/tm.c"
#define NB 20000
static void run(FILE *f)
{
    tm_on = 2;
    for (int k = 0; k < 8; k++) tm_g[k] = (int16_t)(20000 + 1000 * k);
    tm_service();
    static const int8_t semi[12] = { 0, 5, -7, 0, 0, 0, 0, 0, 0, 3, 0, 12 };
    for (int s = 0; s < SL_N; s++) {
        volatile tm_slm *q = &tm_sl[s];
        q->len = 200000 + s * 997; q->ch = (uint8_t)(s & 3); q->lvl = (uint8_t)(20 + s);   /* (no channel overload: the general path clamps per channel, the fast mix only at the output) */ q->a = 0; q->d = 0; q->s = 100; q->r = 10;
        q->semi = semi[s]; q->rev = s == 3 || s == 9; q->mode = s == 4 ? 2 : s == 5 ? 1 : 0; q->pan = (int8_t)(s == 6 ? -30 : s == 10 ? 20 : 0);
        q->pmode = (uint8_t)(s == 2 || s == 9 || s == 11);       /* stretch: down, reverse + up, an octave up */
        if (s == 8) { q->sync = 1; q->bpm = 120 << 8; tm_sl_tr[8] = 72090; }   /* 1.1x */
        { const char *e = getenv("SL_ONLY"), *x = getenv("SL_SKIP"); q->pend = (!e || atoi(e) == s) && (!x || atoi(x) != s); }
    }
    for (int b = 0; b < NB; b++) {
        if (b == 12000) { tm_sl[7].gate = 0; }                    /* a release */
        tm_block(); tm_service();
        fwrite(host_tm_out, 1, sizeof host_tm_out, f);
    }
    FILE *h = fopen("build/slfast_paths", "w"); fprintf(h, "%u %u", sl_fast_blocks, sl_direct_blocks); fclose(h);
    fclose(f);
}
int main(void)
{
    char *buf[2]; size_t len[2]; unsigned fast = 0, direct = 0;
    for (int k = 0; k < 2; k++) {                                /* each run in a fresh child: all static state new */
        int fd[2]; assert(pipe(fd) == 0);
        pid_t p = fork();
        if (!p) { close(fd[0]); host_sl_fmx = k; run(fdopen(fd[1], "wb")); _exit(0); }
        close(fd[1]); FILE *r = fdopen(fd[0], "rb"); buf[k] = 0; len[k] = 0;
        FILE *m = open_memstream(&buf[k], &len[k]); char b[65536]; size_t n;
        while ((n = fread(b, 1, sizeof b, r)) > 0) fwrite(b, 1, n, m);
        fclose(m); fclose(r); int st; waitpid(p, &st, 0); assert(WIFEXITED(st) && !WEXITSTATUS(st));
        if (k) { FILE *h = fopen("build/slfast_paths", "r"); assert(fscanf(h, "%u %u", &fast, &direct) == 2); fclose(h); }
    }
    assert(len[0] == len[1] && len[0] == (size_t)NB * 2 * 64 * 2);
    const int16_t *a = (const int16_t *)buf[0], *b = (const int16_t *)buf[1];
    int maxd = 0; double se = 0, sa = 0;
    for (size_t i = 0; i < len[0] / 2; i++) { const int d = abs(a[i] - b[i]); if (d > maxd) maxd = d; se += (double)d * d; sa += (double)a[i] * a[i]; }
    const double rms_a = sqrt(sa / (len[0] / 2)), rms_d = sqrt(se / (len[0] / 2));
    printf("slfast: max diff %d, diff rms %.2f (signal rms %.0f), fast %u / direct %u slot blocks of %d\n", maxd, rms_d, rms_a, fast, direct, NB * 12);
    if (getenv("SL_ONLY")) return 0;
    assert(rms_a > 1000);                                        /* it played */
    { int first = -1; for (size_t i = 0; i < len[0] / 2; i++) if (abs(a[i] - b[i]) > 128) { first = (int)i; break; }
      if (first >= 0) printf("first > 128 at block %d sample %d: %d vs %d\n", first / 128, first % 128, a[first], b[first]); }
    { int nb = 0, last = -1; for (size_t i = 0; i < len[0] / 2; i++) if (abs(a[i] - b[i]) > 128 && (int)(i / 128) != last) { last = (int)(i / 128); if (nb++ < 12) printf(" block %d", last); }
      printf(" (%d blocks differ > 128)\n", nb); }
    assert(maxd <= 128 && rms_d < 8);                            /* rounding only (several slots at a chunk change: up to ~75) */
    assert(fast > NB * 4 && direct > NB * 5);                    /* the fast paths did most of the work */
    free(buf[0]); free(buf[1]);
    printf("slfast ok\n");
    return 0;
}
