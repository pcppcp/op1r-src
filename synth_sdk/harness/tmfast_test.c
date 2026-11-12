/* Host test (docs/82): tm_fast (the per-block fast path, L1 on the device) gives bit-identical output to the old
 * path: 4 tracks, 1x / 1/2x / reverse / 2x, loops, a loop change, mute; it must handle most blocks. */
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
#define SET32(o, v) (*(int32_t *)(host_tm_xport + (o)) = (v))
static void run(FILE *f)
{
    tm_on = 1; SET32(0x00, 2); SET32(0x08, 0x08000000);
    for (int k = 0; k < 8; k++) tm_g[k] = (int16_t)(20000 + 1000 * k);
    tm_service();
    tm_tr[1].speed = 0x8000; tm_tr[2].speed = -0x10000; tm_tr[3].speed = 0x20000;
    long pos = 200000;
    for (int b = 0; b < 30000; b++) {
        if (b == 100) { tm_tr[0].lin = 190000; tm_tr[0].lout = 190000 + 70000; tm_tr[0].lon = 1; }
        if (b == 9000) { tm_tr[3].lin = 300000; tm_tr[3].lout = 300000 + 40001; tm_tr[3].lon = 1; }
        if (b == 15000) tm_tr[0].lout = 190000 + 50003;          /* loop change */
        if (b == 20000) tm_tr[1].mute = 1;
        SET32(0x14, (int32_t)(pos << 6)); pos += 64;
        tm_block(); tm_service();
        fwrite(host_tm_out, 1, sizeof host_tm_out, f);
        fwrite(tm_v, 1, sizeof tm_v, f);
    }
    fclose(f);
}
int main(void)
{
    char *buf[2]; size_t len[2]; uint32_t hits = 0;
    for (int k = 0; k < 2; k++) {                                /* each run in a fresh child: all static state new */
        int fd[2]; assert(pipe(fd) == 0);
        pid_t p = fork();
        if (!p) { close(fd[0]); tm_nofast = k; run(fdopen(fd[1], "wb")); FILE *h = fopen("build/tmfast_hits", "w"); fprintf(h, "%u", tm_fast_hit); fclose(h); _exit(0); }
        close(fd[1]); FILE *r = fdopen(fd[0], "rb"); buf[k] = 0; len[k] = 0;
        FILE *m = open_memstream(&buf[k], &len[k]); char b[65536]; size_t n;
        while ((n = fread(b, 1, sizeof b, r)) > 0) fwrite(b, 1, n, m);
        fclose(m); fclose(r); int st; waitpid(p, &st, 0); assert(WIFEXITED(st) && !WEXITSTATUS(st));
        if (!k) { FILE *h = fopen("build/tmfast_hits", "r"); assert(fscanf(h, "%u", &hits) == 1); fclose(h); }
    }
    assert(len[0] == len[1] && len[0] > 0 && !memcmp(buf[0], buf[1], len[0]));   /* bit-identical */
    assert(hits > 4 * 30000 * 8 / 10);                           /* and the fast path did the work */
    free(buf[0]); free(buf[1]);
    printf("tmfast ok (fast %u of %d track blocks, identical)\n", hits, 4 * 30000);
    return 0;
}
