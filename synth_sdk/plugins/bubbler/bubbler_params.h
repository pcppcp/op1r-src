/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BB_PARAMS_H
#define BB_PARAMS_H
struct bb_params {
    int scatter, tone, pitch, reverse, density, size_var, pitch_var, jitter;
    int size, resonance;
} ;
void bb_decode(const short p[8], struct bb_params *v);
void bb_encode(short p[8], const struct bb_params *v);
#endif
