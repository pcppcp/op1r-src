/* Boot stage split (docs/79): the db stage (db2 open, Clips::load in plugload.c, FAT init) and the snd stage
 * (patch presets / slots / sample path) timed per call, shown on the resource monitor pages 9 and 10. */
#include <stdint.h>
#include "op1_synth.h"

volatile uint32_t bt_split[8];   /* ms: open, clips, fat, synth presets, drum presets, synth slots, drum slots, sample path */
#define MS(c) ((op1_cycles() - (c)) / 400000u)

void bt_db_open(const char *root)                /* CALL @0x019F02E0 db2::open(root) */
{
    uint32_t c = op1_cycles();
    ((void (*)(const char *))0x0196F912)(root);
    bt_split[0] = MS(c);
}
void bt_fat_init(const char *root)               /* CALL @0x019F02F4 fat_manager::init(root) */
{
    uint32_t c = op1_cycles();
    ((void (*)(const char *))0x019971B0)(root);
    bt_split[2] = MS(c);
}
void bt_sounds(void)                             /* CALL @0x019F0456 FUN_01A059D0: the five patch loads, timed */
{
    static void (*const f[5])(void) = { (void (*)(void))0x01A0567C, (void (*)(void))0x01A04FE8,
        (void (*)(void))0x01A05454, (void (*)(void))0x01A04DC0, (void (*)(void))0x01A017DC };
    for (int i = 0; i < 5; i++) { uint32_t c = op1_cycles(); f[i](); bt_split[3 + i] = MS(c); }
}

/* Preset table fill (load_synth_presets / load_drum_presets, docs/79): set_*_patch(entry, -1, row json) deep-copies
 * the row's Json::Value into the table entry (operator= = copy + swap), and the row vector is freed right after.
 * Here the 16-byte value header is swapped instead (jsoncpp values hold no pointers to themselves): the same
 * table, one deep copy and one free fewer per preset. Other slots: the stock setter. */
static void lp_set(uint32_t *entry, int slot, uint32_t *json, uint32_t stock, uint32_t load)
{
    if (slot != -1) { ((void (*)(uint32_t *, int, uint32_t *))stock)(entry, slot, json); return; }
    entry[0] = (uint32_t)slot;
    for (int i = 0; i < 4; i++) { uint32_t t = entry[1 + i]; entry[1 + i] = json[i]; json[i] = t; }
    *(uint8_t *)(entry + 0xc) = 0;
    ((void (*)(uint32_t *))load)(entry);
}
void lp_set_synth(uint32_t *e, int slot, uint32_t *json) { lp_set(e, slot, json, 0x01A052DC, 0x01A051E8); }
void lp_set_drum(uint32_t *e, int slot, uint32_t *json)  { lp_set(e, slot, json, 0x01A04C48, 0x01A04BB0); }
