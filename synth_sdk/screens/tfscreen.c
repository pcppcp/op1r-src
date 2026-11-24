/* Free tape machine screen (docs/82, approved preview display/tapemachine_preview2.py): replaces R246's tape screen
 * while the free machine is on (op1r settings, white: TAPE STOCK / FREE).
 *   top-left: the active track's number in a box (as stock); top: its head as m:ss:ms; top-right: its speed (blue)
 *   and "rev" (red); middle: a window of +-8 s around its own head (fixed in the middle), clips blue, its loop green;
 *   bottom: the four tracks as thin lanes around their own heads, the active one's clips blue, the others grey.
 * Keys (consumed on this screen): T1..T4 select; s1 / s2 / s3 (0x2D / 0x2E / 0x2F) set the active track's loop in /
 * out at its head and toggle it (out also switches it on), as stock does for the global loop. Knobs: white speed
 * (ratios; Shift: fine), red record level (Shift + red: marker jump), Shift + s5 reverse; blue / green R246's. Every other key stays R246's.
 * Text in the stock font (op1_text_*, lowercase); lines with Raster::line. Clips from db2::Clips (0x01AA8D70:
 * 512 x { track, start, stop } in samples, used flags at + 0x1800).
 * Swap like the MIDI sequencer view (seqdev/ms_fw.c): an iter-class screen object (empty usbout.svg) in the tape
 * screen's slot of the screen table; R246's object back when the machine is off. */
#include "../op1_synth.h"
#include "stroke_font.h"
#include "slotown.h"

typedef struct { int32_t speed; int32_t lin, lout; uint8_t lon, lset, mute, stop; } tm_track;   /* stop: Shift + STOP / PLAY, per track */
extern volatile tm_track tm_tr[4];
extern volatile uint8_t tm_on;
int32_t tm_head_pos(int t);

#define SCREEN_TABLE   ((void **)0x01DF4E54)
#define CUR_MODE_ID    (*(volatile int *)0xFF907D58)              /* tape = 2 */
#define CUR_SCREEN     (*(volatile int *)0x018B65C8)
#define SCREEN_CTOR    ((void *(*)(void *, int, const char *))0x01A3BAB0)
#define ITER_SCREEN_VT ((const uint32_t *)0x019103FC)
#define SVG_LOAD       ((void *(*)(void *))0x01A503D8)
#define SHIFT          (*(volatile uint8_t *)0x01DD7F78)
#define CLIPS          ((const volatile int32_t *)0x01AA8D70)     /* [i * 3 + 0] track, + 1 start, + 2 stop */
#define CLIP_USED      ((const volatile uint8_t *)0x01AAA570)
#define NCLIPS         512
#define SR             44100
#define REC_LEVEL      (*(volatile int16_t *)0x01B4EF6E)          /* R246's tape record level (stock slider: >> 5, docs/11) */
extern volatile int16_t tm_in_peak;                              /* tape/tm.c: peak of the bus R246 records; (0xFF801088,
                                                                  * R246's meter source, stays 0 with the instrument) */
#define IN_PEAK        tm_in_peak
#define T_MODE         (*(volatile int32_t *)0xFF802960)          /* 0 stop, 1 rec, 2 play, 3 wind */
#define TSPEED_RAW     (*(volatile int32_t *)0xFF802968)          /* transport speed (0: stood still) */
#define UI_STATE       (*(volatile int32_t *)0x018C17D4)          /* 6 = record armed (docs/25) */
void tm_set_head(int t, int32_t pos);
#define SEL_TRACK      (*(volatile int32_t *)0xFF802964)          /* R246's selected track: where its recording goes */
#define TAKE_A         (*(volatile int32_t *)0xFF8029E4)          /* the take being recorded (samples, tape_c range) */
#define TAKE_B         (*(volatile int32_t *)0xFF8029E8)
#define TAPE_VT        0x01911B5C                                 /* R246's TapeScreen vtable (docs/22) */
#define TPOS           (*(volatile int32_t *)0xFF802974)          /* samples << 6 */
#define LOCATE(s)      (((void (*)(int32_t))0x01A55024)(s))       /* tape_c::check_pos (0 .. 0xF2BFA0) */
#define TM_MAXPOS      (0x1E5 * 0x8000)                           /* FREE's tape end: tm.c's last chunk (R246's own: 0xF2BFA0) */
#define TE_BPM_BITS    (*(volatile uint32_t *)0x018C1878)         /* tempo, float (read as bits) */
static inline uint32_t now(void) { uint32_t c; __asm__ volatile ("%0 = CYCLES;" : "=d"(c)); return c; }
static int32_t bpm_q8(void)                                      /* as seqdev/ms_fw.c: float bits -> Q8, 120 if odd */
{
    const uint32_t b = TE_BPM_BITS; const int e = (int)((b >> 23) & 0xFF) - 127;
    if ((b >> 31) || e < 5 || e > 7) return 120 << 8;
    return (int32_t)(((b & 0x7FFFFF) | 0x800000) >> (15 - e));
}

static uint32_t tfs_scr[32], tfs_vt[10];
static so_owner tfs_own = { .obj = tfs_scr };                  /* the slot shared with the MIDI seq view and devupd */
static int tfs_id = -1;
static uint8_t tfs_in, sel;

static const int32_t ratios[] = { 0x4000, 0x5555, 0x8000, 0xAAAB, 0xC000, 0x10000, 0x15555, 0x18000, 0x20000, 0x30000, 0x40000 };
static const char *const ratio_txt[] = { "1:4", "1:3", "1:2", "2:3", "3:4", "1:1", "4:3", "3:2", "2:1", "3:1", "4:1" };
#define NR ((int)(sizeof ratios / sizeof ratios[0]))

static const uint8_t c_blue[4] = { 0x4D, 0x9E, 0xFF, 0xFF }, c_green[4] = { 0x00, 0xED, 0x95, 0xFF },
                     c_white[4] = { 0xFF, 0xFF, 0xFF, 0xFF }, c_red[4] = { 0xFF, 0x3A, 0x5D, 0xFF },
                     c_grey[4] = { 0x6F, 0x71, 0x73, 0xFF }, c_dim[4] = { 0x40, 0x42, 0x44, 0xFF },
                     c_dgreen[4] = { 0x00, 0x77, 0x4A, 0xFF }, c_dred[4] = { 0x80, 0x1D, 0x2E, 0xFF };

static void ln(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c) { if (x0 != x1 || y0 != y1) sf_line(r, x0, y0, x1, y1, w, c); }
static char *put(char *q, const char *s) { while (*s) *q++ = *s++; *q = 0; return q; }
static char *put_u(char *q, uint32_t v, int w) { char b[10]; int n = 0; do b[n++] = (char)('0' + v % 10); while ((v /= 10) || n < w); while (n) *q++ = b[--n]; *q = 0; return q; }
static void fmt_speed(char *q, int32_t sp)                       /* "1/2x", "1.37x" */
{
    if (sp < 0) sp = -sp;
    for (int i = 0; i < NR; i++) if (ratios[i] == sp) { put(q, ratio_txt[i]); return; }
    const uint32_t c = (uint32_t)(((int64_t)sp * 100 + 0x8000) >> 16);
    q = put_u(q, c / 100, 1); *q++ = '.'; put_u(q, c % 100, 2);
}

/* stock-font texts: built once, redrawn; set only when the string changes */
enum { T_NUM, T_NUMD, T_TIME, T_SPEED, T_REV, T_DIAG, T_C1, T_C2, T_D0, S_TW = T_D0 + 5, S_TR = S_TW + 8, S_LEN = S_TR + 8,
       S_CHL, S_CH0, S_NW = S_CH0 + 4, S_NG = S_NW + 12, S_EA = S_NG + 12, S_ED, S_ES, S_ER, S_QL, S_PL0, S_PL1, S_PL2,
       S_PV0, S_PV1, S_PV2, S_PL3, S_PV3, S_SY, S_KP, S_KI, S_BW, S_BR, S_PNL, S_PNV, S_HL0, S_HL1, S_HL2, S_HL3, S_HPN, S_LH, S_LHS, S_LNW, S_LNG = S_LNW + 4, S_LC0 = S_LNG + 4, T_N = S_LC0 + 16 };   /* S_: the slot machine */   /* T_TIME: minutes; T_D0..: s s ms ms ms */
static uint32_t tx[T_N][OP1_TEXT_SIZE / 4];
static char shown[T_N][16];
static uint8_t made;
static void txt(void *r, int i, const char *s)
{
    static const int32_t ident[9] = { 1024, 0, 0, 0, 1024, 0, 0, 0, 1024 };
    int same = 1;
    for (int j = 0; j < 16; j++) { if (shown[i][j] != s[j]) { same = 0; break; } if (!s[j]) break; }
    if (!same) { op1_text_set(tx[i], s); int j = 0; for (; j < 15 && s[j]; j++) shown[i][j] = s[j]; shown[i][j] = 0; }
    op1_text_draw(tx[i], r, ident);
}
#define TXT_CTOR(o, x, y, sz, c, a) (((void *(*)(void *, int32_t, int32_t, int32_t, const uint8_t *, int32_t))0x01A61904)( \
                                    (o), (x), (y), (sz), (c), (a)))   /* align: 0 left, 1 centred, 2 right */
static void make(void)
{
    TXT_CTOR(tx[T_NUM], OP1_Q10(13), 8, 12, c_white, 1);       /* as the MIDI sequencer view's track square */
    TXT_CTOR(tx[T_NUMD], OP1_Q10(13), 8, 12, c_grey, 1);        /* the same, a muted track (the colour is the text object's) */
    /* the head m:ss:ms in fixed cells (the stock font is proportional: one text would jitter): minutes right-aligned
     * at 126, each other digit centred in a 13 px cell, the colons in 7 px cells; the whole 114 .. 206, centred at 160 */
    TXT_CTOR(tx[T_TIME], OP1_Q10(126), 4, 20, c_white, 2);
    TXT_CTOR(tx[T_C1], OP1_Q10(130) + 512, 4, 20, c_white, 1);
    TXT_CTOR(tx[T_C2], OP1_Q10(163) + 512, 4, 20, c_white, 1);
    { static const int16_t dx[5] = { 140, 153, 173, 186, 199 };   /* cell centres + 0.5 px */
      for (int i = 0; i < 5; i++) TXT_CTOR(tx[T_D0 + i], OP1_Q10(dx[i]) + 512, 4, 20, c_white, 1); }
    TXT_CTOR(tx[T_SPEED], OP1_Q10(300), 4, 12, c_white, 2);   /* left of the meter */
    TXT_CTOR(tx[T_REV], OP1_Q10(315), 18, 12, c_red, 2);
    {   /* the slot machine: mm:ss:cc in cells (digits 13 px, colons 7 px; 114 .. 206), white and red */
        static const int16_t cx[8] = { 120, 133, 143, 153, 166, 176, 186, 199 };
        for (int i = 0; i < 8; i++) { TXT_CTOR(tx[S_TW + i], OP1_Q10(cx[i]) + 512, 4, 20, c_white, 1);
                                      TXT_CTOR(tx[S_TR + i], OP1_Q10(cx[i]) + 512, 4, 20, c_red, 1); }
        TXT_CTOR(tx[S_LEN], OP1_Q10(160), 31, 12, c_grey, 1);
        TXT_CTOR(tx[S_CHL], OP1_Q10(252), 4, 12, c_grey, 0);
        static const uint8_t *const cc[4] = { c_blue, c_green, c_white, c_red };
        for (int i = 0; i < 4; i++) TXT_CTOR(tx[S_CH0 + i], OP1_Q10(270), 4, 12, cc[i], 0);
        for (int k = 0; k < 12; k++) {                           /* slot numbers, right-aligned left of their bars */
            const int32_t x = OP1_Q10(10 + (k % 3) * 98 + 12); const int y = 54 + (k / 3) * 22 - 7;
            TXT_CTOR(tx[S_NW + k], x, y, 12, c_white, 2); TXT_CTOR(tx[S_NG + k], x, y, 12, c_grey, 2);
        }
        static const int16_t ex[4] = { 45, 120, 195, 270 };
        for (int i = 0; i < 4; i++) TXT_CTOR(tx[S_EA + i], OP1_Q10(ex[i]), 118, 12, cc[i], 1);
        TXT_CTOR(tx[S_QL], OP1_Q10(294), 31, 12, c_white, 2);
        static const int16_t px[4] = { 40, 112, 184, 256 };
        for (int i = 0; i < 3; i++) { TXT_CTOR(tx[S_PL0 + i], OP1_Q10(px[i]), 62, 12, c_grey, 1);
                                      TXT_CTOR(tx[S_PV0 + i], OP1_Q10(px[i]), 80, 14, cc[i], 1); }
        TXT_CTOR(tx[S_PL3], OP1_Q10(px[3]), 62, 12, c_grey, 1); TXT_CTOR(tx[S_PV3], OP1_Q10(px[3]), 80, 14, c_red, 1);
        TXT_CTOR(tx[S_SY], OP1_Q10(4), 31, 12, c_white, 0);
        TXT_CTOR(tx[S_KP], OP1_Q10(286), 4, 12, c_red, 0);   /* right of the channel */
        TXT_CTOR(tx[S_PNL], OP1_Q10(40), 100, 12, c_grey, 1); TXT_CTOR(tx[S_HPN], OP1_Q10(40), 100, 12, c_white, 1);
        for (int i = 0; i < 4; i++) TXT_CTOR(tx[S_HL0 + i], OP1_Q10(px[i]), 62, 12, c_white, 1);
        TXT_CTOR(tx[S_PNV], OP1_Q10(40), 116, 14, c_blue, 1);
        TXT_CTOR(tx[S_LH], OP1_Q10(4), 40, 12, c_grey, 0); TXT_CTOR(tx[S_LHS], OP1_Q10(4), 40, 12, c_grey, 0);
        for (int i = 0; i < 4; i++) {                            /* T4 rows: number, then blue / green / white / red columns */
            const int y = 56 + i * 20 - 7;
            TXT_CTOR(tx[S_LNW + i], OP1_Q10(14), y, 12, c_white, 1); TXT_CTOR(tx[S_LNG + i], OP1_Q10(14), y, 12, c_grey, 1);
            static const int16_t lx[4] = { 70, 134, 196, 248 };
            for (int c = 0; c < 4; c++) TXT_CTOR(tx[S_LC0 + i * 4 + c], OP1_Q10(lx[c]), y, 12, cc[c], 1);
        }
        TXT_CTOR(tx[S_BW], OP1_Q10(160), 4, 20, c_white, 1); TXT_CTOR(tx[S_BR], OP1_Q10(160), 4, 20, c_red, 1);
    }
#ifdef TF_DIAG
    TXT_CTOR(tx[T_DIAG], OP1_Q10(4), 92, 12, c_grey, 0);
#endif
    made = 1;
}

/* the input meter + record level at the right edge, as stock (STEFAN and the slot machine; red encoder = level) */
static void in_meter(void *r, int32_t y0, int32_t y1)
{
    const int32_t x = OP1_Q10(310), hgt = y1 - y0;
    ln(r, x, y0, x, y1, 0x500, c_dim);                           /* the scale dark, the signal red, the level a red dot */
    /* as stock: a dB scale (R246's getter: log, clamped to -24 dB); here -24 .. 0 dB over the height */
    int32_t pk = IN_PEAK < 0 ? -IN_PEAK : IN_PEAK, db8 = -24 * 256;               /* dB, Q8 */
    if (pk > 0) {                                                /* 20 log10(pk / 32768) = 6.0206 (log2 pk - 15) */
        int e = 0; while ((pk >> e) > 1) e++;
        const int32_t fr = ((pk << 8) >> e) - 256;                /* the mantissa above 1, Q8 */
        const int32_t l2 = (e - 15) * 256 + (fr * (362 - (fr * 106 >> 8)) >> 8);   /* log2(1 + x) ~ x (1.414 - 0.414 x) */
        db8 = l2 * 1541 >> 8;                                    /* x 6.0206 */
        if (db8 < -24 * 256) db8 = -24 * 256;
    }
    const int32_t py = y1 - (int32_t)((int64_t)hgt * (db8 + 24 * 256) / (24 * 256));
    if (py < y1) ln(r, x, py, x, y1, 0xA00, c_red);
    const int32_t ly = y1 - (int32_t)((int64_t)hgt * (REC_LEVEL < 0 ? 0 : REC_LEVEL) >> 15);
    static const int8_t hw[7] = { 2, 3, 4, 4, 4, 3, 2 };         /* the dot: stacked lines */
    for (int k = 0; k < 7; k++) ln(r, x - hw[k] * OP1_Q10(1), ly + (k - 3) * OP1_Q10(1), x + hw[k] * OP1_Q10(1), ly + (k - 3) * OP1_Q10(1), 0x480, c_red);
}

/* ---- the slot machine (docs/85, tm_on == 2): keys, recording from the main loop, the page ---- */
#include "../tape/slots.h"
static uint8_t sl_sel;                                           /* the selected slot */
static uint8_t sl_envp;                                          /* the page: 0 slots, 1 envelope (T2), 2 play params (T3) */
static volatile uint8_t sl_arm, sl_tape;                         /* + the tape key held (+ PLAY / STOP: all slots) */
static uint8_t sl_perf, sl_perf_saved = 0xFF;                    /* keyboard: 0 input (notes), 1 perform (slots); red push */                                  /* REC held: a keyboard key starts the recording */
static volatile int8_t sl_want = -1;                             /* a record start asked for (the slot) */
static volatile uint8_t sl_stop_req, sl_dirty;
static uint32_t sl_t0, sl_dirty_t;
static int32_t sl_clip_bpm, sl_clip_len;
static uint8_t sl_lsel;
static void sl_go(int s, int on, int synced);
static volatile int8_t sl_play_after = -1;                       /* PLAY during a take: this slot plays when it ends */                                          /* T4: the selected LFO (encoder pushes) */
static int32_t sl_lfo_saved[SL_N][4];
#define SL_WC 36                                                 /* saved waveform: 36 columns over the length (9 words of 4 bytes) */
static int32_t sl_wv_saved[SL_N][SL_WC / 4];
static uint32_t sl_wv_t;
static void sl_wv_pack(int s, int32_t *w)                        /* the slot's peaks -> 36 columns, 4 per word */
{
    const int32_t len = tm_sl[s].len, nb = len > 0 ? ((len - 1) >> SL_PKS) + 1 : 0;
    for (int i = 0; i < SL_WC / 4; i++) w[i] = 0;
    for (int j = 0; j < SL_WC && nb; j++) {
        int a = 0; for (int32_t b = j * nb / SL_WC; b <= (j + 1) * nb / SL_WC && b < nb; b++) if (tm_sl_peak[s][b] > a) a = tm_sl_peak[s][b];
        w[j >> 2] |= (int32_t)((uint32_t)a << ((j & 3) * 8));
    }
}
static void sl_wv_unpack(int s, const int32_t *w)                /* boot: the columns back over the peaks */
{
    const int32_t len = tm_sl[s].len, nb = len > 0 ? ((len - 1) >> SL_PKS) + 1 : 0;
    for (int32_t b = 0; b < nb && b < SL_PKN; b++) { const int j = (int)(b * SL_WC / nb); tm_sl_peak[s][b] = (uint8_t)((uint32_t)w[j >> 2] >> ((j & 3) * 8)); }
}
static const char *const sl_div_txt[SL_DIVS] = { "4/1", "2/1", "1/1", "1/2", "1/4", "1/4t", "1/8", "1/8t", "1/16", "1/16t", "1/32" };
static const uint8_t sl_div_n[SL_DIVS] = { 4, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1 }, sl_div_d[SL_DIVS] = { 1, 1, 1, 2, 4, 6, 8, 12, 16, 24, 32 };   /* cycle = bar x n / d */
static int32_t sl_lfo_pack(const volatile tm_sllfo *L)
{
    return L->rate | L->sync << 6 | L->wave << 7 | L->type << 10 | (L->tgt & 3) << 12 | (L->amt + 32) << 14;
}
static void sl_lfo_unpack(volatile tm_sllfo *L, int32_t v, int k)
{
    if (v < 0) { L->rate = 31; L->sync = 0; L->wave = 0; L->type = 0; L->tgt = (uint8_t)k; L->amt = 0; return; }   /* ~1 Hz, off */
    L->sync = (uint8_t)(v >> 6 & 1); L->rate = (uint8_t)(v & 63); if (L->sync && L->rate >= SL_DIVS) L->rate = 4;
    L->wave = (uint8_t)(v >> 7 & 7);
    L->type = (uint8_t)(v >> 10 & 3); if (L->type > 2) L->type = 0;
    L->tgt = (uint8_t)(v >> 12 & 3);
    const int a = (int)(v >> 14 & 63) - 32; L->amt = (int8_t)(a < -31 ? -31 : a > 31 ? 31 : a);
}                         /* the last lifted slot's tempo (for its drop) */
static const uint8_t sl_kb[24] = { 39, 13, 40, 14, 41, 15, 42, 28, 32, 29, 22, 30, 31, 23, 48, 24, 49, 25, 50, 51, 26, 52, 27, 53 };   /* F .. E (seqdev/ms_fw.c kb_keys) */
static const uint8_t sl_white[12] = { 0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19 };   /* Shift + the first 12 white keys (F .. C) select */
static const uint8_t sl_qbars4[8] = { 0, 1, 2, 4, 8, 16, 32, 64 };   /* record length: free, then bars x 4 (1/4 .. 16) */
static const char *const sl_qtxt[8] = { "free", "1/4 bar", "1/2 bar", "1 bar", "2 bars", "4 bars", "8 bars", "16 bars" };
#define CLIP_IDX_T  (*(uint8_t **volatile *)0x01B4EF48)           /* chunk -> R246 clip entry (tm.c) */
#define SET_MODE_T  (((void (*)(void))0x01A581F4)())             /* TapeController::set_mode */
#define KEY_DISPATCH_T ((void (*)(int, int))0x019B6C74)
enum { F_LEN, F_CH, F_LVL, F_A, F_D, F_S, F_R, F_P, F_RV, F_M, F_Q, F_PM, F_SY, F_BPM, F_PAN, F_N };   /* the saved fields per slot (op1rsettings "sl" slot field) */
static const char sl_fc[F_N] = { 'l', 'c', 'v', 'a', 'd', 's', 'r', 'p', 'n', 'm', 'q', 't', 'y', 'b', 'k' };
static int32_t sl_saved[SL_N][F_N];
static int32_t sl_field(int s, int f)
{
    volatile tm_slm *q = &tm_sl[s];
    switch (f) { case F_LEN: return q->len; case F_CH: return q->ch; case F_LVL: return q->lvl; case F_A: return q->a;
                 case F_D: return q->d; case F_S: return q->s; case F_R: return q->r; case F_P: return q->semi + 24;
                 case F_RV: return q->rev; case F_M: return q->mode; case F_Q: return q->qlen; case F_PM: return q->pmode; case F_SY: return q->sync; case F_BPM: return q->bpm; default: return q->pan + 50; }
}
static void sl_touch(void) { sl_dirty = 1; sl_dirty_t = now(); }
static int sl_chunk_loaded(int32_t c)
{
    const uint8_t *e = CLIP_IDX_T ? CLIP_IDX_T[c] : 0;
    return e && e[0] && *(const int16_t *)(e + 6) == c;
}
static void sl_service(void)                                     /* main loop (tfs_poll) */
{
    tm_sl_bpm = bpm_q8();                                        /* synced slots follow it (tm.c) */
    for (int s = 0; s < SL_N; s++) {                             /* which LFOs run (the engine skips a slot without) */
        uint8_t b = 0; for (int k = 0; k < 4; k++) if (tm_sl_lfo[s][k].amt) b |= (uint8_t)(1 << k);
        tm_sl_lfoon[s] = b;
    }
    for (int s = 0; s < SL_N; s++) {                             /* their speed ratio, here (a 64-bit division) */
        const int32_t b = tm_sl[s].bpm; int64_t v = b > 0 ? ((int64_t)tm_sl_bpm << 16) / b : 65536;
        tm_sl_tr[s] = (uint32_t)(v < 16384 ? 16384 : v > 262144 ? 262144 : v);
    }
    {                                                            /* LFO phase per block: free rates once, divisions per tempo */
        static int32_t bar_was;
        extern uint32_t ql_inc(int32_t);
        if (!tm_sl_rinc[0]) for (int r = 0; r < 64; r++) tm_sl_rinc[r] = ql_inc((r << 9) | 0x100);
        const int32_t bar = (int32_t)((int64_t)4 * 60 * SR * 256 / tm_sl_bpm);
        if (bar != bar_was) { bar_was = bar;
            for (int d = 0; d < SL_DIVS; d++) tm_sl_dinc[d] = (uint32_t)(((uint64_t)64 << 32) * sl_div_d[d] / ((uint64_t)bar * sl_div_n[d])); }
    }
    tm_sl_bar = (int32_t)((int64_t)4 * 60 * SR * 256 / tm_sl_bpm);   /* the sync grid, from R246's tempo */
    if (sl_arm && sl_want < 0 && tm_sl_rec < 0) tm_sl_queue = ((sl_sel % 3) * SL_MAX) >> 15;   /* armed: the first chunk ready */
    if (sl_want >= 0) {                                          /* start: the transport plays, the slot's first chunk is in
                                                                  * R246's cache, then REC as the stock REC held (10 -> 5) */
        const int s = sl_want, od = tm_sl[s].play && tm_sl[s].len > 0;   /* playing: overdub along it */
        const int32_t c = ((s % 3) * SL_MAX + (od ? tm_sl[s].pos : 0)) >> 15;
        if (UI_STATE == 10 && sl_chunk_loaded(c) && od) {
            tm_sl_od = 1; tm_sl_recwait = 0; tm_sl_full = 0; tm_sl_rec = (int8_t)s; sl_want = -1;
            UI_STATE = 5; SET_MODE_T;
        } else if (UI_STATE == 10 && sl_chunk_loaded(c)) {
            const int qb = sl_qbars4[tm_sl[s].qlen & 7];         /* a fixed length: bars from R246's tempo */
            const int64_t tg = qb ? (int64_t)qb * 60 * SR * 256 / bpm_q8() : SL_MAX;
            tm_sl_rectgt = (int32_t)(tg > SL_MAX ? SL_MAX : tg < 64 ? 64 : tg);
            for (int k = 0; k < SL_PKN; k++) tm_sl_peak[s][k] = 0;   /* a new take: a new waveform */
            tm_sl_od = 0; tm_sl_recwait = tm_sl[s].sync; tm_sl_recpos = 0; tm_sl[s].bpm = tm_sl_bpm; tm_sl[s].len = 0; tm_sl[s].play = 0; tm_sl[s].gate = 0; tm_sl_full = 0;
            tm_sl_rec = (int8_t)s; sl_want = -1;
            UI_STATE = 5; SET_MODE_T;
        } else if (now() - sl_t0 > 400000000u * 3) sl_want = -1;   /* not ready in 3 s: dropped */
        else tm_sl_queue = c;
    }
    int32_t cut = -1;                                            /* a free synced take: its length to the nearest bar (min 1) */
    if (sl_stop_req && !tm_sl_full && tm_sl_rec >= 0 && !tm_sl_od && !tm_sl_recwait) {
        volatile tm_slm *q = &tm_sl[tm_sl_rec];
        if (q->sync && !q->qlen && tm_sl_bar > 0) {
            const int32_t rp = tm_sl_recpos, bar = tm_sl_bar; int32_t n = (rp + bar / 2) / bar; if (n < 1) n = 1;
            const int64_t tg = (int64_t)n * bar;
            if (tg > rp && tg <= SL_MAX) { tm_sl_rectgt = (int32_t)tg; sl_stop_req = 0; }   /* on to the bar: the engine stops it */
            else cut = (int32_t)(tg > SL_MAX ? SL_MAX : tg);    /* past it: now, cut back */
        }
    }
    if ((sl_stop_req || tm_sl_full) && tm_sl_rec >= 0) {         /* stop: as REC released (5 -> 10); R246 closes the take */
        const int s = tm_sl_rec;
        sl_stop_req = 0; tm_sl_full = 0;
        if (UI_STATE == 5) { UI_STATE = 10; SET_MODE_T; }
        tm_sl_rec = -1; tm_sl_od = 0; sl_touch();
        if (cut > 0 && cut < tm_sl[s].len) tm_sl[s].len = cut;
        if (sl_play_after == s) { sl_play_after = -1; sl_go(s, 1, 0); }   /* PLAY ended it: on, now */
    }
    sl_stop_req = 0;
    if (tm_sl_rec < 0 && now() - sl_wv_t > 800000000u) {         /* the waveforms (every 2 s, what changed): visible after a reboot */
        extern void op1r_setting_put(const char *, int);
        sl_wv_t = now();
        for (int s = 0; s < SL_N; s++) {
            int32_t w[SL_WC / 4]; sl_wv_pack(s, w);
            for (int i = 0; i < SL_WC / 4; i++) if (w[i] != sl_wv_saved[s][i]) {
                const char key[5] = { 's', 'w', (char)('a' + s), (char)('0' + i), 0 };
                op1r_setting_put(key, w[i]); sl_wv_saved[s][i] = w[i];
            }
        }
    }
    if (sl_dirty && now() - sl_dirty_t > 400000000u) {           /* into op1rsettings, 1 s after the last change, only what changed */
        sl_dirty = 0;
        extern void op1r_setting_put(const char *, int);
        if (sl_perf != sl_perf_saved) { op1r_setting_put("slkbm", sl_perf); sl_perf_saved = sl_perf; }
        for (int s = 0; s < SL_N; s++) for (int k = 0; k < 4; k++) {   /* the LFOs: one packed value each ("sl" slot digit) */
            const int32_t v = sl_lfo_pack(&tm_sl_lfo[s][k]);
            if (v == sl_lfo_saved[s][k]) continue;
            const char key[5] = { 's', 'l', (char)('a' + s), (char)('0' + k), 0 };
            op1r_setting_put(key, v); sl_lfo_saved[s][k] = v;
        }
        for (int s = 0; s < SL_N; s++) for (int f = 0; f < F_N; f++) {
            const int32_t v = sl_field(s, f);
            if (v == sl_saved[s][f]) continue;
            const char k[5] = { 's', 'l', (char)('a' + s), sl_fc[f], 0 };
            op1r_setting_put(k, v); sl_saved[s][f] = v;
        }
    }
}
void sl_erase_all(void)                                          /* main loop: the tape erased (tape/erase.c): every slot empty */
{
    if (tm_sl_rec >= 0) sl_stop_req = 1;
    for (int s = 0; s < SL_N; s++) {
        volatile tm_slm *q = &tm_sl[s];
        q->pend = 0; q->pstop = 0; q->gate = 0; q->play = 0; q->len = 0; q->bpm = 0;
        for (int k = 0; k < SL_PKN; k++) tm_sl_peak[s][k] = 0;
    }
    sl_touch();
}
void sl_state_load(void)                                         /* boot (tfs_state_load) */
{
    extern int op1r_setting_get(const char *, int);
    static const int16_t def[F_N] = { 0, -1, 80, 0, 0, 100, 10, 24, 0, 0, 0, 0, 0, 0, 50 };   /* channel -1: the slot's track */
    sl_perf = sl_perf_saved = (uint8_t)(op1r_setting_get("slkbm", 0) == 1);
    static const int32_t hi[F_N] = { 0, 3, 100, 100, 100, 100, 100, 48, 1, 2, 7, 1, 1, 300 * 256, 100 };
    for (int s = 0; s < SL_N; s++) for (int k = 0; k < 4; k++) {
        const char key[5] = { 's', 'l', (char)('a' + s), (char)('0' + k), 0 };
        sl_lfo_unpack(&tm_sl_lfo[s][k], op1r_setting_get(key, -1), k); sl_lfo_saved[s][k] = sl_lfo_pack(&tm_sl_lfo[s][k]);
    }
    for (int s = 0; s < SL_N; s++) {
        volatile tm_slm *q = &tm_sl[s];
        for (int f = 0; f < F_N; f++) {
            const char k[5] = { 's', 'l', (char)('a' + s), sl_fc[f], 0 };
            int32_t v = op1r_setting_get(k, def[f] < 0 ? s / 3 : def[f]);
            if (f == F_LEN) v = v < 0 ? 0 : v > SL_MAX ? SL_MAX : v; else v = v < 0 ? 0 : v > hi[f] ? hi[f] : v;
            switch (f) { case F_LEN: q->len = v; break; case F_CH: q->ch = (uint8_t)v; break; case F_LVL: q->lvl = (uint8_t)v; break;
                         case F_A: q->a = (uint8_t)v; break; case F_D: q->d = (uint8_t)v; break; case F_S: q->s = (uint8_t)v; break;
                         case F_R: q->r = (uint8_t)v; break; case F_P: q->semi = (int8_t)(v - 24); break;
                         case F_RV: q->rev = (uint8_t)v; break; case F_M: q->mode = (uint8_t)v; break;
                         case F_Q: q->qlen = (uint8_t)v; break; case F_PM: q->pmode = (uint8_t)v; break; case F_SY: q->sync = (uint8_t)v; break; case F_BPM: q->bpm = v; break; default: q->pan = (int8_t)(v - 50); }
            sl_saved[s][f] = v;
        }
    }
    for (int s = 0; s < SL_N; s++) {                             /* the saved waveforms (after the lengths) */
        for (int i = 0; i < SL_WC / 4; i++) { const char key[5] = { 's', 'w', (char)('a' + s), (char)('0' + i), 0 }; sl_wv_saved[s][i] = op1r_setting_get(key, 0); }
        sl_wv_unpack(s, sl_wv_saved[s]);
    }
}
static void sl_go(int s, int on, int synced)                     /* key thread: start / stop, now or at the bar */
{
    volatile tm_slm *q = &tm_sl[s];
    if (on) { if (q->len <= 0 || tm_sl_rec == (int8_t)s) return; q->pstop = 0; q->pend = (uint8_t)(synced ? 2 : 1); }
    else { q->pend = 0; if (synced && q->play) q->pstop = 1; else { q->pstop = 0; q->gate = 0; } }
}
/* R246's note for a lift over 90 s (tape key handler 0x01A57772): the message screen (0x019DE250 makes it once) shows
 * the text in the colour R246 passes (0x01BAAF58, filled at boot); key thread, as there */
static void lift_too_long(void)
{
#ifndef OP1_HOST
    ((void *(*)(void))0x019DE250)();
    ((void (*)(const char *, const uint8_t *))0x019DE1E0)("max. lift: 90 secs!", (const uint8_t *)0x01BAAF58);
#endif
}
static int sl_key(int code, int pressed)                         /* key thread, slot page shown */
{
    for (int i = 0; i < 24; i++) if (code == sl_kb[i]) {         /* Shift + a white key: select; a key while REC is held: record */
        if (sl_perf) {                                           /* perform: a white key starts / stops, Shift + white selects */
            for (int k = 0; k < SL_N; k++) if (sl_white[k] == i && pressed) {
                if (SHIFT) sl_sel = (uint8_t)k;
                else sl_go(k, !(tm_sl[k].play || tm_sl[k].pend), tm_sl[k].sync);   /* the slot's sync */
            }
            return 1;                                            /* no notes */
        }
        if (SHIFT) { for (int k = 0; k < SL_N; k++) if (sl_white[k] == i && pressed) sl_sel = (uint8_t)k; return 1; }
        if (pressed && sl_arm && sl_want < 0 && tm_sl_rec < 0) { sl_want = (int8_t)sl_sel; sl_t0 = now(); }
        return 0;                                                /* the note plays */
    }
    if (code == 0x24 || code == 0x25) {                          /* < / >: the selected slot's playhead, 1/16 of its length */
        volatile tm_slm *q = &tm_sl[sl_sel];
        if (pressed && q->len > 16) { int32_t p = q->pos + (code == 0x25 ? 1 : -1) * (q->len / 16);
                                      p %= q->len; if (p < 0) p += q->len; q->pos = p; }
        return 1;
    }
    if (code == 0x04) { sl_tape = (uint8_t)pressed; if (pressed) sl_envp = 0; return 0; }
    if (code == 0x3E && sl_envp == 0) { if (pressed) { sl_perf = (uint8_t)!sl_perf; sl_touch(); } return 1; }   /* red push: keyboard mode */
    if (code == 0x3D && sl_envp == 0) { if (pressed) { tm_sl[sl_sel].sync = (uint8_t)!tm_sl[sl_sel].sync; sl_touch(); } return 1; }   /* white push: the slot's sync */    /* tape: back to the slots (R246's handler too) */
    if (code >= 0x06 && code <= 0x09) { if (pressed) sl_envp = (uint8_t)(code - 0x06); return 1; }   /* T1 slots, T2 envelope, T3 play, T4 LFOs */
    if (sl_envp == 3 && code >= 0x3B && code <= 0x3E) { if (pressed) sl_lsel = (uint8_t)(code - 0x3B); return 1; }   /* T4: push selects the LFO */
    if (code == 0x22) {                                          /* PLAY: the selected slot now (Shift: at the bar; tape held: all
                                                                  * now); REC held: record */
        if (pressed && sl_arm) { if (sl_want < 0 && tm_sl_rec < 0) { sl_want = (int8_t)sl_sel; sl_t0 = now(); } return 1; }
        if (pressed && tm_sl_rec >= 0) {                         /* during a take: it ends, the slot plays on (an overdub: it just ends) */
            if (!tm_sl_od) sl_play_after = tm_sl_rec;
            sl_stop_req = 1; return 1;
        }
        if (pressed) for (int s = 0; s < SL_N; s++) if (sl_tape || s == sl_sel) sl_go(s, 1, SHIFT && !sl_tape);
        return 1;
    }
    if (code == 0x23) {                                          /* STOP: released (Shift: all); a recording stops */
        if (pressed) {
            if (tm_sl_rec >= 0 && (sl_tape || tm_sl_rec == (int8_t)sl_sel)) sl_stop_req = 1;
            for (int s = 0; s < SL_N; s++) if (sl_tape || s == sl_sel) sl_go(s, 0, SHIFT && !sl_tape);   /* Shift: at the bar; tape: all */
            sl_want = -1;
        }
        return 1;
    }
    if (code == 0x21) {                                          /* REC held: armed (a keyboard key starts); REC while recording: stop */
        if (pressed) {
            if (tm_sl_rec >= 0 || sl_want >= 0) { sl_stop_req = 1; sl_want = -1; }
            else {
                if (T_MODE == 0) { KEY_DISPATCH_T(0x22, 1); KEY_DISPATCH_T(0x22, 0); }   /* the transport plays (R246's PLAY) */
                sl_arm = 1;
            }
        } else sl_arm = 0;
        return 1;
    }
    if (code == 0x0A) {                                          /* LIFT: into R246's clipboard (when it fits), the slot empty */
        volatile tm_slm *q = &tm_sl[sl_sel];
        if (pressed && tm_sl_rec < 0 && q->len > 0x3C8FE7) lift_too_long();   /* over 90 s (slots hold 120): R246's note,
                                                                 * the slot stays (it used to be emptied without the copy) */
        else if (pressed && tm_sl_rec < 0 && q->len > 0) {
            const int t = sl_sel / 3; const int32_t a = (sl_sel % 3) * SL_MAX, e = a + q->len;
            for (int i = 0; i < NCLIPS; i++)                     /* audio_clipboard::copy needs the clip there */
                if (CLIP_USED[i] && CLIPS[i * 3] == t && CLIPS[i * 3 + 1] <= a && a < CLIPS[i * 3 + 2]) {
                    ((void (*)(int, int32_t, int32_t, int))0x0191BBD0)(t, a, e, i);
                    sl_clip_bpm = q->bpm; sl_clip_len = q->len;
                    break;
                }
            q->pend = 0; q->gate = 0; q->play = 0; q->len = 0; sl_touch();
        }
        return 1;
    }
    if (code == 0x0B) {                                          /* DROP: R246's clipboard (tape audio) onto the slot, from its start */
        const int32_t n = *(volatile int32_t *)0x01AA7104;
        if (pressed && tm_sl_rec < 0 && *(volatile int32_t *)0x01AA7108 == 1 && n > 0 && n <= SL_MAX) {
            volatile tm_slm *q = &tm_sl[sl_sel];
            extern volatile uint8_t tm_sel, tm_reload_after_clip;
            q->pend = 0; q->gate = 0; q->play = 0;
            tm_sel = (uint8_t)(sl_sel / 3);                      /* R246's drop worker reads the selected track (tm.c sets it) */
            ((void (*)(int, int32_t, int))0x0191BEA8)(sl_sel / 3, (sl_sel % 3) * SL_MAX, 0);
            q->len = n; q->bpm = n == sl_clip_len ? sl_clip_bpm : 0;   /* the tempo if it came from a slot */
            tm_reload_after_clip = 1; sl_touch();
        }
        return 1;
    }
    if ((code >= 0x06 && code <= 0x0C) || (code >= 0x2D && code <= 0x2F) || code == 0x3E || code == 0x11) return 1;   /* FREE's keys: not here */
    return 0;
}
static void sl_knob(int k, int st)                               /* key thread: one step per detent */
{
    volatile tm_slm *q = &tm_sl[sl_sel];
    if (sl_envp == 3) {                                          /* T4, the selected LFO: blue rate, green amount, white wave, red
                                                                  * target; Shift: blue sync / free, green type */
        volatile tm_sllfo *L = &tm_sl_lfo[sl_sel][sl_lsel & 3];
        if (SHIFT && k == 0) { const int on = st > 0; if (on != L->sync) { L->sync = (uint8_t)on; L->rate = on ? 4 : 31; } }
        else if (SHIFT && k == 1) { const int v = L->type + st; L->type = (uint8_t)(v < 0 ? 0 : v > 2 ? 2 : v); }
        else if (SHIFT) return;
        else if (k == 0) { const int v = L->rate + st, hi = L->sync ? SL_DIVS - 1 : 63; L->rate = (uint8_t)(v < 0 ? 0 : v > hi ? hi : v); }
        else if (k == 1) { const int v = L->amt + st; L->amt = (int8_t)(v < -31 ? -31 : v > 31 ? 31 : v); }
        else if (k == 2) { const int v = L->wave + st; L->wave = (uint8_t)(v < 0 ? 0 : v > 7 ? 7 : v); }   /* white: wave */
        else L->tgt = (uint8_t)((L->tgt + st) & 3);                                                   /* red: target */
        sl_touch(); return;
    }
    if (sl_envp == 2) {                                          /* blue pitch, green reverse, white loop / once / ping-pong */
        if (k == 0 && SHIFT) { const int v = q->pan + st * 2; q->pan = (int8_t)(v < -50 ? -50 : v > 50 ? 50 : v); }   /* Shift + blue: pan */
        else if (k == 0) { const int v = q->semi + st; q->semi = (int8_t)(v < -24 ? -24 : v > 24 ? 24 : v); }
        else if (k == 1) q->rev = (uint8_t)(st > 0);
        else if (k == 2) { const int v = q->mode + st; q->mode = (uint8_t)(v < 0 ? 0 : v > 2 ? 2 : v); }
        else q->pmode = (uint8_t)(st > 0);                       /* red: tape / stretch */
        sl_touch(); return;
    }
    if (sl_envp) {                                               /* blue a, green d, white s, red r */
        volatile uint8_t *p = k == 0 ? &q->a : k == 1 ? &q->d : k == 2 ? &q->s : &q->r;
        const int v = *p + st * 2; *p = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v); sl_touch(); return;
    }
    if (k == 0) { q->ch = (uint8_t)((q->ch + st) & 3); sl_touch(); }   /* blue: channel */
    else if (k == 1) { const int v = q->lvl + st * 2; q->lvl = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v); sl_touch(); }   /* green: level */
    else if (k == 2) { const int v = q->qlen + st; q->qlen = (uint8_t)(v < 0 ? 0 : v > 7 ? 7 : v); sl_touch(); }   /* white: record length */
    else if (k == 3) { const int32_t v = REC_LEVEL + st * 0x400; REC_LEVEL = (int16_t)(v < 0 ? 0 : v > 0x7FFF ? 0x7FFF : v); }   /* red: input */
}
static void fmt_ms(char *q, int v)                               /* an envelope time: "120ms", "1.2s" */
{
    const int32_t ms = sl_env_ms(v);
    if (ms < 1000) { q = put_u(q, (uint32_t)ms, 1); put(q, "ms"); }
    else { q = put_u(q, (uint32_t)ms / 1000, 1); *q++ = '.'; q = put_u(q, (uint32_t)ms / 100 % 10, 1); put(q, "s"); }
}
static void sl_draw(void *r)
{
    static const uint8_t *const chc[4] = { c_blue, c_green, c_white, c_red };
    const int s = sl_sel, rec = tm_sl_rec == (int8_t)s;
    volatile tm_slm *q = &tm_sl[s];
    char b[16];
    /* the boxed slot number and its state (as STEFAN's track square) */
    ln(r, OP1_Q10(3), OP1_Q10(5), OP1_Q10(23), OP1_Q10(5), 0x500, c_white); ln(r, OP1_Q10(23), OP1_Q10(5), OP1_Q10(23), OP1_Q10(25), 0x500, c_white);
    ln(r, OP1_Q10(23), OP1_Q10(25), OP1_Q10(3), OP1_Q10(25), 0x500, c_white); ln(r, OP1_Q10(3), OP1_Q10(25), OP1_Q10(3), OP1_Q10(5), 0x500, c_white);
    put_u(b, (uint32_t)s + 1, 1); txt(r, T_NUM, b);
    {
        const int32_t x = OP1_Q10(28), y = OP1_Q10(15);
        if (rec || sl_want == (int8_t)s || sl_arm) {             /* recording / armed: a red dot */
            static const int8_t hw[7] = { 2, 3, 4, 4, 4, 3, 2 };
            for (int k = 0; k < 7; k++) ln(r, x + OP1_Q10(4) - hw[k] * OP1_Q10(1), y + (k - 3) * OP1_Q10(1), x + OP1_Q10(4) + hw[k] * OP1_Q10(1), y + (k - 3) * OP1_Q10(1), 0x480, c_red);
        } else if (q->play) {                                    /* playing: a green triangle */
            ln(r, x, y - OP1_Q10(5), x, y + OP1_Q10(5), 0x700, c_green);
            ln(r, x, y - OP1_Q10(5), x + OP1_Q10(8), y, 0x700, c_green);
            ln(r, x, y + OP1_Q10(5), x + OP1_Q10(8), y, 0x700, c_green);
        } else {                                                 /* stopped: a square */
            ln(r, x, y - OP1_Q10(4), x + OP1_Q10(8), y - OP1_Q10(4), 0x700, c_white); ln(r, x + OP1_Q10(8), y - OP1_Q10(4), x + OP1_Q10(8), y + OP1_Q10(4), 0x700, c_white);
            ln(r, x + OP1_Q10(8), y + OP1_Q10(4), x, y + OP1_Q10(4), 0x700, c_white); ln(r, x, y + OP1_Q10(4), x, y - OP1_Q10(4), 0x700, c_white);
        }
    }
    if (q->sync) {                                               /* synced: bar.beat.16th (the slot's tempo), the length in bars */
        const int32_t bpm = rec || q->bpm <= 0 ? tm_sl_bpm : q->bpm;
        const int64_t six = (int64_t)15 * SR * 256 / (bpm > 0 ? bpm : 120 << 8);   /* frames per 16th */
        const int32_t t = rec ? tm_sl_recpos : q->play ? q->pos : 0;
        const uint32_t n = (uint32_t)(t / six);
        char *e = put_u(b, n / 16 + 1, 1); e = put(e, "."); e = put_u(e, n / 4 % 4 + 1, 1); e = put(e, "."); put_u(e, n % 4 + 1, 1);
        txt(r, rec ? S_BR : S_BW, b);
    } else
    /* mm:ss:cc in fixed cells (red while recording), the length below */
    {
        const int32_t t = rec ? tm_sl_recpos : q->play ? q->pos : q->len;
        const uint32_t sec = (uint32_t)t / SR, cs = (uint32_t)t % SR * 100 / SR;
        const uint32_t dg[6] = { sec / 600, sec / 60 % 10, sec % 60 / 10, sec % 10, cs / 10, cs % 10 };
        const int base = rec ? S_TR : S_TW;
        for (int i = 0; i < 8; i++) {
            if (i == 2 || i == 5) b[0] = ':'; else b[0] = (char)('0' + dg[i - (i > 2) - (i > 5)]);
            b[1] = 0; txt(r, base + i, b);
        }
    }
    /* channel (blue) and level (green) */
    txt(r, S_CHL, "ch"); b[0] = (char)('1' + (q->ch & 3)); b[1] = 0; txt(r, S_CH0 + (q->ch & 3), b);
    ln(r, OP1_Q10(252), OP1_Q10(26), OP1_Q10(292), OP1_Q10(26), 0x400, c_dim);
    if (q->lvl) ln(r, OP1_Q10(252), OP1_Q10(26), OP1_Q10(252) + OP1_Q10(40) * q->lvl / 100, OP1_Q10(26), 0x800, c_green);
    txt(r, S_QL, sl_qtxt[q->qlen & 7]);
    txt(r, S_KP, sl_perf ? "p" : "i");                           /* keyboard mode (red push): perform / input */
    if (q->sync) txt(r, S_SY, "sync");                           /* white push */                          /* white: the record length */
    in_meter(r, OP1_Q10(8), OP1_Q10(130));
    if (sl_envp == 3) {                                          /* T4: four LFOs, a row each; the selected: white number, a bar */
        static const char *const tg[4] = { "level", "pan", "pitch", "speed" };
        static const char *const ty[3] = { "run", "restart", "one shot" };
        extern void ql_hz_text(char *, uint32_t), ql_wave_glyph(void *, int, int32_t, int32_t, const uint8_t *);
        static const uint8_t *const ec[4] = { c_blue, c_green, c_white, c_red };
        const int sh = SHIFT;
        txt(r, sh ? S_LHS : S_LH, "");                           /* (keeps the column header objects in use) */
        for (int i = 0; i < 4; i++) {
            volatile tm_sllfo *L = &tm_sl_lfo[s][i];
            const int32_t y = OP1_Q10(56 + i * 20), on = i == (sl_lsel & 3);
            ln(r, OP1_Q10(11), y, OP1_Q10(17), y, OP1_Q10(4), ec[i]);   /* the dot (as the quad page's): the colour of the push that selects it */
            ln(r, OP1_Q10(14), y - OP1_Q10(3), OP1_Q10(14), y + OP1_Q10(3), OP1_Q10(4), ec[i]);
            if (on) ln(r, OP1_Q10(8), y + OP1_Q10(9), OP1_Q10(20), y + OP1_Q10(9), 0x500, c_white);
            if (sh) { txt(r, S_LC0 + i * 4, L->sync ? "sync" : "free"); txt(r, S_LC0 + i * 4 + 1, ty[L->type > 2 ? 0 : L->type]);
                      txt(r, S_LC0 + i * 4 + 2, ""); txt(r, S_LC0 + i * 4 + 3, ""); continue; }
            if (L->sync) txt(r, S_LC0 + i * 4, sl_div_txt[L->rate < SL_DIVS ? L->rate : SL_DIVS - 1]);
            else { char h[12]; ql_hz_text(h, tm_sl_rinc[L->rate & 63]); txt(r, S_LC0 + i * 4, h); }
            { char *e = b; int a = L->amt * 100 / 31; if (a < 0) { *e++ = '-'; a = -a; } put(put_u(e, (uint32_t)a, 1), "%"); txt(r, S_LC0 + i * 4 + 1, b); }
            ql_wave_glyph(r, L->wave & 7, OP1_Q10(196 - 12), y, c_white);   /* white: the wave; red: the target */
            txt(r, S_LC0 + i * 4 + 3, tg[L->tgt & 3]);
            if (L->amt && q->play) {                             /* the output: a tick on a short lane right of the row */
                const int32_t cx = OP1_Q10(292), x = cx + (L->val * OP1_Q10(8) >> 15);
                ln(r, cx - OP1_Q10(8), y, cx + OP1_Q10(8), y, 0x300, c_dim);
                ln(r, x, y - OP1_Q10(4), x, y + OP1_Q10(4), 0x500, c_white);
            }
        }
        return;
    }
    if (sl_envp == 2) {                                          /* T3: pitch, direction, play mode */
        static const char *const md[3] = { "loop", "once", "ping-pong" };
        const int hl = !SHIFT, lb = hl ? S_HL0 : S_PL0;          /* the labels the encoders edit now: white */
        txt(r, lb, "pitch"); txt(r, lb + 1, "direction"); txt(r, lb + 2, "mode"); txt(r, hl ? S_HL3 : S_PL3, "pitch");
        txt(r, S_PV3, q->pmode ? "stretch" : "tape");
        txt(r, hl ? S_PNL : S_HPN, "pan");                       /* Shift + blue */
        if (!q->pan) put(b, "c"); else { char *e = put(b, q->pan < 0 ? "l" : "r"); put_u(e, (uint32_t)(q->pan < 0 ? -q->pan : q->pan) * 2, 1); }
        txt(r, S_PNV, b);
        char *e = b; if (q->semi > 0) *e++ = '+'; else if (q->semi < 0) *e++ = '-';
        put_u(e, (uint32_t)(q->semi < 0 ? -q->semi : q->semi), 1); txt(r, S_PV0, b);
        txt(r, S_PV1, q->rev ? "reverse" : "forward"); txt(r, S_PV2, md[q->mode > 2 ? 0 : q->mode]);
        return;
    }
    if (sl_envp) {                                               /* T2: the envelope (blue a, green d, white s, red r) */
        const int32_t x0 = OP1_Q10(20), y1 = OP1_Q10(108), h = OP1_Q10(60), y0 = y1 - h, ys = y1 - h * q->s / 100;
        const int32_t wa = OP1_Q10(70) * q->a / 100, wd = OP1_Q10(70) * q->d / 100, wr = OP1_Q10(70) * q->r / 100;
        const int32_t xa = x0 + wa, xd = xa + wd, xs = xd + OP1_Q10(60), xr = xs + wr;
        ln(r, x0, y1, xa, y0, 0x600, c_blue); ln(r, xa, y0, xd, ys, 0x600, c_green);
        ln(r, xd, ys, xs, ys, 0x600, c_white); ln(r, xs, ys, xr, y1, 0x600, c_red);
        ln(r, x0, y1 + OP1_Q10(3), OP1_Q10(290), y1 + OP1_Q10(3), 0x300, c_dim);
        fmt_ms(b, q->a); txt(r, S_EA, b); fmt_ms(b, q->d); txt(r, S_ED, b);
        put(put_u(b, q->s, 1), "%"); txt(r, S_ES, b); fmt_ms(b, q->r); txt(r, S_ER, b);
        return;
    }
    /* the 12 slots: 4 rows (a tape track each) x 3 */
    for (int k = 0; k < SL_N; k++) {
        volatile tm_slm *p = &tm_sl[k];
        const int32_t x = OP1_Q10(10 + (k % 3) * 98), bx = x + OP1_Q10(22), w = OP1_Q10(68), y = OP1_Q10(54 + (k / 3) * 22);
        const int on = k == s, rk = tm_sl_rec == (int8_t)k;
        const int waits = p->pend == 2 || p->pstop || sl_want == (int8_t)k || (rk && tm_sl_recwait);   /* waiting for the bar */
        const int lit = waits ? (now() >> 26) & 1 : p->play && !rk;   /* white: playing; blinking: waiting (~3 Hz) */
        put_u(b, (uint32_t)k + 1, 1); txt(r, (lit ? S_NW : S_NG) + k, b);
        ln(r, bx, y, bx + w, y, 0x400, c_dim);
        const int32_t l = rk ? tm_sl_recpos : p->len;
        if (l > 0) {                                             /* the waveform over the slot's length (every 2 px) */
            const uint8_t *col = rk ? c_red : chc[p->ch & 3];
            const int32_t nb = ((l - 1) >> SL_PKS) + 1, npx = 34;
            for (int j = 0; j < npx; j++) {
                const int32_t b0 = j * nb / npx, b1 = (j + 1) * nb / npx;
                int a = 0; for (int32_t b = b0; b <= b1 && b < nb; b++) if (tm_sl_peak[k][b] > a) a = tm_sl_peak[k][b];
                int32_t lg = 0;                                  /* log2 (a) Q8: ~6 dB per px, so quiet takes show too */
                if (a) { int e = 0; while ((a >> e) > 1) e++; lg = e * 256 + ((a << 8 >> e) - 256); }
                const int32_t x = bx + OP1_Q10(1) + j * OP1_Q10(2), hh = OP1_Q10(1) + lg * OP1_Q10(7) / (8 * 256);   /* +-8 px at full */
                ln(r, x, y - hh, x, y + hh, 0x500, col);
            }
        }
        if (p->play && !rk && p->len > 0) {
            const int32_t px = bx + (int32_t)((int64_t)w * p->pos / p->len);   /* over its length, as the waveform */
            ln(r, px, y - OP1_Q10(6), px, y + OP1_Q10(6), 0x600, c_white);
        }
        if (on) {                                                /* the selected slot: brackets */
            const int32_t a = bx - OP1_Q10(3), z = bx + w + OP1_Q10(3);
            ln(r, a, y - OP1_Q10(5), a, y + OP1_Q10(5), 0x480, c_white); ln(r, a, y - OP1_Q10(5), a + OP1_Q10(3), y - OP1_Q10(5), 0x480, c_white); ln(r, a, y + OP1_Q10(5), a + OP1_Q10(3), y + OP1_Q10(5), 0x480, c_white);
            ln(r, z, y - OP1_Q10(5), z, y + OP1_Q10(5), 0x480, c_white); ln(r, z, y - OP1_Q10(5), z - OP1_Q10(3), y - OP1_Q10(5), 0x480, c_white); ln(r, z, y + OP1_Q10(5), z - OP1_Q10(3), y + OP1_Q10(5), 0x480, c_white);
        }
    }
}

/* a track around its own head: clips, its loop; window +-win s over [x0, x1] */
static uint8_t rec_red;                                          /* recording or armed: the active track red */
static volatile uint8_t rec_held;                                /* REC held (arms, as stock) */
/* x of tape position v on a scale of `half` Q10 units per `span` samples around cx (the head's x): positions are counted in
 * whole device pixels from the tape's start (screens/pxsnap.h scale), so two marks keep their pixel distance while the
 * head moves; per mark rounding (head-relative) made them step out of step, one pixel apart now and then */
static int32_t xof(int32_t cx, int32_t half, int32_t span, int32_t v, int32_t head)
{
    const int64_t s = px_dev_scale(), den = (int64_t)span << 20;
    const int64_t pv = (int64_t)(v < 0 ? 0 : v) * half * s / den, ph = (int64_t)(head < 0 ? 0 : head) * half * s / den;
    return cx + (int32_t)(((pv - ph) * (1 << 20) + s / 2) / s);
}
static void lane(void *r, int t, int32_t y, int32_t x0, int32_t x1, int32_t head, int32_t win, int32_t thick, int on, int big)
{
    const int32_t cx = (x0 + x1) / 2, half = (x1 - x0) / 2, span = win * SR;
    #define XOF(v) xof(cx, half, span, (v), head)
    #define CLAMP(v) ((v) < x0 ? x0 : (v) > x1 ? x1 : (v))
    ln(r, x0, y, x1, y, 0x400, c_dim);
    for (int i = 0; i < NCLIPS; i++) {
        if (!CLIP_USED[i] || CLIPS[i * 3] != t) continue;
        const int32_t a = XOF(CLIPS[i * 3 + 1]), b = XOF(CLIPS[i * 3 + 2]);
        if (b <= x0 || a >= x1) continue;
        const int under = on && CLIPS[i * 3 + 1] <= head && head < CLIPS[i * 3 + 2];   /* the clip under the head: red */
        ln(r, CLAMP(a), y, CLAMP(b), y, thick, tm_tr[t].mute || tm_tr[t].stop ? c_dim : on ? (rec_red || under ? c_red : c_blue) : c_grey);
    }
    if (on && T_MODE == 1 && TAKE_B >= TAKE_A) {                 /* the take, while it is recorded */
        const int32_t a = XOF(TAKE_A), b = XOF(TAKE_B);
        if (b > x0 && a < x1) ln(r, CLAMP(a), y, CLAMP(b) == CLAMP(a) ? CLAMP(a) + OP1_Q10(1) : CLAMP(b), y, thick, c_red);
    }
    const int lp = tm_tr[t].lon && tm_tr[t].lout > tm_tr[t].lin;
    if (lp || tm_tr[t].lset) {                                   /* loop in (also set but off): a line, a circle on top */
        const int32_t a = XOF(tm_tr[t].lin), h = thick / 2 + OP1_Q10(big ? 3 : 2), g = big ? 0x800 : 0x4CC;
        const uint8_t *c = on ? c_green : c_dgreen;
        if (a >= x0 && a <= x1) {
            ln(r, a, y - h, a, y + h, g, c);
            if (big) { const int32_t cy = y - h - OP1_Q10(3), q = OP1_Q10(2);   /* the circle: an octagon */
                static const int8_t ox[9] = { 2, 1, -1, -2, -2, -1, 1, 2, 2 }, oy[9] = { 1, 2, 2, 1, -1, -2, -2, -1, 1 };
                for (int k = 0; k < 8; k++) ln(r, a + ox[k] * q / 2, cy + oy[k] * q / 2, a + ox[k + 1] * q / 2, cy + oy[k + 1] * q / 2, 0x300, c); }
        }
        if (lp) {
            const int32_t b = XOF(tm_tr[t].lout);
            if (b >= x0 && b <= x1) {
                ln(r, b, y - h, b, y + h, g, c);
                if (big) { const int32_t cy = y - h - OP1_Q10(3);   /* loop out: a filled circle (stacked lines) */
                    static const int8_t hw[5] = { 1, 2, 2, 2, 1 };
                    for (int k = 0; k < 5; k++) ln(r, b - hw[k] * OP1_Q10(1), cy + (k - 2) * OP1_Q10(1), b + hw[k] * OP1_Q10(1), cy + (k - 2) * OP1_Q10(1), 0x480, c); }
            }
            if (big && b > x0 && a < x1) ln(r, CLAMP(a), y - h, CLAMP(b), y - h, g, c);
        }
    }
    #undef XOF
    #undef CLAMP
}

static void tfs_update(void *scr, int dt)
{
    op1_screen_draw(scr, dt);
    void *r = op1_raster();
    if (!made) { make(); extern void tm_ready(void); tm_ready(); }   /* op1r tape markers: loaded once (GUI thread, as stock) */
    if (tm_on == 2) { sl_draw(r); return; }                      /* the slot machine (docs/85) */
    const int32_t head = tm_head_pos(sel) < 0 ? 0 : tm_head_pos(sel);
    const int rev = tm_tr[sel].speed < 0, mode = T_MODE, armed = UI_STATE == 6 || rec_held;
    rec_red = (uint8_t)(mode == 1 || armed);
    /* boxed track number, dim while the track is muted */
    const uint8_t *cn = tm_tr[sel].mute ? c_grey : c_white;
    ln(r, OP1_Q10(3), OP1_Q10(5), OP1_Q10(23), OP1_Q10(5), 0x500, cn); ln(r, OP1_Q10(23), OP1_Q10(5), OP1_Q10(23), OP1_Q10(25), 0x500, cn);
    ln(r, OP1_Q10(23), OP1_Q10(25), OP1_Q10(3), OP1_Q10(25), 0x500, cn); ln(r, OP1_Q10(3), OP1_Q10(25), OP1_Q10(3), OP1_Q10(5), 0x500, cn);
    char b[16]; b[0] = (char)('1' + sel); b[1] = 0; txt(r, tm_tr[sel].mute ? T_NUMD : T_NUM, b);
    {                                                            /* state, right of the square (y 7..23) */
        const int32_t x = OP1_Q10(28), y = OP1_Q10(15);   /* level with the square (y 5 .. 25) */
        if (mode == 1 || armed) {                                /* recording: a red dot; armed: a red ring */
            static const int8_t hw[7] = { 2, 3, 4, 4, 4, 3, 2 };
            for (int k = 0; k < 7; k++) ln(r, x + OP1_Q10(4) - hw[k] * OP1_Q10(1), y + (k - 3) * OP1_Q10(1), x + OP1_Q10(4) + hw[k] * OP1_Q10(1), y + (k - 3) * OP1_Q10(1), 0x480, c_red);
        } else if ((mode == 2 || mode == 3) && !tm_tr[sel].stop) {   /* playing: a triangle (a stopped track: the square) */
            const int32_t xb = rev ? x + OP1_Q10(8) : x, xt = rev ? x : x + OP1_Q10(8);   /* reverse: flipped */
            ln(r, xb, y - OP1_Q10(5), xb, y + OP1_Q10(5), 0x700, c_green);
            ln(r, xb, y - OP1_Q10(5), xt, y, 0x700, c_green);
            ln(r, xb, y + OP1_Q10(5), xt, y, 0x700, c_green);
        } else {                                                 /* stopped: a square */
            ln(r, x, y - OP1_Q10(4), x + OP1_Q10(8), y - OP1_Q10(4), 0x700, c_white); ln(r, x + OP1_Q10(8), y - OP1_Q10(4), x + OP1_Q10(8), y + OP1_Q10(4), 0x700, c_white);
            ln(r, x + OP1_Q10(8), y + OP1_Q10(4), x, y + OP1_Q10(4), 0x700, c_white); ln(r, x, y + OP1_Q10(4), x, y - OP1_Q10(4), 0x700, c_white);
        }
    }
    const uint32_t hp = head < 0 ? 0 : (uint32_t)head, sec = hp / SR, ms = hp % SR * 1000 / SR;   /* m:ss:ms (per frame) */
    put_u(b, sec / 60, 1); txt(r, T_TIME, b);
    txt(r, T_C1, ":"); txt(r, T_C2, ":");
    const uint32_t dg[5] = { sec % 60 / 10, sec % 10, ms / 100, ms / 10 % 10, ms % 10 };
    for (int i = 0; i < 5; i++) { b[0] = (char)('0' + dg[i]); b[1] = 0; txt(r, T_D0 + i, b); }
    fmt_speed(b, tm_tr[sel].speed); txt(r, T_SPEED, b);
    in_meter(r, OP1_Q10(8), OP1_Q10(100));                       /* right edge, as stock: record level + input meter */
#ifdef TF_DIAG
    {                                                            /* OP1R_TFDIAG=1: why REC may be refused (docs/82) */
        extern volatile uint8_t pu_mode;
        char d[48], *q = d;
        q = put(q, "s"); q = put_u(q, (uint32_t)UI_STATE, 1);
        q = put(q, " q"); q = put_u(q, *(volatile uint32_t *)0x01AA7330 == *(volatile uint32_t *)0x01AA7334, 1);
        q = put(q, " a"); q = put_u(q, *(volatile uint32_t *)0xFF9002B4, 1);
        q = put(q, " c"); q = put_u(q, (uint32_t)*(volatile int32_t *)(0x01AA8D70 + 0x1A00), 1);
        q = put(q, " p"); q = put_u(q, pu_mode, 1);
        { extern volatile uint32_t tm_miss_t[4]; q = put(q, " m"); put_u(q, tm_miss_t[sel], 1); }   /* misses of the active track */
        txt(r, T_DIAG, d);
    }
#endif
    /* the active track around its head */
    {                                                            /* bar marks (from tape 0, R246's tempo) */
        const int64_t bar_q8 = (int64_t)4 * 60 * SR * 256 * 256 / bpm_q8();   /* samples per bar, Q8 */
        const int64_t a = (int64_t)(head - 8 * SR) << 8, b = (int64_t)(head + 8 * SR) << 8;
        for (int64_t k = a <= 0 ? 0 : (a + bar_q8 - 1) / bar_q8; k * bar_q8 <= b && k < 4096; k++) {
            const int32_t x = xof(OP1_Q10(155), OP1_Q10(145), 8 * SR, (int32_t)(k * bar_q8 >> 8), head);
            ln(r, x, OP1_Q10(76), x, OP1_Q10(82), 0x800, c_grey);
        }
    }
    lane(r, sel, OP1_Q10(68), OP1_Q10(10), OP1_Q10(300), head, 8, OP1_Q10(9), 1, 1);
    {                                                            /* op1r tape markers (tape/markers.c): the active track only */
        extern int32_t tm_pos[]; extern int tm_n;
        for (int i = 0; i < tm_n; i++) {
            const int64_t d = (int64_t)(tm_pos[i] - head) * OP1_Q10(145) / (8 * SR);
            if (d < -OP1_Q10(145) || d > OP1_Q10(145)) continue;
            const int32_t x = xof(OP1_Q10(155), OP1_Q10(145), 8 * SR, tm_pos[i], head);   /* as on the stock tape screen (tape/markers.c): a grey
                                                                  * line through the track, a green v above (white at the head) */
            const uint8_t *col = d > -OP1_Q10(2) && d < OP1_Q10(2) ? c_white : c_green;
            ln(r, x, OP1_Q10(58), x, OP1_Q10(78), 0x400, c_grey);
            ln(r, x - OP1_Q10(3), OP1_Q10(50), x, OP1_Q10(55), 0x600, col);
            ln(r, x + OP1_Q10(3), OP1_Q10(50), x, OP1_Q10(55), 0x600, col);
        }
    }
    const uint8_t *hc = rec_red ? c_red : c_white;
    ln(r, OP1_Q10(155), OP1_Q10(48), OP1_Q10(155), OP1_Q10(88), 0x600, hc);
    ln(r, OP1_Q10(155), OP1_Q10(48), OP1_Q10(rev ? 149 : 161), OP1_Q10(48), 0x600, hc);   /* the flag: the play direction */
    /* all four tracks */
    for (int t = 0; t < 4; t++) {
        const int32_t y = OP1_Q10(108 + 8 * t), h = tm_head_pos(t) < 0 ? 0 : tm_head_pos(t);
        lane(r, t, y, OP1_Q10(10), OP1_Q10(300), h, 20, OP1_Q10(3), t == sel, 0);
        ln(r, OP1_Q10(155), y - OP1_Q10(3), OP1_Q10(155), y + OP1_Q10(3), 0x400, t == sel ? c_white : c_grey);
    }
}
static void tfs_init(void *scr) { (void)scr; }

static void tfs_set(int on)                                      /* main loop */
{
    if (on && tfs_id < 0) {                                      /* first time: the tape screen is the current one */
        int id = -1;                                             /* the tape screen's slot, by class (not the current screen) */
        for (int i = 0; i < 0x44; i++) if (SCREEN_TABLE[i] && ((const uint32_t *)SCREEN_TABLE[i])[3] == TAPE_VT) { id = i; break; }
        if (id < 0) return;
        void *saved = SCREEN_TABLE[id];
        SCREEN_CTOR(tfs_scr, id, "usbout.svg");
        SCREEN_TABLE[id] = saved;
        for (int i = 0; i < 10; i++) tfs_vt[i] = ITER_SCREEN_VT[i];
        tfs_vt[3 * 2 + 1] = (uint32_t)tfs_update;
        tfs_vt[4 * 2 + 1] = (uint32_t)tfs_init;
        tfs_scr[3] = (uint32_t)tfs_vt;
        tfs_scr[0] = (uint32_t)SVG_LOAD((void *)tfs_scr[2]);
        if (!tfs_scr[0]) return;
        tfs_id = id;
    }
    if (tfs_id < 0) return;
    if (on) so_install(&tfs_own, SCREEN_TABLE, tfs_id); else so_restore(&tfs_own, SCREEN_TABLE);
    tfs_in = (uint8_t)on;
}
static int8_t wind;                                              /* -1 / +1 while < / > is held on this screen */
static uint32_t wind_t;
void tfs_poll(void)                                              /* main loop, every pass: setting, scrubbing */
{
    sl_service();
    if (tm_on && !tfs_in && CUR_MODE_ID == 2) tfs_set(1);
    else if (!tm_on && tfs_in) tfs_set(0);
    extern volatile int8_t tm_scrub; extern int tm_scrubbing(void);
    extern volatile uint8_t tm_scrub_track; tm_scrub_track = sel;
    tm_scrub = wind;                                             /* the engine moves (and plays) only the active track at up
                                                                  * to 8x; the others keep their speed / position */
    (void)tm_scrubbing;
}
static int shown_now(void) { return tfs_in && CUR_MODE_ID == 2 && tfs_id >= 0 && SCREEN_TABLE[tfs_id] == (void *)tfs_scr; }
int tfs_view_open(void) { return shown_now(); }               /* dispatcher.c tape_page: op1r's tape markers work here too */

int tfs_key(int code, int pressed)                               /* key thread: 1 = consumed */
{
    if (!shown_now()) return 0;
    if (tm_on == 2) return sl_key(code, pressed);                /* the slot machine (docs/85) */
    if (code == 0x21) rec_held = (uint8_t)pressed;               /* REC: shown as armed while held; stock handles it */
    if ((code == 0x22 || code == 0x23) && SHIFT) {               /* Shift + PLAY / STOP: start / stop the active track alone
                                                                  * (its head stands while the transport runs on) */
        if (pressed) tm_tr[sel].stop = (uint8_t)(code == 0x23);  /* saved by tfs_state_flush (flag bit 3) */
        return 1;
    }
    if (code == 0x23 && T_MODE == 0 && pressed) {                /* STOP while stopped (also spinning down): the active track to its loop in
                                                                  * (press only: R246's STOP release turns state 1 into 0, REC arms from 0)
                                                                  * (R246's would jump the transport: all tracks resync) */
        if (pressed) {
            tm_set_head(sel, tm_tr[sel].lset ? tm_tr[sel].lin : 0);
            if (UI_STATE == 1) { UI_STATE = 0; ((void (*)(void))0x01A581F4)(); }   /* R246's second STOP: state 1 -> 0 (only
                                                                  * from 0 does REC arm); its transport jump is left out */
        }
        return 1;
    }
    if (code >= 0x06 && code <= 0x09) {                          /* T: select (R246 records here); Shift + T: mute */
        if (pressed) { if (SHIFT) tm_tr[code - 0x06].mute ^= 1; else { extern volatile uint8_t tm_sel; sel = (uint8_t)(code - 0x06); tm_sel = sel; SEL_TRACK = sel; } }
        return 1;
    }
    if (code == 0x0C && !SHIFT) {                                /* scissors: split the active track's clip at its head */
        if (pressed && T_MODE != 1) {                            /* (R246's split: Clips::update_clip(track, pos, 0)) */
            ((void (*)(int, int32_t, int))0x0195CF9C)(sel, tm_head_pos(sel), 0);
            extern volatile uint8_t tm_reload_after_clip; tm_reload_after_clip = 1;
        }
        return 1;
    }
    if ((code == 0x0A || code == 0x0B) && !SHIFT) {              /* lift / drop: the active track at its head (R246's clipboard) */
        if (pressed) {
            const int32_t h = tm_head_pos(sel);
            if (code == 0x0B) ((void (*)(int, int32_t, int))0x0191BEA8)(sel, h, 0);   /* drop (as the tape key handler) */
            else for (int i = 0; i < NCLIPS; i++)                /* lift: the clip under the head */
                if (CLIP_USED[i] && CLIPS[i * 3] == sel && CLIPS[i * 3 + 1] <= h && h < CLIPS[i * 3 + 2]) {
                    const int32_t a = CLIPS[i * 3 + 1];
                    if (CLIPS[i * 3 + 2] - a > 0x3C8FE7) { lift_too_long(); break; }   /* copy asserts at 90 s
                                                                  * (audio_clipboard.cpp:380): R246's lift checks first */
                    ((void (*)(int, int32_t, int32_t, int))0x0191BBD0)(sel, a, CLIPS[i * 3 + 2], i);   /* audio_clipboard::copy */
                    tm_set_head(sel, a);                         /* as stock: to the lifted clip's start */
                    break;
                }
            extern volatile uint8_t tm_reload_after_clip; tm_reload_after_clip = 1;
        }
        return 1;
    }
    if (code == 0x3E) {                                          /* red press: a marker at the active head; Shift: remove */
        extern void tm_add_at(int32_t), tm_remove_at(int32_t, int32_t);
        if (pressed) { const int32_t h = tm_head_pos(sel); if (SHIFT) tm_remove_at(h, 6 * 2352); else tm_add_at(h); }   /* ~6 px */
        return 1;
    }
    if (code == 0x11) {                                          /* Shift + s5: reverse the active track */
        static uint8_t sw;
        if (pressed && SHIFT) { tm_tr[sel].speed = -tm_tr[sel].speed; sw = 1; return 1; }
        if (!pressed && sw) { sw = 0; return 1; }
        return 0;
    }
    if (code == 0x24 || code == 0x25) {                          /* < / >: our scrubbing (R246's would call TapeScreen
                                                                  * methods on this object); Shift: previous / next marker */
        if (pressed && SHIFT) {                                  /* Shift: previous / next bar (R246's tempo, from tape 0) */
            const int64_t bar_q8 = (int64_t)4 * 60 * SR * 256 * 256 / bpm_q8(), h = (int64_t)tm_head_pos(sel) << 8, m = 2000 << 8;
            int64_t k = code == 0x25 ? (h + m) / bar_q8 + 1 : (h - m <= 0 ? 0 : (h - m - 1) / bar_q8);
            const int32_t v = (int32_t)(k * bar_q8 >> 8);
            tm_set_head(sel, v < 0 ? 0 : v > TM_MAXPOS ? TM_MAXPOS : v);   /* the active track only */
            return 1;
        }
        wind = pressed && !SHIFT ? (code == 0x24 ? -1 : 1) : 0; wind_t = now();
        return 1;
    }
    if (code < 0x2D || code > 0x2F) return 0;
    if (!pressed) return 1;
    volatile tm_track *tr = &tm_tr[sel];
    const int32_t h = tm_head_pos(sel);
    if (code == 0x2D) { tr->lin = h; tr->lset = 1; if (tr->lout <= tr->lin) tr->lon = 0; }   /* s1: in (marker even if off) */
    else if (code == 0x2E) {                                     /* s2: out, the loop on */
        tr->lout = h;
        if (tr->lout < tr->lin) { const int32_t x = tr->lin; tr->lin = tr->lout; tr->lout = x; }
        tr->lon = tr->lout > tr->lin;
    } else if (SHIFT) {                                          /* Shift + s3: the loop = the clip under the head */
        for (int i = 0; i < NCLIPS; i++)
            if (CLIP_USED[i] && CLIPS[i * 3] == sel && CLIPS[i * 3 + 1] <= h && h < CLIPS[i * 3 + 2]) {
                tr->lin = CLIPS[i * 3 + 1]; tr->lout = CLIPS[i * 3 + 2] > TM_MAXPOS ? TM_MAXPOS : CLIPS[i * 3 + 2];
                tr->lset = 1; tr->lon = tr->lout > tr->lin;
                break;
            }
    } else tr->lon = (uint8_t)(!tr->lon && tr->lout > tr->lin);   /* s3: on / off */
    return 1;
}

static int32_t acc[8];
static int step_of(int k, int delta, int per)                    /* detents (2048 per detent on the device: 2 events) */
{
    if ((acc[k] > 0 && delta < 0) || (acc[k] < 0 && delta > 0)) acc[k] = 0;
    acc[k] += delta;
    const int st = acc[k] / per; acc[k] -= st * per;
    return st;
}
int tfs_knob(int knob, int delta)                                /* key thread: 1 = consumed (blue, red) */
{
    if (!shown_now() || knob < 0 || knob > 7) return 0;
    const int k = knob & 3, fine = knob > 3 || SHIFT;
    if (tm_on == 2) {                                            /* slot machine: all four (sl_knob), one step per detent */
        const int st = step_of(k, delta, 1024); if (st) sl_knob(k, st);
        return 1;
    }
    if (k != 2 && k != 3) return 0;                              /* blue / green: R246's */
    volatile tm_track *tr = &tm_tr[sel];
    if (k == 3) {                                                /* red: record level, as stock; Shift + red: marker jump */
        if (fine) {                                              /* the active track only (op1r tape markers, tape/markers.c) */
            const int st = step_of(7, delta, 1024); if (!st) return 1;
            extern int32_t tm_pos[]; extern int tm_n;
            const int32_t h = tm_head_pos(sel), m = 2000;
            int32_t best = -1;
            for (int i = 0; i < tm_n; i++) {
                const int32_t v = tm_pos[i];
                if (st > 0 ? (v > h + m && (best < 0 || v < best)) : (v < h - m && (best < 0 || v > best))) best = v;
            }
            if (best < 0)                                        /* past the last / before the first: wrap to the first / last */
                for (int i = 0; i < tm_n; i++) if (best < 0 || (st > 0 ? tm_pos[i] < best : tm_pos[i] > best)) best = tm_pos[i];
            if (best >= 0) tm_set_head(sel, best);
            return 1;
        }
        const int st = step_of(3, delta, 1024); if (!st) return 1;
        int32_t v = REC_LEVEL + st * 0x400;                      /* 32 clicks over the range */
        REC_LEVEL = (int16_t)(v < 0 ? 0 : v > 0x7FFF ? 0x7FFF : v);
        return 1;
    }
    const int neg = tr->speed < 0;
    int32_t sp = neg ? -tr->speed : tr->speed;
    if (fine) { const int st = step_of(6, delta, 512); if (!st) return 1;
        sp += st * (sp >> 7); if (sp < 0x2000) sp = 0x2000; if (sp > 0x40000) sp = 0x40000; }
    else { const int st = step_of(2, delta, 2048); if (!st) return 1;
        int i = 0; while (i < NR - 1 && ratios[i] < sp) i++;    /* nearest ratio at or above */
        if (ratios[i] != sp && st < 0) i++;
        i += st; i = i < 0 ? 0 : i >= NR ? NR - 1 : i; sp = ratios[i]; }
    tr->speed = neg ? -sp : sp;
    return 1;
}

/* FREE track state across reboots (op1rsettings, docs/82): per track head, loop in / out, speed, flags (loop on | in
 * set << 1 | mute << 2 | stop << 3), and the active track. Saved at key events when changed (as the settings pages, KeyHandler
 * thread), heads only while the tape stands still; loaded at boot. Only changed values are written. */
extern int op1r_setting_get(const char *, int); extern void op1r_setting_put(const char *, int);
static int32_t sv[4][5]; static int32_t sv_sel = -1; static uint8_t sv_have;
static char *sv_key(char *k, int t, char c) { k[0] = 't'; k[1] = 'f'; k[2] = (char)('0' + t); k[3] = c; k[4] = 0; return k; }
void tfs_state_load(void)
{
    {                                                            /* the tape machine setting at boot (settings.c reads it only when
                                                                  * its page is built: lazily, at its first showing) */
        extern int op1r_setting_get(const char *, int);
        const int v = op1r_setting_get("tapemach", -1); if (v >= 0 && v <= 2) tm_on = (uint8_t)v;
    }
    sl_state_load();
    extern volatile uint8_t tm_sel, tm_hold_heads;
    char k[5];
    for (int t = 0; t < 4; t++) {
        volatile tm_track *tr = &tm_tr[t];
        const int32_t h = op1r_setting_get(sv_key(k, t, 'h'), -1), li = op1r_setting_get(sv_key(k, t, 'i'), -1),
                      lo = op1r_setting_get(sv_key(k, t, 'o'), -1), sp = op1r_setting_get(sv_key(k, t, 's'), 0),
                      fl = op1r_setting_get(sv_key(k, t, 'f'), -1);
        if (h >= 0 && h <= 0xF2BFA0) tm_set_head(t, h);             /* (saved up to R246's end before: clamped) */
        if (li >= 0 && li <= 0xF2BFA0 && lo >= 0 && lo <= 0xF2BFA0) {
            tr->lin = li > TM_MAXPOS ? TM_MAXPOS : li; tr->lout = lo > TM_MAXPOS ? TM_MAXPOS : lo;
        }
        if ((sp >= 0x2000 && sp <= 0x40000) || (sp <= -0x2000 && sp >= -0x40000)) tr->speed = sp;   /* 1/8 .. 4x, either way */
        if (fl >= 0 && fl <= 15) { tr->lon = (uint8_t)(fl & 1 && tr->lout > tr->lin); tr->lset = (uint8_t)(fl >> 1 & 1); tr->mute = (uint8_t)(fl >> 2 & 1); tr->stop = (uint8_t)(fl >> 3 & 1); }
        sv[t][0] = h; sv[t][1] = li; sv[t][2] = lo; sv[t][3] = sp; sv[t][4] = fl;
    }
    const int32_t sl = op1r_setting_get("tfsel", -1);
    if (sl >= 0 && sl <= 3) { tm_sel = (uint8_t)sl; SEL_TRACK = sl; }
    sv_sel = sl; sv_have = 1;
    tm_hold_heads = 1;                                           /* tm.c: no resync to the transport before the first play */
}
void tfs_state_flush(void)
{
    extern volatile uint8_t tm_on, tm_sel;
    if (!tm_on || !sv_have) return;
    const int still = T_MODE == 0 && TSPEED_RAW == 0;
    char k[5];
    for (int t = 0; t < 4; t++) {
        const volatile tm_track *tr = &tm_tr[t];
        const int32_t v[5] = { still ? tm_head_pos(t) : sv[t][0], tr->lin, tr->lout, tr->speed, tr->lon | tr->lset << 1 | tr->mute << 2 | tr->stop << 3 };
        static const char key[5] = { 'h', 'i', 'o', 's', 'f' };
        for (int j = 0; j < 5; j++) if (v[j] != sv[t][j]) { op1r_setting_put(sv_key(k, t, key[j]), v[j]); sv[t][j] = v[j]; }
    }
    if (tm_sel != sv_sel) { op1r_setting_put("tfsel", tm_sel); sv_sel = tm_sel; }
}
