/* Stage check (docs/81): the audio core takeover proves each model against R246's original, every block, on the
 * running OP-1 (or op1emu). stage_check() saves the regions the stage writes, runs the original, keeps its result,
 * restores the saved state, runs the model, compares every byte, then puts the original's result back: what plays is
 * always the original. Counts: blocks, differing blocks, and the first difference (region, byte offset, both words). */
#ifndef DSP_STAGECHECK_H
#define DSP_STAGECHECK_H
#include <stdint.h>

typedef struct { void *addr; uint32_t len; } stage_region;
typedef struct { uint32_t blocks, bad, first_region, first_off, first_orig, first_model, cycles_orig, cycles_model; } stage_check_t;
#ifdef STAGE_DUMP                                    /* the first differing block: state before, original's and model's result */
uint8_t stage_dump[3][2048];
#endif

#define STAGE_CHECK_MAX 2048                       /* bytes all regions of one stage may have */
static inline void stage_copy(uint8_t *d, const volatile uint8_t *s, uint32_t n) { for (uint32_t i = 0; i < n; i++) d[i] = s[i]; }
static inline void stage_put(volatile uint8_t *d, const uint8_t *s, uint32_t n) { for (uint32_t i = 0; i < n; i++) d[i] = s[i]; }
static inline uint32_t stage_cyc(void) { uint32_t c; __asm__ volatile ("%0 = CYCLES;" : "=d"(c)); return c; }
static void stage_check(volatile stage_check_t *st, const stage_region *r, unsigned n, void (*orig)(void), void (*model)(void))
{
    static uint8_t before[STAGE_CHECK_MAX], after[STAGE_CHECK_MAX];
    uint32_t off = 0;
    for (unsigned i = 0; i < n; i++) { stage_copy(before + off, (const volatile uint8_t *)r[i].addr, r[i].len); off += r[i].len; }
    uint32_t t0 = stage_cyc();
    void (*volatile call_orig)(void) = orig; call_orig();   /* through a volatile: GCC must not reason about the target */
    uint32_t t1 = stage_cyc();
    off = 0;
    for (unsigned i = 0; i < n; i++) { stage_copy(after + off, (const volatile uint8_t *)r[i].addr, r[i].len); off += r[i].len; }
    off = 0;
    for (unsigned i = 0; i < n; i++) { stage_put((volatile uint8_t *)r[i].addr, before + off, r[i].len); off += r[i].len; }
    uint32_t t2 = stage_cyc();
    model();
    uint32_t t3 = stage_cyc();
    int bad = 0;
    off = 0;
    for (unsigned i = 0; i < n && !bad; i++) {
        const volatile uint8_t *m = (const volatile uint8_t *)r[i].addr;
        for (uint32_t k = 0; k < r[i].len; k++)
            if (m[k] != after[off + k]) {
                bad = 1;
                if (!st->bad) {                          /* the first one: region, offset, the aligned words */
#ifdef STAGE_DUMP
                    for (uint32_t j = 0; j < 2048; j++) { stage_dump[0][j] = before[j]; stage_dump[1][j] = after[j]; }
                    { uint32_t o2 = 0; for (unsigned q = 0; q < n; q++) { stage_copy(stage_dump[2] + o2, (const volatile uint8_t *)r[q].addr, r[q].len); o2 += r[q].len; } }
#endif
                    uint32_t w = k & ~3u;
                    st->first_region = i; st->first_off = k;
                    st->first_orig = after[off + w] | after[off + w + 1] << 8 | after[off + w + 2] << 16 | (uint32_t)after[off + w + 3] << 24;
                    st->first_model = m[w] | m[w + 1] << 8 | m[w + 2] << 16 | (uint32_t)m[w + 3] << 24;
                }
                break;
            }
        off += r[i].len;
    }
    off = 0;
    for (unsigned i = 0; i < n; i++) { stage_put((volatile uint8_t *)r[i].addr, after + off, r[i].len); off += r[i].len; }
    st->blocks++; st->bad += (uint32_t)bad;
    st->cycles_orig = t1 - t0; st->cycles_model = t3 - t2;
}
#endif
