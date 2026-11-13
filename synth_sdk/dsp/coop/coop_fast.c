/* coop-fw's rewrites of stock DSP loops (dsp/coop/*_fast.S, coldsoldr, GPL-3.0-or-later with the OP-1 firmware
 * permission; docs/84): their switches (1 = the rewrite, 0 = the stock loop; OP1R_COOPFAST=0 builds all off, for A/B)
 * and the run counters they keep. Patch sites: make_firmware.py HOOKS / link.ld (the stock bytes checked). */
#include <stdint.h>
#ifndef COOP_FAST
#define COOP_FAST 1
#endif
volatile uint32_t g_coop_fast_dsynth = COOP_FAST, g_coop_fast_cwo = COOP_FAST, g_coop_fast_punch = COOP_FAST,
                  g_coop_fast_phone = COOP_FAST, g_coop_fast_nitro = COOP_FAST, g_coop_fast_grid = COOP_FAST,
                  g_coop_fast_spring = COOP_FAST, g_coop_fast_delay = COOP_FAST, g_coop_fast_phase = COOP_FAST,
                  g_coop_fast_voltage = COOP_FAST;
volatile uint32_t g_nitro_hit_calls, g_phase_hit_calls, g_phase_hit_wrap1, g_phase_hit_wrap2, g_phone_hit_capa,
                  g_phone_hit_capb, g_phone_hit_end, g_phone_hit_rise, g_phone_hit_slow, g_phone_hit_swap,
                  g_spring_hit_calls, g_spring_hit_split, g_spring_hit_stock, g_spring_hit_wrap, g_voltage_hit_calls;
volatile uint32_t coop_hits[10];   /* OP1R_COOP_HITS test builds: runs of dsynth cwo punch phone nitro grid spring delay phase voltage */
