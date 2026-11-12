/* Host test of seqdev/ms_core.c (docs/39): record notes + CCs on the tape timeline, playback with note lengths, slot
 * splits, persistence through the file, streaming through the RAM cache, cut / copy / paste, a damaged slot, the loop
 * wrap, MIDI channel routing, chains after a lost slot write, a full clipboard, a failed quantize move.   cc -I.. -fsanitize=address,undefined mseq_test.c ../seqdev/ms_core.c */
#include "../seqdev/ms_core.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int32_t tpos; static int tplay, lon; static int32_t lin, lout;
static ms_slot disk[MS_SLOTS]; static int disk_ok[MS_SLOTS]; static ms_head dhead; static int dhead_ok;
static int reads, writes, drop_slot = -1;           /* drop_slot: that slot's writes are lost (power off) */
typedef struct { int t, dest, ch, kind, d1, d2, on; int32_t pos; } call_t;
static call_t calls[4096]; static int ncalls;
static int32_t e_pos(void) { return tpos; }
static int e_playing(void) { return tplay; }
static int e_loop(int32_t *a, int32_t *b) { *a = lin; *b = lout; return lon; }
static void e_play(int t, int dest, int ch, int kind, int d1, int d2, int on)
{ if (ncalls < 4096) calls[ncalls++] = (call_t){ t, dest, ch, kind, d1, d2, on, tpos }; }
static int e_rs(int sl, ms_slot *o) { reads++; if (!disk_ok[sl]) return -1; *o = disk[sl]; return 0; }
static int e_ws(int sl, const ms_slot *i) { writes++; if (sl == drop_slot) return -1; disk[sl] = *i; disk_ok[sl] = 1; return 0; }
static int e_rh(void *h, int n) { if (!dhead_ok) return -1; memcpy(h, &dhead, n); return 0; }
static int e_wh(const void *h, int n) { memcpy(&dhead, h, n); dhead_ok = 1; return 0; }
static uint32_t e_off(void) { return 0; }
static void e_on(uint32_t m) { (void)m; }
static const ms_env env = { e_pos, e_playing, e_loop, e_play, e_rs, e_ws, e_rh, e_wh, e_off, e_on };
static ms_t A, B;                      /* (large: static) */

static void run(ms_t *s, int32_t to, int step) { while (tpos < to) { tpos += step; ms_block(s); } }
static void rec(ms_t *s, int t, int32_t from, int n, int step)   /* n short notes on track t */
{
    tplay = 1; tpos = from; ms_block(s); s->sel = t; s->rec = 1;
    for (int i = 0; i < n; i++) { ms_note_in(s, 60, 100, 0, 1); ms_note_in(s, 60, 0, 0, 0); tpos += step; ms_block(s); }
    s->rec = 0; tplay = 0; ms_block(s);
}
static void settle(ms_t *s) { for (int k = 0; k < 10; k++) ms_service(s, 1000); }
static int last_slot(int t, int p) { int sl = dhead.first[t][p]; while (disk[sl].next != MS_NONE) sl = disk[sl].next; return sl; }
static void reseal(int sl) { disk[sl].crc = 0; disk[sl].crc = ms_crc(&disk[sl], 16 + 8 * disk[sl].n); }
static void hang(int sig) { (void)sig; printf("FAIL: a chain walk did not return (cycle)\n"); _exit(1); }
static int notes_on(int from, int t) { int n = 0; for (int i = from; i < ncalls; i++) n += calls[i].on && calls[i].kind == MS_NOTE && calls[i].t == t; return n; }

int main(void)
{
    setvbuf(stdout, 0, _IONBF, 0);
    /* record track 0: C4 0.5 s long at ~1 s, E4 at ~2 s, CC 74 sweep */
    ms_init(&A, &env);
    tplay = 1; tpos = 0; A.sel = 0; A.rec = 1;
    run(&A, 44100, 64); ms_note_in(&A, 60, 100, 0, 1);
    run(&A, 66150, 64); ms_note_in(&A, 60, 0, 0, 0);
    run(&A, 88200, 64); ms_note_in(&A, 64, 90, 0, 1);
    for (int v = 0; v < 100; v++) { ms_cc_in(&A, 74, v, 0); run(&A, tpos + 64, 64); }   /* thinned to 1 per 10 ms */
    ms_note_in(&A, 64, 0, 0, 0);
    A.rec = 0; tplay = 0; ms_block(&A);
    int cc = ms_events(&A, 0, 0, MS_MAXPOS, 0, 0, 1000) - 2;
    assert(cc >= 10 && cc <= 20);
    ms_ev ev[8]; int32_t ps[8];
    int ne = ms_events(&A, 0, 0, 88300, ev, ps, 8);
    printf("  %d events, C4 at %d len %d blocks, E4 at %d\n", ne, ps[0], ev[0].len, ps[1]);
    int nn = 0; for (int i = 0; i < ne; i++) nn += ev[i].kind == MS_NOTE;
    assert(nn == 2 && ev[0].d1 == 60 && ev[0].len >= 22050 / 64 && ev[0].len <= 22050 / 64 + 2);
    /* playback from the start: two note-ons, C4 released ~0.5 s later */
    ncalls = 0; tplay = 1; tpos = 0; ms_block(&A); run(&A, 130000, 64);
    assert(notes_on(0, 0) == 2);
    int on = -1, off = -1;
    for (int i = 0; i < ncalls; i++) if (calls[i].kind == MS_NOTE && calls[i].d1 == 60) { if (calls[i].on) on = i; else off = i; }
    assert(on >= 0 && off > on && calls[off].pos - calls[on].pos >= 22050 - 128 && calls[off].pos - calls[on].pos <= 22050 + 128);
    tplay = 0; ms_block(&A);
    printf("record / playback (notes, lengths, %d CCs) ok\n", cc);

    /* slot split: 200 notes in one page stay sorted; then persistence */
    tplay = 1; tpos = 10 * 65536; ms_block(&A); A.sel = 1; A.rec = 1;
    for (int i = 0; i < 200; i++) { ms_note_in(&A, 40 + i % 40, 64, 0, 1); ms_note_in(&A, 40 + i % 40, 0, 0, 0); tpos += 64; ms_block(&A); }
    A.rec = 0; tplay = 0; ms_block(&A);
    static ms_ev big[300]; static int32_t bp[300];
    int nb = ms_events(&A, 1, 0, MS_MAXPOS, big, bp, 300);
    assert(nb == 200);
    for (int i = 1; i < nb; i++) assert(bp[i] >= bp[i - 1]);
    for (int k = 0; k < 20; k++) ms_service(&A, 1000);
    assert(dhead_ok && !A.head_dirty);
    ms_init(&B, &env); assert(ms_load(&B) == 0);
    tpos = 0; ms_service(&B, 0);
    assert(ms_load_range(&B, 1, 0, MS_PAGES - 1) == 0 && ms_count(&B, 1) == 200 && ms_count(&B, 0) == ms_count(&A, 0));
    printf("slot splits (200 notes in one page), file round trip ok\n");

    /* streaming: notes on track 2 every page over 3 minutes; play through with ms_service every 16 blocks */
    ms_init(&A, &env); dhead_ok = 0; memset(disk_ok, 0, sizeof disk_ok);
    A.sel = 2; A.rec = 1; tplay = 1; tpos = 0; ms_block(&A);
    for (int p = 0; p < 120; p++) {
        tpos = p * 65536 + 1000; ms_block(&A);
        for (int k = 0; k < 3; k++) { ms_note_in(&A, 50 + k, 80, 0, 1); ms_note_in(&A, 50 + k, 0, 0, 0); }
        if (p % 8 == 0) ms_service(&A, 1000);
    }
    A.rec = 0; tplay = 0; ms_block(&A);
    for (int k = 0; k < 20; k++) ms_service(&A, 1000);
    ms_init(&B, &env); assert(ms_load(&B) == 0);
    reads = 0; ncalls = 0; tpos = 0; ms_service(&B, 0); tplay = 1; ms_block(&B);
    for (int blk = 0; tpos < 120 * 65536; blk++) { tpos += 64; ms_block(&B); if (blk % 16 == 0) ms_service(&B, 4); }
    assert(notes_on(0, 2) == 360 && B.underruns == 0);
    int resident = 0; for (int b = 0; b < MS_RAM; b++) resident += B.ram_slot[b] != MS_NONE;
    printf("streaming: 360 notes over 120 pages, %d slot reads, %d resident at the end, 0 underruns ok\n", reads, resident);
    tplay = 0; ms_block(&B);
    {   /* loop over pages 40..44 with a full cache (after the streaming pass): the loop start stays resident */
        lon = 1; lin = 40 * 65536 + 1000; lout = 44 * 65536 + 30000; B.underruns = 0;
        tpos = lin; tplay = 0; ms_block(&B);
        for (int k = 0; k < 20; k++) ms_service(&B, 1000);           /* settle at the loop start */
        ncalls = 0; tplay = 1; ms_block(&B);
        for (int blk = 0; blk < 3 * ((lout - lin) / 64) + 50; blk++) {
            tpos += 64; if (tpos > lout) tpos = lin + (tpos - lout);
            ms_block(&B); if (blk % 16 == 0) ms_service(&B, 4);
        }
        assert(B.underruns == 0 && notes_on(0, 2) >= 3 * 3 * 4);
        tplay = 0; lon = 0; ms_block(&B);
        printf("loop over pages 40..44: start pages kept, 0 underruns ok\n");
    }

    /* cut / copy / paste on track 2: cut pages 10..19, paste at page 200 */
    B.sel = 2;
    int before = (ms_load_range(&B, 2, 0, MS_PAGES - 1), ms_count(&B, 2));
    static ms_ev e0[64], e1[64]; static int32_t p0[64], p1[64];
    int n0 = ms_events(&B, 2, 10 * 65536 - 5, 20 * 65536, e0, p0, 64);
    assert(ms_cut(&B, 10 * 65536 - 5, 20 * 65536, 1 << 2) == 0 && B.nclip == 30 && ms_count(&B, 2) == before - 30);
    assert(B.clip_first == p0[0] && !B.clip_all);           /* the cursor goes to the first lifted note */
    assert(ms_paste(&B, B.clip_first) == 0 && ms_count(&B, 2) == before);
    assert(ms_events(&B, 2, 10 * 65536 - 5, 20 * 65536, e1, p1, 64) == n0 && !memcmp(p0, p1, sizeof(int32_t) * n0));
    assert(ms_cut(&B, 10 * 65536, 20 * 65536, 1 << 2) == 0 && ms_paste(&B, 200 * 65536) == 0 && ms_count(&B, 2) == before);
    assert(ms_events(&B, 2, 200 * 65536, 210 * 65536, 0, 0, 100) == 30);
    assert(ms_copy(&B, 0, 65536 * 3, 1 << 2) == 0 && B.nclip == 9 && ms_count(&B, 2) == before);
    {   /* all tracks: lift, paste back into the source tracks while another track is selected */
        int c0 = ms_count(&B, 0), c2 = ms_count(&B, 2);
        assert(ms_cut(&B, 0, 65536 * 3, 15) == 0 && B.clip_all && ms_count(&B, 2) == c2 - 9);
        B.sel = 1;
        assert(ms_paste(&B, B.clip_first) == 0 && ms_count(&B, 2) == c2 && ms_count(&B, 0) == c0);
        B.sel = 2;
    }
    printf("cut / copy / paste ok\n");

    {   /* quantize: grid steps at 120 bpm, a take, a selection, a note crossing a page boundary */
        assert(ms_grid_step(MS_Q_4, 120 << 8) == 22050 && ms_grid_step(MS_Q_16, 120 << 8) == 5512 &&
               ms_grid_step(MS_Q_8T, 120 << 8) == 7350 && ms_grid_step(MS_Q_OFF, 120 << 8) == 0);
        static ms_t QQ; ms_t *Q = &QQ; ms_init(Q, &env); lon = 0;
        int32_t st = ms_grid_step(MS_Q_16, 120 << 8);
        static ms_ev qe[32]; static int32_t qp[32];
        Q->sel = 1; Q->rec = 1; tplay = 1;
        int32_t at[4] = { st * 10 + 100, st * 11 - 300, 65536 - 200, st * 30 + st / 2 + 5 };   /* 65536-200 -> next page */
        for (int i = 0; i < 4; i++) { tpos = at[i]; ms_block(Q); ms_note_in(Q, 60 + i, 90, 0, 1); tpos += 640; ms_block(Q); ms_note_in(Q, 60 + i, 0, 0, 0); }
        Q->rec = 0; tplay = 0; ms_block(Q);
        assert(Q->ntake == 4 && ms_quantize_take(Q, st) == 4 && Q->ntake == 0);
        int n = ms_events(Q, 1, 0, 40 * st, qe, qp, 32);
        assert(n == 4);
        for (int i = 0; i < n; i++) assert(qp[i] % st == 0 && qe[i].len == 10);
        assert(qp[0] == st * 10 && qp[1] == st * 11 && qp[2] == (65536 + st / 2) / st * st && qp[3] == st * 31);
        /* selection: a note off the grid, outside the selection one stays */
        Q->rec = 1; tplay = 1; tpos = st * 50 + 777; ms_block(Q); ms_note_in(Q, 70, 90, 0, 1); ms_note_in(Q, 70, 0, 0, 0);
        tpos = st * 60 + 777; ms_block(Q); ms_note_in(Q, 71, 90, 0, 1); ms_note_in(Q, 71, 0, 0, 0); Q->rec = 0; tplay = 0; ms_block(Q);
        ms_take_reset(Q);
        assert(ms_quantize(Q, 1, st * 45, st * 55, st) == 1);
        n = ms_events(Q, 1, st * 45, st * 65, qe, qp, 32);
        assert(n == 2 && qp[0] == st * 50 && qp[1] == st * 60 + 777);
        printf("quantize: grid, take, selection, page boundary ok\n");
    }

    /* damaged slot: loads as empty, others survive */
    for (int k = 0; k < 20; k++) ms_service(&B, 1000);
    int victim = B.h.first[2][5];
    disk[victim].ev[0].d1 ^= 1;
    ms_init(&A, &env); assert(ms_load(&A) == 0);
    ms_load_range(&A, 2, 0, MS_PAGES - 1);
    assert(ms_count(&A, 2) == before - 3);
    printf("damaged slot: dropped, the rest loads ok\n");

    /* loop wrap: loop 1.0 .. 1.4 s on a note at 1.2 s plays each pass; MIDI destination uses the track channel */
    ms_init(&A, &env); dhead_ok = 0; memset(disk_ok, 0, sizeof disk_ok);
    A.sel = 3; A.rec = 1; tplay = 1; tpos = 52920; ms_block(&A); ms_note_in(&A, 70, 100, 5, 1); ms_note_in(&A, 70, 0, 5, 0);
    A.rec = 0; A.h.dest[3] = MS_DEST_MIDI; A.h.ch[3] = 9;
    lon = 1; lin = 44100; lout = 61740; tplay = 0; ms_block(&A);
    ncalls = 0; tplay = 1; tpos = 44100; ms_block(&A);
    for (int i = 0; i < 3 * 276 + 10; i++) { tpos += 64; if (tpos > lout) tpos = lin + (tpos - lout); ms_block(&A); }
    assert(notes_on(0, 3) == 3);
    for (int i = 0; i < ncalls; i++) if (calls[i].on) assert(calls[i].dest == MS_DEST_MIDI && calls[i].ch == 9);
    printf("loop wrap, MIDI channel routing ok\n");

    signal(SIGALRM, hang); alarm(20);
    {   /* a slot write lost (power off) after a cut freed the old first slot: the stale copy on disk points back */
        ms_init(&A, &env); dhead_ok = 0; memset(disk_ok, 0, sizeof disk_ok); lon = 0;
        rec(&A, 0, 1000, 100, 256); settle(&A);                  /* slot 0 -> slot 1 */
        ms_cut(&A, 0, 1000 + 40 * 256, 1); settle(&A);           /* slot 0 freed, first = 1 */
        drop_slot = 0; rec(&A, 0, 1000 + 40 * 256 + 64, 70, 64); settle(&A); drop_slot = -1;   /* 1 splits into 0: lost */
        assert(disk[1].next == 0 && disk[0].next == 1 && !(dhead.used[0] & 1));
        ms_init(&B, &env); assert(ms_load(&B) == 0);
        tpos = 0; ms_service(&B, 4);                             /* (hung here every boot) */
        assert(ms_load_range(&B, 0, 0, 0) == 0 && ms_count(&B, 0) == disk[1].n && B.ram[B.where[1]].next == MS_NONE);
        settle(&B); assert(disk[1].next == MS_NONE);             /* the cut chain is saved */
    }
    {   /* a cycle through used slots, a link into another page's chain: cut there, the events of both stay */
        ms_init(&A, &env); dhead_ok = 0; memset(disk_ok, 0, sizeof disk_ok);
        rec(&A, 1, 1000, 100, 256); rec(&A, 1, 3 * 65536, 5, 256); settle(&A);
        int l0 = last_slot(1, 0);
        assert(l0 != dhead.first[1][0]);
        disk[l0].next = dhead.first[1][0]; reseal(l0);
        ms_init(&B, &env); assert(ms_load(&B) == 0);
        tpos = 0; ms_service(&B, 4);
        assert(ms_load_range(&B, 1, 0, MS_PAGES - 1) == 0 && ms_count(&B, 1) == 105);
        disk[l0].next = dhead.first[1][3]; reseal(l0);
        ms_init(&B, &env); assert(ms_load(&B) == 0);
        assert(ms_load_range(&B, 1, 0, MS_PAGES - 1) == 0 && ms_count(&B, 1) == 105);
        /* resident cycle (RAM): playback and drawing walks stay bounded, the next prefetch cuts it */
        B.ram[B.where[l0]].next = B.h.first[1][0];
        ms_events(&B, 1, 0, 65535, 0, 0, 1 << 30);
        tplay = 1; tpos = 0; ms_block(&B); run(&B, 65536, 64); tplay = 0; ms_block(&B);
        assert(ms_load_range(&B, 1, 0, 0) == 0 && ms_count(&B, 1) == 105);
    }
    printf("chains after a lost slot write: stale / foreign links and cycles cut, walks bounded ok\n");
    {   /* LIFT of more events than the clipboard holds: refused, nothing lost */
        ms_init(&A, &env); dhead_ok = 0; memset(disk_ok, 0, sizeof disk_ok);
        A.sel = 0; A.rec = 1; tplay = 1; tpos = 0; ms_block(&A);
        for (int i = 0; i < 600; i++) { tpos += 441; ms_block(&A); ms_cc_in(&A, 1, i & 127, 0); }
        A.rec = 0; tplay = 0; ms_block(&A);
        int n = ms_count(&A, 0);
        assert(n > MS_CLIP && ms_cut(&A, 0, 4 * 65536, 1) == -1 && ms_count(&A, 0) == n && A.clip_len == -1);
        assert(ms_paste(&A, 0) == -1 && ms_count(&A, 0) == n);
        assert(ms_cut(&A, 0, 65536, 1) == 0 && A.nclip == n - ms_count(&A, 0) && ms_paste(&A, A.clip_first) == 0 && ms_count(&A, 0) == n);
    }
    printf("full clipboard: LIFT refused, nothing lost ok\n");
    {   /* quantize move into a page that can't get a slot (no free buffer): the note stays */
        ms_init(&A, &env); dhead_ok = 0; memset(disk_ok, 0, sizeof disk_ok);
        int32_t st = ms_grid_step(MS_Q_16, 120 << 8);
        rec(&A, 3, 65536 - 200, 1, 64);                           /* snaps to the next page */
        for (int b = 0; b < MS_RAM; b++) if (A.ram_slot[b] == MS_NONE) A.ram_slot[b] = MS_SLOTS - 1 - b;   /* taken */
        assert(ms_quantize_take(&A, st) == 0 && ms_count(&A, 3) == 1);
        for (int b = 0; b < MS_RAM; b++) if (A.ram_slot[b] == MS_SLOTS - 1 - b) A.ram_slot[b] = MS_NONE;
        rec(&A, 3, 65536 - 300, 1, 64);
        assert(ms_quantize_take(&A, st) == 1 && ms_count(&A, 3) == 2);
    }
    printf("quantize: a failed move keeps the note ok\n");
    alarm(0);
    printf("mseq_test: ok\n");
    return 0;
}
