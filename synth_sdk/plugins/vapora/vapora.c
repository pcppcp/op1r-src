/* Copyright (C) 2026 AiAAAA. GPL-3.0-or-later with the OP-1 firmware permission. */
/* Native adapter. All pitch/envelope/slot access uses matched SDK declarations. */
#ifdef VAPORA_HOST
#include "tests/native_mock.h"
#else
#include "op1_plugin.h"
#endif
#include "vapora_mailbox.h"
static ps_voice voices[PS_VOICES];
static uint32_t serial, ticks[PS_VOICES];
ps_mailbox ps_mail[PS_VOICES];
volatile uint32_t ps_held_serial[PS_VOICES];

static int valid_voice(void *self)
{
    uintptr_t d=(uintptr_t)self-(uintptr_t)OP1_VOICE_BASE;
    if(d>=OP1_VOICE_SIZE*PS_VOICES || d%OP1_VOICE_SIZE)return -1;
    int i=op1_voice_index(self);
    return (unsigned)i<PS_VOICES?i:-1;
}
static void note_on(void *self,const int16_t *words)
{
    int i=valid_voice(self); if(i<0)return;
    ps_params p=ps_decode(words);
    ++serial; if(!serial)++serial;
    ps_start(&voices[i],&p,0x50414453u^(serial*0x9e3779b9u)^(unsigned)i,serial);
    /* Only the private part, never the firmware vtable. */
    uint32_t *private=(uint32_t *)((uint8_t *)self+4);
    private[0]=0x50414453u; private[1]=serial;
    ps_held_serial[i]=serial;
    ticks[i]=(uint32_t)i; /* spread chord telemetry instead of six publications together */
}
static void note_off(void *self)
{
    int i=valid_voice(self);if(i<0)return;
    ps_held_serial[i]=0; /* visual gate only; firmware still owns the T2 release */
}
static void render(void *self,int16_t *out,const int16_t *words)
{
    int i=valid_voice(self);
    if(i<0){for(int n=0;n<PS_BLOCK;n++)out[n]=0;return;}
    const uint32_t *private=(const uint32_t *)((uint8_t *)self+4);
    if(private[0]!=0x50414453u||private[1]!=voices[i].serial||!voices[i].ready){
        for(int n=0;n<PS_BLOCK;n++)out[n]=0;
        return;
    }
    ps_params p=ps_decode(words);
    ps_render(&voices[i],out,OP1_PITCH_INC,op1_voice_vel_gain(self),&p);
    if((ticks[i]++&7u)==0){
        ps_snapshot s;
        ps_analyze(&voices[i],out,OP1_VOICE_ENV,(uint32_t)OP1_PITCH_INC[64],&s);
        ps_mailbox *m=&ps_mail[i];
        ++m->sequence; PS_BARRIER();
        m->slot=op1_current_slot()&7; m->selector=words[4];
        m->snapshot.serial=s.serial; m->snapshot.pitch=s.pitch; m->snapshot.level=s.level;
        for(int k=0;k<PS_TRACE;k++)m->snapshot.trace[k]=s.trace[k];
        for(int k=0;k<4;k++)m->snapshot.primary[k]=s.primary[k];
        PS_BARRIER(); ++m->sequence;
    }
}
const op1_engine_t ENGINE_vapora={note_on,render,note_off,
    {OP1_PRESS_PAGE,OP1_PRESS_PAGE,OP1_PRESS_PAGE,OP1_PRESS_PAGE}};
