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
static int16_t tr_src[TR_N][TR_SRC];

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

/* USB out setting (screens/usbout.c): 0 = master (track channels silent, no track work: saves CPU),
 * 1 = master + tracks. Always 6 channels, so it switches at once without re-enumeration. */
volatile uint8_t usb_tracks_live;

void tr_resample(int32_t pos, int32_t inc, const int16_t *gain_inc)
{
    extern int fr_tracks_needed(void);                       /* FX2 aux in use (screens/fxroute.c) */
    if (!usb_tracks_live && !fr_tracks_needed()) { tr_rec[0] = 0; return; }   /* no track work */
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
        if (!(got >> t & 1)) { for (int i = 0; i < TR_SRC; i++) tr_src[t][i] = 0; got |= (uint8_t)(1 << t); }
        for (int i = 0; i < cnt; i++) {                    /* add: at the loop wrap the mixer makes 2 passes into */
            int32_t v = tr_src[t][off + i] + src[i];          /* the same region (end + start of the loop), like L += */
            tr_src[t][off + i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
    }
    tr_rec[0] = 0;
    extern volatile uint8_t sc_blk, tr_at;
    tr_at = (uint8_t)(sc_blk + 1);                         /* fresh for the block fr_snap counts next */
    for (int t = 0; t < TR_N; t++) {
        int16_t *o = tr_out[t];
        int peak = 0;
        if (!(got >> t & 1)) { for (int j = 0; j < 64; j++) o[j] = 0; }
        else {
            int32_t p = pos, g = GAIN0, gi = gain_inc ? *gain_inc : 0;
            for (int j = 0; j < 64; j++) {
                p += inc;                                  /* as the master: advance first */
                int32_t idx = p >> 6, ph = 63 - (p & 63), acc = 0;
                if (idx >= 0 && idx + 16 <= TR_SRC) {
                    const int16_t *s = &tr_src[t][idx], *c = COEF + ph;
                    for (int k = 0; k < 16; k++) acc += c[64 * k] * s[k];
                }
                int16_t v = sat16(((acc >> 15) * g) >> 15);
                o[j] = v;
                if (v > peak) peak = v; else if (-v > peak) peak = -v;
                g = sat16(g + gi);
            }
        }
        tr_peak[t] = (int16_t)(peak > tr_peak[t] ? peak : tr_peak[t] - (tr_peak[t] >> 6));
    }
}

/* ---- USB: 6 channels to the host (main L / R + tracks 1..4), docs/21. Step a: tracks sent as silence. ---- */
#ifndef OP1_HOST
#define USB_DESC_INIT   ((int (*)(void))0x01A6A2F0)                 /* usb_audio::desc_init */
#define USB_ISO_IN      ((int (*)(void *, const void *, int))0x01A6A474)   /* smxUSBD iso IN queue (dev, buf, bytes) */
#define USB_CH          6
#define USB_MAXPKT      (46 * USB_CH * 3)                            /* 828 bytes, <= 1023 (full-speed iso) */
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
    for (int f = 0; f < 64; f++) {
        int32_t *d = usb6_ring[(wr0 + f) & 0x1FF];
        for (int t = 0; t < TR_N; t++) d[t] = silent ? 0 : (int32_t)tr_out[t][f] << 8;
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
