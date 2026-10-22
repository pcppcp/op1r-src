/* Host (PC) replacements for the firmware calls/addresses used by engines and screens.
 * Included by op1_synth.h when compiled with -DOP1_HOST (see harness/screen_harness.c).
 * Screen calls are recorded and dumped as JSON by the harness; render.py turns that into a PNG. */
#ifndef OP1_HOST_H
#define OP1_HOST_H
#include <stdint.h>

extern int16_t  host_sine[8192];
extern int32_t  host_pitch_inc[128];
extern uint8_t  host_voice_mem[6 * 0x24];
extern int16_t  host_voice_vel[6];
extern int16_t  host_voice_env[128];             /* stock envelope of the voice (audio_harness.c fills it) */
extern int16_t  host_accel[3];                  /* accelerometer X, Y, Z (aether) */
extern int16_t  host_knobs[8][8];
extern int      host_slot;
extern const uint8_t *host_sample_buffer[8];
extern int16_t  host_fx_knobs[3][8];
extern int16_t  host_fx_meter[3];
extern void    *host_fx_scratch[3];
#define OP1_FX_MASTER 2

#define OP1_PITCH_INC    ((const int32_t *)host_pitch_inc)
#define OP1_SINE_TABLE   ((const int16_t *)host_sine)
#define OP1_VOICE_BASE   ((uintptr_t)host_voice_mem)
#define OP1_VOICE_VEL    ((const int16_t *)host_voice_vel)
#define OP1_VOICE_ENV    ((const int16_t *)host_voice_env)

void *host_find_node(void *scr, const char *id);
void  host_node_set_transform(void *node, const int32_t *m9);
void  host_node_set_visible(void *node, int on);
uint8_t *host_node_style_rgba(void *scr, const char *id);   /* line or fill: one colour per id */
void  host_screen_draw(void *scr, int dt);
void *host_raster(void);
void  host_raster_line(void *r, const void *p0, const void *p1, int32_t w_q10, const uint8_t *rgba);

#define op1_find_node(scr, id)            host_find_node((scr), (id))
#define op1_node_set_transform(node, m9)  host_node_set_transform((node), (m9))
#define op1_node_set_visible(node, on)    host_node_set_visible((node), (on))
#define op1_node_line_rgba(scr, id)       host_node_style_rgba((scr), (id))
#define op1_node_fill_rgba(scr, id)       host_node_style_rgba((scr), (id))
#define op1_screen_draw(scr, dt)          host_screen_draw((scr), (dt))
#define op1_raster()                      host_raster()
#define op1_raster_line(r, p0, p1, w, c)  host_raster_line((r), (p0), (p1), (w), (c))
void  host_raster_dim(void *r);
#define op1_raster_dim(r)                 host_raster_dim(r)
#define OP1_TEXT_SIZE 0xA4
void *host_text_ctor(void *obj, int32_t x_q10, int32_t y, int32_t size, const uint8_t *rgba);
void  host_text_set(void *t, const char *s);
void  host_text_draw(void *t, void *r, const int32_t *m9);
#define op1_text_ctor(obj, x, y, sz, c)   host_text_ctor((obj), (x), (y), (sz), (c))
#define op1_text_set(t, s)                host_text_set((t), (s))
#define op1_text_draw(t, r, m9)           host_text_draw((t), (r), (m9))
#define OP1_SCREEN_SLOT                   host_slot

static inline int op1_current_slot(void) { return host_slot; }
static inline const uint8_t *op1_sample_buffer(int slot) { return host_sample_buffer[slot & 7]; }
static inline const int16_t *op1_sample_data(const uint8_t *sb) { return *(int16_t *const *)sb + 8; }
static inline const int16_t *op1_sample_peaks(const uint8_t *sb) { return (const int16_t *)(sb + sizeof(void *)); }
static inline const int16_t *op1_synth_knobs(int slot) { return host_knobs[slot & 7]; }
static inline void op1_synth_knobs_live(int slot, int16_t out[4])
{ for (int i = 0; i < 4; i++) out[i] = host_knobs[slot & 7][i]; }
static inline void *op1_fx_scratch(int pos) { return host_fx_scratch[pos % 3]; }
static inline volatile int16_t *op1_fx_meter(int pos) { return &host_fx_meter[pos % 3]; }
static inline int op1_fx_screen_pos(void) { return 0; }
static inline const int16_t *op1_fx_knobs(int pos) { return host_fx_knobs[pos % 3]; }
static inline int32_t op1_fx_param_q15(const int16_t *p, int i) { return p[i] < 0 ? 0 : p[i]; }
__attribute__((weak)) int16_t host_fx_lfo[4];   /* added to the live FX knobs only, as an OP-1 LFO (tests) */
static inline void op1_fx_knobs_live(int16_t out[4])
{ for (int i = 0; i < 4; i++) { int32_t x = host_fx_knobs[0][i] + host_fx_lfo[i]; out[i] = (int16_t)(x < 0 ? 0 : (x > 32767 ? 32767 : x)); } }
#endif
