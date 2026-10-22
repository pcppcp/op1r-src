#ifndef VAPORA_DSP_H
#define VAPORA_DSP_H
#include "vapora_params.h"
enum { PS_BLOCK=128, PS_VOICES=6, PS_TRACE=16 };
typedef struct {
    uint32_t phase[2], lfo, serial;
    int32_t smooth[PS_CONTROLS], lp1, lp2;
    int32_t aux_gain, ratio;
    int interval, ready;
    int bank_ready,bank_family,bank_lod,old_family,old_lod,bank_mix;
    int32_t up[8];
    unsigned up_pos;
} ps_voice;
typedef struct {
    uint32_t serial, pitch;
    int32_t level;
    int16_t trace[PS_TRACE];
    int16_t primary[4];
} ps_snapshot;
void ps_start(ps_voice *v, const ps_params *p, uint32_t seed, uint32_t serial);
void ps_render(ps_voice *v, int16_t out[PS_BLOCK], const int32_t pitch[PS_BLOCK],
               int32_t velocity, const ps_params *p);
void ps_analyze(const ps_voice *v, const int16_t out[PS_BLOCK],
                const int16_t env[PS_BLOCK], uint32_t pitch, ps_snapshot *s);
uint32_t ps_loop_increment(int32_t pitch);
int ps_level_for(uint32_t increment, int32_t *blend);
#endif
