/* lazy stock screens (dispatcher.c lz_load / lz_guard), as a model: the logic as in dispatcher.c, the Blackfin parts
 * (cli / sti, SP, IPEND, VDK::Sleep, svg_load + init) faked. A find_node from a parse's own init must never wait for
 * that parse, also after another thread's parse or a nested parse in between; another thread's find_node still waits.
 * Keep in step with dispatcher.c. */
#include <stdio.h>
#include <stdint.h>

static uint32_t *lz_obj[0x44];
static volatile uint8_t lz_state[0x44];
static volatile uint32_t lz_sp[0x44];
static uint32_t cur_sp;                       /* the running thread's SP (fake) */
static long sleeps;                           /* VDK::Sleep(1) calls from lz_guard */
static void (*init_of[0x44])(void);

static int lz_load(int i)
{
    int mine = lz_state[i] == 1;
    if (mine) lz_state[i] = 2;
    if (!mine) return 0;
    lz_sp[i] = cur_sp;
    cur_sp -= 0x200;                          /* the init runs deeper on the same stack */
    if (init_of[i]) init_of[i]();
    cur_sp += 0x200;
    lz_sp[i] = 0;
    lz_state[i] = 0;
    return 1;
}
static void lz_guard(uint32_t *o)
{
    for (int i = 0; i < 0x44; i++) {
        if (lz_obj[i] != o) continue;
        if (lz_state[i] == 1) { lz_load(i); return; }
        if (lz_state[i] != 2) return;
        uint32_t sp = cur_sp - 0x40, ip = 0;
        const uint32_t ls = lz_sp[i];
        if ((ls && sp <= ls && ls - sp < 0x10000) || (ip & 0x7FC0)) return;
        for (int n = 0; n < 100 && lz_state[i] == 2; n++) sleeps++;
        return;
    }
}

#define G_SP 0x01200000u                      /* Gfx thread's stack */
#define K_SP 0x00F00000u                      /* KeyHandler's stack */
static uint32_t scrA[4], scrB[4], scrC[4];
static int mode;                              /* 0: plain, 1: K preempts mid-init, 2: A's init parses C (nested) */
static void init_B(void) { lz_guard(scrB); }
static void init_C(void) { lz_guard(scrC); lz_guard(scrA); }  /* C's init also looks into A (being parsed) */
static void init_A(void)
{
    for (int n = 0; n < 20; n++) {
        if (n == 5 && mode == 1) { uint32_t s = cur_sp; cur_sp = K_SP; lz_guard(scrB); cur_sp = s; }
        if (n == 5 && mode == 2) lz_guard(scrC);
        lz_guard(scrA);
    }
}
static void setup(void)
{
    lz_obj[0] = scrA; lz_obj[1] = scrB; lz_obj[2] = scrC;
    init_of[0] = init_A; init_of[1] = init_B; init_of[2] = init_C;
    for (int i = 0; i < 3; i++) { lz_state[i] = 1; lz_sp[i] = 0; }
    sleeps = 0;
}
static int fails;
static void expect(const char *what, long got, long want)
{
    if (got != want) { printf("FAIL %s: %ld, want %ld\n", what, got, want); fails++; }
}

int main(void)
{
    for (mode = 0; mode < 3; mode++) {        /* Gfx parses A in the background; its init looks up A's nodes */
        setup(); cur_sp = G_SP;
        lz_load(0);
        const char *w[] = { "plain parse: self-waits", "parse preempted by another thread's parse: self-waits",
                            "parse with a nested parse: self-waits" };
        expect(w[mode], sleeps, 0);
        expect("A parsed", lz_state[0], 0);
    }
    /* another thread's lookup into a screen being parsed still waits (here: the parse never ends, 100 sleeps) */
    setup(); lz_state[0] = 2; lz_sp[0] = G_SP; cur_sp = K_SP;
    lz_guard(scrA);
    expect("other thread waits for the parse", sleeps, 100);
    /* ... also while that other thread parses a screen of its own */
    setup(); lz_state[0] = 2; lz_sp[0] = G_SP; lz_state[1] = 2; lz_sp[1] = K_SP; cur_sp = K_SP - 0x200;
    lz_guard(scrA);
    expect("other thread (inside its own parse) waits", sleeps, 100);
    if (fails) return 1;
    printf("lzguard ok\n");
    return 0;
}
