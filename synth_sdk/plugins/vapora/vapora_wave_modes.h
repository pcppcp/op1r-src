/* Fourteen selected wave styles, indexed by the existing Character word. */
#ifndef VAPORA_WAVE_MODES_H
#define VAPORA_WAVE_MODES_H
enum { PS_WAVE_MODES=PS_CHARACTERS, PS_IMPACTS=12, PS_IMPACT_FRAMES=22, PS_IMPACT_HOLD_FRAMES=6 };
static int wave_mode;
static const char *const wave_names[PS_WAVE_MODES]={
    "ORIGINAL","RIPPLES","ROLL","FOLD","ORBIT","RINGS","RAIN",
    "PULSE","TIDE","RIBBONS","WEAVE","DUNES","ECHO","JELLY"
};
static int wave_sin(uint32_t phase){return rounded_wave(phase+0x40000000u)>>5;}
static int wave_field(int mode,int col,int row,uint32_t phase)
{
    uint32_t x=(uint32_t)col*0x10000000u,y=(uint32_t)row*0x10000000u;
    int xx=col-5,yy=row-2,r=xx*xx*3+yy*yy*8;
    int a=wave_sin(x+phase),b=wave_sin(y-phase),v=0;
    switch(mode){
        case 1:v=wave_sin(x*3+y+phase*2);break;
        case 2:v=wave_sin(x+y/2-phase);break;
        case 3:v=wave_sin((uint32_t)(xx<0?-xx:xx)*0x18000000u+phase);break;
        case 4:{
            int ox=xx*256-wave_sin(phase)/2,oy=yy*256-wave_sin(phase+0x40000000u)/3;
            v=1024-ps_clamp((ox*ox+oy*oy)/512,0,2048);break;
        }
        case 5:v=wave_sin((uint32_t)r*0x02000000u-phase*2);break;
        case 6:{
            int r2=(xx+2)*(xx+2)*3+(yy-1)*(yy-1)*8;
            v=(wave_sin((uint32_t)r*0x03000000u-phase*3)+
               wave_sin((uint32_t)r2*0x02000000u-phase*2))/2;break;
        }
        case 7:v=(a*a>>9)-1024;break;
        case 8:v=wave_sin(x/3-y/2+phase);break;
        case 9:v=wave_sin(x*2+(uint32_t)row*0x28000000u-phase);break;
        case 10:v=a*b>>10;break;
        case 11:v=coherent(col*112u+(phase>>21),row*170u+(phase>>22));break;
        case 12:v=(wave_sin(x-phase)*2+wave_sin(x*2+phase+y))/3;break;
        case 13:v=(wave_sin(x+phase)+wave_sin(y+phase*2)+wave_sin(x-y-phase))/3;break;
    }
    return ps_clamp(v,-1024,1024);
}
typedef struct {
    uint32_t age,extension,owners[PS_VOICES];
    int x,y;
} ps_impact;
static ps_impact impacts[PS_IMPACTS];
static uint32_t impact_rng=0x7472616du,impact_count,note_serial[PS_VOICES];
static int impact_next,impact_pending;
static uint32_t pending_serial[PS_VOICES];
static void impacts_reset(void)
{
    for(int i=0;i<PS_IMPACTS;i++){
        impacts[i].age=(PS_IMPACT_FRAMES+PS_IMPACT_HOLD_FRAMES)*25000000u;
        impacts[i].extension=0;
        for(int v=0;v<PS_VOICES;v++)impacts[i].owners[v]=0;
    }
    for(int v=0;v<PS_VOICES;v++)pending_serial[v]=0;
    impact_pending=0;impact_next=0;
}
static void impact_spawn(void)
{
    impact_rng^=impact_rng<<13;impact_rng^=impact_rng>>17;impact_rng^=impact_rng<<5;
    ps_impact *p=&impacts[impact_next];impact_next=(impact_next+1)%PS_IMPACTS;
    p->x=640+(int)(impact_rng%1281u); /* grid columns 2.5..7.5, Q8 */
    p->y=256+(int)((impact_rng>>16)%641u); /* rows 1..3.5 */
    p->age=0;p->extension=0;
    for(int v=0;v<PS_VOICES;v++){p->owners[v]=pending_serial[v];pending_serial[v]=0;}
    impact_count++;
}
static void impacts_tick(uint32_t step)
{
    for(int i=0;i<PS_IMPACTS;i++){
        ps_impact *p=&impacts[i];
        if(p->age-p->extension>=PS_IMPACT_FRAMES*25000000u)continue;
        int held=0;
        for(int v=0;v<PS_VOICES;v++)
            if(p->owners[v]&&ps_held_serial[v]==p->owners[v])held=1;
        /* Credit at most 375ms, at one quarter of held time. The fade always
         * advances, and the front keeps moving: no held-note retrigger or freeze. */
        if(held){
            p->extension+=step/4;
            if(p->extension>PS_IMPACT_HOLD_FRAMES*25000000u)
                p->extension=PS_IMPACT_HOLD_FRAMES*25000000u;
        }
        p->age+=step;
    }
    int fresh=0;
    for(int v=0;v<PS_VOICES;v++){
        ps_snapshot s;
        if(ps_read_mail(v,view_slot,view_selector,&s,&sequence[v])){
            if(s.serial&&s.serial!=note_serial[v]){fresh=1;pending_serial[v]=s.serial;}
            note_serial[v]=s.serial;
        }
    }
    /* One extra GUI tick collects a chord across staggered voice publications.
     * Sustained traces/pitch/level never generate additional impacts. */
    if(impact_pending){impact_spawn();impact_pending=0;}
    else if(fresh)impact_pending=1;
}
/* Integer distance in grid Q8: no floating point or native sqrt import. */
static int impact_distance(unsigned n)
{
    unsigned root=0,bit=1u<<22;
    while(bit>n)bit>>=2;
    while(bit){if(n>=root+bit){n-=root+bit;root=(root>>1)+bit;}else root>>=1;bit>>=2;}
    return (int)root;
}
/* Keep the incoming edge sharp. Held credit widens only the trailing return,
 * up to twice its width, and lowers the small rebound from 1/8 to 1/16. */
static int impact_profile(int offset,int amplitude,uint32_t extension)
{
    int hold=ps_clamp((int)(extension/250000u),0,256);
    int tail_width=320+((hold*320)>>8);
    int width=offset<0?tail_width:320;
    int w=ps_clamp(width-(offset<0?-offset:offset),0,width)*1024/width;
    int wake_width=256+(hold>>1),wake=offset+tail_width;
    wake=ps_clamp(wake_width-(wake<0?-wake:wake),0,wake_width)*1024/wake_width;
    return ((w*w>>10)*amplitude-(wake*wake>>10)*(amplitude/(8+(hold>>5))))>>10;
}
static int impact_displacement(int col,int row)
{
    if(col<2||col>8||row==0||row==PS_ROWS-1)return 0;
    int total=0;
    for(int i=0;i<PS_IMPACTS;i++){
        unsigned frame=impacts[i].age/25000000u;
        unsigned fade_frame=(impacts[i].age-impacts[i].extension)/25000000u;
        if(fade_frame>=PS_IMPACT_FRAMES)continue;
        int dx=col*256-impacts[i].x,dy=(row*256-impacts[i].y)*5/4;
        int distance=impact_distance((unsigned)(dx*dx+dy*dy));
        /* One outward pulse, not an oscillating spring at the impact point.
         * Its front travels 4.5 cells; the small wake is just 1/8 strength. */
        int front=128+(int)frame*56,offset=distance-front;
        int remaining=PS_IMPACT_FRAMES-(int)fade_frame;
        int amplitude=remaining*remaining*12288/(PS_IMPACT_FRAMES*PS_IMPACT_FRAMES);
        total+=impact_profile(offset,amplitude,impacts[i].extension);
    }
    /* Absorb the pulse near the fixed silhouette instead of reflecting it. */
    int taper=(col==2||col==8)?512:1024;
    if(row==1||row==4)taper=taper*3/4;
    return ps_clamp(total*taper>>10,-8*1024,8*1024);
}
#endif
