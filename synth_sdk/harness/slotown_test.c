/* Host test of screens/slotown.c: the tape page's screen slot with its three owners (ISSUES Y8). Models the slot logic
 * of the MIDI seq view (seqdev/ms_fw.c view_set), FREE (screens/tfscreen.c tfs_set / tfs_poll) and the update prompt
 * (devupd/du_fw.c prompt_show), once as before (each saves "the tape screen" itself) and once with so_install /
 * so_restore, and runs every sequence of on / off toggles up to 9 steps. After each step (and two main loop passes:
 * FREE's first install waits for one that sees the stock object) the slot must hold the tape screen when all three are off, never the object of one that is off, and
 * one of theirs while any is on.
 *   cc -DOP1_HOST -I.. -Wall -fsanitize=address,undefined slotown_test.c ../screens/slotown.c -o build/slotown_test */
#include "../screens/slotown.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { MS, FREE, DU, N };
static const char *nm[N] = { "seq view", "FREE", "prompt" };
static int tape, objs[N];                       /* the stock TapeScreen; ours */
#define T   ((void *)&tape)
#define OBJ(i) ((void *)&objs[i])
static void *tab[1];                            /* the tape slot (id 0; also the current screen) */
static int want[N];                             /* view / tm_on / prompt */

/* ---- before: as origin/main ---- */
static int o_ms_id = -1, o_free_id = -1, o_free_in, o_du_id = -1;
static void *o_ms_tape, *o_free_tape, *o_du_saved;
static void old_view_set(int on)                /* ms_fw.c: tape_obj saved on first open, restored blindly */
{
    if (on && o_ms_id < 0) { o_ms_tape = tab[0]; o_ms_id = 0; }
    if (o_ms_id >= 0) tab[0] = on ? OBJ(MS) : o_ms_tape;
}
static void old_tfs_set(int on)                 /* tfscreen.c: slot found by class (the stock object), saved once */
{
    if (on && o_free_id < 0) { if (tab[0] != T) return; o_free_tape = tab[0]; o_free_id = 0; }
    if (o_free_id < 0) return;
    if (!on && tab[0] != OBJ(FREE)) { o_free_in = 0; return; }
    tab[0] = on ? OBJ(FREE) : o_free_tape;
    o_free_in = on;
}
static void old_prompt_show(int on)             /* du_fw.c */
{
    if (o_du_id >= 0 && (!on || tab[0] != OBJ(DU))) { if (tab[0] == OBJ(DU)) tab[0] = o_du_saved; o_du_id = -1; }
    if (on && o_du_id < 0) { o_du_saved = tab[0]; o_du_id = 0; tab[0] = OBJ(DU); }
}
static void old_pass(void)                      /* main loop: tfs_poll, du_service */
{
    if (want[FREE] && !o_free_in) old_tfs_set(1); else if (!want[FREE] && o_free_in) old_tfs_set(0);
    if (want[DU] || o_du_id >= 0) old_prompt_show(want[DU]);
}
static void old_reset(void) { o_ms_id = o_free_id = o_du_id = -1; o_free_in = 0; }

/* ---- after: the three through slotown ---- */
static so_owner own[N];
static int n_free_id = -1, n_free_in;
static void new_view_set(int on) { if (on) so_install(&own[MS], tab, 0); else so_restore(&own[MS], tab); }
static void new_tfs_set(int on)
{
    if (on && n_free_id < 0) { if (tab[0] != T) return; n_free_id = 0; }
    if (n_free_id < 0) return;
    if (on) so_install(&own[FREE], tab, 0); else so_restore(&own[FREE], tab);
    n_free_in = on;
}
static void new_prompt_show(int on)
{
    if (own[DU].in && (!on || tab[0] != OBJ(DU))) so_restore(&own[DU], tab);
    if (on && !own[DU].in) so_install(&own[DU], tab, 0);
}
static void new_pass(void)
{
    if (want[FREE] && !n_free_in) new_tfs_set(1); else if (!want[FREE] && n_free_in) new_tfs_set(0);
    if (want[DU] || own[DU].in) new_prompt_show(want[DU]);
}
static void new_reset(void)
{
    n_free_id = -1; n_free_in = 0;
    for (int i = 0; i < N; i++) { own[i].in = 0; own[i].obj = OBJ(i); }
}

static int run(int newer, const int *seq, int n, int verbose)    /* toggles; 0 = ok, else the bad step (1-based) */
{
    tab[0] = T; memset(want, 0, sizeof want);
    if (newer) new_reset(); else old_reset();
    for (int k = 0; k < n; k++) {
        const int i = seq[k];
        want[i] ^= 1;
        if (i == MS) { if (newer) new_view_set(want[MS]); else old_view_set(want[MS]); }   /* key thread, at once */
        for (int p = 0; p < 2; p++) { if (newer) new_pass(); else old_pass(); }   /* FREE's first install waits for a
                                                                  * pass that sees the stock object (found by class) */
        int any = 0, holder = -1;
        for (int j = 0; j < N; j++) { any |= want[j]; if (tab[0] == OBJ(j)) holder = j; }
        const int bad = holder >= 0 ? !want[holder] : tab[0] != T ? 1 : any;   /* off owner's object / tape screen while one is on */
        if (verbose) {
            printf("    %s %-8s -> slot %s%s\n", want[i] ? "on " : "off", nm[i], holder >= 0 ? nm[holder] : "tape screen",
                   bad ? (holder >= 0 ? "  <- its owner is off" : "  <- an owner is on but not shown") : "");
        }
        if (bad) return k + 1;
    }
    return 0;
}

static int all(int newer, int len, int *first, int *first_len)
{
    int seq[16], bad = 0, total = 1;
    for (int i = 0; i < len; i++) total *= N;
    for (int c = 0; c < total; c++) {
        for (int i = 0, x = c; i < len; i++, x /= N) seq[i] = x % N;
        const int r = run(newer, seq, len, 0);
        if (r && r == len) {                    /* count each failing prefix once: at its own length */
            if (!bad) { memcpy(first, seq, sizeof(int) * len); *first_len = len; }
            bad++;
        }
    }
    return bad;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    /* FREE used once before (its slot known), then the view on, FREE on (over the view), FREE off, view off */
    static const int y8[] = { FREE, FREE, MS, FREE, FREE, MS };
    /* FREE on, view on (over FREE), FREE off, view off */
    static const int y8b[] = { FREE, MS, FREE, MS };
    /* FREE's slot known, view on, FREE on over it, view off: FREE must stay shown */
    static const int y8c[] = { FREE, FREE, MS, FREE, MS };
    /* the prompt over FREE, FREE off, prompt off */
    static const int y8d[] = { FREE, DU, FREE, DU };
    const struct { const int *s; int n; const char *what; } cases[] = {
        { y8, 6, "(FREE on, off,) view on, FREE on, FREE off, view off" }, { y8b, 4, "FREE on, view on, FREE off, view off" },
        { y8c, 5, "(FREE on, off,) view on, FREE on, view off" }, { y8d, 4, "FREE on, prompt on, FREE off, prompt off" },
    };
    int old_bad = 0;
    for (unsigned c = 0; c < sizeof cases / sizeof *cases; c++) {
        printf("%s\n  before:\n", cases[c].what);
        const int o = run(0, cases[c].s, cases[c].n, 1);
        printf("  after:\n");
        const int n = run(1, cases[c].s, cases[c].n, 1);
        assert(!n);
        old_bad += o != 0;
    }
    assert(old_bad == 4);                       /* y8: view lost while on; y8b, y8d: FREE's object left with FREE off;
                                                 * y8c: FREE lost while on */
    int tot_old = 0, tot_new = 0;
    for (int len = 1; len <= 9; len++) {
        int first[16], fl = 0;
        const int o = all(0, len, first, &fl);
        fl = 0;
        const int n = all(1, len, first, &fl);
        if (n && !tot_new) { printf("after, first wrong:\n"); run(1, first, fl, 1); }
        tot_old += o; tot_new += n;
    }
    printf("every toggle sequence up to 9 steps: before %d end wrong, after %d\n", tot_old, tot_new);
    assert(tot_old > 0 && tot_new == 0);
    printf("slotown: ok\n");
    return 0;
}
