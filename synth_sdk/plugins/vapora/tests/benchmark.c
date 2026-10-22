/* Comparative host timing only. No adapter, GUI, T2, effects or Blackfin timing. */
#include "vapora_dsp.h"
#include <stdint.h>
#include <stdio.h>
#include <time.h>
int main(void)
{
    static const int16_t words[8]={19000,28000,20500,14000,0x5300,8192,16384,6500};
    static const uint32_t frequencies[6]={131,196,262,330,392,523};
    volatile int32_t checksum=0;
    for(int test=0;test<5;test++){
        int count=test==0?1:test==1?3:6;
        ps_params p=ps_decode(words);ps_voice v[6];
        int32_t pitches[6][PS_BLOCK];int16_t out[PS_BLOCK];
        for(int j=0;j<count;j++){
            ps_start(&v[j],&p,(uint32_t)j+100,(uint32_t)j+1);
            for(int n=0;n<PS_BLOCK;n++)
                pitches[j][n]=(int32_t)((uint64_t)frequencies[j]*4294967296ull/44100);
        }
        clock_t begin=clock();
        for(int b=0;b<30000;b++){
            if(test==3)p.character=(b/64)&1;
            for(int j=0;j<count;j++){
                if(test==4)for(int n=0;n<PS_BLOCK;n++)
                    pitches[j][n]+=((b/32)&1)?200:-200;
                ps_render(&v[j],out,pitches[j],32767,&p);
                checksum=out[b&127];
            }
        }
        printf("%s%.9f",test?",":"",(clock()-begin)/(double)CLOCKS_PER_SEC);
    }
    printf("\n");return checksum==INT32_MIN;
}
