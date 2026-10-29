/* Actual adapter callbacks, identical audio schedule; no GIF rendering. */
#define main vapora_validation_main
#include "../tests/host.c"
#undef main
int main(int argc,char **argv)
{
    assert(argc==3);int release=atoi(argv[2]);
    reset();for(int k=0;k<4;k++)op1_eng_page[k]=0;ps_host_shift=0;
    SCREEN_vapora.init(0);SCREEN_vapora.update(0,16);ps_test_audition_reset(0);
    ps_test_footer_set(0);set_pitch(261.625565);
    ENGINE_vapora.note_on(ps_host_voices[0],ps_host_words[0]);
    char path[1024];snprintf(path,sizeof path,"%s/audio-%d.pcm",argv[1],release);
    FILE *pcm=fopen(path,"wb");assert(pcm);
    snprintf(path,sizeof path,"%s/hold-%d.csv",argv[1],release);FILE *csv=fopen(path,"w");assert(csv);
    fputs("frame,age_cycles,extension_cycles,held,impacts\n",csv);
    for(int frame=0;frame<48;frame++){
        if(frame==release)ENGINE_vapora.note_off(ps_host_voices[0]);
        int16_t samples[128];
        for(int block=0;block<22;block++){
            ENGINE_vapora.render(ps_host_voices[0],samples,ps_host_words[0]);
            assert(fwrite(samples,sizeof samples,1,pcm)==1);
        }
        if(frame==8||frame==16||frame==22||frame==26||frame==30){
            snprintf(path,sizeof path,"%s/hold-%d-%02d.svg",argv[1],release,frame);
            drawing=fopen(path,"w");assert(drawing);
            fputs("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"320\" height=\"160\">\n",drawing);
        }
        ps_test_geometry_phase(0,0);ps_host_cycles+=25000000;SCREEN_vapora.update(0,16);
        uint32_t age,extension;ps_test_impact_last_state(&age,&extension);
        fprintf(csv,"%d,%u,%u,%d,%u\n",frame,age,extension,ps_held_serial[0]!=0,ps_test_impact_count());
        if(drawing){fputs("</svg>\n",drawing);fclose(drawing);drawing=0;}
    }
    assert(ps_test_impact_count()==1);fclose(csv);fclose(pcm);
    /* Native text calls must retain all fourteen sound names. */
    for(int family=0;family<14;family++){
        snprintf(path,sizeof path,"%s/character-name-%02d.svg",argv[1],family+1);
        drawing=fopen(path,"w");assert(drawing);fputs("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"320\" height=\"160\">\n",drawing);
        ps_test_character_dial((int16_t)(8192+family*256));
        fputs("</svg>\n",drawing);fclose(drawing);drawing=0;
    }
    return 0;
}
