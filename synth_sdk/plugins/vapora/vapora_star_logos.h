/* Five outline stars alternate with five green crossed-line signatures. */
#ifndef VAPORA_STAR_LOGOS_H
#define VAPORA_STAR_LOGOS_H
typedef struct {uint8_t tips,inner,rx,ry;uint16_t turn;} ps_star_logo;
static const ps_star_logo ps_star_logos[PS_INTERVALS]={
    {4,30,9,7,0},       {2,100,9,9,0},
    {5,43,8,8,24576},   {2,100,9,9,4096},
    {6,46,8,8,0},       {3,100,9,9,0},
    {7,45,9,8,24576},   {4,100,9,9,0},
    {8,48,9,8,0},       {4,100,10,7,0}
};
static op1_pt star_logo_vertex(const ps_star_logo *logo,int vertex,int x,int y,unsigned rotation,int thickness)
{
    unsigned angle=(logo->turn+(unsigned)vertex*32768u/(logo->tips*2))&32767u;
    int a=angle>>10,b=a+1,f=angle&1023;
    int dx=ps_circle[a][0]+((ps_circle[b][0]-ps_circle[a][0])*f>>10);
    int dy=ps_circle[a][1]+((ps_circle[b][1]-ps_circle[a][1])*f>>10);
    int radius=(vertex&1)?logo->inner:100;
    int xx=dx*logo->rx*radius/100,yy=dy*logo->ry*radius/100;
    int scale=65536+(ps_clamp(thickness,0,32767)*39322+16383)/32767;
    xx=(xx*scale)>>16;yy=(yy*scale)>>16;
    rotation&=32767u;a=rotation>>10;b=a+1;f=rotation&1023;
    int cr=ps_circle[a][0]+((ps_circle[b][0]-ps_circle[a][0])*f>>10);
    int sr=ps_circle[a][1]+((ps_circle[b][1]-ps_circle[a][1])*f>>10);
    return (op1_pt){x*1024+((xx*cr-yy*sr)>>10),y*1024+((xx*sr+yy*cr)>>10)};
}
static unsigned star_logo_seed(int interval)
{
    unsigned seed=0x4c4f474fu+(unsigned)interval*0x85ebca6bu;
    seed^=seed>>16;seed*=0x7feb352du;seed^=seed>>15;return seed;
}
static void star_logo_position(int interval,int *x,int *y)
{
    unsigned seed=star_logo_seed(interval);
    /* Full left-to-right path; 16px margins fit the star at 160% scale. */
    *x=16+(int)(((uint64_t)(uint32_t)(particle_phase+seed)*288u)>>32);
    *y=23+(int)((seed>>20)%15)+(rounded_wave(particle_phase+seed)>>14);
}
static uint32_t star_logo_sqrt(uint64_t n)
{
    uint64_t result=0,bit=(uint64_t)1<<62;
    while(bit>n)bit>>=2;
    while(bit){if(n>=result+bit){n-=result+bit;result=(result>>1)+bit;}else result>>=1;bit>>=2;}
    return (uint32_t)result;
}
static void star_logo_segment(void *r,op1_pt a,op1_pt b,int cy,int opening,const uint8_t *color)
{
    /* Native Solid is painted before procedural lines. Clip signature ink at
     * its disk so the star passes behind it without relocating. A one-pixel
     * margin includes the outline's half-width. Other suns draw over us later. */
    const int radius=21*1024,cx=160*1024,yy=cy*1024;
    if(sun_design!=0||opening==0||
       (a.x<cx-radius&&b.x<cx-radius)||(a.x>cx+radius&&b.x>cx+radius)||
       (a.y<yy-radius&&b.y<yy-radius)||(a.y>yy+radius&&b.y>yy+radius)){
        line(r,a.x,a.y,b.x,b.y,1536,color);return;
    }
    int64_t dx=b.x-a.x,dy=b.y-a.y,ox=a.x-cx,oy=a.y-yy;
    int64_t aa=dx*dx+dy*dy,bb=2*(ox*dx+oy*dy),cc=ox*ox+oy*oy-(int64_t)radius*radius;
    int64_t discriminant=bb*bb-4*aa*cc;
    if(discriminant<=0){line(r,a.x,a.y,b.x,b.y,1536,color);return;}
    int64_t root=star_logo_sqrt((uint64_t)discriminant);
    int64_t enter=(-bb-root)*65536/(2*aa),leave=(-bb+root)*65536/(2*aa);
    if(leave<=0||enter>=65536){line(r,a.x,a.y,b.x,b.y,1536,color);return;}
    if(enter>0)line(r,a.x,a.y,a.x+(int)(dx*enter/65536),a.y+(int)(dy*enter/65536),1536,color);
    if(leave<65536)line(r,a.x+(int)(dx*leave/65536),a.y+(int)(dy*leave/65536),b.x,b.y,1536,color);
}
static void draw_star_logo(void *r,const ps_params *p,int cy)
{
    int interval=ps_clamp(p->interval,0,PS_INTERVALS-1),x,y;
    star_logo_position(interval,&x,&y);
    const ps_star_logo *logo=&ps_star_logos[interval];
    int light=(255*(192+((32767-p->q[PS_BRIGHT])>>9)))>>8;
    const uint8_t *paint=ps_text_colors[1];
    uint8_t color[4]={0,0,0,255};
    for(int c=0;c<3;c++)color[c]=(uint8_t)((paint[c]*light+127)/255);
    #ifdef VAPORA_HOST
    ps_host_particle_layer=2;
    #endif
    unsigned rotation=(unsigned)ps_clamp(p->q[PS_THICK],0,32767)*32768u/32767u;
    if(interval&1){
        /* Plus, diagonal cross, six-ray asterisk, eight-ray asterisk,
         * and a cross-hatched plus. Independent strokes, no polygon fill. */
        int design=interval/2;
        for(int v=0;v<logo->tips;v++){
            op1_pt a=star_logo_vertex(logo,v,x,y,rotation,p->q[PS_THICK]);
            op1_pt b={2*x*1024-a.x,2*y*1024-a.y};
            if(design==4 && (v&1)){ /* Short diagonal crossbars. */
                a.x=x*1024+(a.x-x*1024)/2;a.y=y*1024+(a.y-y*1024)/2;
                b.x=2*x*1024-a.x;b.y=2*y*1024-a.y;
            }
            star_logo_segment(r,a,b,cy,p->q[PS_BRIGHT],color);
            if(design<2){
                /* Capped compass plus / forked diagonal snowflake. */
                for(int end=0;end<2;end++){
                    op1_pt tip=end?b:a;
                    int dx=tip.x-x*1024,dy=tip.y-y*1024;
                    op1_pt u={x*1024+dx*2/3-dy/5,y*1024+dy*2/3+dx/5};
                    op1_pt v={x*1024+dx*2/3+dy/5,y*1024+dy*2/3-dx/5};
                    if(design==0)star_logo_segment(r,u,v,cy,p->q[PS_BRIGHT],color);
                    else {
                        star_logo_segment(r,u,tip,cy,p->q[PS_BRIGHT],color);
                        star_logo_segment(r,tip,v,cy,p->q[PS_BRIGHT],color);
                    }
                }
            }
        }
        return;
    }
    op1_pt first=star_logo_vertex(logo,0,x,y,rotation,p->q[PS_THICK]),previous=first;
    for(int v=1;v<=logo->tips*2;v++){
        op1_pt next=v==logo->tips*2?first:star_logo_vertex(logo,v,x,y,rotation,p->q[PS_THICK]);
        star_logo_segment(r,previous,next,cy,p->q[PS_BRIGHT],color);previous=next;
    }
}
#endif
