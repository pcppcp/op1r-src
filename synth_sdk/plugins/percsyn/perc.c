#pragma GCC optimize ("no-tree-loop-distribute-patterns") /* no implicit libc calls */
/* perc: drum synth engine for op1r. One drum sound per patch, played from any key (C4 = the sound's own pitch).
 *
 *   blue    pitch          +-24 semitones          Shift + blue   type: 10 built-in sounds, then transferred ones
 *   green   length         decays /10 .. x10       Shift + green  distortion (added to the sound's own)
 *   white   punch          pitch drop, attack      Shift + white  level +-12 dB
 *   orange  tone           noise filter, EQ +-3 octaves
 *
 * Sounds are PO-32 patches (21 parameters), so a PO-32 / Microtonic transfer recorded into the slot's sample can
 * add its sounds (perc_xfer). GPL-3.0-or-later with the OP1R firmware permission. */
#include "op1_plugin.h"
#include "perc_dsp.h"
#include "perc_tables.h"
#include "perc.h"

static perc_voice_t voices[OP1_VOICES];
static volatile uint8_t pending[OP1_VOICES];
volatile uint32_t perc_hit_seq;
volatile int32_t perc_hit_vel;

/* the type word (Shift + blue, PERC_TYPES detents) -> sound */
int perc_type_of(const int16_t *params) { int w = params[5] > 0 ? params[5] : 0; return (w * PERC_TYPES) >> 15; }

const uint16_t *perc_patch_of(int type)
{
    if (type < PERC_FACTORY_COUNT) return PERC_FACTORY[type];
    return perc_xfer_patch(type - PERC_FACTORY_COUNT);           /* 0 when that transfer slot is empty */
}

static void macros(perc_macro_t *m, const int16_t *k)
{
    int32_t c[4];
    for (int i = 0; i < 4; i++) c[i] = (k[i] < 0 ? 0 : k[i]) - 16384;   /* -16384 .. 16383 */
    m->pitch = c[0] * 8;                                  /* +-2 octaves, Q16 */
    m->length = (c[1] * 13) + (c[1] >> 2);                /* +-log2(10) ~ 13.29 per unit, Q16 */
    m->punch = c[2] * 4;                                  /* -1 .. 1, Q16 */
    m->tone = c[3] * 12;                                  /* +-3 octaves, Q16 */
    m->dist = (k[6] > 0 ? k[6] : 0) * 2;                  /* 0 .. 1, Q16 */
    m->level_db = k[7] > 0 ? (k[7] - 16384) * 48 : 0;     /* +-12 dB, Q16; 0 = a sound from before the Shift page */
}

static void v_note_on(void *self, const int16_t *params)
{
    (void)params;
    int i = op1_voice_index(self);
    if (i >= 0 && i < OP1_VOICES) pending[i] = 1;          /* velocity is known in render */
}

static void v_render(void *self, int16_t *out, const int16_t *params)
{
    int i = op1_voice_index(self), j;
    if (i < 0 || i >= OP1_VOICES) { for (j = 0; j < OP1_BLOCK; j++) out[j] = 0; return; }
    perc_voice_t *v = &voices[i];
    if (pending[i]) {
        pending[i] = 0;
        const uint16_t *patch = perc_patch_of(perc_type_of(params));
        if (patch) {
            perc_macro_t m; macros(&m, params);
            perc_prepare(&v->p, patch, &m, OP1_VOICE_VEL[i] >> 8);
            perc_trigger(v);
            perc_hit_vel = op1_voice_vel_gain(self); perc_hit_seq++;
        } else v->active = 0;
        v->prev = 0;
    }
    int16_t half[OP1_BLOCK / 2];
    int32_t key = perc_log2((uint32_t)OP1_PITCH_INC[0]) - PERC_REF_INC_L2;
    if (!perc_render(v, half, OP1_BLOCK / 2, key)) {
        for (j = 0; j < OP1_BLOCK; j++) out[j] = 0;
        v->prev = 0;
        return;
    }
    int32_t prev = v->prev;                               /* 44.1 -> 88.2 kHz, linear */
    for (j = 0; j < OP1_BLOCK / 2; j++) {
        int32_t cur = half[j];
        out[2 * j] = (int16_t)((prev + cur) >> 1);
        out[2 * j + 1] = (int16_t)cur;
        prev = cur;
    }
    v->prev = prev;
}

/* Shift on the engine page: a sound from before gets the level word's centre, so Shift + white starts at 0 dB
 * (as granular's secondary_defaults: the firmware's own preset-word setter, after the patch saver is done). */
#ifndef OP1_HOST
#define PATCH(s) ((uint8_t *)0x01DDD7E8 + ((s) & 7) * 0x38)
static int control_slot(void) { return *(volatile int *)0xFF8016D4 & 7; }
#endif
static void shift_defaults(void)
{
#ifndef OP1_HOST
    int s = control_slot();
    int16_t *w = (int16_t *)(uintptr_t)op1_synth_knobs(s);
    if (w[7]) return;
    op1_patch_wait();
    w[7] = 16384;
    ((void (*)(void *, const int16_t *))0x01A00D14)(PATCH(s), w);
#endif
}

static int e_key(int code, int pressed, int page)
{
    if (code == 0x26 && pressed && page) shift_defaults();
    return 0;
}

static void e_service(void) { perc_xfer_service(); }

const op1_engine_ext_t ENGINE_perc = {
    .engine = { v_note_on, v_render, 0,
                { OP1_PRESS_PAGE_STEPS(PERC_TYPES), OP1_PRESS_SHIFT, OP1_PRESS_BROWSE, OP1_PRESS_NONE } },
    .key = e_key,
    .service = e_service,
    .shift_page = 0x07,
};
