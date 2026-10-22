#define main vapora_validation_main
#include "../tests/host.c"
#undef main
int main(int argc,char **argv)
{
    assert(argc==2);reset();memcpy(ps_host_words[0],preset_words[8],sizeof defaults);
    ps_host_words[0][2]=32767;SCREEN_vapora.init(0);
    assert(ps_test_sun_design()==0);
    for(int f=-32;f<=80;f++){
        double t=f<0?0:f/80.;
        ps_host_words[0][2]=(int16_t)lround(32767*(.5+.5*cos(t*3.141592653589793)));
        if(f==-1)ps_host_words[0][2]=32766;
        if(f>=0){
            char path[1024];snprintf(path,sizeof path,"%s/frame-%03d.svg",argv[1],f);
            drawing=fopen(path,"w");assert(drawing);
            fputs("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"320\" height=\"160\">\n",drawing);
        }
        ps_host_cycles+=25000000;SCREEN_vapora.update(0,63);
        if(drawing){fputs("</svg>\n",drawing);fclose(drawing);drawing=0;}
        for(int v=0;v<6;v++)assert(ps_mail[v].sequence==0);
    }
    puts("PASS: Holy Dust idle showcase, default Solid, no note_on/render calls and no note telemetry.");
    return 0;
}
