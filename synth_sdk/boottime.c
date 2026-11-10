/* Boot stage split (docs/79): the db stage (db2 open, Clips::load in plugload.c, FAT init) and the snd stage
 * (patch presets / slots / sample path) timed per call, shown on the resource monitor pages 9 and 10. */
#include <stdint.h>
#include "op1_synth.h"

volatile uint32_t bt_split[8];   /* ms: open, clips, fat, synth presets, drum presets, synth slots, drum slots, sample path */
#define MS(c) ((op1_cycles() - (c)) / 400000u)

#ifdef L1READ
static void l1_read_test(void);
#endif
#ifdef L1RUNTIME
static void l1_runtime_apply(void);
#endif
void bt_db_open(const char *root)                /* CALL @0x019F02E0 db2::open(root) */
{
#ifdef L1RUNTIME
    l1_runtime_apply();                          /* first: before anything could run the patched L1 */
#endif
#ifdef L1READ
    l1_read_test();                              /* after the patches: the checksum of the L1 that runs */
#endif
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

/* DTEST access to L1 instruction SRAM (docs/56, the Linux kernel's isram_read / isram_write; measured on the unit) */
#if defined(L1READ) || defined(L1RUNTIME)
static uint32_t dtest_cmd(uint32_t a)            /* instruction memory, data array (the Linux kernel's IADDR2DTEST) */
{
    return ((a & (1u << 11)) << (26 - 11)) | (1u << 24) | ((a & (1u << 15)) << (23 - 15)) | ((a & (3u << 12)) << (16 - 12))
         | (a & 0x47F8u) | (1u << 2);
}
static void dtest_read(uint32_t a, uint32_t *d0, uint32_t *d1)
{
    uint32_t m; __asm__ volatile ("cli %0;" : "=d"(m));
    *(volatile uint32_t *)0xFFE00300 = dtest_cmd(a);
    __asm__ volatile ("csync;");
    *d0 = *(volatile uint32_t *)0xFFE00400; *d1 = *(volatile uint32_t *)0xFFE00404;
    *(volatile uint32_t *)0xFFE00300 = 0;
    __asm__ volatile ("csync; sti %0;" : : "d"(m));
}
static void dtest_write(uint32_t a, uint32_t d0, uint32_t d1)
{
    uint32_t m; __asm__ volatile ("cli %0;" : "=d"(m));
    *(volatile uint32_t *)0xFFE00400 = d0; *(volatile uint32_t *)0xFFE00404 = d1;
    *(volatile uint32_t *)0xFFE00300 = dtest_cmd(a) | 2u;
    __asm__ volatile ("ssync;");
    *(volatile uint32_t *)0xFFE00300 = 0;
    __asm__ volatile ("ssync; sti %0;" : : "d"(m));
}
#endif

/* L1 patches at boot (OP1R_L1_RUNTIME=1, docs/56): the LDR's L1 stays as TE built it; here, in the main thread before
 * the audio interrupt starts (db stage), each poke of l1_patches.json (build/l1_runtime.c) is written by DTEST: the
 * aligned 8-byte blocks it covers read, its original bytes checked, the new ones merged, written, read back. A poke
 * whose original does not match is left out (counted). Page 9 shows "l1p <applied>/<n> <failed>" only if one failed. */
volatile uint32_t l1rt[4];                       /* applied, failed, -, 1 = ran */
#ifdef L1RUNTIME
typedef struct { uint32_t addr; uint8_t len; uint8_t data[16], orig[16]; } l1_poke_t;
extern const l1_poke_t l1_pokes[];
extern const unsigned l1_pokes_n;
static int l1_poke(const l1_poke_t *p)
{
    uint32_t a0 = p->addr & ~7u, a1 = (p->addr + p->len + 7) & ~7u;
    uint8_t blk[32];                             /* up to 16 bytes + alignment: 4 blocks of 8 */
    if (a1 - a0 > sizeof blk) return 0;
    for (uint32_t a = a0; a < a1; a += 8) {
        uint32_t d0, d1; dtest_read(a, &d0, &d1);
        for (int i = 0; i < 4; i++) { blk[a - a0 + i] = (uint8_t)(d0 >> (8 * i)); blk[a - a0 + 4 + i] = (uint8_t)(d1 >> (8 * i)); }
    }
    const uint32_t o = p->addr - a0;
    for (unsigned i = 0; i < p->len; i++) if (blk[o + i] != p->orig[i]) return 0;   /* not the expected original */
    for (unsigned i = 0; i < p->len; i++) blk[o + i] = p->data[i];
    for (uint32_t a = a0; a < a1; a += 8) {
        const uint8_t *b = blk + (a - a0);
        uint32_t d0 = b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24, d1 = b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24, r0, r1;
        dtest_write(a, d0, d1);
        dtest_read(a, &r0, &r1);
        if (r0 != d0 || r1 != d1) return 0;
    }
    return 1;
}
static void l1_runtime_apply(void)
{
    uint32_t ok = 0, bad = 0;
    for (unsigned i = 0; i < l1_pokes_n; i++) { if (l1_poke(&l1_pokes[i])) ok++; else bad++; }
    l1rt[0] = ok; l1rt[1] = bad; l1rt[3] = 1;
}
#endif
volatile uint32_t l1rd[4];                       /* diagnostic OP1R_L1READ: ran, first word, FNV-1a over the image (0: off) */
#ifdef L1READ
static void l1_read_test(void)
{
    uint32_t h = 0x811C9DC5u;
    for (uint32_t a = 0xFFA00000u; a < 0xFFA00000u + ((0xBFB4u + 7) & ~7u); a += 8) {
        uint32_t d0, d1; dtest_read(a, &d0, &d1);
        if (a == 0xFFA00000u) l1rd[1] = d0;
        h = (h ^ d0) * 0x01000193u;
        if (a + 4 < 0xFFA00000u + 0xBFB4u) h = (h ^ d1) * 0x01000193u;
    }
    l1rd[2] = h; l1rd[0] = 1;
}
#endif
