/* sampler+ core part (docs/46): what stays in the firmware for the sampler+ plugin file (plugins/samplerplus, kind
 * "sampler"). SynthSampler vtable slots render / note_on / note_off and the Adsr_render entry (hook.S) go to the
 * plugin if one is loaded, else to the stock functions (a plain sampler, unchanged); synth patches' "op1r" objects
 * reach the plugin through dp_keep_op1r (drumplus/dp_core.c, kept also without the plugin); the synth type browser
 * job is the plugin's (its 'sampler+' row) or the stock one. */
#include "../op1_synth.h"

const op1_sampler_t *op1_sampler;                             /* set by the plugin loader (plugload.c) */

#define STOCK_RENDER   ((void (*)(void *, int16_t *, const int16_t *))0x01A51A7C)
#define STOCK_NOTE_ON  ((void (*)(void *, const int16_t *))0x01A518F4)
#define STOCK_NOTE_OFF ((void (*)(void *))0x01A51964)
#define STOCK_TYPE_JOB ((void (*)(void *))0x0193FFAC)
int sp_adsr_stock(void *adsr, int16_t *env, const int16_t *words, const int16_t *vel);   /* hook.S */

void sp_fw_render(void *self, int16_t *out, const int16_t *k)
{
    const op1_sampler_t *s = op1_sampler;
    uint32_t t0 = op1_cycles();
    if (s) s->render(self, out, k); else STOCK_RENDER(self, out, k);
    sm_cy[SMC_SAMPLER] += op1_cycles() - t0;
}
void sp_fw_note_on(void *self, const int16_t *k) { if (op1_sampler) op1_sampler->note_on(self, k); else STOCK_NOTE_ON(self, k); }
void sp_fw_note_off(void *self) { if (op1_sampler) op1_sampler->note_off(self); else STOCK_NOTE_OFF(self); }
void inst_fade_pre(void);
/* the Adsr_render hook only goes through sp_adsr while needed (hook.S fast path): a sampler slot is current with the
 * sampler+ plugin loaded, or a tool's instrument fade runs (screens/overlay.c) */
volatile uint8_t sp_hook_live;
extern int inst_fade_active(void);
void sp_hook_update(void)                                       /* main loop */
{
    int s = op1_current_slot();
    sp_hook_live = (uint8_t)((op1_sampler && (unsigned)s < 8 && ((const volatile int32_t *)0xFF8016E4)[s] == 6) || inst_fade_active());
}
int sp_adsr(void *adsr, int16_t *env, const int16_t *words, const int16_t *vel)
{
    const op1_sampler_t *s = op1_sampler;
    uint32_t t0 = op1_cycles();
    inst_fade_pre();                                              /* tools: instrument fade (screens/overlay.c) */
    int r = s ? s->adsr(adsr, env, words, vel) : sp_adsr_stock(adsr, env, words, vel);
    sm_cy[SMC_ENV] += op1_cycles() - t0;                          /* every synth voice's envelope */
    return r;
}
void sp_slot_loaded(int slot, void *op1r) { if (op1_sampler) op1_sampler->slot_loaded(slot, op1r); }
/* the boot loads the synth patches before the plugins: once the plugin is in, it gets every slot's "op1r" */
#define SYNTH_PATCHES 0x01DDD7E8
#define JV_CGET ((const void *(*)(const void *, const char *))0x019BEB70)
#define JV_KEY  ((void *(*)(void *, const char *))0x019BFEB4)
void sp_replay(void)
{
    for (int slot = 0; slot < 8; slot++) {
        void *json = (uint8_t *)(SYNTH_PATCHES + slot * 0x38) + 4;
        const void *c = JV_CGET(json, "op1r");                   /* const lookup: no member added */
        sp_slot_loaded(slot, c && ((const uint8_t *)c)[8] == 7 ? JV_KEY(json, "op1r") : 0);
    }
}
void spc_ui_build(void) { if (op1_sampler) op1_sampler->ui_build(); }
void spc_ui_load(void) { if (op1_sampler) op1_sampler->ui_load(); }
void *spc_screen_for(int id) { return op1_sampler ? op1_sampler->screen_for(id) : 0; }
void spc_sync(int editor_open_on_6) { if (op1_sampler) op1_sampler->sync(editor_open_on_6); }
int spc_knob(int knob, int delta) { return op1_sampler ? op1_sampler->knob(knob, delta) : 0; }
int spc_key(int code, int pressed) { return op1_sampler ? op1_sampler->key(code, pressed) : 0; }
int sb_job(void *job, void (*type_job)(void *));                 /* screens/sbrowse.c: engine sample browser load */
void pj_begin(void), pj_end(void);                                /* patchjob.c: a patch job runs (docs/66) */
void sp_type_job(void *job)
{
    void (*j)(void *) = op1_sampler ? op1_sampler->type_job : STOCK_TYPE_JOB;
    pj_begin();
    if (!sb_job(job, j)) j(job);
    pj_end();
}
