/* Track outs, step 1 (docs/21): the four tape tracks resampled like the master, in parallel to it.
 * The L1 tape mix loops log, per pass and track, where the track's N samples come from (track buffer) and where they go
 * in the mixed source buffers (0x01B5F434 / 0x01B5EF84, 600 samples each); hook.S _mix_hook_fwd / _rev. At the
 * resampler call (hook.S _l1_rs_probe -> tr_resample, before the master resampler runs) the same tracks' samples are
 * gathered into per-track source buffers at the same offsets and run through the master's filter: 16 taps, 64 phases,
 * coefficients 0xFF802154 [16][64] (row stride 64), position / increment in 1/64 samples, gain ramp from
 * W[0xFF8029DC] + W[*gain_inc] per sample. Result: tr_out[4][64] per block (pre-fader), and peaks for the monitor. */
#include "../op1_synth.h"

#define TR_SRC   600
#define TR_N     4
#ifdef OP1_HOST
typedef intptr_t tr_word;                                   /* host pointers are 64-bit */
#else
typedef int32_t tr_word;                                    /* layout written by hook.S */
#endif
volatile tr_word tr_rec[1 + 16 * 4];                      /* count, then {src, dest, track, N} (hook.S) */
int16_t tr_out[TR_N][64];
volatile int16_t tr_peak[TR_N];
/* the tracks' gathered samples as pairs: word i of pair p = track 2p (low half) | track 2p + 1 (high half), so one
 * dual MAC filters two tracks (screens/tr_rs.S); the gather writes the halves through int16 views */
static uint32_t tr_pair[2][TR_SRC + 1] __attribute__((aligned(4)));   /* + 1: the kernel's last read-ahead */
static uint8_t tr_clean;                                    /* tracks whose half is all zero */
#define TR_SRC16(t) ((int16_t *)tr_pair[(t) >> 1] + ((t) & 1))     /* track t's samples, stride 2 */

#ifdef OP1_HOST
extern const int16_t *tr_host_coef; extern int16_t tr_host_gain;
#define COEF   tr_host_coef
#define GAIN0  tr_host_gain
#define SRC_L  ((uintptr_t)0x1000)
#define SRC_R  ((uintptr_t)0x2000)
#else
#define COEF   ((const int16_t *)0xFF802154)
#define GAIN0  (*(volatile int16_t *)0xFF8029DC)
#define SRC_L  ((uintptr_t)0x01B5F434)
#define SRC_R  ((uintptr_t)0x01B5EF84)
#endif

static inline int16_t sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }

/* One pair's 64 outputs, R246's resampler arithmetic (tape_resample_read 0xFFA06194, per channel): per output
 * p += inc, idx = p >> 6, ph = 63 - (p & 63); A = sum over k = 0..15 of 2 c[ph + 64 k] s[idx + k], saturated to 32 bits
 * after every tap (W32); out = 2 g (A >> 16) >> 16 rounded ties to even, saturated (R.H = R.L * R.H); g += gi (S).
 * idx .. idx + 15 must lie in the source for all 64 outputs (the caller checks). */
typedef struct { const uint32_t *src; const int16_t *coef; int16_t *out_a, *out_b; int32_t pos, inc; int16_t g, gi; } tr_rs_job;
static inline int32_t sat32(int64_t v) { return v > 0x7FFFFFFF ? 0x7FFFFFFF : v < -0x7FFFFFFF - 1 ? -0x7FFFFFFF - 1 : (int32_t)v; }
static inline int16_t rmul(int16_t g, int16_t h)            /* R.H = g * h, fractional, rounded ties to even (the
                                                             * Blackfin default, docs/81), saturated */
{
    if (g == -32768 && h == -32768) return 32767;
    const int32_t p = 2 * (int32_t)g * h, q = p >> 16, r = p & 0xFFFF;
    return (int16_t)(r > 0x8000 || (r == 0x8000 && (q & 1)) ? q + 1 : q);
}
#if defined(__bfin__) && !defined(TR_RS_C_REF)
void tr_rs_pair(const tr_rs_job *j);                         /* screens/tr_rs.S */
#else
static void tr_rs_pair(const tr_rs_job *j)
{
    int32_t p = j->pos; int16_t g = j->g;
    for (int n = 0; n < 64; n++) {
        p += j->inc;
        const int32_t idx = p >> 6, ph = 63 - (p & 63);
        int32_t a = 0, b = 0;
        for (int k = 0; k < 16; k++) {
            const int32_t c = j->coef[ph + 64 * k], w = (int32_t)j->src[idx + k];
            const int32_t sa = (int16_t)w, sb = (int16_t)(w >> 16);   /* fractional MAC: -1 x -1 saturates */
            a = sat32((int64_t)a + (c == -32768 && sa == -32768 ? 0x7FFFFFFF : 2 * c * sa));
            b = sat32((int64_t)b + (c == -32768 && sb == -32768 ? 0x7FFFFFFF : 2 * c * sb));
        }
        j->out_a[n] = rmul(g, (int16_t)(a >> 16));
        j->out_b[n] = rmul(g, (int16_t)(b >> 16));
        g = sat16(g + j->gi);
    }
}
#endif

/* USB out setting (screens/usbout.c): 0 = master (track channels silent, no track work: saves CPU),
 * 1 = master + tracks. Always 6 channels, so it switches at once without re-enumeration. */
volatile uint8_t usb_tracks_live;

/* tape mix takeover (docs/83, step 2 + 3): R246's mix loops only log (hook.S skips their MAC loops), the resampler
 * call runs tp_resample: the four tracks resampled here (tr_rs.S) and mixed with R246's per-track gains (tm_g, tape/
 * tm.c) into tape L / R. Off in OP1R_TAPEMIX=stock builds (R246's mix and resampler, track outs as before). */
volatile uint8_t tp_on;                                     /* set per block by tp_decide (fxroute.c fr_snap) */
/* the takeover runs only while the tracks are wanted (USB track outs, FX2 aux, sidechain): there it saves R246's mix
 * plus the second resampling; without them R246's path is the cheaper one and runs untouched. Decided once per block,
 * after the tape mix, for the next one (the mix hooks and the resampler of one block see the same choice). */
void tp_decide(void)
{
#if defined(TAPEMIX_STOCK) || defined(TAPEMIX_CHECK)
    tp_on = 0;
#else
    extern int fr_tracks_needed(void);
    tp_on = (uint8_t)(usb_tracks_live || fr_tracks_needed());
#endif
}
/* OP1R_TAPEMIX=check: R246's mix and resampler play; tp_check_pre (before its resampler) computes ours into a shadow,
 * tp_check_post (its return, hook.S) compares the raw resampler output with it: blocks, blocks with any difference,
 * the largest difference (sysmon / peek) */
#ifdef TAPEMIX_CHECK
volatile uint8_t tp_check = 1;
#else
volatile uint8_t tp_check = 0;
#endif
volatile uint32_t tp_ck_blocks, tp_ck_diff, tp_ret_to, tp_ck_run, tp_ck_rundiff; volatile int32_t tp_ck_runmax; volatile int32_t tp_ck_max;
static int16_t tp_sh[2][64];
volatile int16_t tp_dbg[264]; volatile int32_t tp_dbg_rec[68]; volatile uint8_t tp_dbg_have;
static int32_t tr_dbg_rec[65], tr_dbg_pos, tr_dbg_inc;   /* check builds only */

void tr_resample(int32_t pos, int32_t inc, const int16_t *gain_inc)
{
    extern int fr_tracks_needed(void);                       /* FX2 aux in use (screens/fxroute.c) */
    if (!tp_on && !tp_check && !usb_tracks_live && !fr_tracks_needed()) { tr_rec[0] = 0; return; }   /* no track work */
#ifdef TAPEMIX_CHECK
    for (int k = 0; k < 65; k++) tr_dbg_rec[k] = (int32_t)tr_rec[k];   /* check build: this block's log */
    tr_dbg_pos = pos; tr_dbg_inc = inc;
#endif
    int n = tr_rec[0] > 16 ? 16 : tr_rec[0];
    uint8_t got = 0;
    for (int r = 0; r < n; r++) {                          /* gather: same offsets as the mixed buffers */
        const volatile tr_word *e = &tr_rec[1 + 4 * r];
        const int16_t *src = (const int16_t *)(uintptr_t)e[0];
        uintptr_t dst = (uintptr_t)e[1];
        int t = e[2], cnt = e[3];
        int32_t off = dst >= SRC_L && dst < SRC_L + 2 * TR_SRC ? (int32_t)(dst - SRC_L) / 2
                    : dst >= SRC_R && dst < SRC_R + 2 * TR_SRC ? (int32_t)(dst - SRC_R) / 2 : -1;
        if (t < 0 || t >= TR_N || off < 0 || cnt <= 0) continue;
        if (off + cnt > TR_SRC) cnt = TR_SRC - off;
        int16_t *d = TR_SRC16(t);
        if (!(got >> t & 1)) { for (int i = 0; i < TR_SRC; i++) d[2 * i] = 0; got |= (uint8_t)(1 << t); tr_clean &= (uint8_t)~(1 << t); }
        for (int i = 0; i < cnt; i++) {                    /* add: at the loop wrap the mixer makes 2 passes into */
            int32_t v = d[2 * (off + i)] + src[i];            /* the same region (end + start of the loop), like L += */
            d[2 * (off + i)] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
    }
    tr_rec[0] = 0;
    extern volatile uint8_t sc_blk, tr_at;
    tr_at = (uint8_t)(sc_blk + 1);                         /* fresh for the block fr_snap counts next */
    /* the 64 outputs read idx .. idx + 15 with idx = (pos + n inc) >> 6, n = 1 .. 64 (monotonic): all inside the
     * source, else that pair is silent (R246's own read is the same span; never seen out of range) */
    const int32_t i0 = (pos + inc) >> 6, i1 = (pos + 64 * inc) >> 6;
    const int ok = i0 >= 0 && i1 >= 0 && i0 + 16 <= TR_SRC && i1 + 16 <= TR_SRC;
    for (int pr = 0; pr < 2; pr++) {
        const int a = 2 * pr, b = a + 1, ga = got >> a & 1, gb = got >> b & 1;
        for (int t = a; t <= b; t++)                       /* a half without records: silent (cleared once) */
            if (!(got >> t & 1) && !(tr_clean >> t & 1)) { int16_t *d = TR_SRC16(t); for (int i = 0; i < TR_SRC; i++) d[2 * i] = 0; tr_clean |= (uint8_t)(1 << t); }
        if ((ga | gb) && ok) {
            const tr_rs_job j = { tr_pair[pr], COEF, tr_out[a], tr_out[b], pos, inc, GAIN0, (int16_t)(gain_inc ? *gain_inc : 0) };
            tr_rs_pair(&j);
        } else for (int n = 0; n < 64; n++) tr_out[a][n] = tr_out[b][n] = 0;
    }
    for (int t = 0; t < TR_N; t++) {
        const int16_t *o = tr_out[t];
        int peak = 0;
        for (int j = 0; j < 64; j++) { const int v = o[j] < 0 ? -o[j] : o[j]; if (v > peak) peak = v; }
        tr_peak[t] = (int16_t)(peak > tr_peak[t] ? peak : tr_peak[t] - (tr_peak[t] >> 6));
    }
}

/* the takeover's resampler (hook.S _l1_rs_probe, in place of R246's body): the tracks, then L / R = the sum over the
 * tracks of the track x its gain, rounded as R246's gather (R.H = g * s), saturating adds in track order. Returns the
 * position after the block, as R246's (pos + 64 inc). */
extern volatile int16_t tm_g[8];
#ifdef OP1_HOST
int16_t tp_host_out[2][64];
#define TP_L tp_host_out[0]
#define TP_R tp_host_out[1]
#else
#define TP_L ((volatile int16_t *)0xFF800D34)
#define TP_R ((volatile int16_t *)0xFF800DB4)
#endif
static void tp_mix(volatile int16_t *L, volatile int16_t *R);
int32_t tp_resample(int32_t pos, int32_t inc, const int16_t *gain_inc)
{
    tr_resample(pos, inc, gain_inc);
    tp_mix(TP_L, TP_R);
    return pos + 64 * inc;
}
void tp_check_pre(int32_t pos, int32_t inc, const int16_t *gain_inc)   /* check build: ours into the shadow */
{
    tr_resample(pos, inc, gain_inc);
    tp_mix(tp_sh[0], tp_sh[1]);
}
void tp_check_post(void)                                       /* check build: R246's raw output vs the shadow */
{
    int32_t mx = 0;
    for (int n = 0; n < 64; n++) {
        int32_t a = TP_L[n] - tp_sh[0][n], b = TP_R[n] - tp_sh[1][n];
        if (a < 0) a = -a; if (b < 0) b = -b;
        if (a > mx) mx = a; if (b > mx) mx = b;
    }
    tp_ck_blocks++;
    if (mx) tp_ck_diff++;
    if (tr_dbg_inc) { tp_ck_run++; if (mx) tp_ck_rundiff++; if (mx > tp_ck_runmax) tp_ck_runmax = mx; }   /* tape moving */
    if (mx > 64 && tr_dbg_inc && !tp_dbg_have) {                           /* the first bad block, for a look (peek tp_dbg) */
        tp_dbg_have = 1;
        for (int k = 0; k < 64; k++) { tp_dbg[k] = TP_L[k]; tp_dbg[64 + k] = tp_sh[0][k]; tp_dbg[128 + k] = tr_out[0][k]; tp_dbg[192 + k] = tr_out[1][k]; }
        for (int k = 0; k < 8; k++) tp_dbg[256 + k] = tm_g[k];
        for (int k = 0; k < 65; k++) tp_dbg_rec[k] = (int32_t)tr_dbg_rec[k];
        tp_dbg_rec[65] = tr_dbg_pos; tp_dbg_rec[66] = tr_dbg_inc; tp_dbg_rec[67] = tp_ck_blocks;
    }
    if (mx > tp_ck_max) tp_ck_max = mx;
}
static void tp_mix(volatile int16_t *L, volatile int16_t *R)
{
    int16_t gl[TR_N], gr[TR_N];
    for (int t = 0; t < TR_N; t++) { gl[t] = tm_g[t]; gr[t] = tm_g[4 + t]; }
    for (int n = 0; n < 64; n++) {
        int32_t l = 0, r = 0;
        for (int t = 0; t < TR_N; t++) {
            const int16_t x = tr_out[t][n];
            l = sat16(l + rmul(gl[t], x));
            r = sat16(r + rmul(gr[t], x));
        }
        L[n] = (int16_t)l; R[n] = (int16_t)r;
    }
}

/* ---- USB: 6 channels to the host (main L / R + tracks 1..4), docs/21. Step a: tracks sent as silence. ---- */
#ifndef OP1_HOST
#define USB_DESC_INIT   ((int (*)(void))0x01A6A2F0)                 /* usb_audio::desc_init */
#define USB_ISO_IN      ((int (*)(void *, const void *, int))0x01A6A474)   /* smxUSBD iso IN queue (dev, buf, bytes) */
#define USB_CH          6
#define USB_MAXPKT      (46 * USB_CH * 3)                            /* 828 bytes, <= 1023 (full-speed iso) */
#define T_VOL           ((volatile int16_t *)0xFF801928)              /* mixer, per track (docs/54): volume */
#define T_LEV           ((volatile int16_t *)0xFF8018B4)              /* level */
#define T_MUTE          ((volatile int16_t *)0xFF8018C4)              /* mute fade (Core::update_mutes) */
static uint8_t usb6_pkt[USB_MAXPKT + 4] __attribute__((aligned(4)));

/* CALL @0x01A6ED96: after the stock template fill, widen the to-host stream */
extern int usbout_load(void);                                       /* screens/usbout.c: saved setting */
int usb6_desc_init(void)
{
    int r = USB_DESC_INIT();
    usb_tracks_live = (uint8_t)usbout_load();
    uint8_t *ac  = *(uint8_t **)(*(uintptr_t *)0x018FF308 + 0x0C);  /* AudioControl templates (0x018C1C68) */
    uint8_t *fmt = *(uint8_t **)(*(uintptr_t *)0x018FF318 + 0x24);  /* to-host AS templates (0x018C1A38) */
    ac[0xB1]  = USB_CH;                                              /* IT 11 bNrChannels (wChannelConfig stays L R) */
    fmt[0x3D] = USB_CH;                                              /* FORMAT_TYPE I bNrChannels */
    *(volatile uint16_t *)0x018FF1CA = USB_MAXPKT;                   /* EP 0x86 wMaxPacketSize */
    return r;
}

/* Track ring, parallel to the stock to-host ring 0x01AAEB60 (512 frames, write index 0x018B5FE4, read index
 * 0x018B5FE8): written right after the stock push (same index), read by the packet builder at the same frames. */
#define RING_WR (*(volatile uint32_t *)0x018B5FE4)
#define RING_RD (*(volatile uint32_t *)0x018B5FE8)
static int32_t usb6_ring[512][TR_N];                               /* 24-bit values (int16 << 8) */
/* test build OP1R_USB_RAWIN (docs/29, 24-bit input): channels 3 / 4 = the codec's raw input words L / R (top 24 bits of
 * the 32-bit SPORT words, the audio callback's 2nd argument, midi/pclk.c pc_in_b), channels 5 / 6 = tracks 3 / 4 */
#ifdef USB_RAWIN
extern const int32_t *volatile pc_in_b;
#endif
static void ring_put(uint32_t wr0, int silent)
{
    uint32_t wr1 = RING_WR;
    if (wr1 == wr0) return;                                         /* USB audio not active: nothing pushed */
#ifndef USB_RAWIN
    if (!usb_tracks_live) silent = 1;
#endif
    int32_t g[TR_N];                                                /* post fader (usb_tracks_live 2): the track's mixer volume
                                                                     * x level x mute fade, as Core_mix_master (and the FX2 aux
                                                                     * sends, fxroute.c); pan left out (mono channels); Q15 */
    for (int t = 0; t < TR_N; t++)
        g[t] = usb_tracks_live == 2 ? (((T_VOL[t] * T_LEV[t]) >> 15) * T_MUTE[t]) >> 15 : 32768;
    for (int f = 0; f < 64; f++) {
        int32_t *d = usb6_ring[(wr0 + f) & 0x1FF];
        for (int t = 0; t < TR_N; t++) d[t] = silent ? 0 : (tr_out[t][f] * g[t]) >> 7;   /* int16 << 8, x gain */
#ifdef USB_RAWIN
        const int32_t *raw = pc_in_b;
        if (raw) { d[0] = raw[2 * f] >> 8; d[1] = raw[2 * f + 1] >> 8; }   /* always, also with tracks off */
#endif
    }
}
/* L1 audio callback: CALL (P1) to usb_audio::push_lr (0xFFA049FC) / push_interleaved (0xFFA0487C, output muted) */
void usb6_push_lr(void *l, void *r)
{
    uint32_t wr0 = RING_WR;
    ((void (*)(void *, void *))0x019E1B3C)(l, r);
    ring_put(wr0, 0);
}
void usb6_push_interleaved(void *buf)
{
    uint32_t wr0 = RING_WR;
    ((void (*)(void *))0x019E1A88)(buf);
    ring_put(wr0, 1);
}

/* the three iso IN calls in usb_class_event_cb: 0x019E151C first packet and 0x019E1812 start-up (both silent: the
 * stock code zero-fills the buffer right before), 0x019E161C data (hardware: first build had 161C / 1812 swapped). Stock packet = frames x 6 bytes (L R, 24 bit) -> frames x 18 bytes (L R + 4 tracks). The stock
 * loop has just read `frames` frames up to the read index (an underrun repeats the last one; a resync jump is rare). */
static int iso_build(void *dev, const uint8_t *buf, int bytes, int silent)
{
    int frames = bytes / 6;
    if (frames > 46) frames = 46;
    uint32_t rd = RING_RD - (uint32_t)frames;
    uint8_t *o = usb6_pkt;
    for (int f = 0; f < frames; f++, buf += 6) {
        for (int i = 0; i < 6; i++) *o++ = buf[i];
        const int32_t *s = usb6_ring[(rd + (uint32_t)f) & 0x1FF];
        for (int t = 0; t < TR_N; t++) {                              /* 24-bit little endian */
            int32_t v = silent ? 0 : s[t];
            *o++ = (uint8_t)v; *o++ = (uint8_t)(v >> 8); *o++ = (uint8_t)(v >> 16);
        }
    }
    return USB_ISO_IN(dev, usb6_pkt, frames * USB_CH * 3);
}
int usb6_iso_in(void *dev, const uint8_t *buf, int bytes) { return iso_build(dev, buf, bytes, 0); }
int usb6_iso_in_silent(void *dev, const uint8_t *buf, int bytes) { return iso_build(dev, buf, bytes, 1); }
#endif
