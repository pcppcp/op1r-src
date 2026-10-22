/* Original 48-star base plus stock-count, stock-size Interval overlays.
 * Original geometry/logic; GPL-3.0-or-later with OP-1 firmware permission. */
#ifndef VAPORA_PARTICLES_H
#define VAPORA_PARTICLES_H
#ifdef VAPORA_HOST
extern int ps_host_particle_layer;
#endif
typedef struct {uint8_t count,large,medium,tiny;} ps_star_field;
static const ps_star_field ps_star_fields[5]={
    {12,8,4,0}, {32,8,8,0}, {24,0,18,0}, {40,0,8,0}, {48,0,0,1}
};
typedef struct {int16_t x,y;} ps_star_position;
static void particle_layer(void *r,const ps_params *p,int cy,int overlay,ps_star_position *base)
{
    #ifdef VAPORA_HOST
    ps_host_particle_layer=overlay;
    #endif
    int interval=ps_clamp(p->interval,0,PS_INTERVALS-1);
    const ps_star_field *field=&ps_star_fields[interval/2];
    int night=32767-p->q[PS_BRIGHT],green=overlay&&(interval&1);
    int count=overlay?field->count:PS_PARTICLES;
    for(int k=0;k<count;k++){
        unsigned seed=(unsigned)k*0x9e3779b9u+(overlay?(0xa17e2b91u+(unsigned)interval*0x85ebca6bu):0x53544152u);
        seed^=seed>>16;seed*=0x7feb352du;seed^=seed>>15;
        int x=5+(int)(((uint64_t)(uint32_t)(seed+particle_phase)*307u)>>32);
        int wander=coherent((particle_phase>>20)+(seed&1023),(seed>>20)&1023);
        int y=7+(int)((seed>>16)%43);
        y+=(rounded_wave(particle_phase+(unsigned)k*0x23000000u)>>14);
        y+=(wander*(2+(night>>13)))>>10;
        y=ps_clamp(y,5,55);
        if(!overlay){base[k].x=(int16_t)x;base[k].y=(int16_t)y;}
        else {
            /* Keep the original strokes visible. Move added stars past a base
             * stroke's footprint, with a bounded search and no allocations. */
            int clear=0;
            for(int attempt=0;attempt<PS_PARTICLES*2+1;attempt++){
                int collision=-1;
                for(int b=0;b<PS_PARTICLES;b++){
                    int bx=x-base[b].x,by=y-base[b].y;
                    if(bx>-9&&bx<9&&by>-6&&by<6){collision=b;break;}
                }
                if(collision<0){clear=1;break;}
                x=base[collision].x+9;
                if(x>311)x=5;
            }
            if(!clear)continue;
        }
        int dx=x-160,dy=y-cy;
        int shape=ps_sun_shapes[sun_design];
        int radius=shape==18?30:shape==2?24:(shape==21||shape==22)?22:18;
        int exclusion=overlay?28:24;
        if(shape==21&&dx*dx+dy*dy<exclusion*exclusion)continue;
        int light=ps_clamp((dx*dx+dy*dy-radius*radius)*6,0,255);
        light=(light*(192+(night>>9)))>>8;
        int large=overlay?k<field->large:k%6==0;
        int medium=overlay?(!large&&k<(field->large+field->medium)):(k%6==1||k%6==2);
        int tiny=overlay&&field->tiny;
        uint8_t dot[4]={(uint8_t)(((large?170:medium?140:115)*light)>>8),
            (uint8_t)(((large?220:medium?195:175)*light)>>8),(uint8_t)light,255};
        if(green)for(int c=0;c<3;c++)dot[c]=(uint8_t)((ps_text_colors[1][c]*light+127)/255);
        line(r,x*1024,y*1024,x*1024+(large?1200:medium?800:650),y*1024,
             (large?PS_BIG_DOT_STROKE:medium?PS_MED_DOT_STROKE:tiny?PS_TINY_DOT_STROKE:PS_DOT_STROKE),dot);
    }
}
#include "vapora_star_logos.h"
static void draw_particles(void *r,const ps_params *p,int cy)
{
    ps_star_position base[PS_PARTICLES];
    draw_star_logo(r,p,cy);
    particle_layer(r,p,cy,0,base);
    particle_layer(r,p,cy,1,base);
    #ifdef VAPORA_HOST
    ps_host_particle_layer=-1;
    #endif
}
#endif
