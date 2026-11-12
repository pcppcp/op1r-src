/* perc: shared between the engine, the transfer reader and the screen. GPL-3.0-or-later with the OP1R permission. */
#ifndef PERC_H
#define PERC_H
#include <stdint.h>
#define PERC_XFER_SLOTS 16                       /* PO-32 instruments a transfer can bring in */
#define PERC_TYPES (10 + PERC_XFER_SLOTS)        /* Shift + blue detents: built-in, then transferred */
int perc_type_of(const int16_t *params);
const uint16_t *perc_patch_of(int type);         /* 0: empty transfer slot */
/* transfer (perc_xfer.c): patches decoded from the current slot's sample */
const uint16_t *perc_xfer_patch(int instrument);
void perc_xfer_service(void);
/* for the screen: bumped on every hit, with that hit's velocity (Q15) */
extern volatile uint32_t perc_hit_seq;
extern volatile int32_t perc_hit_vel;
#endif
