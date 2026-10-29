/* Full production rendering: all 14 Character styles at the same clock and note schedule. */
#define main vapora_validation_main
#include "../tests/host.c"
#undef main
extern void ps_test_audition_reset(int);
static void frame(const char *dir,const char *prefix,int index)
{
    char path[1024];snprintf(path,sizeof path,"%s/%s-%03d.svg",dir,prefix,index);
    drawing=fopen(path,"w");assert(drawing);
    fputs("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"320\" height=\"160\">\n",drawing);
    ps_host_cycles+=25000000u;SCREEN_vapora.update(0,16);
    fputs("</svg>\n",drawing);fclose(drawing);drawing=0;
}
int main(int argc,char **argv)
{
    assert(argc==2);
    for(int mode=0;mode<14;mode++){
        reset();for(int k=0;k<4;k++)op1_eng_page[k]=0;
        ps_host_words[0][5]=(int16_t)(8192+mode*256);op1_eng_page[0]=1;ps_test_footer_set(1);
        ps_host_words[0][0]=24576; /* show substantial motion at 75% Morph */
        SCREEN_vapora.init(0);SCREEN_vapora.update(0,16);ps_test_audition_reset(mode);
        char prefix[32];snprintf(prefix,sizeof prefix,"mode-%02d",mode+1);
        for(int i=0;i<64;i++)frame(argv[1],prefix,i);
    }
    reset();for(int k=0;k<4;k++)op1_eng_page[k]=0;ps_test_footer_set(0);
    SCREEN_vapora.init(0);SCREEN_vapora.update(0,16);ps_test_audition_reset(0);
    set_pitch(261.625565);int16_t audio[128];
    for(int i=0;i<96;i++){
        int notes=i==8?1:i==40?3:0;
        for(int v=0;v<notes;v++)ENGINE_vapora.note_on(ps_host_voices[v],ps_host_words[0]);
        if(i>=8)for(int block=0;block<22;block++)for(int v=0;v<(i>=40?3:1);v++)
            ENGINE_vapora.render(ps_host_voices[v],audio,ps_host_words[0]);
        /* Keep the ambient wave at one phase so the temporary deformation is
         * easy to see. Notes use actual DSP publication; only clock is fixed. */
        ps_test_geometry_phase(0,0);frame(argv[1],"trampoline",i);
    }
    assert(ps_test_impact_count()==2);
    /* The same moving wave continues beneath a latched readout backing. */
    reset();for(int k=0;k<4;k++)op1_eng_page[k]=0;
    ps_host_words[0][0]=32767;ps_host_words[0][5]=8192+8*256;
    SCREEN_vapora.init(0);SCREEN_vapora.update(0,16);ps_test_audition_reset(8);
    ps_test_footer_set(0);
    for(int i=0;i<96;i++){
        ps_host_shift=(i==32||i==64);frame(argv[1],"footer",i);
        assert(ps_test_footer()==(i>=32&&i<64));
    }
    /* Measure the radius of displaced vertices at one isolated fixed-phase hit. */
    reset();SCREEN_vapora.init(0);SCREEN_vapora.update(0,16);ps_test_audition_reset(0);
    ps_test_geometry_phase(0,0);ps_params p=ps_decode(defaults);
    op1_pt rest[PS_ROWS*PS_COLS],points[PS_ROWS*PS_COLS],center;int16_t curves[PS_EDGES];
    ps_test_geometry(&p,rest,curves);test_note_mail(0,0x7f000001u);
    ps_test_impact_tick(25000000);ps_test_impact_tick(25000000);ps_test_impact_last(&center);
    char path[1024];snprintf(path,sizeof path,"%s/ripple.csv",argv[1]);FILE *csv=fopen(path,"w");assert(csv);
    fputs("frame,mean_squared_radius_cells,positive_energy,negative_energy,peak_pixels\n",csv);
    double early=0,late=0;int early_n=0,late_n=0;
    for(int f=0;f<24;f++){
        ps_test_geometry(&p,points,curves);double weighted=0,total=0;int positive=0,negative=0,peak=0;
        for(int i=0;i<PS_ROWS*PS_COLS;i++){
            int d=points[i].y-rest[i].y;double dx=(i%PS_COLS)-center.x/256.,dy=((i/PS_COLS)-center.y/256.)*1.25;
            if(d>0){weighted+=d*(dx*dx+dy*dy);total+=d;positive+=d;}else negative-=d;
            if(abs(d)>peak)peak=abs(d);
        }
        double radius=total?weighted/total:0;
        fprintf(csv,"%d,%.6f,%d,%d,%.6f\n",f,radius,positive,negative,peak/1024.);
        if(f<3&&total){early+=radius;early_n++;}
        if(f>=8&&f<=12&&total){late+=radius;late_n++;}
        if(f>=22)assert(!memcmp(rest,points,sizeof rest));
        ps_test_impact_tick(25000000);
    }
    fclose(csv);assert(early_n&&late_n&&late/late_n>early/early_n*3);
    printf("Ripple spreads: mean squared radius %.3f -> %.3f grid cells; exact settling at frame 22\n",early/early_n,late/late_n);
    return 0;
}
