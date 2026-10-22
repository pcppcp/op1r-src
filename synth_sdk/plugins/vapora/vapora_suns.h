/* Approved sun silhouettes, with 1.1.4 display order and stable internal shapes.
 * Fractional circle geometry translates on whole pixels; overlapping joins avoid seams.
 * GPL-3.0-or-later with OP-1 firmware permission. */
#ifndef VAPORA_SUNS_H
#define VAPORA_SUNS_H
static const char *const ps_sun_names[PS_SUN_DESIGNS]={
    "SOLID","LINES","HALO","RETRO","BANDS","SATURN","TRIPLE"};
static uint8_t ps_sun_color[4]={255,255,255,255};
static uint8_t *solid_fill;
/* One free-running display LFO represents the shared rate/depth controls.
 * Audio voices retain their independent phases; this is not a voice meter. */
static void sun_color(uint32_t phase,int depth)
{
    /* Share the Orange lane token with its text and dial (#ff423f). */
    const uint8_t *orange=ps_colors[3];
    int pulse=(rounded_wave(phase)+32768)/2;
    int amount=(pulse*ps_clamp(depth,0,32767)+16383)/32767;
    for(int k=0;k<3;k++){
        ps_sun_color[k]=(uint8_t)(255-((255-orange[k])*amount+16383)/32767);
        if(solid_fill&&solid_fill[k]!=ps_sun_color[k])solid_fill[k]=ps_sun_color[k];
    }
}
static void sun_line(void *r,int cy,int x,int y,int xx,int yy)
{line(r,160*1024+x,cy*1024+y,160*1024+xx,cy*1024+yy,PS_SUN_STROKE,ps_sun_color);}
static void sun_arc(void *r,int cy,int cx,int yy,int rx,int ry,int start,int count,int stride)
{
    for(int j=start;j<start+count;j+=stride){
        int a=j&31,b=(j+stride)&31;
        int x=ps_circle[a][0]*rx,y=ps_circle[a][1]*ry;
        int xx=ps_circle[b][0]*rx,yyy=ps_circle[b][1]*ry;
        int dx=(xx-x)/8,dy=(yyy-y)/8;
        sun_line(r,cy,cx*1024+x-dx,yy*1024+y-dy,
                     cx*1024+xx+dx,yy*1024+yyy+dy);
    }
}
/* Approved solid keeper: native cubic disk and same-radius moving cover. */
static void *solid_group,*solid_cover;
static void sun_init(void *scr)
{
    solid_group=op1_find_node(scr,"eclipse_curve");
    solid_cover=op1_find_node(scr,"cover_curve");
    solid_fill=op1_node_fill_rgba(scr,"sunfill");
    sun_color(0,0);
    if(solid_group)op1_node_set_visible(solid_group,0);
}
static int sun_offset(int opening)
{
    int q=ps_clamp(opening,0,32767);
    return (int)(((int64_t)q*q*(40*1024))/(32767LL*32767));
}
static void sun_prepare(int cy,int design,int opening,int hidden)
{
    if(!solid_group||!solid_cover)return;
    op1_node_set_visible(solid_group,!hidden&&design==0&&opening>0);
    int32_t m[9]={1024,0,0,0,1024,0,140*1024,(cy-20)*1024,1024};
    op1_node_set_transform(solid_group,m);
    m[6]=sun_offset(opening);m[7]=0;
    op1_node_set_transform(solid_cover,m);
    op1_node_set_visible(solid_cover,opening<32767);
}
static int sun_half_width(int y,int radius)
{
    int x=radius;while(x>0&&x*x+y*y>radius*radius)x--;
    return x;
}
static void draw_sun(void *r,int cy,int design,int opening)
{
    switch(ps_sun_shapes[design]){
    case 1: sun_arc(r,cy,0,0,15,15,0,32,1);break;
    case 2:
        for(int y=-20;y<=-1;y++){int x=sun_half_width(y,21);sun_line(r,cy,-x*1024,y*1024,x*1024,y*1024);}
        for(int y=4;y<=14;y+=5){int x=sun_half_width(y,21);sun_line(r,cy,-x*1024,y*1024,x*1024,y*1024);}break;
    case 3:
        sun_arc(r,cy,0,0,15,15,0,32,1);
        for(int y=-10;y<=10;y+=5){int x=sun_half_width(y,15);sun_line(r,cy,-x*1024,y*1024,x*1024,y*1024);}break;
    case 21: break; /* Solid is rendered by the native SVG before stars/water. */
    case 22: {
        /* Approved trial 21: seven direct 4px strokes, on a 6px row grid. */
        static const uint8_t span[20]={20,19,19,19,19,19,19,18,18,17,17,16,16,15,14,13,12,10,8,6};
        int offset=(sun_offset(opening)+512)/1024;
        for(int y=-18;y<=18;y+=6){
            int x=span[y<0?-y:y],right=ps_clamp(offset-x,-x,x);
            if(right>-x)line(r,(160-x)*1024,(cy+y)*1024,
                              (160+right)*1024,(cy+y)*1024,4096,ps_sun_color);
        }break;}
    case 10: sun_arc(r,cy,0,0,12,12,0,32,1);sun_arc(r,cy,0,2,21,5,0,32,1);break;
    case 18:
        sun_arc(r,cy,-17,0,10,10,0,32,1);sun_arc(r,cy,0,0,14,14,0,32,1);sun_arc(r,cy,17,0,10,10,0,32,1);break;
    }
}
#endif
