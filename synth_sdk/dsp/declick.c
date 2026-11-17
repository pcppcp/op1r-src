/* FX switch declick (dsp/declick.h; after hundekraft 3.3 kennel/kn_declick.c, Hundekraft, GPL-3.0-or-later with the
 * OP-1 firmware permission). Every mono and master FX render passes through here:
 *   mono   the seven stock mono FX vtables' render slot (make_firmware.py SLOTS dc_mono_*) and the filter type's
 *          fx_render (dispatcher.c: the stock filter and every plugin FX). One state: R246 has one mono FX object.
 *   master fr_route (screens/fxroute.c), which every master render goes through, after the routing mixed it back.
 * The empty-pointer block of R246's swap: mono from the bus hook in_bus (input/in_host.c, every block, before the
 * FX); not at the master (kennel does it from an L1 trampoline in the output stage; here not done). */
#include <stdint.h>
#include "declick.h"
#define FRAME_COUNTER (*(volatile uint32_t *)0xFF8016E0)        /* one per audio block */
#define MONO_FX_PTR(pos) (((void *volatile *)0x01AAAD60)[pos])   /* synth 0, drum 1 */

uint32_t fx_ident(int master);                                   /* dispatcher.c */
static dc_state_t dc_m, dc_l, dc_r;
volatile uint32_t dc_switches[2];                                /* mono, master (monitor / tests) */

void dc_mono(uint32_t id, int16_t *buf, int16_t dry0)
{
    uint16_t n0 = dc_m.switches;
    dc_run(&dc_m, id, buf, 64, dry0, FRAME_COUNTER);
    if (dc_m.switches != n0) dc_switches[0]++;
}
void dc_master(const void *fx, int16_t *l, int16_t *r, int16_t l0, int16_t r0)
{
    uint32_t id = ((uint32_t)fx ^ fx_ident(1) << 1) | 1u, blk = FRAME_COUNTER;
    uint16_t n0 = dc_l.switches;
    dc_run(&dc_l, id, l, 64, l0, blk);
    dc_run(&dc_r, id, r, 64, r0, blk);
    if (dc_l.switches != n0) dc_switches[1]++;
}
void dc_mono_gap(int pos, int16_t *bus)                          /* in_bus: before the mono FX would render */
{
    if (!MONO_FX_PTR(pos & 1)) dc_gap(&dc_m, bus, 64, FRAME_COUNTER);
}

/* the seven stock mono FX renders other than the filter: render(this, int16_t bus[64], params[8]) */
typedef void (*dc_render_t)(void *, int16_t *, const int16_t *);
#define DC_WRAP(name, k, orig) \
    void name(void *self, int16_t *buf, const int16_t *p) { int16_t d0 = buf[0]; ((dc_render_t)(orig))(self, buf, p); dc_mono(k, buf, d0); }
DC_WRAP(dc_mono_delay,  1, 0x0197D8C4)
DC_WRAP(dc_mono_grid,   2, 0x019AF338)
DC_WRAP(dc_mono_punch,  3, 0x019E6260)
DC_WRAP(dc_mono_phone,  4, 0x01A1BB74)
DC_WRAP(dc_mono_spring, 5, 0x01A326E8)
DC_WRAP(dc_mono_nitro,  6, 0x019A62A4)
DC_WRAP(dc_mono_cwo,    7, 0x0194480C)
