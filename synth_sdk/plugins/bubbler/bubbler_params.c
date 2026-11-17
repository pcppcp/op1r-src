/* SPDX-License-Identifier: GPL-3.0-or-later
 * ESC_CODE / Volker Junginger's Delay Trio (github.com/VolkerJunginger/OP-1, Delay-Trio, e88427c), unchanged.
 * Current format only: Density always owns native parameter 2.
 * Extra controls are packed into words 4–7; no legacy interpretation.
 */
#include "bubbler_params.h"
static int clamp(int x,int a,int b) {
    return x<a?a:x>b?b:x;
}
void bb_decode(const short p[8],struct bb_params *v)
{
    unsigned a=(unsigned short)p[4],b=(unsigned short)p[5],c=(unsigned short)p[6];
    v->scatter=(int)(a&255u)*32767/255;
    v->tone=(int)((a>>8)&127u);
    v->pitch=0;
    v->reverse=(int)(b&255u)*32767/255;
    v->pitch_var=clamp((int)((b>>8)&127u),0,100);
    v->density=(clamp(p[2],0,32767)*100+16383)/32767;
    v->size=clamp(((int)(c&127u)*32767+47)/48,0,32767);
    v->size_var=(int)((c>>7)&255u)*32767/255;
    v->jitter=(int)((unsigned short)p[7]&255u)*32767/255;
    v->resonance=((unsigned short)p[7]>>8)&63u;
}
void bb_encode(short p[8],const struct bb_params *v)
{
    int scatter=(clamp(v->scatter,0,32767)*255+16383)/32767;
    int reverse=(clamp(v->reverse,0,32767)*255+16383)/32767;
    int size=(clamp(v->size_var,0,32767)*255+16383)/32767;
    int jitter=(clamp(v->jitter,0,32767)*255+16383)/32767;
    p[2]=(short)((clamp(v->density,0,100)*32767+50)/100);
    p[4]=(short)(((unsigned short)p[4]&0x8000u) | scatter | (clamp(v->tone,0,127)<<8));
    p[5]=(short)(((unsigned short)p[5]&0x8000u) | reverse | (clamp(v->pitch_var,0,100)<<8));
    p[6]=(short)(((unsigned short)p[6]&0x8000u) | (clamp(v->size,0,32767)*48/32767) | (size<<7));
    p[7]=(short)(((unsigned short)p[7]&0x4000u) | 0x8000u | (clamp(v->resonance,0,63)<<8) | jitter);
}
