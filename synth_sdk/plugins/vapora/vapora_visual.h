/* One place for native layout, stroke and activity budgets. */
#ifndef VAPORA_VISUAL_H
#define VAPORA_VISUAL_H
enum { PS_COLS=11, PS_ROWS=6, PS_EDGES=165, PS_PARTICLES=48, PS_SUN_DESIGNS=7,
       PS_LEFT=2, PS_RIGHT=317, PS_TOP=24, PS_BOTTOM=126, PS_WATER_BOTTOM=151,
       PS_LABEL_Y=130, PS_TEXT_SIZE=12, PS_DIAL_Y=136,
       PS_TEXT_STROKE=1450, PS_MESH_STROKE=1950,
       PS_SUN_STROKE=2200, PS_SUN_FILL_STROKE=2048, PS_TINY_DOT_STROKE=1200, PS_DOT_STROKE=1900, PS_MED_DOT_STROKE=2500, PS_BIG_DOT_STROKE=3200, PS_SUN_RADIUS=15 };
/* Release 1.1.4: new user-facing order; shape IDs preserve approved artwork. */
static const uint8_t ps_sun_ids[PS_SUN_DESIGNS]={1,2,3,4,5,6,7};
static const uint8_t ps_sun_shapes[PS_SUN_DESIGNS]={21,22,1,2,3,10,18};
static inline int ps_sun_y(int q)
/* 75% of the former 26px rise, rounded to whole display pixels. */
{ return 56-(ps_clamp(q,0,32767)*39+32767)/65534; }
static const int ps_depth[PS_ROWS]={64,72,82,94,108,120};
static const int ps_width[PS_ROWS]={314,318,324,332,342,354};
/* Centre response is 100%; outer vertices receive 4%, shoulders interpolate. */
static const int ps_response[PS_COLS]={1311,4096,12288,24576,32768,32768,32768,24576,12288,4096,1311};
/* Faceted side silhouette, ordered in depth: pointed corners without folds.
 * Each ridge fans inward, referencing the green-marked peak in IMG_0108. */
static const int ps_side_x[PS_ROWS]={2,15,2,14,2,8};
static const int ps_side_y[PS_ROWS]={32,61,72,91,101,120};
static const int ps_shoulder_x[PS_ROWS]={43,43,34,44,35,40};
static const int ps_shoulder_y[PS_ROWS]={55,75,87,100,112,123};
static inline int ps_visual_speed_millihz(int32_t q)
{ return 20+(((q*q)>>15)*520>>15); }
static const int16_t ps_circle[33][2]={
{1024,0},{1004,200},{946,392},{851,569},{724,724},{569,851},{392,946},{200,1004},{0,1024},{-200,1004},{-392,946},{-569,851},{-724,724},{-851,569},{-946,392},{-1004,200},{-1024,0},{-1004,-200},{-946,-392},{-851,-569},{-724,-724},{-569,-851},{-392,-946},{-200,-1004},{0,-1024},{200,-1004},{392,-946},{569,-851},{724,-724},{851,-569},{946,-392},{1004,-200},{1024,0}
};
/* Readout-only palette; keep the existing dial paints independent. */
static const uint8_t ps_text_colors[2][4]={{105,142,255,255},{0,237,149,255}};
static const uint8_t ps_colors[4][4]={
    {90,155,255,255},{0,225,155,255},{235,240,245,255},{255,66,63,255}
};
#endif
