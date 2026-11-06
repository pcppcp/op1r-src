/* Plugin-local host adapter. Not firmware or a native renderer. */
#ifndef VAPORA_NATIVE_MOCK_H
#define VAPORA_NATIVE_MOCK_H
#include <stdint.h>
#include <stddef.h>
#define OP1_HOST 1
enum {OP1_BLOCK=128,OP1_VOICES=6,OP1_SAMPLE_RATE=44100,
      OP1_PRESS_NONE=0,OP1_PRESS_PAGE=1};
#define OP1_VOICE_SIZE 36u
extern uint32_t ps_host_voices[6][9],ps_host_cycles;
extern int32_t ps_host_pitch[128];
extern int16_t ps_host_env[128],ps_host_words[8][8],ps_host_velocity[6];
extern int ps_host_slot,ps_host_shift;
extern volatile uint8_t op1_eng_page[4];
#define OP1_VOICE_BASE ((uintptr_t)ps_host_voices)
#define OP1_PITCH_INC ps_host_pitch
#define OP1_VOICE_ENV ps_host_env
#define OP1_SCREEN_SLOT ps_host_slot
static inline int op1_voice_index(const void *v)
{return (int)(((uintptr_t)v-OP1_VOICE_BASE)/36);}
static inline int32_t op1_voice_vel_gain(const void *v)
{return ps_host_velocity[op1_voice_index(v)];}
static inline int op1_current_slot(void){return ps_host_slot;}
static inline const int16_t *op1_synth_knobs(int s){return ps_host_words[s&7];}
static inline void op1_synth_knobs_live(int s,int16_t *out)
{for(int i=0;i<4;i++)out[i]=ps_host_words[s&7][i];}
typedef struct {int32_t x,y;} op1_pt;
typedef struct {
    void(*note_on)(void*,const int16_t*);
    void(*render)(void*,int16_t*,const int16_t*);
    void(*note_off)(void*);
    uint8_t press[4];
} op1_engine_t;
typedef struct {void(*init)(void*);void(*update)(void*,int);} op1_screen_t;
void *op1_find_node(void*,const char*);
uint8_t *op1_node_fill_rgba(void*,const char*);
void op1_node_set_visible(void*,int);
void op1_node_set_transform(void*,const int32_t*);
void op1_screen_draw(void*,int);
void *op1_raster(void);
void op1_raster_line(void*,const op1_pt*,const op1_pt*,int32_t,const uint8_t*);
#define OP1_TEXT_SIZE 0xA4
void op1_text_ctor(void*,int32_t,int,int,const uint8_t*);
void op1_text_set(void*,const char*);
void op1_text_draw(void*,void*,const int32_t*);
extern const op1_engine_t ENGINE_vapora;
extern const op1_screen_t SCREEN_vapora;
#endif
