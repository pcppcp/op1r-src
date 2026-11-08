/* FX list groups (screens/fxgroups.c): the in-place filter of a fake preset-browser vector. */
#include "../screens/fxgroups.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static uint8_t store[FG_MAX_ROWS * FG_ROW]; static uint8_t *vec[3];   /* (pad), begin, end */
static void row(int i, const char *name, int id)
{
    uint8_t *r = store + i * FG_ROW; int n = (int)strlen(name);
    memset(r, 0, FG_ROW);
    if (n > 15) { char *p = malloc(n + 1); strcpy(p, name); memcpy(r + 4, &p, sizeof p); *(uint32_t *)(r + 0x18) = 31; }
    else { memcpy(r + 4, name, n); *(uint32_t *)(r + 0x18) = 15; }
    *(int32_t *)(r + 0x14) = n; *(int32_t *)(r + 0x1C) = id;
}
static int n_rows(void) { return (int)((vec[2] - vec[1]) / FG_ROW); }
static int id_at(int i) { return *(int32_t *)(store + i * FG_ROW + 0x1C); }
static int lrows(void *l) { uint8_t **v = l; return (int)((v[2] - v[1]) / FG_ROW); }
static int lid(void *l, int i) { uint8_t **v = l; return *(int32_t *)(v[1] + i * FG_ROW + 0x1C); }
int main(void)
{
    const char *names[] = { "clouds", "cwo", "delay", "ente", "father", "in a room", "a very long plugin name", "tape" };
    int n = 8;
    for (int i = 0; i < n; i++) row(i, names[i], 100 + i);
    vec[1] = store; vec[2] = store + n * FG_ROW;
    uint8_t before[sizeof store]; memcpy(before, store, sizeof store);
    assert(fg_group_of(FG_FX, "in a room", 9) == 1 && fg_group_of(FG_FX, "tape", 4) == 2 && fg_group_of(FG_FX, "tap", 3) == 8 && fg_group_of(FG_SYNTH, "sampler+", 8) == 6 && fg_group_of(FG_SYNTH, "x", 1) == 8);
    assert(fg_list(FG_FX, vec, 0) == (void *)vec);
    void *r = fg_list(FG_FX, vec, 1);
    assert(r && lrows(r) == 2 && lid(r, 0) == 104 && lid(r, 1) == 105);
    void *d = fg_list(FG_FX, vec, 2);
    assert(d != r && lrows(d) == 2 && lid(d, 0) == 102 && lid(d, 1) == 107);
    assert(lrows(r) == 2 && lid(r, 0) == 104);                         /* the previous list stays valid (two buffers) */
    assert(!fg_list(FG_FX, vec, 4));                                      /* empty */
    assert(fg_next(FG_FX, vec, 3, 1) == 5 && fg_next(FG_FX, vec, 5, -1) == 3);   /* modulation skipped */
    assert(fg_next(FG_FX, vec, 8, 1) == 0 && fg_next(FG_FX, vec, 0, -1) == 8);
    void *o = fg_list(FG_FX, vec, 8);
    assert(lrows(o) == 1 && lid(o, 0) == 106);                          /* the long (heap) name */
    assert(!memcmp(before, store, sizeof store));                       /* the source list is untouched */
    { char *p; memcpy(&p, store + 6 * FG_ROW + 4, sizeof p); free(p); }
    printf("fxgroups: ok\n");
    return 0;
}
