#include "vapora_dsp.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
/* Dry full-envelope sums; deliberately omit any firmware bus attenuation. */
int main(void) {
    const int layouts[4][6]={{36,43,48,52,55,60},{48,55,60,64,67,72},
        {60,61,62,63,64,65},{60,60,60,60,60,60}};
    int worst=0,solo=0,wc=0,wi=0,wm=0,wt=0,wl=0;
    long long samples=0,clips=0;
    for(int run=0;run<4;run++)for(int c=0;c<PS_CHARACTERS;c++)for(int in=0;in<PS_INTERVALS;in++)
    for(int m=0;m<3;m++)for(int t=0;t<2;t++)for(int l=0;l<4;l++) {
        ps_params p={{m*16383,t*32767,32767,(run&1)?32767:0,(run&1)?32767:8192},c,in};
        ps_voice voices[6];int32_t pitch[6][128];
        for(int v=0;v<6;v++) {
            ps_start(&voices[v],&p,0x50414453u^((run*29+v+1)*0x9e3779b9u)^(unsigned)v,v+1);
            int32_t inc=(int32_t)llround(4294967296.*440.*pow(2.,(layouts[l][v]-69+(run==1?-24:run==2?12:0))/12.)/44100.);
            for(int n=0;n<128;n++)pitch[v][n]=inc;
        }
        for(int b=0;b<350;b++) {
            int sum[128]={0};int16_t out[128];
            for(int v=0;v<6;v++) {
                ps_render(&voices[v],out,pitch[v],32767,&p);
                for(int n=0;n<128;n++) {
                    if(abs(out[n])>solo)solo=abs(out[n]);
                    sum[n]+=out[n];
                }
            }
            for(int n=0;n<128;n++) {
                int a=abs(sum[n]);samples++;
                clips+=sum[n]<-32768||sum[n]>32767;
                if(a>worst){worst=a;wc=c;wi=in;wm=m;wt=t;wl=l;}
            }
        }
    }
    assert(clips==0);
    assert(worst<29490); /* At least ~0.9 dB dry headroom in this sweep. */
    printf("{\"cases\":%d,\"samples\":%lld,\"solo_peak\":%d,\"six_voice_peak\":%d,\"worst\":[%d,%d,%d,%d,%d],\"clipped_samples\":%lld}\n",PS_CHARACTERS*10*3*2*4*4,samples,solo,worst,wc,wi,wm,wt,wl,clips);
    return 0;
}
