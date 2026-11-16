/* Fast tape erase (Shift + tape, all four track keys). The stock erase (clipboard command 4 ->
 * tape_c::write_chunk 0x01A54B08) writes zeros over the whole tape file: 485 chunks x 4 tracks x 64 KB = 124 MB of
 * NAND writes, whatever was recorded. Here only the chunks that overlap a clip of that track are zeroed: the rest of
 * the file is already zero (recording adds a clip for the recorded range, lift zeroes what it removes). Same
 * preparation and cleanup as the stock function; the erase flow around it (delete all clips, reset) is unchanged.
 * Replaces the CALL 0x01A54B08 at 0x0191B9F0 (audio_clipboard::service, case 4). docs/29. */
#include <stdint.h>

#define TE_CHUNKS      485                      /* 0x1E5 chunks of 32768 samples (64 KB) per track */
#define TE_CHUNK_SHIFT 15
#define TE_MAX_CLIPS   512

typedef struct { int32_t ch, start, stop; } te_clip_t;       /* db2::Clips record (samples) */

/* chunk mask per track from the clip table: bit i of mask[t] set = chunk i of track t holds a clip */
void te_chunk_mask(const te_clip_t *clips, const uint8_t *has, uint32_t mask[4][(TE_CHUNKS + 31) / 32])
{
    for (int t = 0; t < 4; t++)
        for (int w = 0; w < (TE_CHUNKS + 31) / 32; w++) mask[t][w] = 0;
    for (int c = 0; c < TE_MAX_CLIPS; c++) {
        if (!has[c]) continue;
        const te_clip_t *k = &clips[c];
        if (k->ch < 0 || k->ch > 3 || k->stop <= k->start || k->start < 0) continue;
        int a = k->start >> TE_CHUNK_SHIFT, b = (k->stop - 1) >> TE_CHUNK_SHIFT;
        if (b >= TE_CHUNKS) b = TE_CHUNKS - 1;
        for (int i = a; i <= b; i++) mask[k->ch][i >> 5] |= 1u << (i & 31);
    }
}

#ifndef OP1_HOST
#define CLIPS          ((const te_clip_t *)0x01AA8D70)   /* Clips singleton (0x0195BBB4): records */
#define CLIPS_HAS      ((const uint8_t *)0x01AAA570)     /* .. +0x1800: has_clip[512] */
#define TAPE_FD        (*(volatile int32_t *)0x01B4EF44) /* tape file handle */
#define TAPE_BASE      ((const int32_t *)0x018C1810)     /* file offset of each track */
#define TAPE_ZERO      ((const void *)0x01B4EF8C)        /* 64 KB zero buffer (also lift's zero source) */
#define TAPE_PROGRESS  (*(volatile int32_t *)0x018C17C8) /* bytes written (erase progress) */
#define tape_prepare   ((void (*)(void))0x01A54078)      /* invalidate clean cache entries + xfade */
#define tape_xfade_inv ((void (*)(int))0x01A54020)
#define file_pwrite    ((int (*)(int, const void *, int, int))0x01A99F9C)   /* (fd, buf, len, offset) */

void fast_tape_erase(void)
{
    static uint32_t mask[4][(TE_CHUNKS + 31) / 32];
    tape_prepare();
    te_chunk_mask(CLIPS, CLIPS_HAS, mask);
    TAPE_PROGRESS = 0;
    for (int i = 0; i < TE_CHUNKS; i++)
        for (int t = 0; t < 4; t++)
            if (mask[t][i >> 5] & (1u << (i & 31))) {
                TAPE_PROGRESS += 0x10000;
                if (file_pwrite(TAPE_FD, TAPE_ZERO, 0x10000, TAPE_BASE[t] + (i << 16)) != 0x10000)
                    goto done;                  /* stock asserts here (locks the OP-1); stop instead */
            }
done:
    TAPE_PROGRESS = 0;
    tape_xfade_inv(0xF);
    { extern void sl_erase_all(void); sl_erase_all(); }         /* ELSE-MARIE's slots: empty too (screens/tfscreen.c) */
}
#endif
