/* Dry DSP evidence generator; native firmware ADSR is deliberately absent. */
#include "sid_dsp.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char **argv){
 if(argc!=4)return 2;sid_state s;sid_reset(&s);
 int16_t q[4]={(int16_t)atoi(argv[1]),(int16_t)atoi(argv[3]),24576,8192};
 sid_configure(&s,(uint32_t)strtoul(argv[2],0,0),q);
 for(int i=0;i<88200;i++){int16_t sample=sid_sample(&s);fwrite(&sample,2,1,stdout);}return ferror(stdout)?1:0;
}
