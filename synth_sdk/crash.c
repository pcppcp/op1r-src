/* Crash screen (docs/65). The stock exception handler (0xFFA00380) prints "SEQSTAT : RETX" / "ADDR : STATUS" and calls
 * the frame-pointer printer 0x0191995C, which can only show the handler's own frames (the vector stub's CALL also
 * overwrites RETS). Hooked at its entry (make_firmware.py HOOKS hook_crash): the same message, then
 *   op1r <version> [build time]
 *   the cause (EXCAUSE) and IPEND (the level that crashed)
 *   sp / fp of the crashed code, RETI, R0..R2 and P0..P2 (from the stock handler's frame), the ICPLB fault address
 *   (instruction faults)
 *   calls: the crashed code's frame-pointer chain (exact return addresses); if that gives fewer than 3, stack: values
 *   on the stack that look like code addresses (candidates). Code addresses as 0x... (plugins as file+0x offset),
 *   for tools/op1crash.py (with that release's symbol map, symmap.py); data values (registers, sp, fp) bare hex, so
 *   the decoder does not take them for code.
 * Exception context: no allocation, reads only plausible stack memory, text into the stock buffer. */
#include "op1_synth.h"

#define CRASH_TXT  ((char *)0x01BA138C)                         /* 512 bytes shown on the error screen */
#define CRASH_MAX  511
#define SNPRINTF   ((int (*)(char *, int, const char *, ...))0x01A4CCD8)
#define BOOT_TEXT  ((const char *)0x018F7358)                   /* "...op1r <n> [build time]" (make_firmware.py) */
struct plug_img { uint32_t base, size; char file[24]; };
extern struct plug_img plug_imgs[32];
extern uint32_t plug_img_n;
extern char _image_end[];                                 /* link.ld __image_end (C names get a leading _) */

static int plug_of(uint32_t v)
{
    for (uint32_t i = 0; i < plug_img_n && i < 32; i++) if (v - plug_imgs[i].base < plug_imgs[i].size) return (int)i;
    return -1;
}
static int code_addr(uint32_t v)
{
    if (v & 1) return 0;
    return (v >= 0x01900000 && v < 0x01B00000) || (v >= 0x01E00000 && v < (uint32_t)_image_end) ||
           (v >= 0xFFA00000 && v < 0xFFA14000) || plug_of(v) >= 0;
}
static char *add(char *p, const char *s) { while (*s && p < CRASH_TXT + CRASH_MAX) *p++ = *s++; *p = 0; return p; }

static const char *cause(uint32_t seqstat)                   /* EXCAUSE (SEQSTAT bits 5..0), the ones that crash */
{
    switch (seqstat & 0x3F) {
    case 0x21: return "undefined instruction"; case 0x22: return "illegal instr combination";
    case 0x23: return "data access protection"; case 0x24: return "misaligned data access";
    case 0x26: return "data cplb miss"; case 0x27: return "data cplb multiple hits";
    case 0x28: return "watchpoint"; case 0x29: return "misaligned instruction";
    case 0x2A: return "instruction protection"; case 0x2B: return "instruction cplb miss";
    case 0x2C: return "instruction cplb multiple"; case 0x2D: return "illegal supervisor instr";
    case 0x2E: return "illegal supervisor access"; case 0x3F: return "hardware error";
    default: return "exception";
    }
}
/* reads only inside the stack window the handler itself just used (SP .. SP + 2 KB): anything else (a junk FP: op1r's
 * -O2 code uses FP as a register) can miss the CPLBs, and a miss inside the exception handler is a double fault that
 * locks the core (device: no screen, no hang-detector reset) */
static uint32_t win_lo, win_hi;
static int ram_ok(uint32_t a)
{
    return !(a & 3) && a >= win_lo && a + 4 <= win_hi &&
           ((a >= 0x1000 && a < 0x02000000) || (a >= 0xFFB00000 && a < 0xFFB01000));
}
static char *code_txt(char *t, int n, uint32_t w, const char *sep)   /* plugin file+offset, else the raw address */
{
    int pi = plug_of(w);
    if (pi < 0) { SNPRINTF(t, n, "%#x%s", w, sep); return t; }
    char f[16]; int k = 0;
    for (; k < 15 && plug_imgs[pi].file[k] && plug_imgs[pi].file[k] != '.'; k++) f[k] = plug_imgs[pi].file[k];
    f[k] = 0;
    SNPRINTF(t, n, "%s+%#x%s", f, w - plug_imgs[pi].base, sep);
    return t;
}

/* the stock handler's frame (0xFFA00374: LINK 0x14 then ASTAT, R0..R3, R7, P0..P2 pushed): [fp] = the crashed code's
 * FP, [fp + 4] = the stub's return 0xFFA035A8, its SP = fp + 8; the saved registers below fp */
#define HF_R0 (-0x1C)
#define HF_R1 (-0x20)
#define HF_R2 (-0x24)
#define HF_P0 (-0x30)
#define HF_P1 (-0x34)
#define HF_P2 (-0x38)
static uint32_t rd(uint32_t a) { return *(const volatile uint32_t *)a; }

void crash_print(const char *msg)
{
#ifdef WDOG
    { extern void wd_crash(void); wd_crash(); }                 /* the hang detector waits ~35 s (wdog.c): time for a photo */
#endif
    /* a black screen under the text: the stock tail (hook.S crash_stub) draws it with the built-in bitmap font into both
     * display frame buffers (0x01941558: 16-bit pixels, 0x140 rows of 0x200 bytes) and sends one (0x01A36AB6) */
    for (int b = 0; b < 2; b++) {                              /* (the DMA sends up to 0x190 rows: 0x32000 bytes) */
        const uint32_t a = b ? 0x03000000u : 0x02000000u;
        for (uint32_t i = 0; i < 0x32000; i += 4) *(volatile uint32_t *)(a + i) = 0;
        ((void (*)(uint32_t, uint32_t, int))0x01AA185C)(a, a + 0x32000, 0);   /* flush_data_cache_range: out to SDRAM */
    }
    char *p = CRASH_TXT, t[64];
    while (*p && p < CRASH_TXT + CRASH_MAX) p++;
    p = add(add(add(p, "Congratulations! Report this.\n\n"), msg), "\n");
    uint32_t sp, seq, reti;
    __asm__ volatile ("%0 = SP; %1 = SEQSTAT; %2 = RETI;" : "=d"(sp), "=d"(seq), "=d"(reti));
    const uint32_t ipend = *(const volatile uint32_t *)0xFFE02108, icplb = *(const volatile uint32_t *)0xFFE0100C;
    win_lo = sp; win_hi = sp + 2048;
    const char *v = BOOT_TEXT;                                  /* the version from the boot text */
    for (int i = 0; i < 80 && v[i]; i++) if (v[i] == 'o' && v[i + 1] == 'p' && v[i + 2] == '1' && v[i + 3] == 'r') { v += i; break; }
    if (v[0] != 'o') v = "op1r ?";
    int n = 0; while (n < 28 && v[n] >= ' ') n++;               /* "op1r 67 2026-11-12 21:32" */
    char ver[32]; for (int i = 0; i < n; i++) ver[i] = v[i]; ver[n] = 0;
    p = add(add(p, ver), "\n");
    SNPRINTF(t, sizeof t, "%s ipend %x\n", cause(seq), ipend);     /* ipend: which level crashed (audio, main) */
    p = add(p, t);
    /* the handler's frame: the slot of 0xFFA035A8 above the current SP (1 KB at most) */
    uint32_t hf = 0;
    for (uint32_t a = sp; ram_ok(a) && a < sp + 1024; a += 4) if (rd(a) == 0xFFA035A8u) { hf = a - 4; break; }
    if (!ram_ok(hf + HF_P2)) hf = 0;
    if (hf && ram_ok(hf + HF_P2)) {
        SNPRINTF(t, sizeof t, "sp %x fp %x reti %#x\n", hf + 8, rd(hf), reti); p = add(p, t);
        SNPRINTF(t, sizeof t, "r0 %x r1 %x r2 %x\n", rd(hf + HF_R0), rd(hf + HF_R1), rd(hf + HF_R2)); p = add(p, t);
        SNPRINTF(t, sizeof t, "p0 %x p1 %x p2 %x\n", rd(hf + HF_P0), rd(hf + HF_P1), rd(hf + HF_P2)); p = add(p, t);
    } else { SNPRINTF(t, sizeof t, "sp %x reti %#x\n", sp, reti); p = add(p, t); }
    if ((seq & 0x3F) >= 0x29 && (seq & 0x3F) <= 0x2C) { SNPRINTF(t, sizeof t, "icplb addr %x\n", icplb); p = add(p, t); }
    /* the calls: the crashed code's frame-pointer chain ([fp + 4] = return address, [fp] = the caller's fp) */
    int found = 0;
    if (hf) {
        uint32_t fp = rd(hf);
        p = add(p, "calls");
        for (int k = 0; k < 8 && ram_ok(fp) && ram_ok(fp + 4); k++) {
            const uint32_t ret = rd(fp + 4), nx = rd(fp);
            if (!code_addr(ret)) break;
            p = add(p, (found % 3) ? " " : found ? "" : " ");
            p = add(p, code_txt(t, sizeof t, ret, (found % 3) == 2 ? "\n" : ""));
            found++;
            if (nx <= fp) break;                                 /* callers lie higher: else the chain is broken */
            fp = nx;
        }
        if (found % 3) p = add(p, "\n");
        if (!found) p = add(p, " -\n");
    }
    /* the stack: values that look like code addresses (leaf functions keep no frame; candidates, nearest first) */
    uint32_t lo = hf ? hf + 8 : sp, hi = lo + 1024;
    int st = 0;
    if (found < 3) {
        p = add(p, "stack");
        for (uint32_t a = lo; ram_ok(a) && a < hi && st < 6; a += 4) {
            const uint32_t w = rd(a);
            if (!code_addr(w)) continue;
            p = add(p, " ");
            p = add(p, code_txt(t, sizeof t, w, (st % 3) == 2 ? "\n" : ""));
            st++;
        }
        if (st % 3 || !st) p = add(p, "\n");
    }
    p = add(p, "\nPress any key to restart...\n");
}

/* ---- after the screen: wait for a key, then restart (no interrupts run here: the keys are read by polled TWI) ----
 * The key expanders (MCP23017 at TWI 0x22..0x26) as R246 sets them up (gpio::scan_all: IOCON 0x60 / 0x20 = bank 0,
 * no address increment): GPIOA 0x12, GPIOB 0x13, one byte per read (a read also clears their interrupt). A bit going
 * 1 -> 0 (a key pressed, an encoder turned; active low) restarts via the watchdog as TE reboots (0x01A88794); a key
 * released (the one held at the crash) does not. The hang detector's ~35 s count (wd_crash) stays the fallback. */
#define TWI(o)        (*(volatile uint16_t *)(0xFFC01400u + (o)))
#define TWI_MCTL      0x14
#define TWI_MADDR     0x1C
#define TWI_ISTAT     0x20
#define TWI_IMASK     0x24
#define TWI_FIFOCTL   0x28
#define TWI_XMT       0x80
#define TWI_RCV       0x88
#define TWI_MCOMP     0x10
#define TWI_MERR      0x20
static int twi_wait(void)                                    /* 1 done, 0 error / timeout */
{
    for (int i = 0; i < 400000; i++) {
        const uint16_t st = TWI(TWI_ISTAT);
        if (st & TWI_MERR) { TWI(TWI_ISTAT) = 0xFF; return 0; }
        if (st & TWI_MCOMP) { TWI(TWI_ISTAT) = 0xFF; return 1; }
    }
    return 0;
}
static int twi_reg(int dev, int reg)                         /* one register byte, -1 if the transfer failed */
{
    TWI(TWI_MCTL) = 0; TWI(TWI_FIFOCTL) = 3; TWI(TWI_FIFOCTL) = 0; TWI(TWI_ISTAT) = 0xFF;
    TWI(TWI_MADDR) = (uint16_t)dev;
    TWI(TWI_XMT) = (uint16_t)reg;
    TWI(TWI_MCTL) = (1u << 6) | 1u;                          /* 1 byte, transmit, master on */
    if (!twi_wait()) return -1;
    TWI(TWI_MCTL) = (1u << 6) | 4u | 1u;                     /* 1 byte, receive */
    if (!twi_wait()) return -1;
    return TWI(TWI_RCV) & 0xFF;
}
/* only the key pins (op1emu's key map): 0x22 the encoder pushes (B 4..7; its other pins are the encoders' quadrature,
 * which can rest at 0), 0x23..0x25 all, 0x26 A 0..5 (its other pins are outputs / other signals) */
static const uint8_t key_mask[10] = { 0x00, 0xF0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x3F, 0x00 };
static void keys_read(uint8_t *k, uint8_t *ok)               /* 10 bytes: 0x22..0x26 x A / B; ok[i]: the read worked */
{
    for (int d = 0; d < 5; d++)
        for (int r = 0; r < 2; r++) {
            const int i = d * 2 + r;
            if (!key_mask[i]) { k[i] = 0xFF; ok[i] = 1; continue; }
            const int v = twi_reg(0x22 + d, 0x12 + r);
            ok[i] = v >= 0; k[i] = (uint8_t)(v < 0 ? 0xFF : v);
        }
}
void crash_wait(void)
{
    TWI(TWI_IMASK) = 0;                                       /* polled (the TWI ISR cannot run anyway) */
    uint8_t k[10], ok[10];
    int idle = 0, seen = 0;                                   /* armed after 2 scans with no key down (a key held at the
                                                               * crash is let go first; no timer: CYCLES is not wall time
                                                               * in op1emu); then a press read twice in a row restarts */
    for (;;) {
        keys_read(k, ok);
        int down = 0, good = 0;
        for (int i = 0; i < 10; i++) if (ok[i]) { good++; down |= ~k[i] & key_mask[i]; }
        if (good < 8) { seen = 0; continue; }                 /* (most reads failed: no decision) */
        if (idle < 2) { idle = down ? 0 : idle + 1; continue; }
        seen = down ? seen + 1 : 0;
        if (seen >= 2) break;
    }
    *(volatile uint16_t *)0xFFC00200 = 0x0AD0;                /* watchdog: reset event, at once (as TE's reboot) */
    *(volatile uint32_t *)0xFFC00204 = 0x100;
    *(volatile uint32_t *)0xFFC00208 = 0;
    *(volatile uint16_t *)0xFFC00200 = 0x0FF0;
    for (;;) __asm__ volatile ("IDLE;");
}
