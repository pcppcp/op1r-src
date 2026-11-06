/* GPL-3.0-or-later with OP-1 firmware permission; see LICENSE/NOTICE. */
#ifndef VAPORA_PARAMS_H
#define VAPORA_PARAMS_H
#include <stdint.h>
#include "vapora_characters.h"
enum { PS_INTERVALS=10, PS_UNISON=2, PS_FIFTH=5, PS_OCTAVE=9 };
/* Ordered musical choices; legacy 8192/16384/30000 retain unison/fifth/octave. */
static const int8_t ps_semitones[PS_INTERVALS]={-12,-7,0,3,4,7,9,10,11,12};
static const char *const ps_interval_names[PS_INTERVALS]={
    "-OCTAVE","-FIFTH","UNISON","+MIN3","+MAJ3","+FIFTH","+MAJ6","+MIN7","+MAJ7","+OCTAVE"};
enum { PS_MORPH, PS_THICK, PS_BRIGHT, PS_MOTION, PS_SPEED, PS_CONTROLS };
typedef struct { int32_t q[PS_CONTROLS]; int character, interval; } ps_params;
static inline int32_t ps_clamp(int32_t x, int32_t lo, int32_t hi)
{ return x < lo ? lo : x > hi ? hi : x; }
static inline int32_t ps_secondary(int16_t raw)
{ return raw <= 0 ? 8192 : raw; }
static inline ps_params ps_decode(const int16_t words[8])
{
    ps_params p;
    for (int i=0;i<4;i++) p.q[i]=ps_clamp(words[i],0,32767);
    p.q[PS_SPEED]=ps_secondary(words[7]);
    p.character=(((ps_secondary(words[5])+128)>>8)+PS_CHARACTERS-(32%PS_CHARACTERS))%PS_CHARACTERS;
    p.interval=(ps_secondary(words[6])*PS_INTERVALS)>>15;
    return p;
}
static inline int ps_speed_millihz(int32_t q)
{ return 20 + (((q*q)>>15)*1980 >> 15); }
static inline int32_t ps_interval_ratio(int i)
{
    static const int32_t ratios[PS_INTERVALS]={16384,21870,32768,38968,41285,49097,55109,58386,61858,65536};
    return ratios[ps_clamp(i,0,PS_INTERVALS-1)];
}
#endif
