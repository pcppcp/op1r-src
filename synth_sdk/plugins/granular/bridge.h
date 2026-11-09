#ifndef GR_BRIDGE_H
#define GR_BRIDGE_H
#include "op1_plugin.h"
/* the engine (op1_engine_ext_t, op1r 60: key callback keys.c, service, Shift pages green / red) and the state
 * keys.c and the engine share (was the granular_ctl tool plugin's bridge) */
struct gr_bridge {
    op1_engine_ext_t x;
    volatile uint8_t frozen,sync[8],asm_ok;
    void (*service)(void);
};
extern struct gr_bridge ENGINE_granular;
#endif
