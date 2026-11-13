/* MIDI sequencer device (docs/39): 4 tracks of notes + CCs at tape positions, stored in a file, streamed through a
 * RAM cache. Pure C: firmware access (tape state, destinations, file I/O, interrupts) goes through ms_env, so the
 * host test (harness/mseq_test.c) runs the same code.
 *
 * Positions are tape samples. Time is cut into pages of 65536 samples (1.49 s); an event stores its offset inside the
 * page (u16). Each (track, page) holds a chain of 512-byte slots, every slot up to MS_SLOT_EV events sorted by offset,
 * the slots of a chain in ascending, disjoint offset ranges. A full slot splits in two (insert cost <= MS_SLOT_EV
 * moves, independent of the song length). Slots live on disk (file slot n) and, while needed, in one of MS_RAM RAM
 * buffers; the main loop prefetches the pages around the playhead and writes dirty slots back. The audio thread only
 * touches resident slots (a missing one plays nothing and counts an underrun). */
#ifndef MS_CORE_H
#define MS_CORE_H
#include <stdint.h>

#define MS_TRACKS    4
#define MS_PAGE_BITS 16
#define MS_PAGES     244                    /* 6 min of tape (0xF2BFA0 samples) */
#define MS_MAXPOS    0xF2BFA0
#define MS_SLOT_EV   62
#define MS_SLOTS     2048                   /* file slots (1 MB of events) */
#define MS_RAM       96                     /* RAM slot buffers (48 KB) */
#define MS_NONE      0xFFFF
#define MS_ACTIVE    64                     /* sounding notes */
#define MS_HELD      32                     /* notes being recorded (pressed, not released yet) */
#define MS_CLIP      512                    /* clipboard events */
#define MS_TAKE      256                    /* notes of one recording take (quantized when it ends) */

enum { MS_NOTE = 0, MS_CC = 1 };
enum { MS_DEST_INSTR = 0, MS_DEST_SEQ = 1, MS_DEST_MIDI = 2 };

typedef struct { uint16_t off; uint8_t kind, d1, d2, ch; uint16_t len; } ms_ev;   /* 8 bytes; len in 64-sample blocks */

typedef struct {                            /* 512 bytes: one file slot / RAM buffer */
    uint16_t magic, slot;                   /* 'MS', own file slot */
    uint8_t  track, n, ver, pad;
    uint16_t page, next;                    /* (track, page) it belongs to; next slot of the chain (MS_NONE: last) */
    uint32_t crc;                           /* over the events [0, n) and the fields above (crc = 0 while computed) */
    ms_ev    ev[MS_SLOT_EV];
} ms_slot;

typedef struct {                            /* firmware / host services */
    int32_t (*tape_pos)(void);              /* samples */
    int (*tape_playing)(void);
    int (*loop)(int32_t *in, int32_t *out); /* 1: loop on */
    void (*play)(int track, int dest, int ch, int kind, int d1, int d2, int on);   /* note on / off, CC (on = 1) */
    int (*read_slot)(int slot, ms_slot *out);          /* 0 ok, >0 not in the file (as damage), <0 read error (retried) */
    int (*write_slot)(int slot, const ms_slot *in);
    int (*read_head)(void *h, int len);                /* as read_slot (>0: no store yet) */
    int (*write_head)(const void *h, int len);
    uint32_t (*irq_off)(void);
    void (*irq_on)(uint32_t);
} ms_env;

typedef struct {                            /* file header, 4 KB, written after the slots it points to */
    uint32_t magic, ver;                    /* 'MSEQ', 1 */
    uint8_t  dest[MS_TRACKS], ch[MS_TRACKS], mute[MS_TRACKS];
    uint8_t  in[MS_TRACKS];                 /* recording input: low nibble MS_DEST_* (I keyboard, S players, M MIDI in), high nibble MIDI channel */
    uint16_t first[MS_TRACKS][MS_PAGES];    /* first slot of each (track, page) chain, MS_NONE: empty */
    uint8_t  used[MS_SLOTS / 8];            /* file slot allocation */
    uint32_t crc;
} ms_head;

typedef struct {
    const ms_env *env;
    ms_head h;
    ms_slot ram[MS_RAM];
    uint16_t ram_slot[MS_RAM];              /* file slot in each buffer (MS_NONE: free) */
    uint8_t  ram_dirty[MS_RAM], ram_use[MS_RAM];
    uint8_t  where[MS_SLOTS];               /* RAM buffer of each file slot (0xFF: not resident) */
    uint8_t  head_dirty;
    uint8_t  ro;                            /* header read error: no new slots, no writes until ms_service reads it */
    uint8_t  seen[MS_SLOTS / 8];            /* used map rebuild (ms_core.c scan_step): slots reached by a chain */
    int scan;                               /* chain being scanned (t * MS_PAGES + p), -1 none */
    uint16_t scan_sl;                       /* its next slot (MS_NONE: the chain's first) */
    uint32_t gen, scan_gen;                 /* slot allocations + frees; at the scan start */
    /* transport / recording */
    int32_t prev;                           /* tape position of the last block (-1: stopped) */
    int sel, rec;                           /* selected track, recording on */
    struct { uint8_t track, note, dest, ch; int32_t left; } act[MS_ACTIVE];   /* sounding: samples left */
    int nact;
    struct { uint8_t note, ch; int32_t pos; } held[MS_HELD];
    int nheld;
    int32_t cc_last[128];                   /* recording: position of the last recorded value per controller */
    uint32_t underruns, dropped;
    ms_ev clip[MS_CLIP];                    /* clipboard: offsets relative to the first copied event (len kept) */
    int32_t clip_pos[MS_CLIP];
    uint8_t clip_trk[MS_CLIP];              /* source track of each event */
    int nclip, clip_all;                    /* clip_all: paste into the source tracks, else into the selected one */
    int32_t clip_len, clip_first;           /* clip_len -1: empty; clip_first: position of the first copied event */
    struct { int32_t pos; uint8_t note, ch, track; } take[MS_TAKE];   /* notes recorded since ms_take_reset */
    int ntake;
} ms_t;

/* quantize grids (one per song): 0 off, 1/4, 1/8, 1/16, 1/32, 1/8T, 1/16T */
enum { MS_Q_OFF, MS_Q_4, MS_Q_8, MS_Q_16, MS_Q_32, MS_Q_8T, MS_Q_16T, MS_Q_N };

void ms_init(ms_t *s, const ms_env *env);  /* empty song */
int  ms_load(ms_t *s);                      /* header from the file (0 ok; 1 none / damaged: an empty song; -1 read error:
                                             * empty and read-only until ms_service reads it) */
void ms_block(ms_t *s);                     /* audio thread, once per 64-sample block */
void ms_service(ms_t *s, int budget);       /* main loop: prefetch around the playhead, write dirty slots (budget I/O) */
/* recording input (any thread; the audio thread inserts at its next block in the firmware glue, here directly) */
void ms_note_in(ms_t *s, int note, int vel, int ch, int on);
void ms_cc_in(ms_t *s, int cc, int val, int ch);
/* editing (main loop): range [a, b) of the selected track */
int  ms_copy(ms_t *s, int32_t a, int32_t b, int mask);   /* mask: tracks (bit t) */
int  ms_cut(ms_t *s, int32_t a, int32_t b, int mask);
int  ms_paste(ms_t *s, int32_t at);
void ms_all_off(ms_t *s);
int32_t ms_grid_step(int grid, int32_t bpm_q8);                 /* grid step in samples (0: off) */
int  ms_quantize(ms_t *s, int track, int32_t a, int32_t b, int32_t step);   /* notes starting in [a, b) to the grid */
int  ms_quantize_take(ms_t *s, int32_t step);                  /* the notes of the last take (then the list is empty) */
void ms_take_reset(ms_t *s);
int  ms_load_range(ms_t *s, int track, int pa, int pb);          /* main loop: make pages resident (0 ok) */
/* queries (drawing, tests) */
int  ms_count(ms_t *s, int track);                               /* resident events of a track */
int  ms_events(ms_t *s, int track, int32_t a, int32_t b, ms_ev *out, int32_t *pos, int max);   /* resident only */
uint32_t ms_crc(const void *p, int n);
#endif
