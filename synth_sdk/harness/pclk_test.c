/* Host test for midi/pclk.c: pulses on the input -> ticks (96 per quarter, spread, pulse-aligned), FA on the first
 * pulse, tempo timestamps per quarter, FC after 2 silent beats, MIDI clock dropped, all PPQ settings, selector.
 *   cc -DOP1_HOST pclk_test.c ../midi/pclk.c ../screens/op1r_settings.c */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../op1_synth.h"
#include "../screens/op1r_settings.h"

int16_t host_pc_in[64];
int32_t host_pc_raw[128];                                    /* codec block: L, R (high half) */
int32_t host_sync_mode = 2, host_ticks, host_ctrl;
int32_t host_input_source = 1, host_line_jack;          /* line in */
uint32_t host_ms;
static uint32_t ts[64]; static int nts;
static uint8_t inj[16]; static int ninj;
void host_bpm_clock(uint32_t lo, uint32_t hi) { assert(hi == 0); if (nts < 64) ts[nts++] = lo; }
void host_rx_inject(uint8_t b) { inj[ninj++] = b; }
void pc_block(void), pc_poll(void), pc_filter(volatile uint8_t *pk), pc_host_turn(int d);
extern volatile uint8_t pc_src;
static int pc_ppq_of(int src) { static const int p[6] = { 0, 1, 2, 4, 24, 48 }; return p[src]; }
extern volatile uint32_t pc_pulses;

static uint32_t smp;                                         /* test sample clock */
/* one block: a gate of 5 ms starting at each multiple of `per` samples (if per), through a crude high-pass (the
 * firmware's input filter makes a spike on the rising edge); then the tick-side poll, as PlayerManager does */
static int32_t hp_prev, hp_y;
static uint32_t g_start;
static int gate_on(uint32_t t, uint32_t per, uint32_t until) { return per && t >= g_start && t < until && (t - g_start) % per < 220; }
static void block(uint32_t per, uint32_t until)
{
    for (int i = 0; i < 64; i++) {
        int32_t x = gate_on(smp + i, per, until) ? 20000 : 0;
        hp_y = (hp_y * 31) / 32 + x - hp_prev; hp_prev = x;  /* one-pole high-pass */
        host_pc_in[i] = (int16_t)(hp_y > 32767 ? 32767 : hp_y < -32768 ? -32768 : hp_y);
    }
    smp += 64; host_ms = (uint32_t)((uint64_t)smp * 1000 / 44100);
    pc_block(); pc_poll();
}

static void run_ppq(int src, int ppq)
{
    pc_src = (uint8_t)src; host_ticks = 0; nts = 0; ninj = 0;
    uint32_t per = 44100 * 60 / 120 / ppq;                    /* 120 bpm */
    if (44100 * 60 / 120 % ppq) per = 22050 / ppq;           /* (integer periods in the test) */
    uint32_t start = smp + 1000, until = start + per * ppq * 8;   /* 8 beats of pulses */
    g_start = start;
    int max_step = 0, last_ticks = 0;
    while (smp < until + 3 * 22050) {                        /* + 3 beats of silence */
        uint32_t before = smp;
        block(per, until);
        int d = host_ticks - last_ticks; last_ticks = host_ticks;
        if (d > max_step && before > start + per + 64) max_step = d;   /* (first interval: no period yet, one burst) */
        /* at each pulse the count is pulse-aligned: after the block holding pulse k (k >= 1), ticks >= k * 96 / ppq */
        if (before > start && before < until) {
            uint32_t k = (before + 64 - start - 1) / per;      /* pulses completed (0 = the start pulse) */
            if (host_ticks > (int)((k + 1) * 96 / ppq)) fprintf(stderr, "ahead: ppq %d smp %u start %u per %u k %u ticks %d\n", ppq, before, start, per, k, host_ticks);
            assert(host_ticks <= (int)((k + 1) * 96 / ppq));  /* never ahead of the next pulse */
            if (smp - 64 > start + k * per + 64 && host_ticks < (int)(k * 96 / ppq)) fprintf(stderr, "behind: ppq %d k %u ticks %d pulses %u\n", ppq, k, host_ticks, pc_pulses);
            if (smp - 64 > start + k * per + 64)              /* settled after pulse k */
                assert(host_ticks >= (int)(k * 96 / ppq));
        }
    }
    int pulses = ppq * 8;
    fprintf(stderr, "ppq %2d: ticks %d (want %d), max per block %d, quarters %d, injected %d\n",
           ppq, host_ticks, (pulses - 1) * 96 / ppq, max_step, nts, ninj);
    int want = (pulses - 1) * 96 / ppq;                       /* the start pulse is tick 0 */
    assert(host_ticks >= want && host_ticks < want + 96 / ppq); /* + the spread toward a pulse that did not come (< 1 step) */
    assert(ninj == 2 && inj[0] == 0xFA && inj[1] == 0xFC);
    assert(nts == 8);                                         /* one per quarter */
    for (int i = 1; i < nts; i++) { int32_t d = (int32_t)(ts[i] - ts[i - 1]); assert(d > 498000 && d < 502000); }  /* 500 ms */
    if (ppq <= 4) assert(max_step <= 2);                      /* spread, not bursts */
}

/* analog input model: gate -> resonant 2-pole high-pass (coupling caps, rings after each edge) -> level -> noise ->
 * the firmware's 1-pole high-pass; returns the pulses the detector counted for `n` gates at `per` samples */
static uint32_t rng = 1;
static int32_t noise(int amp) { rng = rng * 1103515245u + 12345u; return (int32_t)((rng >> 16) % (2 * amp + 1)) - amp; }
static int analog_count(uint32_t per, int n, double gain_lo, double gain_hi, int width)   /* width: gate length */
{
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;                    /* biquad high-pass, 40 Hz, Q 2 (rings) */
    const double w0 = 2 * 3.14159265 * 40 / 44100, al = __builtin_sin(w0) / (2 * 2.0), c = __builtin_cos(w0);
    const double b0 = (1 + c) / 2 / (1 + al), b1 = -(1 + c) / (1 + al), b2 = b0, a1 = -2 * c / (1 + al), a2 = (1 - al) / (1 + al);
    int32_t fp = 0, fy = 0;
    double cp = 0, cy = 0;                                    /* the second AC coupling (8 Hz): slow overshoot */
    memset(host_pc_in, 0, sizeof host_pc_in); ninj = 0;
    for (int i = 0; i < 1400; i++) { pc_block(); pc_poll(); }   /* 2 s silence before the clock is plugged in */
    uint32_t before = pc_pulses, total = per * (uint32_t)n + 44100;
    for (uint32_t t = 0; t < total; t += 64) {
        for (int i = 0; i < 64; i++) {
            uint32_t s = t + (uint32_t)i;
            double g = gain_hi - (gain_hi - gain_lo) * (double)s / total;   /* level sweeps down over the run */
            double x = (s / per < (uint32_t)n && s % per < (uint32_t)width) ? 12000 * g : 0;
            cy = 0.99886 * (cy + x - cp); cp = x; x = cy;
            double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = x; y2 = y1; y1 = y;
            int32_t v = (int32_t)y + noise(150);
            fy = (fy * 31) / 32 + v - fp; fp = v;
            host_pc_in[i] = (int16_t)(fy > 32767 ? 32767 : fy < -32768 ? -32768 : fy);
        }
        uint32_t had = pc_pulses;
        pc_block(); pc_poll();
        if (pc_pulses != had && getenv("PCLK_TRACE")) fprintf(stderr, "  pulse at gate %u\n", (t + 63) / per);
    }
    return (int)(pc_pulses - before);
}

int main(void)
{
    host_ms = 0;
    run_ppq(3, 4);  run_ppq(1, 1);  run_ppq(2, 2);  run_ppq(4, 24);  run_ppq(5, 48);
    /* PPQ changed while the clock runs (hardware: PlayerManager.cpp:366 assert midi_ext_clock_in >= 0): the pending
     * ticks must never go down, whatever the switch */
    {
        static const uint8_t sw[][2] = { { 1, 3 }, { 1, 5 }, { 3, 5 }, { 5, 1 }, { 2, 4 }, { 4, 1 } };
        for (unsigned k = 0; k < sizeof sw / sizeof sw[0]; k++) {
            pc_src = sw[k][0]; host_ticks = 0; ninj = 0; g_start = smp + 64;
            uint32_t per0 = 22050 / pc_ppq_of(sw[k][0]), until = g_start + 44100 * 3;
            int last = 0;
            for (int b = 0; b < 3000; b++) {
                if (b == 860) pc_src = sw[k][1];             /* mid-interval: ticks already spread with the old share */
                block(b < 860 ? per0 : 22050 / pc_ppq_of(sw[k][1]), until);
                assert(host_ticks >= last); last = host_ticks;
            }
        }
        fprintf(stderr, "ppq switch while running: ticks never go down\n");
    }
    /* MIDI clock dropped while JACK is on; kept on MIDI or outside follow mode */
    pc_src = 3; uint8_t pk[4] = { 0x0F, 0xF8, 0, 0 }; pc_filter(pk); assert((pk[0] & 15) == 0);
    pc_src = 0; pk[0] = 0x0F; pc_filter(pk); assert(pk[0] == 0x0F);
    pc_src = 3; host_sync_mode = 1; pc_filter(pk); assert(pk[0] == 0x0F);
    /* not following: no ticks at all */
    host_ticks = 0; ninj = 0; g_start = smp; for (int i = 0; i < 2000; i++) block(5512, smp + 100000); assert(host_ticks == 0 && ninj == 0);
    host_sync_mode = 2;
    /* selector: MIDI -> right = 4 PPQ, then 24, 48 (stops); left down to MIDI; saved */
    pc_src = 0; pc_host_turn(1024); assert(pc_src == 3 && op1r_setting_get("clksrc", -1) == 3);   /* one value per click (1024) */
    pc_host_turn(4096); assert(pc_src == 5); pc_host_turn(2048); assert(pc_src == 5);
    for (int i = 0; i < 6; i++) pc_host_turn(-2048); assert(pc_src == 0 && op1r_setting_get("clksrc", -1) == 0);
    /* jack input: ringing, level 0.15 .. 1, noise; every gate counted once (4 PPQ 120 bpm, 24 PPQ 120 bpm, 1 PPQ 40 bpm) */
    pc_src = 3;
    /* gate lengths as measured on the device (pulse_clock.py through the notebook's headphone output): 1 ms locked,
     * 5 / 20 ms counted extra pulses on the falling edge; 24 PPQ DIN sync is about 50 % duty */
    static const struct { int src; uint32_t per; int n; double lo, hi; int w; } an[] = {
        { 3, 5512, 64, 0.15, 1.0, 220 }, { 4, 919, 192, 0.15, 1.0, 220 }, { 1, 66150, 12, 0.3, 1.0, 220 },
        { 3, 5512, 64, 1.0, 1.0, 220 }, { 3, 5512, 64, 0.1, 0.1, 220 }, { 3, 5512, 64, 1.0, 1.0, 44 },
        { 3, 5512, 64, 1.0, 1.0, 882 }, { 3, 5512, 64, 0.3, 1.0, 2756 }, { 4, 919, 192, 0.3, 1.0, 460 },
        { 5, 459, 384, 0.3, 1.0, 230 } };
    for (unsigned i = 0; i < sizeof an / sizeof an[0]; i++) {
        pc_src = (uint8_t)an[i].src;
        int c = analog_count(an[i].per, an[i].n, an[i].lo, an[i].hi, an[i].w);
        fprintf(stderr, "analog: ppq %2d period %5u gate %4d, %3d gates, level %.2f..%.2f -> %d pulses\n", (int[]){0,1,2,4,24,48}[an[i].src],
                an[i].per, an[i].w, an[i].n, an[i].lo, an[i].hi, c);
        assert(c == an[i].n);
    }
    printf("pclk_test: ok\n");
        {   /* the jack clock listens to line in / USB audio in only; pc_mute while it follows */
        extern int pc_input_ok(void), pc_mute(void);
        pc_src = 3; host_sync_mode = 2;
        host_input_source = 0; host_line_jack = 0; assert(!pc_input_ok() && !pc_mute());   /* mic */
        host_input_source = 2; host_line_jack = 1; assert(!pc_input_ok());                  /* radio */
        host_input_source = 0; host_line_jack = 1; assert(pc_input_ok() && pc_mute());      /* a cable in the line jack */
        host_input_source = 3; host_line_jack = 0; assert(pc_input_ok() && pc_mute());      /* USB audio in */
        host_sync_mode = 1; assert(!pc_mute());                                              /* not following */
        host_sync_mode = 2; pc_src = 0; assert(!pc_mute());                                 /* MIDI clock */
        host_input_source = 1; pc_src = 3;
        printf("pclk: input gating ok\n");
    }
    {   /* one channel (Shift + red): the clock from it, the other channel stays the input's audio */
        extern volatile uint8_t pc_chan; extern int pc_split(void), pc_mute(void); extern void pc_block(void);
        extern volatile uint32_t pc_pulses; extern void pc_poll(void);
        host_sync_mode = 2; pc_src = 3; host_input_source = 1;
        for (int i = 0; i < 64; i++) { host_pc_raw[2 * i] = (i >= 32 ? 20000 : 0) << 16; host_pc_raw[2 * i + 1] = (int32_t)(i * 100) << 16; }
        pc_chan = 0; assert(pc_mute() && !pc_split());                                   /* both: muted, untouched */
        pc_chan = 1; assert(!pc_mute() && pc_split());                                   /* left clock: right = audio */
        for (int i = 0; i < 64; i++) assert(host_pc_in[i] == i * 100);
        pc_chan = 2; assert(pc_split());
        for (int i = 0; i < 64; i++) assert(host_pc_in[i] == (i >= 32 ? 20000 : 0));
        pc_chan = 1; memset(host_pc_in, 0, sizeof host_pc_in);                           /* the edge on the left only */
        uint32_t p0 = pc_pulses; pc_block(); pc_poll(); assert(pc_pulses == p0 + 1);
        pc_chan = 2; for (int i = 0; i < 64; i++) host_pc_raw[2 * i] = 0;                /* right: a slow ramp, no edge */
        for (int k = 0; k < 4; k++) pc_block();
        pc_poll(); assert(pc_pulses == p0 + 1);
        host_sync_mode = 1; assert(!pc_split());                                         /* not following: stereo sum */
        pc_chan = 0; host_sync_mode = 2;
        printf("pclk: channel ok\n");
    }
    return 0;
}
