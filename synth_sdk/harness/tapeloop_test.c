/* Host test for tape/tapeloop.c: loop lengths from clips (bar snapping, out point, empty tracks), the private entry,
 * free-running tracks across global loop wraps (a 1-bar and a 3-bar track in a 4-bar loop), chunk boundaries, the
 * seam crossfade, a locate (resync), 2x speed, and the stock entry while not plainly playing.
 * The tape tick is modelled: 64 samples per block, one lookup per pass, a new pass at a chunk end or the loop wrap. */
#include "../op1_synth.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct te_entry te_entry_t;
int32_t host_tape_state = 10, host_tape_speed = 64 << 21, host_tape_pos, host_loop_in, host_loop_out, host_loop_on = 1,
        host_tl_beat = 1000;
te_entry_t **host_chunk_tab;
const int32_t (*host_clips)[3]; const uint8_t *host_clips_has;
te_entry_t *host_tail;
static int16_t spl[4][2 * 297];
int16_t *host_splice[4] = { spl[0], spl[1], spl[2], spl[3] };
static int requests;
void host_request(int idx) { (void)idx; requests++; }
te_entry_t *host_lookup(int32_t pos, int32_t *dir);
te_entry_t *host_pop_lru(void);
#include "../tape/tapeloop.c"

#define NC 8                                               /* chunks with data: tape 0 .. 8 * 32768 */
static te_entry_t ents[NC], priv, before_priv;
static te_entry_t *tab[TL_NCHUNK];
static int16_t value(int t, int32_t p) { return (int16_t)(((p * 7) ^ (t * 4099)) & 0x7FFF); }
te_entry_t *host_lookup(int32_t pos, int32_t *dir) { (void)dir; return tab[pos >> TL_SH]; }
te_entry_t *host_pop_lru(void) { te_entry_t *e = host_tail; host_tail = e->prev; e->prev = 0; return e; }

static int32_t clips[512][3]; static uint8_t has[512];
static void clip(int i, int t, int32_t a, int32_t b) { clips[i][0] = t; clips[i][1] = a; clips[i][2] = b; has[i] = 1; }

/* one block of the modelled tick: reads n samples per track starting at the global position, wrapping at out */
static int16_t got[4][64 * 4];
static int block(int n)
{
    int32_t pos = host_tape_pos >> 6, done = 0;
    while (done < n) {
        int32_t dir = 1;
        te_entry_t *e = tl_lookup(pos, &dir);
        assert(e && e->loaded && e->idx == (pos >> TL_SH) && e->base == (pos & ~(TL_CHUNK - 1)));
        int32_t run = n - done, lim = host_loop_on ? host_loop_out : 0x7FFFFFFF;
        if (run > lim - pos) run = lim - pos;
        if (run > e->base + TL_CHUNK - pos) run = e->base + TL_CHUNK - pos;
        for (int t = 0; t < 4; t++)
            for (int i = 0; i < run; i++) got[t][done + i] = e->data[t * TL_CHUNK + (pos - e->base) + i];
        done += run; pos += run;
        if (host_loop_on && pos >= host_loop_out) pos = host_loop_in;
    }
    host_tape_pos = pos << 6;
    return n;
}

int main(void)
{
    setvbuf(stdout, 0, _IONBF, 0);
    for (int c = 0; c < NC; c++) {
        ents[c].data = malloc(4 * TL_CHUNK * 2); ents[c].idx = (int16_t)c; ents[c].base = c << TL_SH; ents[c].loaded = 1;
        for (int t = 0; t < 4; t++) for (int i = 0; i < TL_CHUNK; i++) ents[c].data[t * TL_CHUNK + i] = value(t, (c << TL_SH) + i);
        tab[c] = &ents[c];
    }
    priv.data = calloc(4 * TL_CHUNK, 2); priv.idx = -1; priv.prev = &before_priv; host_tail = &priv;
    host_chunk_tab = tab; host_clips = (const int32_t (*)[3])clips; host_clips_has = has;

    /* 4-bar loop (bar = 4000) across a chunk boundary; clips: 1.025 bars, 2.975 bars, to the out point, none */
    host_loop_in = 30000; host_loop_out = 46000;
    clip(0, 0, 30000, 34100); clip(1, 1, 30500, 41900); clip(2, 2, 30000, 46000); clip(3, 1, 31000, 32000);
    tl_lengths();
    assert(tl_len[0] == 4000 && tl_len[1] == 12000 && tl_len[2] == 0 && tl_len[3] == 0);
    has[0] = 0; clip(4, 0, 30000, 31000); tl_lengths(); assert(tl_len[0] == 4000);      /* a quarter bar: one bar */
    has[4] = 0; has[0] = 1;

    /* not on: the stock entry */
    host_tape_pos = 30000 << 6;
    { int32_t d = 1; assert(tl_lookup(30000, &d) == &ents[0]); }
    tl_toggle(); tl_service();
    assert(tl_on && tl_ent == &priv && tl_len[1] == 12000);

    /* play 3 global loops + : track 0 repeats every 4000, track 1 every 12000 (free running), track 2 as the tape */
    int32_t tau = 0;                                        /* samples played since the start (= since the sync) */
    int checked = 0, seams = 0;
    for (int b = 0; b < 1000; b++) {
        int32_t gpos = host_tape_pos >> 6;
        block(64);
        for (int i = 0; i < 64; i++, tau++) {
            int32_t g = gpos + i; if (g >= host_loop_out) g -= host_loop_out - host_loop_in;
            assert(got[2][i] == value(2, g) && got[3][i] == value(3, g));
            for (int t = 0; t < 2; t++) {
                int32_t L = t ? 12000 : 4000, q = tau % L;
                int16_t want = value(t, 30000 + q);
                if (tau >= L && q < TL_XF) {                 /* seam: crossfaded with the tail after the loop end */
                    int32_t gg = (q << 15) / TL_XF;
                    want = (int16_t)((value(t, 30000 + q) * gg + value(t, 30000 + L + q) * (32768 - gg)) >> 15);
                    seams++;
                }
                if (got[t][i] != want) { printf("t%d tau %d q %d: got %d want %d\n", t, tau, q, got[t][i], want); return 1; }
                checked++;
            }
        }
    }
    assert(tau == 64000 && seams > 0);                      /* 4 global loops: tracks never resynced at the wrap */
    printf("free run: %d samples checked, %d in seams\n", checked, seams);

    /* the splice buffers, rewritten before the wrap: the track's audio straight across it */
    for (int i = 0; i < 2 * 297; i++) spl[0][i] = spl[1][i] = spl[2][i] = 0x1234;
    while ((host_tape_pos >> 6) < host_loop_out - 6000) { block(64); tau += 64; }
    for (int b = 0; b < 20; b++) { block(64); tau += 64; }
    {
        int32_t g = host_tape_pos >> 6, to_wrap = host_loop_out - g;   /* the next wrap is to_wrap samples ahead */
        for (int i = 0; i < 2 * 297; i++) {
            int32_t tt = tau + to_wrap - 297 + i;                         /* the track's time at that splice sample */
            assert(spl[0][i] == value(0, 30000 + tt % 4000));
            assert(spl[1][i] == value(1, 30000 + tt % 12000));
        }
        assert(spl[2][0] == 0x1234 && spl[3][0] == 0);                    /* not looping: the stock buffer stays */
    }
    printf("splice ok\n");
    /* a locate (more than TL_JUMP away) restarts the tracks in phase */
    host_tape_pos = 30200 << 6;
    block(64);
    for (int i = 0; i < 64; i++) assert(got[0][i] == value(0, 30200 + i) && got[1][i] == value(1, 30200 + i));
    host_tape_pos = 38000 << 6;
    block(64);
    for (int i = 0; i < 64; i++) {
        int32_t g = (i << 15) / TL_XF;                      /* 1-bar track: 3rd repeat, seam crossfaded */
        assert(got[0][i] == (int16_t)((value(0, 30000 + i) * g + value(0, 34000 + i) * (32768 - g)) >> 15));
        assert(got[1][i] == value(1, 30000 + 8000 + i));
    }
    /* 2x speed: 128 source samples per block still come from the private windows */
    host_tape_speed = 128 << 21; host_tape_pos = 30000 << 6;
    { int32_t d = 1; tl_sync_req = 1; (void)tl_lookup(30000, &d); }
    block(128);
    for (int i = 0; i < 128; i++) assert(got[0][i] == value(0, 30000 + i) && got[1][i] == value(1, 30000 + i));
    printf("2x ok\n");
    /* recording: the stock entry, the tracks show no playheads */
    host_tape_state = 5;
    { int32_t d = 1; assert(tl_lookup(31000, &d) == &ents[0] && tl_pos[0] == -1); }
    host_tape_state = 10;
    /* toggle off: stock */
    tl_toggle();
    { int32_t d = 1; assert(tl_lookup(31000, &d) == &ents[0]); }
    printf("tapeloop_test: ok (%d requests)\n", requests);
    return 0;
}
