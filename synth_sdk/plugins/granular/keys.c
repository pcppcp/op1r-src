#pragma GCC optimize ("no-tree-loop-distribute-patterns") /* no implicit libc calls */
/* Granular's key callback and service (op1_engine_ext_t, op1r 60): freeze latch, sync toggle, cleanup on slot /
 * preset / mode changes. Was the granular_ctl tool plugin of the Granular 0.10.0 port; Shift pages are the core's now
 * (shift_page). GPL-3.0-or-later with the OP1R firmware permission. */
#include "op1_plugin.h"
#include "bridge.h"
extern int sb_is_open(int), se_is_open(void);
#ifdef OP1_HOST
/* host: the port's test (tests/test_port.c, GR_TEST_HOOKS) defines these; defaults for the generic harnesses */
#ifdef GR_TEST_HOOKS
extern int gr_host_mode,gr_host_screen;
extern int gr_host_sync[8];
extern uintptr_t gr_host_sound[8];
extern int gr_host_post(int,int);
#else
int gr_host_mode,gr_host_screen;
int gr_host_sync[8];
uintptr_t gr_host_sound[8];
int gr_host_post(int c,int d){(void)c;(void)d;return 1;}
int sb_is_open(int n){(void)n;return 0;}
int se_is_open(void){return 0;}
#endif
static uintptr_t sound_id(int s){return gr_host_sound[s&7];}
#define MODE gr_host_mode
#define SCREEN gr_host_screen
static int control_slot(void){return host_slot&7;}
static int post(int c,int d){return gr_host_post(c,d);}
static int sync_read(int s){return gr_host_sync[s&7];}
static void sync_write(int s,int v){gr_host_sync[s&7]=v;}
#else
#define MODE (*(volatile int *)0x01DE4C40)
#define SCREEN (*(volatile int *)0x018B65C8)
static int control_slot(void){return *(volatile int *)0xFF8016D4 & 7;}
#define PATCH(s) ((uint8_t *)0x01DDD7E8 + ((s)&7)*0x38)
#define JV_CGET ((const void *(*)(const void *,const char *))0x019BEB70)
#define JV_KEY ((void *(*)(void *,const char *))0x019BFEB4)
#define JV_ASSIGN ((void *(*)(void *,const void *))0x019BDDC0)
#define JV_INT_C ((void *(*)(void *,int))0x019BD890)
#define JV_DTOR ((void (*)(void *,int))0x019BDD18)
#define PATCH_SAVE ((void (*)(void *))0x01A00D90)
/* Queue releases in the same order as physical keys; retry a full queue. */
static int post(int code,int down) {
    volatile uint32_t *rd=(volatile uint32_t *)0x01DD5244,*wr=(volatile uint32_t *)0x01DD5248;
    uint32_t mask; __asm__ volatile("cli %0;":"=d"(mask));
    uint32_t w=*wr,n=(w+1)&255; int ok=n!=*rd && !*(volatile uint8_t *)0x01DD524D;
    if(ok){((volatile uint16_t *)0x01DD5044)[w]=(uint16_t)(code|(down<<8));*wr=n;}
    __asm__ volatile("sti %0;"::"d"(mask));
    if(ok)((void (*)(void))0x019C2F04)();
    return ok;
}
static uintptr_t sound_id(int slot){return *(const uintptr_t *)(PATCH(slot)+4);}
static int sync_read(int slot) {
    const void *v=JV_CGET(PATCH(slot)+4,"granular_sync");
    return v && ((const uint8_t *)v)[8]==1 && *(const int32_t *)v==1;
}
static void sync_write(int slot,int on) {
    /* Called only on the key thread, with the firmware saver out of the JSON. */
    uint32_t tmp[4]; op1_patch_wait();
    JV_INT_C(tmp,on);JV_ASSIGN(JV_KEY(PATCH(slot)+4,"granular_sync"),tmp);JV_DTOR(tmp,2);
    PATCH_SAVE(PATCH(slot));
}
#endif
static int engine_number(void){return op1_synth_knobs(control_slot())[4]-OP1_ENGINE_MAGIC;}
static struct gr_bridge *current(void){return MODE==0?&ENGINE_granular:0;}   /* called while it is the slot's engine */
static const uint8_t note_keys[24]={0x0D,0x0E,0x0F,0x16,0x17,0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F,0x20,0x27,0x28,0x29,0x2A,0x30,0x31,0x32,0x33,0x34,0x35};
static volatile uint8_t finger[24],os_down[24],sent[24];
static struct gr_bridge *owner;
static int owner_slot=-1;
static uintptr_t owner_sound;
static uint8_t swallow[4];
static void secondary_defaults(void){
    int s=control_slot()&7;
    int16_t *w=(int16_t *)(uintptr_t)op1_synth_knobs(s);
    if(w[6])return;
#ifndef OP1_HOST
    op1_patch_wait();
#endif
    w[6]=2730; /* middle of Hann's six-way step, still the same sound */
#ifndef OP1_HOST
    ((void (*)(void *,const int16_t *))0x01A00D14)(PATCH(s),w);
#endif
}
static int note_index(int c){for(int i=0;i<24;i++)if(note_keys[i]==c)return i;return -1;}
static void release_held(void){
    for(int i=0;i<24;i++)if(os_down[i]&&!finger[i]&&!sent[i]){
        sent[i]++;if(!post(note_keys[i],0))sent[i]--;
    }
}
static void thaw(void){if(owner)owner->frozen=0;release_held();}
static void context(void){
    struct gr_bridge *b=current();int slot=control_slot()&7;
    uintptr_t sound=sound_id(slot);
    if(b!=owner||slot!=owner_slot||sound!=owner_sound){thaw();owner=b;owner_slot=slot;owner_sound=sound;if(owner)owner->frozen=0;}
}
static int control_key(int code,int pressed){
    context();
    if(code==0x26){
        if(pressed && owner && SCREEN==0x14 && !sb_is_open(engine_number()) && !se_is_open())secondary_defaults();
        return 0;                                    /* the core shows the Shift pages (shift_page) */
    }
    int i=note_index(code);
    if(i>=0){
        if(!owner && !os_down[i] && !sent[i]){finger[i]=(uint8_t)pressed;return 0;}
        if(pressed){
            finger[i]=1;
            if(os_down[i])return 1;
            os_down[i]=1;return 0;
        }
        if(sent[i]){sent[i]--;if(finger[i])return 1;}else finger[i]=0;
        if(owner&&owner->frozen&&os_down[i])return 1;
        os_down[i]=0;return 0;
    }
    if(code>=0x3B&&code<=0x3E&&!pressed&&swallow[code-0x3B]){swallow[code-0x3B]=0;return 1;}
    /* Release before switching instruments/slots/octaves or starting recording. */
    if(pressed && (code==0x02||code==0x03||code==0x24||code==0x25||code==0x2B||
                  (code>=0x10&&code<=0x14)||(code>=0x2D&&code<=0x2F)))thaw();
    int n=engine_number();
    if(!owner||SCREEN!=0x14||se_is_open()||sb_is_open(n))return 0;
    if(code==0x3E){
        if(pressed){
            owner->frozen^=1;if(!owner->frozen)release_held();
            swallow[3]=1;
        }
        return 1;
    }
    if(code==0x3D&&op1_shift_held()){
        if(pressed){int s=control_slot()&7;sync_write(s,!sync_read(s));owner->sync[s]=(uint8_t)sync_read(s);swallow[2]=1;}
        return 1;
    }
    if(code==0x3C && op1_shift_held()){
        if(pressed){secondary_defaults();swallow[1]=1;}
        return 1;
    }
    if(code==0x3C && pressed)secondary_defaults();
    /* Plain white press reaches OP1R's sb_key unchanged: open, select, load. */
    if(code==0x3D&&pressed)thaw();
    return 0;
}
static void control_service(void){
    context();if(owner){
        owner->sync[owner_slot]=(uint8_t)sync_read(owner_slot);
        /* Only this thread calls the expensive assembly self-test / peak scan. */
        owner->service();
    }
    if(!owner||!owner->frozen)release_held();
}
int gr_key(int code,int pressed,int page){(void)page;return control_key(code,pressed);}
void gr_service(void){control_service();}
