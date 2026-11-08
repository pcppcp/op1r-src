/* TB-3PO pattern generator. Port of TB-3PO (O_C Phazerville, software/src/applets/TB3PO.h):
 *
 *   Copyright (c) 2020, Logarhythm
 *   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
 *   documentation files (the "Software"), to deal in the Software without restriction, including without limitation
 *   the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and
 *   to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *   The above copyright notice and this permission notice shall be included in all copies or substantial portions of
 *   the Software.
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
 *   THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 *   CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *   IN THE SOFTWARE.
 *   (Contributions: Qiemem's reseed(); extensive modifications by djphazer.)
 *
 * The generation as the applet's regenerate_pitches() / apply_density() (two halves of 16 steps, each with the
 * random generator re-seeded from seed + phase, so a seed always gives the same pattern for a density and scale).
 * The random generator is our own (xorshift32; the applet used Arduino's random()), so a seed gives other patterns
 * than on the module. Scales: a fixed list instead of the O_C quantizer; octave flags move by one scale span (as
 * the applet's "+ scale_size"). GPL-3.0-or-later for the OP-1 adaptation (see LICENSE and NOTICE). */
#include "tb_core.h"

static const uint8_t sc_steps[TB_SCALES][13] = {   /* size, then semitones */
    { 7, 0, 2, 3, 5, 7, 8, 10 },                         /* minor */
    { 7, 0, 2, 4, 5, 7, 9, 11 },                         /* major */
    { 7, 0, 2, 3, 5, 7, 9, 10 },                         /* dorian */
    { 7, 0, 1, 3, 5, 7, 8, 10 },                         /* phrygian */
    { 7, 0, 2, 4, 5, 7, 9, 10 },                         /* mixolydian */
    { 7, 0, 2, 3, 5, 7, 8, 11 },                         /* harmonic minor */
    { 5, 0, 3, 5, 7, 10 },                               /* minor pentatonic */
    { 5, 0, 2, 4, 7, 9 },                                /* major pentatonic */
    { 6, 0, 3, 5, 6, 7, 10 },                            /* blues */
    { 12, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 },        /* chromatic */
};
const char *const tb_scale_name[TB_SCALES] = { "minor", "major", "dorian", "phrygian", "mixolyd", "harm min",
                                               "min pent", "maj pent", "blues", "chromatic" };
int tb_scale_size(int scale) { return sc_steps[scale < 0 || scale >= TB_SCALES ? 0 : scale][0]; }

static uint32_t rng;
static void seed_rng(uint32_t s) { rng = s * 2654435761u + 0x9E3779B9u; if (!rng) rng = 1; }
static uint32_t rnd(uint32_t max)                    /* 0 .. max-1 */
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return max ? rng % max : 0;
}
static int rand_bit(int prob) { return (int)rnd(100) < prob; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void pitches(tb_pat *p, int half)             /* regenerate_pitches(), steps 16 * half .. */
{
    int size = tb_scale_size(p->scale), dens = clampi(p->density, 0, 8), avail;
    if (dens > 7) avail = size - 1;
    else if (dens < 2) avail = dens;                 /* root only, then root and the next degree (303 half steps) */
    else {
        int range = size - 3; if (range < 4) range = 4;
        avail = clampi(3 + (dens - 3) * range / 4, 1, size - 1);
    }
    if (!half) p->oct_up = p->oct_down = 0;
    for (int s = 16 * half; s < 16 * half + 16; s++) {
        if (s > 0 && rand_bit(50 - dens * 6)) p->notes[s] = p->notes[s - 1];   /* repeat the previous note */
        else {
            p->notes[s] = (uint8_t)rnd((uint32_t)avail + 1);
            p->oct_up <<= 1; p->oct_down <<= 1;
            uint32_t c = rnd(200);
            if (c < 80) { if (c & 1) p->oct_up |= 1; else p->oct_down |= 1; }   /* 40 %: an octave up or down */
        }
    }
}
static void density(tb_pat *p, int half)             /* apply_density() */
{
    int onoff = p->density > 7 ? p->density - 7 : 7 - p->density, prob = 10 + onoff * 14, ls = 0, la = 0;
    if (!half) p->gates = p->slides = p->accents = 0;
    for (int i = 0; i < 16; i++) {
        p->gates = p->gates << 1 | (uint32_t)rand_bit(prob);
        ls = rand_bit(ls ? 10 : 18); p->slides = p->slides << 1 | (uint32_t)ls;    /* fewer consecutive slides */
        la = rand_bit(la ? 7 : 16); p->accents = p->accents << 1 | (uint32_t)la;   /* ... and accents */
    }
}
void tb_generate(tb_pat *p)
{
    p->density = (uint8_t)clampi(p->density, 0, 14);
    seed_rng(p->seed + 1u); pitches(p, 0);           /* the applet's phases 1..4, each from seed + phase */
    seed_rng(p->seed + 2u); density(p, 0);
    seed_rng(p->seed + 3u); pitches(p, 1);
    seed_rng(p->seed + 4u); density(p, 1);
}

int tb_semitones(const tb_pat *p, int step, int transpose)
{
    int size = tb_scale_size(p->scale), sc = p->scale < 0 || p->scale >= TB_SCALES ? 0 : p->scale;
    int s = step & (TB_STEPS - 1), d = p->notes[s] + transpose;
    if (p->oct_up >> s & 1) d += size; else if (p->oct_down >> s & 1) d -= size;
    int oct = d >= 0 ? d / size : -((-d + size - 1) / size);
    return oct * 12 + sc_steps[sc][1 + d - oct * size];
}
