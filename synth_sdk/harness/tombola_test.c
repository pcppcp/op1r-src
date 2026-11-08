/* Host test for seq/tombola.c: what R246's tombola_player_advance receives for a Gfx frame of n ticks. Every advance
 * must stay at or under a normal frame's TOMBOLA_STEP ticks, at or over R246's 10-tick floor, and add up to the
 * stock clamped total; normal frames pass through unchanged. */
#include <stdio.h>
#include "../seq/tombola.h"

static int calls, got[16], fails;
static void *seen;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

void host_tombola_advance(void *player, int ticks)
{
    seen = player;
    if (calls < 16) got[calls] = ticks;
    calls++;
}

int main(void)
{
    int player;
    for (int t = 0; t <= 400; t++) {
        calls = 0; seen = 0;
        tombola_advance(&player, t);
        int sum = 0, lo = 1 << 30, hi = 0;
        for (int i = 0; i < calls && i < 16; i++) { sum += got[i]; if (got[i] < lo) lo = got[i]; if (got[i] > hi) hi = got[i]; }
        CHECK(seen == &player, "t %d: player pointer not passed through", t);
        if (t <= TOMBOLA_STEP) {                        /* a normal frame: one stock advance, unchanged */
            CHECK(calls == 1 && got[0] == t, "t %d: %d calls, first %d", t, calls, got[0]);
            continue;
        }
        int want = t > TOMBOLA_MAX ? TOMBOLA_MAX : t;
        CHECK(sum == want, "t %d: advances add up to %d, want %d", t, sum, want);
        CHECK(hi <= TOMBOLA_STEP, "t %d: an advance of %d ticks (the stock single step that lets balls escape)", t, hi);
        CHECK(lo >= 10, "t %d: an advance of %d ticks, R246 would skip it", t, lo);
        CHECK(hi - lo <= 1, "t %d: uneven advances %d..%d", t, lo, hi);
        CHECK(calls <= 4, "t %d: %d advances", t, calls);
    }
    calls = 0; tombola_advance(&player, 102);
    CHECK(calls == 4 && got[0] == 26 && got[1] == 26 && got[2] == 25 && got[3] == 25, "102: %d calls %d %d %d %d", calls, got[0], got[1], got[2], got[3]);
    printf(fails ? "tombola_test: %d FAILED\n" : "tombola_test: ok\n", fails);
    return fails != 0;
}
