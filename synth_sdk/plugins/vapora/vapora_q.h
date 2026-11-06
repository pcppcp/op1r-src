static unsigned u,z;
static uint32_t t;
static uint8_t b[3][OP1_TEXT_SIZE] __attribute__((aligned(4)));
static int a[3][4]={{23,15,1,1},{147,70,-1,1},{246,120,1,-1}},h;
static void q(int k,int v)
{
    if(z||k==4){z=u=0;return;}
    if(k==0&&v)u=1;
    else if(k==1&&v&&u==1)u=2;
    else if(k==3&&v&&u==2)u=3;
    else if(k==2&&u==3){u=0;z=1;t=cycles();}
    else u=0;
}
static int j(uint32_t n,uint32_t d)
{
    if(d>100000000u)z=u=0;
    if(z==1&&n-t>=2800000000u)z=2;
    return z==2;
}
static void v(void *r,int d)
{
    const char *s[3]={"ian","jisu","<3"};
    for(int i=0;i<3;i++){
        if(!h){op1_text_ctor(b[i],0,0,PS_TEXT_SIZE,ps_colors[i==2?3:i]);op1_text_set(b[i],s[i]);}
        if(d)for(int k=0;k<2;k++){
            a[i][k]+=a[i][k+2];
            if(a[i][k]<=4||a[i][k]>=(k?136:276))a[i][k+2]=-a[i][k+2];
        }
        int32_t m[9]={1024,0,0,0,1024,0,a[i][0]*1024,a[i][1]*1024,1024};
        op1_text_draw(b[i],r,m);
    }
    h=1;
}
