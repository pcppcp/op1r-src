/* Host test for midi/pclk.c: an audio block (pc_block) with a pulse preempts pc_poll in the middle. The pulse is in
 * the queue but newer than the poll's clock if that was read first: since = now - last wraps, the follower stops and
 * restarts (FC FA) mid-play. Here the block runs inside pc_active() (host_ctrl is read there), a 4 PPQ jack clock at
 * 120 bpm, and the follower must keep running: one FA, no FC, the ticks keep coming.
 *   cc -DOP1_HOST pclk_race_test.c */
#include <stdio.h>
#include <stdint.h>
int32_t host_ctrl_fn(void);
#define host_ctrl host_ctrl_fn()                     /* CTRL_MODE: the audio block can land here */
#include "../midi/pclk.c"

int16_t host_pc_in[64]; int32_t host_pc_raw[128];
int32_t host_sync_mode = 2, host_ticks, host_input_source = 1, host_line_jack; uint32_t host_ms;
static uint8_t inj[16]; static int ninj;
void host_bpm_clock(uint32_t lo, uint32_t hi) { (void)lo; (void)hi; }
void host_rx_inject(uint8_t b) { if (ninj < 16) inj[ninj++] = b; }
int op1r_setting_get(const char *k, int d) { (void)k; return d; }
void op1r_setting_put(const char *k, int v) { (void)k; (void)v; }

static uint32_t smp; static int pulse_at = -1, block_in_poll;
static void audio_block(void)
{
    for (int i = 0; i < 64; i++) host_pc_in[i] = i == pulse_at ? 20000 : 0;
    pc_block(); smp += 64; host_ms = (uint32_t)((uint64_t)smp * 1000 / 44100);
}
int32_t host_ctrl_fn(void) { if (block_in_poll) { block_in_poll = 0; audio_block(); } return 0; }

int main(void)
{
    pc_src = 3;                                       /* jack, 4 PPQ */
    const uint32_t per = 5512;                        /* 120 bpm */
    uint32_t next = 1000; int raced = 0, ticks_before = 0;
    for (int b = 0; b < 2000; b++) {
        pulse_at = next >= smp && next < smp + 64 ? (int)(next - smp) : -1;
        if (pulse_at >= 0) next += per;
        if (b >= 1000 && !raced && pulse_at > 0) {    /* this pulse's block preempts the poll */
            ticks_before = host_ticks; block_in_poll = 1; pc_poll(); raced = 1;
            continue;
        }
        audio_block(); pc_poll();
    }
    int fc = 0; for (int i = 0; i < ninj; i++) fc += inj[i] == 0xFC;
    printf("race %s; injected", raced ? "hit" : "missed"); for (int i = 0; i < ninj; i++) printf(" %02X", inj[i]);
    printf("; ticks %d at the race, %d at the end, running %d\n", ticks_before, host_ticks, pc_running);
    int ok = raced && ninj == 1 && inj[0] == 0xFA && !fc && pc_running && host_ticks > ticks_before + 200;
    printf(ok ? "OK\n" : "FAIL: the follower stopped / restarted on a pulse queued during the poll\n");
    return !ok;
}
