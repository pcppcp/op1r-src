/* Host recording of the native Solid nodes; not an OP-1 raster emulator. */
typedef struct {int visible;int32_t m[9];} ps_host_node;
static ps_host_node ps_native_nodes[2];
static uint8_t ps_host_sun_fill[4]={255,255,255,255};
uint8_t *op1_node_fill_rgba(void *scr,const char *id)
{(void)scr;assert(!strcmp(id,"sunfill"));return ps_host_sun_fill;}
void *op1_find_node(void *scr,const char *id)
{
    (void)scr;
    if(!strcmp(id,"eclipse_curve"))return &ps_native_nodes[0];
    if(!strcmp(id,"cover_curve"))return &ps_native_nodes[1];
    assert(0);return 0;
}
void op1_node_set_visible(void *node,int visible)
{assert(node);((ps_host_node*)node)->visible=!!visible;}
void op1_node_set_transform(void *node,const int32_t *m)
{
    assert(node&&m[0]==1024&&m[4]==1024&&m[8]==1024);
    assert(m[6]>=0&&m[6]<=320*1024&&m[7]>=0&&m[7]<160*1024);
    memcpy(((ps_host_node*)node)->m,m,9*sizeof(*m));
}
static void ps_host_disk(double x,double y,const char *color)
{
    if(drawing)fprintf(drawing,"<g transform=\"translate(%.4f %.4f)\"><path fill=\"%s\" d=\"M 40 20 C 40 31.0457 31.0457 40 20 40 C 8.9543 40 0 31.0457 0 20 C 0 8.9543 8.9543 0 20 0 C 31.0457 0 40 8.9543 40 20 z\"/></g>\n",x,y,color);
}
void op1_screen_draw(void *scr,int dt)
{
    (void)scr;(void)dt;
    if(!ps_native_nodes[0].visible)return;
    double x=ps_native_nodes[0].m[6]/1024.,y=ps_native_nodes[0].m[7]/1024.;
    char color[8];snprintf(color,sizeof color,"#%02x%02x%02x",ps_host_sun_fill[0],ps_host_sun_fill[1],ps_host_sun_fill[2]);
    ps_host_disk(x,y,color);
    if(ps_native_nodes[1].visible)ps_host_disk(x+ps_native_nodes[1].m[6]/1024.,y,"#000000");
}
